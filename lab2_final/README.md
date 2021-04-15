to build: g++ filter.cpp algorithms.cpp lab2.cpp -o lab2 -luhd -lboost_program_options -lpthread -lfftw3f -lfftw3f_threads -DUSE_VOLK -lvolk 

default values

rx-rate 1,000,000

alpha 0.8

threshold 2e-5

U 4

D 5


Example inputs that allow us to detect packets at a decent rate

./lab2 --rx-rate 25000000 --threshold 8e-5

./lab2 --rx-rate 10000000 --threshold 4.5e-5

./lab2 --alpha 0.1 --threshold 1e-5
