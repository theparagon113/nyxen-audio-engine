#include <nyxen/Engine.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

using namespace nyxen;
constexpr double pi = std::numbers::pi;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Fn> void rejects(Fn fn) {
    bool rejected = false; try { fn(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "expected invalid parameter rejection");
}
double db(double amplitude) { return 20 * std::log10(amplitude); }
double theoretical(const BiquadCoefficients& c, double hz, double rate) {
    const auto z = std::polar(1.0, -2 * pi * hz / rate);
    return db(std::abs((c.b0 + c.b1 * z + c.b2 * z * z) / (1.0 + c.a1 * z + c.a2 * z * z)));
}
std::vector<double> impulse(Biquad& filter, int length) {
    filter.reset();
    std::vector<double> h(length);
    for (int n = 0; n < length; ++n) h[n] = filter.processSample(n == 0 ? 1 : 0, 0);
    return h;
}
double measured(const std::vector<double>& h, double hz, double rate) {
    std::complex<double> sum{};
    const auto step = std::polar(1.0, -2 * pi * hz / rate);
    std::complex<double> phase{1, 0};
    for (const auto x : h) { sum += x * phase; phase *= step; }
    return db(std::abs(sum));
}
double sineGain(Biquad& f, double hz, int rate) {
    f.reset();
    double xx = 0, yy = 0;
    for (int i = 0; i < rate * 2; ++i) {
        const double x = 0.2 * std::sin(2 * pi * hz * i / rate);
        const auto y = f.processSample(x, 0);
        if (i >= rate) { xx += x * x; yy += y * y; }
    }
    return db(std::sqrt(yy / xx));
}
using Signal = std::vector<std::vector<float>>;
Signal process(Signal input, int rate, int block, ProcessingMode mode = ProcessingMode::EqReference) {
    Engine engine;
    engine.prepare({double(rate), input.size(), std::size_t(block)}, mode);
    std::array<float*, 2> ptr{};
    for (std::size_t offset = 0; offset < input[0].size();) {
        const auto count = std::min<std::size_t>(block, input[0].size() - offset);
        for (std::size_t ch = 0; ch < input.size(); ++ch) ptr[ch] = input[ch].data() + offset;
        engine.process({ptr.data(), input.size()}, count);
        offset += count;
    }
    return input;
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "artifact directory");
        std::filesystem::create_directories(argv[1]);
        // Truncate only generated test reports; never audio or user source files.
        std::ofstream response(std::filesystem::path(argv[1]) / "filter-response.csv");
        response << "sample_rate,filter,gain_db,frequency_hz,expected_db,impulse_db,sine_db\n" << std::setprecision(12);
        std::ofstream curve(std::filesystem::path(argv[1]) / "preset-response.csv");
        curve << "sample_rate,frequency_hz,impulse_db,theoretical_db\n" << std::setprecision(12);
        double maximumResponseError = 0, maximumPresetError = 0, maximumBlockError = 0, nearFullScalePeak = 0;
        int measurements = 0, stabilityCases = 0;
        for (const int rate : {44100, 48000, 96000}) {
            for (const auto band : referencePreset) for (const double gain : {-6.0, 6.0}) {
                auto testBand = band; testBand.gainDb = gain;
                Biquad f; f.prepare(rate, testBand);
                const auto h = impulse(f, rate * 2);
                const char* name = band.type == FilterType::LowShelf ? "low-shelf" : band.type == FilterType::HighShelf ? "high-shelf" : "peaking";
                for (const double hz : {0.0, band.frequencyHz, rate * 0.5}) {
                    double expected = 0;
                    if (band.type == FilterType::Peaking && hz == band.frequencyHz) expected = gain;
                    if (band.type == FilterType::LowShelf) expected = hz == 0 ? gain : hz == band.frequencyHz ? gain * 0.5 : 0;
                    if (band.type == FilterType::HighShelf) expected = hz == 0 ? 0 : hz == band.frequencyHz ? gain * 0.5 : gain;
                    const double result = measured(h, hz, rate);
                    const auto error = std::abs(expected - result);
                    maximumResponseError = std::max(maximumResponseError, error);
                    check(error < 0.003, "measured filter response (0.003 dB)");
                    check(std::abs(result - theoretical(f.coefficients(), hz, rate)) < 0.003, "impulse vs transfer function");
                    response << rate << ',' << name << ',' << gain << ',' << hz << ',' << expected << ',' << result << ',';
                    if (hz == band.frequencyHz) {
                        const auto sine = sineGain(f, hz, rate);
                        check(std::abs(sine - result) < 0.003, "sine vs impulse response");
                        response << sine;
                    }
                    response << '\n';
                    ++measurements;
                }
            }
            for (const auto band : referencePreset) for (const double gain : {-12.0, 0.0, 12.0})
                for (const double frequency : {5.0, band.frequencyHz, rate * 0.499})
                    for (const double shape : {0.1, band.type == FilterType::Peaking ? 10.0 : 1.0}) {
                        Biquad f; f.prepare(rate, {band.type, frequency, gain, shape});
                        std::uint32_t seed = 0x5eed;
                        for (int n = 0; n < 100000; ++n) {
                            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                            const double noise = (double(seed) / 4294967295.0 - 0.5) * 0.8;
                            const double y = f.processSample(n == 0 ? 1 : n < 50000 ? noise : 0, 0);
                            check(std::isfinite(y) && std::abs(y) < 100, "finite stable impulse/noise/silence response");
                            check(f.processSample(0, 1) == 0, "filter channel independence");
                        }
                        ++stabilityCases;
                    }
            for (const int channels : {1, 2}) {
                Signal noise(channels, std::vector<float>(16387));
                std::uint32_t seed = 13579;
                for (auto& channel : noise) for (auto& x : channel) {
                    seed = seed * 1664525u + 1013904223u;
                    x = float((double(seed) / 4294967295.0 - 0.5) * 0.8);
                }
                noise[0][0] = 1; noise[0][4096] = -0.75f;
                const auto expected = process(noise, rate, 4096);
                check(process(noise, rate, 4096) == expected, "repeatability");
                for (const int block : {1, 7, 257, 8193}) {
                    const auto result = process(noise, rate, block);
                    for (std::size_t ch = 0; ch < noise.size(); ++ch) for (std::size_t n = 0; n < noise[ch].size(); ++n) {
                        const auto error = std::abs(double(result[ch][n]) - expected[ch][n]);
                        maximumBlockError = std::max(maximumBlockError, error);
                        check(error <= 1e-7, "block size invariance");
                    }
                }
                check(process(noise, rate, 257, ProcessingMode::Bypass) == noise, "exact bypass regression");
                Engine reused;
                reused.prepare({double(rate), std::size_t(channels), noise[0].size()}, ProcessingMode::EqReference);
                auto first = noise;
                std::array<float*, 2> p{};
                for (int ch = 0; ch < channels; ++ch) p[ch] = first[ch].data();
                reused.process({p.data(), std::size_t(channels)}, first[0].size());
                reused.reset();
                auto second = noise;
                for (int ch = 0; ch < channels; ++ch) p[ch] = second[ch].data();
                reused.process({p.data(), std::size_t(channels)}, second[0].size());
                check(first == second, "explicit reset");
                reused.prepare({double(rate), std::size_t(channels), noise[0].size()}, ProcessingMode::EqReference);
                second = noise;
                for (int ch = 0; ch < channels; ++ch) p[ch] = second[ch].data();
                reused.process({p.data(), std::size_t(channels)}, second[0].size());
                check(first == second, "prepare resets between files");
            }
            Signal left(2, std::vector<float>(rate)); left[0][0] = 1;
            const auto stereo = process(left, rate, 257);
            check(std::all_of(stereo[1].begin(), stereo[1].end(), [](float x) { return x == 0; }), "engine crosstalk");
            check(process({left[0]}, rate, 257)[0] == stereo[0], "mono vs stereo channel response");
            Signal h(1, std::vector<float>(rate * 2)); h[0][0] = 1;
            auto hp = process(h, rate, 257);
            const std::vector<double> hd(hp[0].begin(), hp[0].end());
            for (int k = 0; k <= 40; ++k) {
                const double hz = 20 * std::pow((rate * 0.49) / 20, k / 40.0);
                double theory = 0;
                for (const auto band : referencePreset) { Biquad f; f.prepare(rate, band); theory += theoretical(f.coefficients(), hz, rate); }
                const auto actual = measured(hd, hz, rate);
                maximumPresetError = std::max(maximumPresetError, std::abs(actual - theory));
                check(std::abs(actual - theory) <= 0.003, "preset impulse response");
                curve << rate << ',' << hz << ',' << actual << ',' << theory << '\n';
            }
            Signal hot(1, std::vector<float>(rate * 2));
            for (std::size_t n = 0; n < hot[0].size(); ++n) hot[0][n] = float(0.99 * std::sin(2 * pi * 40 * n / rate));
            const auto boosted = process(hot, rate, 4096);
            for (const float x : boosted[0]) nearFullScalePeak = std::max(nearFullScalePeak, std::abs(double(x)));
        }
        check(nearFullScalePeak > 1, "document actual EQ headroom risk without implicit limiting");
        Biquad bad;
        for (const auto band : {EqBand{FilterType::Peaking, 24000, 1, 1}, {FilterType::Peaking, 0, 1, 1},
             {FilterType::LowShelf, 100, 1, 2}, {FilterType::Peaking, 100, 13, 1},
             {FilterType::Peaking, 100, 1, 0}, {FilterType::Peaking, NAN, 1, 1}})
            rejects([&] { bad.prepare(48000, band); });
        rejects([&] { bad.prepare(INFINITY, referencePreset[0]); });
        rejects([&] { bad.processSample(NAN, 0); });
        std::cout << std::setprecision(12) << "PASS DSP: " << measurements << " response anchors, " << stabilityCases
                  << " stability cases; mono/stereo/block/reset/repeatability/bypass checks\n"
                  << "Maximum response error: " << maximumResponseError << " dB (tolerance 0.003 dB)\n"
                  << "Maximum float32 preset response error: " << maximumPresetError << " dB (tolerance 0.003 dB)\n"
                  << "Maximum block error: " << maximumBlockError << " (tolerance 1e-7 FS)\n"
                  << "Near-full-scale 40 Hz input peak 0.99 -> EQ peak " << nearFullScalePeak
                  << " (" << db(nearFullScalePeak) << " dBFS)\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL DSP: " << e.what() << '\n'; return 1; }
}
