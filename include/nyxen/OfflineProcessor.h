#pragma once
#include <nyxen/Engine.h>
#include <cstdint>
#include <filesystem>
#include <string>

namespace nyxen {
struct AudioStats {
    double peak = 0;
    double rms = 0;
    std::uint64_t samplesAtOrAboveFullScale = 0;
};
struct ProcessingReport {
    std::string inputFormat;
    double sampleRate = 0;
    unsigned channels = 0;
    std::int64_t frames = 0;
    AudioStats input, output;
    double elapsedSeconds = 0;
    ProcessingMode mode = ProcessingMode::Bypass;
};
// Throws std::exception on failure. Existing destinations are always rejected.
ProcessingReport processFile(const std::filesystem::path& input,
                             const std::filesystem::path& output,
                             int blockSize = 4096,
                             ProcessingMode mode = ProcessingMode::Bypass);
}
