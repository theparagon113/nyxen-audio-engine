#include <nyxen/Biquad.h>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace nyxen {
void Biquad::prepare(double sampleRate, EqBand band) {
    const bool peaking = band.type == FilterType::Peaking;
    if (band.type != FilterType::LowShelf && !peaking && band.type != FilterType::HighShelf)
        throw std::invalid_argument("Unknown biquad type");
    if (!std::isfinite(sampleRate) || sampleRate <= 0 || !std::isfinite(band.frequencyHz)
        || band.frequencyHz <= 0 || band.frequencyHz >= sampleRate * 0.5
        || !std::isfinite(band.gainDb) || std::abs(band.gainDb) > 12
        || !std::isfinite(band.shape) || band.shape < 0.1 || band.shape > (peaking ? 10 : 1))
        throw std::invalid_argument("Invalid EQ parameters (gain +/-12 dB, Q 0.1..10, S 0.1..1)");
    BiquadCoefficients c;
    if (band.gainDb != 0) {
        const double a = std::pow(10.0, band.gainDb / 40.0);
        const double w = 2 * std::numbers::pi * band.frequencyHz / sampleRate;
        const double cs = std::cos(w), sn = std::sin(w);
        const double alpha = peaking ? sn / (2 * band.shape)
            : sn * 0.5 * std::sqrt((a + 1 / a) * (1 / band.shape - 1) + 2);
        double a0;
        if (peaking) {
            c.b0 = 1 + alpha * a; c.b1 = -2 * cs; c.b2 = 1 - alpha * a;
            a0 = 1 + alpha / a; c.a1 = -2 * cs; c.a2 = 1 - alpha / a;
        } else {
            const double beta = 2 * std::sqrt(a) * alpha;
            if (band.type == FilterType::LowShelf) {
                c.b0 = a * ((a + 1) - (a - 1) * cs + beta);
                c.b1 = 2 * a * ((a - 1) - (a + 1) * cs);
                c.b2 = a * ((a + 1) - (a - 1) * cs - beta);
                a0 = (a + 1) + (a - 1) * cs + beta;
                c.a1 = -2 * ((a - 1) + (a + 1) * cs);
                c.a2 = (a + 1) + (a - 1) * cs - beta;
            } else {
                c.b0 = a * ((a + 1) + (a - 1) * cs + beta);
                c.b1 = -2 * a * ((a - 1) + (a + 1) * cs);
                c.b2 = a * ((a + 1) + (a - 1) * cs - beta);
                a0 = (a + 1) - (a - 1) * cs + beta;
                c.a1 = 2 * ((a - 1) - (a + 1) * cs);
                c.a2 = (a + 1) - (a - 1) * cs - beta;
            }
        }
        c.b0 /= a0; c.b1 /= a0; c.b2 /= a0; c.a1 /= a0; c.a2 /= a0;
    }
    // Jury conditions: poles strictly within the unit circle, including after rounding.
    if (!std::isfinite(c.b0) || !std::isfinite(c.b1) || !std::isfinite(c.b2)
        || !std::isfinite(c.a1) || !std::isfinite(c.a2) || std::abs(c.a2) >= 1
        || 1 + c.a1 + c.a2 <= 0 || 1 - c.a1 + c.a2 <= 0)
        throw std::invalid_argument("Numerically unstable EQ coefficients");
    coefficients_ = c;
    reset();
}
void Biquad::reset() noexcept { states_ = {}; }
double Biquad::processSample(double input, std::size_t channel) {
    if (channel >= states_.size() || !std::isfinite(input))
        throw std::invalid_argument("Invalid biquad sample/channel");
    double output = 0;
    if (!tryProcessSample(input, channel, output)) throw std::runtime_error("EQ numerical overflow");
    return output;
}
bool Biquad::tryProcessSample(double input, std::size_t channel, double& result) noexcept {
    if (channel >= states_.size() || !std::isfinite(input)) return false;
    auto& s = states_[channel];
    const auto& c = coefficients_;
    const double output = c.b0 * input + s.z1;
    const double z1 = c.b1 * input - c.a1 * output + s.z2;
    const double z2 = c.b2 * input - c.a2 * output;
    if (!std::isfinite(output) || !std::isfinite(z1) || !std::isfinite(z2))
        return false;
    s.z1 = std::abs(z1) < 1e-300 ? 0 : z1;
    s.z2 = std::abs(z2) < 1e-300 ? 0 : z2;
    result = output;
    return true;
}
}
