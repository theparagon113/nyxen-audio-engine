#include <nyxen/OfflineProcessor.h>
#include <nyxen/Engine.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nyxen {
namespace {
juce::File file(const std::filesystem::path& p) {
    const auto utf8 = std::filesystem::absolute(p).u8string();
    return juce::File(juce::String::fromUTF8(reinterpret_cast<const char*>(utf8.c_str())));
}
struct Accumulator {
    long double squares = 0;
    std::uint64_t count = 0;
    AudioStats stats;
    void add(const juce::AudioBuffer<float>& b, int frames) {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int n = 0; n < frames; ++n) {
                const double x = b.getSample(ch, n);
                if (!std::isfinite(x)) throw std::runtime_error("Decoded audio contains NaN or infinity");
                stats.peak = std::max(stats.peak, std::abs(x));
                squares += static_cast<long double>(x) * x;
                ++count;
                if (std::abs(x) >= 1.0) ++stats.samplesAtOrAboveFullScale;
            }
    }
    AudioStats finish() {
        stats.rms = count ? std::sqrt(static_cast<double>(squares / count)) : 0;
        return stats;
    }
};
// JUCE WAV readers can pad truncated data with zeros. Reject broken RIFF chunks
// before decoding rather than silently treating a missing payload as silence.
void validateWav(const std::filesystem::path& path) {
    std::ifstream s(path, std::ios::binary);
    auto u32 = [&]() {
        unsigned char b[4]{};
        s.read(reinterpret_cast<char*>(b), 4);
        if (!s) throw std::runtime_error("Truncated WAV header");
        return std::uint32_t(b[0]) | (std::uint32_t(b[1]) << 8)
             | (std::uint32_t(b[2]) << 16) | (std::uint32_t(b[3]) << 24);
    };
    char id[4]{};
    s.read(id, 4);
    if (std::string(id, 4) != "RIFF")
        throw std::runtime_error("M0.1 supports RIFF WAV only (RF64/BW64 pending)");
    const std::uint64_t end = std::uint64_t(u32()) + 8;
    if (end < 12 || end > std::filesystem::file_size(path))
        throw std::runtime_error("Truncated WAV RIFF payload");
    s.read(id, 4);
    if (std::string(id, 4) != "WAVE") throw std::runtime_error("Invalid WAV header");
    std::uint64_t pos = 12;
    bool data = false;
    while (pos < end) {
        if (end - pos < 8) throw std::runtime_error("Truncated WAV chunk header");
        s.read(id, 4);
        const auto size = u32();
        const auto next = pos + 8 + size + (size & 1u);
        if (next > end) throw std::runtime_error("Truncated WAV chunk payload");
        if (std::string(id, 4) == "data") data = true;
        pos = next;
        s.seekg(static_cast<std::streamoff>(pos));
    }
    if (!data) throw std::runtime_error("WAV has no data chunk");
}
struct TemporaryOutput {
    std::filesystem::path path;
    ~TemporaryOutput() { std::error_code ec; std::filesystem::remove(path, ec); }
};
void publish(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
    // Unlike JUCE moveFileTo(), never deletes/replaces a destination, even in a race.
    if (!MoveFileExW(from.c_str(), to.c_str(), 0))
        throw std::runtime_error("Cannot publish output (Windows error " + std::to_string(GetLastError()) + ")");
#else
    std::filesystem::create_hard_link(from, to); // atomic, fails if target exists
    std::filesystem::remove(from);
#endif
}
}

ProcessingReport processFile(const std::filesystem::path& input,
                             const std::filesystem::path& output, int blockSize, ProcessingMode mode) {
    const auto start = std::chrono::steady_clock::now();
    if (blockSize < 1 || blockSize > 1048576) throw std::invalid_argument("Invalid block size");
    const auto in = std::filesystem::absolute(input);
    const auto out = std::filesystem::absolute(output);
    if (!std::filesystem::is_regular_file(in)) throw std::runtime_error("Input is not a regular file");
    if (std::filesystem::symlink_status(out).type() != std::filesystem::file_type::not_found)
        throw std::runtime_error("Output already exists; overwriting is forbidden");
    auto extension = out.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (extension != ".wav") throw std::runtime_error("Output must have .wav extension");
    if (!std::filesystem::is_directory(out.parent_path())) throw std::runtime_error("Output directory does not exist");

    juce::AudioFormatManager formats;
    formats.registerFormat(new juce::WavAudioFormat, true);
    formats.registerFormat(new juce::FlacAudioFormat, false);
    formats.registerFormat(new juce::MP3AudioFormat, false);
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file(in).createInputStream()));
    if (!reader) throw std::runtime_error("Cannot decode input: unsupported or corrupt audio");
    if (reader->getFormatName().containsIgnoreCase("WAV")) validateWav(in);
    if (reader->numChannels < 1 || reader->numChannels > 2 || reader->lengthInSamples <= 0
        || !std::isfinite(reader->sampleRate) || reader->sampleRate <= 0)
        throw std::runtime_error("Input must contain non-empty mono or stereo audio at a valid sample rate");
    // Keep output in classic RIFF range; JUCE can otherwise switch to RF64.
    if (reader->lengthInSamples > (0xffffffffLL - 4096) / (reader->numChannels * 4))
        throw std::runtime_error("Output exceeds M0.1 RIFF WAV size limit");

    TemporaryOutput temporary{out.parent_path() / (".nyxen-" + juce::Uuid().toString().toStdString() + ".tmp")};
    std::unique_ptr<juce::OutputStream> stream(file(temporary.path).createOutputStream());
    if (!stream) throw std::runtime_error("Cannot open output stream");
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions()
        .withSampleRate(reader->sampleRate).withNumChannels(static_cast<int>(reader->numChannels))
        .withBitsPerSample(32).withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = wav.createWriterFor(stream, options);
    if (!writer || !writer->isFloatingPoint()) throw std::runtime_error("Cannot create float32 WAV writer");
    Engine engine;
    engine.prepare({reader->sampleRate, reader->numChannels, static_cast<std::size_t>(blockSize)}, mode);
    juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), blockSize);
    Accumulator original, processed;
    for (juce::int64 offset = 0; offset < reader->lengthInSamples;) {
        const int frames = static_cast<int>(std::min<juce::int64>(blockSize, reader->lengthInSamples - offset));
        if (!reader->read(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), offset, frames))
            throw std::runtime_error("Decoding failed at frame " + std::to_string(offset));
        original.add(buffer, frames);
        engine.process({buffer.getArrayOfWritePointers(), static_cast<std::size_t>(buffer.getNumChannels())}, frames);
        processed.add(buffer, frames);
        if (!writer->writeFromAudioSampleBuffer(buffer, 0, frames)) throw std::runtime_error("WAV export failed");
        offset += frames;
    }
    if (!writer->flush()) throw std::runtime_error("Cannot flush WAV output");
    writer.reset(); // finalise header before publishing
    validateWav(temporary.path);
    std::unique_ptr<juce::AudioFormatReader> verification(formats.createReaderFor(file(temporary.path).createInputStream()));
    if (!verification || verification->lengthInSamples != reader->lengthInSamples
        || verification->numChannels != reader->numChannels || verification->sampleRate != reader->sampleRate
        || !verification->usesFloatingPointData || verification->bitsPerSample != 32)
        throw std::runtime_error("Exported WAV header verification failed");
    verification.reset();
    publish(temporary.path, out);
    return {reader->getFormatName().toStdString(), reader->sampleRate, reader->numChannels,
            reader->lengthInSamples, original.finish(), processed.finish(),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), mode};
}
}
