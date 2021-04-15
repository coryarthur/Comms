#include <uhd/exception.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/static.hpp>
#include <uhd/utils/thread.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/math/special_functions/round.hpp>
#include <boost/program_options.hpp>
#include <boost/thread/thread.hpp>
#include <csignal>
#include <fstream>
#include <iostream>
#include <array>
#include <mutex>
#include <thread>
#include <chrono>
#include "queue.hpp"
#include "filters.hpp"
#include "algorithms.hpp"

int lpf_len = 80;
std::complex<float> lpf_coef[80] = {
  0.0004677995457,-0.0005329191335,-0.001023291261,-0.001823803177,-0.002877128776,
  -0.004106027074,-0.005393796135,-0.006584335584,-0.007493105251,-0.007929120213,
  -0.007722722366,-0.006756400689,-0.004994347692,-0.002505606972,0.0005250737886,
   0.003801849438, 0.006939678919, 0.009503492154,   0.0110612642,   0.0112457443,
   0.009817645885, 0.006721359212, 0.002122201025,-0.003579673823,-0.009763498791,
   -0.01563161425,  -0.0202895198,  -0.0228331387,  -0.0224616155, -0.01857960224,
   -0.01088931505,0.0005469227908,  0.01527797151,  0.03248177841,  0.05102368444,
    0.06955442578,  0.08663711697,   0.1008892059,    0.111122936,    0.116469197,
      0.116469197,    0.111122936,   0.1008892059,  0.08663711697,  0.06955442578,
    0.05102368444,  0.03248177841,  0.01527797151,0.0005469227908, -0.01088931505,
   -0.01857960224,  -0.0224616155,  -0.0228331387,  -0.0202895198, -0.01563161425,
  -0.009763498791,-0.003579673823, 0.002122201025, 0.006721359212, 0.009817645885,
     0.0112457443,   0.0110612642, 0.009503492154, 0.006939678919, 0.003801849438,
  0.0005250737886,-0.002505606972,-0.004994347692,-0.006756400689,-0.007722722366,
  -0.007929120213,-0.007493105251,-0.006584335584,-0.005393796135,-0.004106027074,
  -0.002877128776,-0.001823803177,-0.001023291261,-0.0005329191335,0.0004677995457
};


namespace po = boost::program_options;

// class used to pass sample blocks and block numbers to FIFOs 
template<std::size_t BufferSize>
class SampleBuffer {
    public:
        std::complex<float> SampleBlock[BufferSize];
        int blockNum;
};

/***********************************************************************
 * Signal handlers
 **********************************************************************/
static bool stop_signal_called = false;
void sig_int_handler(int) {
    stop_signal_called = true;
}

class SharedPrinter {
  private:
    std::mutex mtx;
  public:
    void print(std::string message) {
        mtx.lock();
        std::cout << message;
        mtx.unlock();
    }
}; 


/***********************************************************************
 * recv_to_fifo function
 **********************************************************************/
void recv_to_fifo(uhd::usrp::multi_usrp::sptr usrp,
    const std::string& cpu_format,
    const std::string& wire_format,
    size_t samps_per_buff,
    int num_requested_samples,
    double settling_time,
    std::vector<size_t> rx_channel_nums,
    MutexFIFO<SampleBuffer<10000>>& fifo) {

    int num_total_samps = 0;
    // create a receive streamer
    uhd::stream_args_t stream_args(cpu_format, wire_format);
    stream_args.channels             = rx_channel_nums;
    uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args);

    // Prepare buffers for received samples and metadata
    uhd::rx_metadata_t md;

    SampleBuffer<10000> rxBuffer;




    bool overflow_message = true;
    double timeout =
        settling_time + 0.1f; // expected settling time + padding for first recv

    // setup streaming
    uhd::stream_cmd_t stream_cmd(stream_cmd.STREAM_MODE_START_CONTINUOUS);
    stream_cmd.num_samps  = num_requested_samples;
    stream_cmd.stream_now = true;
    rx_stream->issue_stream_cmd(stream_cmd);

while (not stop_signal_called
            and (num_requested_samples > num_total_samps or num_requested_samples == 0)) {

        size_t num_rx_samps = rx_stream->recv(rxBuffer.SampleBlock, samps_per_buff, md, timeout);
        timeout             = 0.1f; // small timeout for subsequent recv

        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
            std::cout << boost::format("Timeout while streaming") << std::endl;
            break;
        }
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
            if (overflow_message) {
                overflow_message = false;
                std::cerr
                    << boost::format(
                        "Got an overflow indication. Please consider the following:\n"
                        "  Your write medium must sustain a rate of %fMB/s.\n"
                        "  Dropped samples will not be written to the file.\n"
                        "  Please modify this example for your purposes.\n"
                        "  This message will not appear again.\n")
                        % (usrp->get_rx_rate() * sizeof(std::complex<int16_t>) / 1e6);
            }
            continue;
        }
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
            throw std::runtime_error(
                str(boost::format("Receiver error %s") % md.strerror()));
        }
        num_total_samps += num_rx_samps;
        fifo.lock();
        fifo.push(rxBuffer);
        fifo.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Shut down receiver
    stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    rx_stream->issue_stream_cmd(stream_cmd);


}

void filter_Block(MutexFIFO<SampleBuffer<10000>>& fifo, MutexFIFO<SampleBuffer<1000>>& power_fifo, SharedPrinter& printer, 
                    FilterPolyphase& filter, float alpha, float threshold) {
    SampleBuffer<10000> rawSamples;
    int outLen = filter.out_len();
    std::complex<float> *filteredBlock = new std::complex<float>[outLen]();
    float previous_Output = 0;
    SampleBuffer<1000> powerBlock;
    powerBlock.blockNum = 0;
    std::ostringstream message;
    bool sampleDetected = false;
    while (not stop_signal_called) {
        if (fifo.size() != 0) {
            // grab raw samples
            fifo.lock();
            fifo.pop(rawSamples);
            fifo.unlock();
            // filter raw samples
            filter.filter(rawSamples.SampleBlock, filteredBlock);
            filter.set_head(0); 
            for(int i = 0; i < outLen-1070; i++) {
                sampleDetected = power_Detection(alpha, threshold, filteredBlock[i], &previous_Output);
                if (sampleDetected) {
                    // copy next 1000 samples and push to FIFO for processing in another thread
                    memcpy(powerBlock.SampleBlock, &filteredBlock[i], 1000*(sizeof(std::complex<float>)));
                    powerBlock.blockNum++;
                    power_fifo.lock();
                    power_fifo.push(powerBlock);
                    power_fifo.unlock();
                    // threshold was met so jump to next possible block index
                    i += 999;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

void calculate_Power(MutexFIFO<SampleBuffer<1000>>& power_fifo, SharedPrinter& printer, double rx_rate) {
    SampleBuffer<1000> powerBlock;
    std::complex<float> avg_Pow = 0;
    std::ostringstream message;
    std::ofstream myfile;
    while (not stop_signal_called) {
        // if FIFO has data then process it, otherwise sleep
        if(power_fifo.size() != 0) {
            power_fifo.lock();
            power_fifo.pop(powerBlock);
            power_fifo.unlock();
            // if statement used here to essentially ignore the first set of packets we receive
            if (powerBlock.blockNum > ((rx_rate/1000000)*2 + 10)) {
                myfile.open ("outputs.txt");
                // write samples to file and calculate power
                for(size_t i = 0; i < 1000; i++) {
                    myfile << powerBlock.SampleBlock[i] << "\n";
                    avg_Pow += abs(powerBlock.SampleBlock[i])*abs(powerBlock.SampleBlock[i]);
                }
                myfile.close();
                avg_Pow /= 1000.0;
                message << "Threshold met. Average power for sample block = " << real(avg_Pow) << std::endl;
                printer.print(message.str());
                message.str("");
                message.clear();
                avg_Pow = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

int UHD_SAFE_MAIN(int argc, char *argv[]) {

    // receive variables to be set by po
    std::string rx_args, rx_ant, rx_subdev, rx_channels, ref, otw;
    size_t total_num_samps, spb;
    double rx_rate, rx_freq, rx_gain, rx_bw;
    double settling;

    int nt_filter, U, D;
    float alpha, threshold;
    // Construct FIFO to hold all messages
    MutexFIFO<SampleBuffer<10000>> fifo;
    MutexFIFO<SampleBuffer<1000>> power_fifo;

    // Instantiate the SharedPrinter object
    SharedPrinter printer;


    // setup the program options
    po::options_description desc("Allowed options");
    // clang-format off
    desc.add_options()
        ("help", "help message")
        ("rx-args", po::value<std::string>(&rx_args)->default_value(""), "uhd receive device address args")
        ("nsamps", po::value<size_t>(&total_num_samps)->default_value(0), "total number of samples to receive")
        ("settling", po::value<double>(&settling)->default_value(double(0.2)), "settling time (seconds) before receiving")
        ("spb", po::value<size_t>(&spb)->default_value(10000), "samples per buffer, 0 for default")
        ("rx-rate", po::value<double>(&rx_rate)->default_value(1000000), "rate of receive incoming samples")
        ("rx-freq", po::value<double>(&rx_freq)->default_value(2473000000), "receive RF center frequency in Hz")
        ("rx-gain", po::value<double>(&rx_gain)->default_value(0), "gain for the receive RF chain")
        ("rx-ant", po::value<std::string>(&rx_ant), "receive antenna selection")
        ("rx-subdev", po::value<std::string>(&rx_subdev), "receive subdevice specification")
        ("rx-bw", po::value<double>(&rx_bw), "analog receive filter bandwidth in Hz")
        ("ref", po::value<std::string>(&ref)->default_value("internal"), "clock reference (internal, external, mimo)")
        ("otw", po::value<std::string>(&otw)->default_value("sc16"), "specify the over-the-wire sample mode")
        ("rx-channels", po::value<std::string>(&rx_channels)->default_value("0"), "which RX channel(s) to use (specify \"0\", \"1\", \"0,1\", etc)")
        ("rx-int-n", "tune USRP RX with integer-N tuning")

        ("nt-filter", po::value<int>(&nt_filter)->default_value(1), "number of filter threads")
        ("U", po::value<int>(&U)->default_value(4), "upsampling rate")
        ("D", po::value<int>(&D)->default_value(5), "downsampling rate")

        ("alpha", po::value<float>(&alpha)->default_value(0.8), "instantaneous power smoothing parameter")
        ("threshold", po::value<float>(&threshold)->default_value(1.9e-5), "instantaneous power threshold")
    ;
    // clang-format on
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    // print the help message
    if (vm.count("help")) {
        std::cout << boost::format("UHD TXRX Loopback to File %s") % desc << std::endl;
        return ~0;
    }

    // create a usrp device
    std::cout << boost::format("Creating the receive usrp device with: %s...") % rx_args
              << std::endl;
    uhd::usrp::multi_usrp::sptr rx_usrp = uhd::usrp::multi_usrp::make(rx_args);

    if (vm.count("rx-subdev"))
        rx_usrp->set_rx_subdev_spec(rx_subdev);

    std::vector<std::string> rx_channel_strings;
    std::vector<size_t> rx_channel_nums;
    boost::split(rx_channel_strings, rx_channels, boost::is_any_of("\"',"));
    for (size_t ch = 0; ch < rx_channel_strings.size(); ch++) {
        size_t chan = std::stoi(rx_channel_strings[ch]);
        if (chan >= rx_usrp->get_rx_num_channels()) {
            throw std::runtime_error("Invalid RX channel(s) specified.");
        } else
            rx_channel_nums.push_back(std::stoi(rx_channel_strings[ch]));
    }

    // Lock mboard clocks
    if (vm.count("ref")) {
        rx_usrp->set_clock_source(ref);
    }

    // set the receive sample rate
    if (not vm.count("rx-rate")) {
        std::cerr << "Please specify the sample rate with --rx-rate" << std::endl;
        return ~0;
    }
    std::cout << boost::format("Setting RX Rate: %f Msps...") % (rx_rate / 1e6)
              << std::endl;
    rx_usrp->set_rx_rate(rx_rate);
    std::cout << boost::format("Actual RX Rate: %f Msps...")
                     % (rx_usrp->get_rx_rate() / 1e6)
              << std::endl
              << std::endl;

    std::cout << boost::format("Using RX Device: %s") % rx_usrp->get_pp_string()
              << std::endl;


    for (size_t ch = 0; ch < rx_channel_nums.size(); ch++) {
        size_t channel = rx_channel_nums[ch];
        if (rx_channel_nums.size() > 1) {
            std::cout << "Configuring RX Channel " << channel << std::endl;
        }

        // set the receive center frequency
        if (not vm.count("rx-freq")) {
            std::cerr << "Please specify the center frequency with --rx-freq"
                      << std::endl;
            return ~0;
        }
        std::cout << boost::format("Setting RX Freq: %f MHz...") % (rx_freq / 1e6)
                  << std::endl;
        uhd::tune_request_t rx_tune_request(rx_freq);
        if (vm.count("rx-int-n"))
            rx_tune_request.args = uhd::device_addr_t("mode_n=integer");
        rx_usrp->set_rx_freq(rx_tune_request, channel);
        std::cout << boost::format("Actual RX Freq: %f MHz...")
                         % (rx_usrp->get_rx_freq(channel) / 1e6)
                  << std::endl
                  << std::endl;

        // set the receive rf gain
        if (vm.count("rx-gain")) {
            std::cout << boost::format("Setting RX Gain: %f dB...") % rx_gain
                      << std::endl;
            rx_usrp->set_rx_gain(rx_gain, channel);
            std::cout << boost::format("Actual RX Gain: %f dB...")
                             % rx_usrp->get_rx_gain(channel)
                      << std::endl
                      << std::endl;
        }

        // set the receive analog frontend filter bandwidth
        if (vm.count("rx-bw")) {
            std::cout << boost::format("Setting RX Bandwidth: %f MHz...") % (rx_bw / 1e6)
                      << std::endl;
            rx_usrp->set_rx_bandwidth(rx_bw, channel);
            std::cout << boost::format("Actual RX Bandwidth: %f MHz...")
                             % (rx_usrp->get_rx_bandwidth(channel) / 1e6)
                      << std::endl
                      << std::endl;
        }

        // set the receive antenna
        if (vm.count("rx-ant"))
            rx_usrp->set_rx_antenna(rx_ant, channel);
    }

    // Check Ref and LO Lock detect
    std::vector<std::string> rx_sensor_names;
    rx_sensor_names = rx_usrp->get_rx_sensor_names(0);
    if (std::find(rx_sensor_names.begin(), rx_sensor_names.end(), "lo_locked")
        != rx_sensor_names.end()) {
        uhd::sensor_value_t lo_locked = rx_usrp->get_rx_sensor("lo_locked", 0);
        std::cout << boost::format("Checking RX: %s ...") % lo_locked.to_pp_string()
                  << std::endl;
        UHD_ASSERT_THROW(lo_locked.to_bool());
    }

    rx_sensor_names = rx_usrp->get_mboard_sensor_names(0);
    if ((ref == "mimo")
        and (std::find(rx_sensor_names.begin(), rx_sensor_names.end(), "mimo_locked")
                != rx_sensor_names.end())) {
        uhd::sensor_value_t mimo_locked = rx_usrp->get_mboard_sensor("mimo_locked", 0);
        std::cout << boost::format("Checking RX: %s ...") % mimo_locked.to_pp_string()
                  << std::endl;
        UHD_ASSERT_THROW(mimo_locked.to_bool());
    }
    if ((ref == "external")
        and (std::find(rx_sensor_names.begin(), rx_sensor_names.end(), "ref_locked")
                != rx_sensor_names.end())) {
        uhd::sensor_value_t ref_locked = rx_usrp->get_mboard_sensor("ref_locked", 0);
        std::cout << boost::format("Checking RX: %s ...") % ref_locked.to_pp_string()
                  << std::endl;
        UHD_ASSERT_THROW(ref_locked.to_bool());
    }

    if (total_num_samps == 0) {
        std::signal(SIGINT, &sig_int_handler);
        std::cout << "Press Ctrl + C to stop streaming..." << std::endl;
    }
    int num_filter_threads = 1;
    int num_power_threads = 1;
    int nt_total = num_power_threads + num_filter_threads + 1;

    // generate filter
    FilterPolyphase lpf(U, D, spb, lpf_len, lpf_coef, nt_filter);
    lpf.set_head(1);

    std::thread worker[nt_total];

    for (int i=0; i<num_filter_threads; i++) {
        worker[i] = std::thread(&filter_Block, std::ref(fifo), std::ref(power_fifo), std::ref(printer), std::ref(lpf), alpha, threshold);
    }
    for (int i=num_filter_threads; i<num_filter_threads+num_power_threads; i++) {
        worker[i] = std::thread(&calculate_Power, std::ref(power_fifo), std::ref(printer), rx_rate);
    }

   worker[nt_total-1] = std::thread(&recv_to_fifo, rx_usrp, "fc32", "sc16", spb, total_num_samps, settling, 
                              rx_channel_nums, std::ref(fifo));

    // Join all worker threads
    for (int i=0; i<nt_total; i++) {
        if (worker[i].joinable()) worker[i].join();
    }

    std::cout << std::endl;
    return EXIT_SUCCESS;
}