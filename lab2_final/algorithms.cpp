#include <complex>      // std::complex
#include <cmath>        // std::abs
#include "algorithms.hpp"

bool power_Detection(float alpha, float threshold, std::complex<float> current_Input, float* previous_Output) {
    float current_Output = 0;

    /* instantaneous power detector  
    y[n] = a*y[n-1] + (1-a)|x[n]|^2 */
    current_Output = alpha * (*previous_Output) + (1-alpha) * abs(current_Input)*abs(current_Input);

    *previous_Output = current_Output;
    if (current_Output > threshold)
        return true;
    else
        return false;
}