// Isolated experiment: production Engine float buffers vs double samples using
// the exact same production Biquad coefficients/states. No production API changes.
#include <nyxen/Engine.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <vector>

using namespace nyxen;
namespace fs = std::filesystem;
template<class T> using Signal = std::vector<std::vector<T>>;
volatile double observed = 0; // prevent removing timed work
void require(bool ok, const char* msg) { if (!ok) throw std::runtime_error(msg); }
Signal<float> signal(const std::string& kind, int rate, int frames) {
    Signal<float> result(2, std::vector<float>(frames));
    std::uint32_t seed = 0x1234abcd;
    for (int ch = 0; ch < 2; ++ch) for (int n = 0; n < frames; ++n) {
        const double t = double(n) / rate;
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        const double noise = (double(seed) / 4294967295.0 - 0.5) * 0.8;
        double x = 0;
        if (kind == "impulse") x = n == (ch ? 257 : 0) ? 0.9 : 0;
        if (kind == "sine") x = 0.7 * std::sin(2 * std::numbers::pi * (997 + 503 * ch) * t);
        if (kind == "noise") x = noise;
        if (kind == "near-full-scale") x = 0.99 * std::sin(2 * std::numbers::pi * 40 * t + ch * 0.1);
        if (kind == "multitone") x = 0.2 * (std::sin(2 * std::numbers::pi * 80 * t) +
            std::sin(2 * std::numbers::pi * (350 + ch * 101) * t) + std::sin(2 * std::numbers::pi * 6500 * t));
        if (kind == "low-amplitude") x = 1e-12 * noise;
        result[ch][n] = static_cast<float>(x); // canonical, identical input for A/B/reference
    }
    return result;
}
struct FloatPipeline {
    std::vector<Engine> engines;
    FloatPipeline(int rate, int repetitions) : engines(repetitions) {
        for (auto& e : engines) e.prepare({double(rate), 2, 257}, ProcessingMode::EqReference);
    }
    void reset() { for (auto& e : engines) e.reset(); }
    void process(Signal<float>& s) {
        for (auto& e : engines) for (std::size_t offset = 0; offset < s[0].size(); offset += 257) {
            const auto frames = std::min<std::size_t>(257, s[0].size() - offset);
            std::array<float*, 2> p{s[0].data() + offset, s[1].data() + offset};
            e.process(p, frames);
        }
    }
};
struct DoublePipeline {
    std::vector<Biquad> filters;
    DoublePipeline(int rate, int repetitions) {
        for (int i = 0; i < repetitions; ++i) for (const auto band : referencePreset) {
            Biquad f; f.prepare(rate, band); filters.push_back(f);
        }
    }
    void reset() { for (auto& f : filters) f.reset(); }
    void process(Signal<double>& s) {
        for (std::size_t ch = 0; ch < s.size(); ++ch) for (auto& x : s[ch])
            for (auto& f : filters) x = f.processSample(x, ch);
    }
};
Signal<double> promoted(const Signal<float>& s) {
    Signal<double> result;
    for (const auto& ch : s) result.emplace_back(ch.begin(), ch.end());
    return result;
}
template<class T> void binary(const fs::path& path, const Signal<T>& s) {
    std::ofstream out(path, std::ios::binary);
    for (const auto& ch : s) out.write(reinterpret_cast<const char*>(ch.data()), ch.size() * sizeof(T));
    require(bool(out), "binary write");
}
template<class T> void wav(const fs::path& path, const Signal<T>& s, int rate) {
    juce::AudioBuffer<float> buffer(2, static_cast<int>(s[0].size()));
    for (int ch = 0; ch < 2; ++ch) for (int n = 0; n < buffer.getNumSamples(); ++n)
        buffer.setSample(ch, n, static_cast<float>(s[ch][n]));
    const auto name = path.u8string();
    juce::File file(juce::String::fromUTF8(reinterpret_cast<const char*>(name.c_str())));
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    juce::WavAudioFormat format;
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions().withSampleRate(rate)
        .withNumChannels(2).withBitsPerSample(32).withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    require(writer && writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()) && writer->flush(), "WAV export");
}
double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const double index = fraction * (values.size() - 1);
    const auto lo = static_cast<std::size_t>(index);
    return values[lo] + (values[std::min(lo + 1, values.size() - 1)] - values[lo]) * (index - lo);
}
int main(int argc, char** argv) {
    try {
#ifdef NDEBUG
        require(sizeof(void*) == 8, "benchmark requires Release x64");
#else
        throw std::runtime_error("benchmark requires Release (NDEBUG)");
#endif
        require(argc == 2, "benchmark output directory required");
        const auto folder = fs::absolute(argv[1]);
        require(!fs::exists(folder), "benchmark destination already exists");
        fs::create_directories(folder);
        std::ofstream cases(folder / "cases.csv"), coefficients(folder / "coefficients.csv"), cpu(folder / "cpu.csv"), timing(folder / "timing-samples.csv");
        cases << "sample_rate,preset_repetitions,signal,frames,stem\n";
        coefficients << "sample_rate,b0,b1,b2,a1,a2\n";
        cpu << "sample_rate,preset_repetitions,frames,iterations,a_median_ms,a_iqr_ms,b_median_ms,b_iqr_ms,a_working_bytes,b_working_bytes\n" << std::setprecision(12);
        timing << "sample_rate,preset_repetitions,iteration,a_ms,b_ms\n" << std::setprecision(12);
        for (const int rate : {44100, 48000, 96000}) {
            for (const auto band : referencePreset) {
                Biquad f; f.prepare(rate, band); const auto c = f.coefficients();
                coefficients << std::defaultfloat << rate << std::hexfloat << ',' << c.b0 << ',' << c.b1 << ',' << c.b2 << ',' << c.a1 << ',' << c.a2 << '\n';
            }
            for (const int repetitions : {1, 4, 12}) {
                FloatPipeline a(rate, repetitions); DoublePipeline b(rate, repetitions);
                for (const std::string kind : {"silence", "impulse", "sine", "noise", "near-full-scale", "multitone", "low-amplitude"}) {
                    constexpr int frames = 1024;
                    const auto original = signal(kind, rate, frames);
                    auto af = original; auto bd = promoted(original);
                    a.reset(); b.reset(); a.process(af); b.process(bd);
                    const auto stem = std::to_string(rate) + "-" + std::to_string(repetitions) + "-" + kind;
                    binary(folder / (stem + "-input.f32"), original);
                    binary(folder / (stem + "-a.f32"), af);
                    binary(folder / (stem + "-b.f64"), bd);
                    wav(folder / (stem + "-a.wav"), af, rate);
                    wav(folder / (stem + "-b.wav"), bd, rate);
                    cases << rate << ',' << repetitions << ',' << kind << ',' << frames << ',' << stem << '\n';
                }
                constexpr int frames = 16384, iterations = 11;
                const auto input = signal("noise", rate, frames);
                std::vector<double> at, bt;
                for (int iteration = -2; iteration < iterations; ++iteration) { // two warmups
                    auto af = input; auto bd = promoted(input);
                    a.reset(); b.reset();
                    double ams, bms;
                    auto ta = [&] { const auto start = std::chrono::steady_clock::now(); a.process(af);
                        ams = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); observed = af[0].back(); };
                    auto tb = [&] { const auto start = std::chrono::steady_clock::now(); b.process(bd);
                        bms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); observed = bd[0].back(); };
                    if (iteration % 2) { tb(); ta(); } else { ta(); tb(); }
                    if (iteration >= 0) { at.push_back(ams); bt.push_back(bms);
                        timing << rate << ',' << repetitions << ',' << iteration << ',' << ams << ',' << bms << '\n'; }
                }
                const auto memoryA = 2 * frames * sizeof(float) + repetitions * sizeof(Engine);
                const auto memoryB = 2 * frames * sizeof(double) + repetitions * 3 * sizeof(Biquad);
                cpu << rate << ',' << repetitions << ',' << frames << ',' << iterations << ',' << percentile(at, 0.5) << ','
                    << percentile(at, 0.75) - percentile(at, 0.25) << ',' << percentile(bt, 0.5) << ','
                    << percentile(bt, 0.75) - percentile(bt, 0.25) << ',' << memoryA << ',' << memoryB << '\n';
            }
        }
        require(cases && coefficients && cpu && timing, "benchmark report write");
        std::cout << "PASS numerical fixture/CPU execution: 63 cases, 9 CPU configurations, 11 iterations + 2 warmups\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL benchmark: " << e.what() << '\n'; return 1; }
}
