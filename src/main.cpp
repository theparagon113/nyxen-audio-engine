#include <nyxen/OfflineProcessor.h>
#include <juce_core/juce_core.h>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
double db(double x) { return x > 0 ? 20 * std::log10(x) : -INFINITY; }
int run(const std::vector<std::filesystem::path>& args) {
    try {
        if (args.size() == 1 && args[0] == "--help") {
            std::cout << "nyxen-cli --input <audio> --output <new.wav> --mode bypass|eq-reference\n"
                         "  [--output-safety preserve|safe-gain] [--true-peak-ceiling -1.0]\n"
                         "  [--ffmpeg <executable>] (safe-gain requires Python and FFmpeg)\n";
            return 0;
        }
        std::map<std::string, std::filesystem::path> options;
        for (std::size_t i = 0; i < args.size(); i += 2) {
            const auto key = args[i].string();
            if ((key != "--input" && key != "--output" && key != "--mode" && key != "--output-safety"
                 && key != "--true-peak-ceiling" && key != "--ffmpeg") || i + 1 >= args.size()
                || !options.emplace(key, args[i + 1]).second)
                throw std::invalid_argument("Unknown, duplicate or incomplete argument: " + key);
        }
        if (!options.contains("--input") || !options.contains("--output") || !options.contains("--mode")
            || (options.at("--mode") != "bypass" && options.at("--mode") != "eq-reference"))
            throw std::invalid_argument("Required: --input <audio> --output <new.wav> --mode bypass|eq-reference");
        const auto safety = options.contains("--output-safety") ? options.at("--output-safety").string() : "preserve";
        if (safety != "preserve" && safety != "safe-gain") throw std::invalid_argument("Invalid output safety mode");
        const auto ceilingText = options.contains("--true-peak-ceiling") ? options.at("--true-peak-ceiling").string() : "-1.0";
        std::size_t consumed = 0;
        const double ceiling = std::stod(ceilingText, &consumed);
        if (consumed != ceilingText.size() || !std::isfinite(ceiling) || ceiling < -20 || ceiling > -0.1)
            throw std::invalid_argument("True peak ceiling must be -20..-0.1 dBTP");
        if (safety == "safe-gain") {
#ifdef NYXEN_PYTHON_EXECUTABLE
            auto toString = [](const std::filesystem::path& p) {
                const auto s = p.u8string();
                return juce::String::fromUTF8(reinterpret_cast<const char*>(s.c_str()));
            };
            juce::StringArray arguments{NYXEN_PYTHON_EXECUTABLE, "-B", NYXEN_SAFETY_SCRIPT,
                "--cli", juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
                "--input", toString(options.at("--input")), "--output", toString(options.at("--output")),
                "--mode", toString(options.at("--mode")), "--true-peak-ceiling", ceilingText};
            if (options.contains("--ffmpeg")) { arguments.add("--ffmpeg"); arguments.add(toString(options.at("--ffmpeg"))); }
            juce::ChildProcess process;
            if (!process.start(arguments)) throw std::runtime_error("Cannot launch Python output safety helper");
            std::cout << process.readAllProcessOutput();
            if (!process.waitForProcessToFinish(-1)) throw std::runtime_error("Output safety process did not finish");
            return process.getExitCode() == 0 ? 0 : 1;
#else
            throw std::runtime_error("safe-gain unavailable: Python was not found when configuring this build");
#endif
        }
        const auto mode = options.at("--mode") == "bypass" ? nyxen::ProcessingMode::Bypass : nyxen::ProcessingMode::EqReference;
        const auto r = nyxen::processFile(options.at("--input"), options.at("--output"), 4096, mode);
        std::cout << "Mode: " << (mode == nyxen::ProcessingMode::Bypass ? "bypass" : "eq-reference") << '\n';
        if (mode == nyxen::ProcessingMode::EqReference) {
            std::cout << "Preset: " << nyxen::referencePresetId << '\n';
            for (const auto& band : nyxen::referencePreset) {
                const auto name = band.type == nyxen::FilterType::LowShelf ? "low-shelf" :
                    band.type == nyxen::FilterType::HighShelf ? "high-shelf" : "peaking";
                std::cout << name << ": frequency=" << band.frequencyHz << " Hz gain=" << band.gainDb
                          << " dB " << (band.type == nyxen::FilterType::Peaking ? "Q=" : "S=") << band.shape << '\n';
            }
        }
        std::cout << std::setprecision(12)
            << "Input format: " << r.inputFormat << "\nSample rate: " << r.sampleRate
            << " Hz\nChannels: " << r.channels << "\nFrames: " << r.frames
            << "\nDuration: " << r.frames / r.sampleRate << " s\n";
        for (const auto& [name, s] : {std::pair{"Input", r.input}, std::pair{"Output", r.output}})
            std::cout << name << " peak: " << s.peak << " (" << db(s.peak) << " dBFS)\n"
                      << name << " RMS: " << s.rms << " (" << db(s.rms) << " dBFS)\n"
                      << name << " samples |x| >= 1: " << s.samplesAtOrAboveFullScale << "\n";
        std::cout << "Output format: WAV IEEE float32\nProcessing time: " << r.elapsedSeconds << " s\n";
        std::cout << "Output safety: preserve\nTrue peak ceiling (not applied): " << ceiling
                  << " dBTP\nApplied gain: 0 dB\nAttenuation required: false\n"
                     "True peak before/after: not measured\nFinal safety verification: NOT REQUESTED\n";
        std::cout << "Safety sample peak before/after: " << db(r.output.peak) << " dBFS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
}
int main(int argc, char** argv) {
    std::vector<std::filesystem::path> args;
#ifdef _WIN32
    int count = 0;
    auto** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide) { std::cerr << "Cannot read command line\n"; return 1; }
    for (int i = 1; i < count; ++i) args.emplace_back(wide[i]);
    LocalFree(wide);
    (void)argc; (void)argv;
#else
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif
    return run(args);
}
