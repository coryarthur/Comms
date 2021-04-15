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
#include <mutex>
#include <thread>
#include <chrono>
#include "queue.hpp"

namespace po = boost::program_options;

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
    MutexFIFO<std::complex<float>>& fifo) {

    int num_total_samps = 0;
    // create a receive streamer
    uhd::stream_args_t stream_args(cpu_format, wire_format);
    stream_args.channels             = rx_channel_nums;
    uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args);

    // Prepare buffers for received samples and metadata
    uhd::rx_metadata_t md;
    std::complex<float> sample_Buffer[10000];

    std::complex<float> block_Num = 0;




    bool overflow_message = true;
    double timeout =
        settling_time + 0.1f; // expected settling time + padding for first recv

    // setup streaming
    uhd::stream_cmd_t stream_cmd(stream_cmd.STREAM_MODE_START_CONTINUOUS);
    stream_cmd.num_samps  = num_requested_samples;
    stream_cmd.stream_now = true;
    rx_stream->issue_stream_cmd(stream_cmd);

    while (not stop_signal_called) {
        while (not stop_signal_called
            and (num_requested_samples > num_total_samps or num_requested_samples == 0)) {
            size_t num_rx_samps = rx_stream->recv(sample_Buffer, samps_per_buff, md, timeout);
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
        }
        num_total_samps = 0;
        fifo.lock();
        fifo.push(block_Num);
        block_Num += 1;
        for (size_t i = 0; i < 10000; i++) {
            fifo.push(sample_Buffer[i]);
        }
        fifo.unlock();
        usleep(50000);
        //std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}


void calculate_Power(MutexFIFO<std::complex<float>>& fifo, SharedPrinter& printer) {
        std::complex<float> sample = 0;
        std::complex<float> avg_Pow = 0;
        std::complex<float> block_Num = 0;
    while (not stop_signal_called) {
        while(fifo.size() != 10001);
        fifo.lock();
        fifo.pop(block_Num);
        for(size_t i = 0; i < 10000; i++) {
            fifo.pop(sample);
            avg_Pow += abs(sample)*abs(sample);
        }
        fifo.unlock();
        avg_Pow /= 10000.0;
        std::ostringstream message;
        message << "Average power for sample block " << real(block_Num) << " = " << real(avg_Pow) << std::endl;
        printer.print(message.str());
        sample = 0;
        avg_Pow = 0;
        usleep(50000);
        //std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

int UHD_SAFE_MAIN(int argc, char *argv[]) {

    // receive variables to be set by po
    std::string rx_args, rx_ant, rx_subdev, rx_channels, ref, otw;
    size_t total_num_samps, spb;
    double rx_rate, rx_freq, rx_gain, rx_bw;
    double settling;

    // Construct FIFO to hold all messages
    MutexFIFO<std::complex<float>> fifo;

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
        ("spb", po::value<size_t>(&spb)->default_value(0), "samples per buffer, 0 for default")
        ("rx-rate", po::value<double>(&rx_rate), "rate of receive incoming samples")
        ("rx-freq", po::value<double>(&rx_freq), "receive RF center frequency in Hz")
        ("rx-gain", po::value<double>(&rx_gain), "gain for the receive RF chain")
        ("rx-ant", po::value<std::string>(&rx_ant), "receive antenna selection")
        ("rx-subdev", po::value<std::string>(&rx_subdev), "receive subdevice specification")
        ("rx-bw", po::value<double>(&rx_bw), "analog receive filter bandwidth in Hz")
        ("ref", po::value<std::string>(&ref)->default_value("internal"), "clock reference (internal, external, mimo)")
        ("otw", po::value<std::string>(&otw)->default_value("sc16"), "specify the over-the-wire sample mode")
        ("rx-channels", po::value<std::string>(&rx_channels)->default_value("0"), "which RX channel(s) to use (specify \"0\", \"1\", \"0,1\", etc)")
        ("rx-int-n", "tune USRP RX with integer-N tuning")
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
    int num_Pow_Threads = 2;


    std::thread worker[num_Pow_Threads+1];
    for (int i=0; i<num_Pow_Threads; i++) {
        worker[i] = std::thread(&calculate_Power,std::ref(fifo), std::ref(printer));
    }

   worker[num_Pow_Threads] = std::thread(&recv_to_fifo, rx_usrp, "fc32", "sc16", 10000, 10000, settling, 
                              rx_channel_nums, std::ref(fifo));

    // Join all worker threads
    for (int i=0; i<num_Pow_Threads+1; i++) {
        if (worker[i].joinable()) worker[i].join();
    }

    std::cout << std::endl;
    return EXIT_SUCCESS;
}