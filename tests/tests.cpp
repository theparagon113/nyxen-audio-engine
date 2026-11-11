#include <nyxen/Engine.h>
#include <nyxen/OfflineProcessor.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
void require(bool ok, const std::string& msg) { if (!ok) throw std::runtime_error(msg); }
juce::File jf(const fs::path& p) {
    auto s = fs::absolute(p).u8string();
    return juce::File(juce::String::fromUTF8(reinterpret_cast<const char*>(s.c_str())));
}
std::vector<char> bytes(const fs::path& p) {
    std::ifstream s(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(s), std::istreambuf_iterator<char>()};
}
void writeAudio(const fs::path& p, juce::AudioFormat& f, const juce::AudioBuffer<float>& b, double rate, int bits) {
    std::unique_ptr<juce::OutputStream> s(jf(p).createOutputStream());
    auto options = juce::AudioFormatWriterOptions().withSampleRate(rate)
        .withNumChannels(b.getNumChannels()).withBitsPerSample(bits);
    if (bits == 32) options = options.withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto w = f.createWriterFor(s, options);
    require(w != nullptr, "fixture writer");
    require(w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()), "fixture write");
    if (f.getFormatName().containsIgnoreCase("WAV")) require(w->flush(), "fixture flush");
    w.reset(); // FLAC finalises its encoder in the destructor; flush() is unsupported.
}
std::unique_ptr<juce::AudioFormatReader> read(const fs::path& p) {
    juce::AudioFormatManager m;
    m.registerFormat(new juce::WavAudioFormat, true);
    m.registerFormat(new juce::FlacAudioFormat, false);
    m.registerFormat(new juce::MP3AudioFormat, false);
    return std::unique_ptr<juce::AudioFormatReader>(m.createReaderFor(jf(p)));
}
struct Difference { double maximum = 0; long double squares = 0; std::uint64_t count = 0; };
Difference compare(const fs::path& a, const fs::path& b) {
    auto original = read(a), exported = read(b);
    require(original && exported, "redecode output");
    require(original->lengthInSamples == exported->lengthInSamples, "frame count");
    require(original->sampleRate == exported->sampleRate, "sample rate");
    require(original->numChannels == exported->numChannels, "channels");
    require(exported->usesFloatingPointData && exported->bitsPerSample == 32, "float32 output");
    juce::AudioBuffer<float> x(static_cast<int>(original->numChannels), 997), y(x.getNumChannels(), 997);
    Difference diff;
    for (juce::int64 offset = 0; offset < original->lengthInSamples;) {
        const auto n = static_cast<int>(std::min<juce::int64>(997, original->lengthInSamples - offset));
        require(original->read(x.getArrayOfWritePointers(), x.getNumChannels(), offset, n), "read original");
        require(exported->read(y.getArrayOfWritePointers(), y.getNumChannels(), offset, n), "read export");
        for (int ch = 0; ch < x.getNumChannels(); ++ch) for (int i = 0; i < n; ++i) {
            const double delta = double(x.getSample(ch, i)) - y.getSample(ch, i);
            require(std::isfinite(delta), "finite difference");
            diff.maximum = std::max(diff.maximum, std::abs(delta));
            diff.squares += delta * delta;
            ++diff.count;
        }
        offset += n;
    }
    return diff;
}
template<class Fn> void rejects(Fn fn, const std::string& name) {
    bool rejected = false;
    try { fn(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "expected rejection: " + name);
}

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--compare") {
            const auto d = compare(argv[2], argv[3]);
            require(d.maximum <= 1e-7, "decoded transparency tolerance");
            std::cout << "max_abs=" << d.maximum << " rms_diff="
                      << std::sqrt(double(d.squares / d.count)) << '\n';
            return 0;
        }
        require(argc == 2, "test artifact directory required");
        const fs::path dir = fs::absolute(argv[1]) / juce::Uuid().toString().toStdString();
        fs::create_directories(dir);
        juce::WavAudioFormat wav;
        juce::FlacAudioFormat flac;
        Difference total;
        int cases = 0;
        for (const int rate : {44100, 48000, 96000}) for (const int channels : {1, 2})
            for (const auto& signal : {std::string("silence"), std::string("impulse"), std::string("sine"), std::string("headroom")})
                for (const int depth : {16, 24, 32}) {
                    constexpr int frames = 8193; // tail across 4096 and 257 frame blocks
                    juce::AudioBuffer<float> b(channels, frames);
                    b.clear();
                    for (int ch = 0; ch < channels; ++ch) for (int i = 0; i < frames; ++i) {
                        float v = 0;
                        if (signal == "impulse" && (i == 0 || i == 4096 || i == frames - 1)) v = ch ? -0.75f : 0.75f;
                        if (signal == "sine") v = float(0.6 * std::sin(2 * std::numbers::pi * (ch ? 1703 : 997) * i / rate));
                        if (signal == "headroom") v = (i % 2 ? -1.f : 1.f) * (depth == 32 ? 1.25f : 0.999f);
                        b.setSample(ch, i, v);
                    }
                    const auto stem = std::to_string(rate) + "-" + std::to_string(channels) + "-" + signal + "-" + std::to_string(depth);
                    const auto input = dir / (stem + ".wav"), output = dir / (stem + "-bypass.wav");
                    writeAudio(input, wav, b, rate, depth);
                    const auto before = bytes(input);
                    const auto r = nyxen::processFile(input, output, cases % 2 ? 257 : 4096);
                    require(bytes(input) == before, "source unchanged");
                    require(r.frames == frames && r.channels == unsigned(channels) && r.sampleRate == rate, "report metadata");
                    require(r.input.peak == r.output.peak && r.input.rms == r.output.rms, "no gain change");
                    require(r.input.samplesAtOrAboveFullScale == r.output.samplesAtOrAboveFullScale, "no introduced clipping");
                    if (signal == "silence") require(r.output.peak == 0 && r.output.rms == 0, "silence");
                    const auto d = compare(input, output);
                    require(d.maximum <= 1e-7, "WAV transparency tolerance");
                    total.maximum = std::max(total.maximum, d.maximum);
                    total.squares += d.squares; total.count += d.count;
                    ++cases;
                }
        // Independent FLAC encode/read/decode path, all required rates and channels.
        for (const int rate : {44100, 48000, 96000}) for (const int channels : {1, 2}) {
            juce::AudioBuffer<float> b(channels, 5003);
            for (int ch = 0; ch < channels; ++ch) for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample(ch, i, float(0.4 * std::sin((ch + 1) * i * 0.07)));
            const auto stem = "flac-" + std::to_string(rate) + "-" + std::to_string(channels);
            const auto input = dir / (stem + ".flac"), output = dir / (stem + ".wav");
            writeAudio(input, flac, b, rate, 24);
            const auto before = bytes(input);
            nyxen::processFile(input, output);
            const auto d = compare(input, output);
            require(d.maximum <= 1e-7, "FLAC decoded transparency");
            require(bytes(input) == before, "FLAC source unchanged");
            total.maximum = std::max(total.maximum, d.maximum);
            total.squares += d.squares; total.count += d.count;
            ++cases;
        }
        const auto valid = dir / "44100-1-sine-32.wav";
        const auto protectedBytes = bytes(valid);
        rejects([&] { nyxen::processFile(valid, valid); }, "same input/output");
        const auto existing = dir / "existing.wav";
        std::ofstream(existing) << "keep me";
        const auto existingBytes = bytes(existing);
        rejects([&] { nyxen::processFile(valid, existing); }, "existing output");
        require(bytes(existing) == existingBytes && bytes(valid) == protectedBytes, "protected files unchanged");
        const auto corrupt = dir / "corrupt.wav";
        std::ofstream(corrupt) << "RIFF broken";
        const auto badOutput = dir / "bad-output.wav";
        rejects([&] { nyxen::processFile(corrupt, badOutput); }, "corrupt WAV");
        const auto truncated = dir / "truncated.wav";
        fs::copy_file(valid, truncated);
        fs::resize_file(truncated, fs::file_size(truncated) - 17);
        rejects([&] { nyxen::processFile(truncated, badOutput); }, "truncated WAV");
        const auto unsupported = dir / "unsupported.txt";
        std::ofstream(unsupported) << "not audio";
        rejects([&] { nyxen::processFile(unsupported, badOutput); }, "unsupported input");
        rejects([&] { nyxen::processFile(dir / "missing.wav", badOutput); }, "missing input");
        rejects([&] { nyxen::processFile(valid, dir / "missing" / "out.wav"); }, "missing output directory");
        rejects([&] { nyxen::processFile(valid, dir / "out.flac"); }, "output extension");
        rejects([&] { nyxen::processFile(valid, badOutput, 0); }, "zero block size");
        juce::AudioBuffer<float> three(3, 7); three.clear();
        writeAudio(dir / "three.wav", wav, three, 48000, 32);
        rejects([&] { nyxen::processFile(dir / "three.wav", badOutput); }, "three channels");
        juce::AudioBuffer<float> nonfinite(1, 7); nonfinite.clear();
        nonfinite.setSample(0, 2, std::numeric_limits<float>::quiet_NaN());
        writeAudio(dir / "nan.wav", wav, nonfinite, 48000, 32);
        rejects([&] { nyxen::processFile(dir / "nan.wav", badOutput); }, "nonfinite samples");
        require(!fs::exists(badOutput), "no partial output");
        for (const auto& entry : fs::directory_iterator(dir)) require(entry.path().extension() != ".tmp", "temporary cleanup");
        nyxen::Engine engine;
        rejects([&] { engine.process({}, 0); }, "unprepared engine");
        rejects([&] { engine.prepare({48000, 3, 256}); }, "invalid spec");
        engine.prepare({48000, 1, 256});
        float sample = 0.25f; float* ptr = &sample;
        engine.process({&ptr, 1}, 1);
        require(sample == 0.25f, "engine bypass");
        rejects([&] { engine.process({&ptr, 1}, 257); }, "oversize block");
        std::cout << "PASS: " << cases << " WAV/FLAC round trips plus failure/core checks\n"
                  << "Compared scalar samples: " << total.count << "\nMaximum absolute error: " << total.maximum
                  << "\nRMS difference: " << std::sqrt(double(total.squares / total.count))
                  << "\nTolerance: 1e-7 full scale\nArtifacts: " << dir.string() << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
