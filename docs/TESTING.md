# Reproducing the existing checks

Use the Release x64 build and CTest commands in the README. The complete MSVC configuration with Python, FFmpeg, and AddressSanitizer registers ten tests:

| CTest test | Existing coverage |
| --- | --- |
| nyxen-continuous | Synthetic in-memory stream partitions, transitions, invalid contracts, long logical streams, allocation probe and timing reports |
| nyxen-continuous-asan | Separately instrumented continuous engine with MSVC ASan; no intrusive allocator override or timing claims |
| nyxen-audio | Synthetic WAV/FLAC decoding, bypass transparency, dimensions and input/output error contracts |
| nyxen-dsp | EQ response, stability, channel independence, reset and block consistency |
| nyxen-rt-source-audit | Limited token/call-whitelist checks of processing functions and detector controls |
| nyxen-asan-detector | Deliberately faulty isolated child must trigger ASan heap-buffer-overflow detection |
| nyxen-cli-integration | Synthetic WAV/MP3 CLI cases, paths, format errors, preservation and output protection |
| nyxen-evaluation | Synthetic comparison/corpus/session tooling and rejection criteria |
| nyxen-output-safety | True-peak safe-gain behavior, errors, post-verification and convergence |
| nyxen-numerical-precision | 63 Decimal-reference cases, float32 export comparison and numerical/timing harness validation |

Check `ctest --test-dir build -N` and individual output. Python/FFmpeg discovery is conditional in CMake. Missing FFmpeg also leaves the MP3 fixture branch pending. Full validation requires all ten suites and the FFmpeg branch, not just a zero exit code on fewer tests. No listening preference or physical callback test is present.

Tests generate fixtures and reports in the configured binary directory, with unique subdirectories where required. Do not commit them. The ASan detector intentionally runs an erroneous child as a detector check; an expected detector hit is not an engine defect. The normal simulator's global C++ new/delete probes do not intercept all malloc/Win32/DLL allocation mechanisms. The source audit is not a C++ parser or a proof of lock/allocation freedom. Simulation timing excludes physical device, driver and scheduler behavior.

For an out-of-source validation build, replace `build` with a private external directory in all three commands and set `-DNYXEN_JUCE_DIR=<verified-checkout>`. Keep generated diagnostics outside the source tree. See `VALIDATION.md` for results actually obtained in the documented run.
