# Validation record

Run: **2026-11-10**, Windows Release x64; Ninja; `BUILD_TESTING=ON`; out-of-source build directory. JUCE was supplied externally at revision `7c9d3783b127263d72bb65fe0a7e2dc8a02a7ac2` (8.0.13). No dependencies, binaries or test audio are included in this repository.

Toolchain used: MSVC 19.51.36244.0 (Visual Studio Build Tools 2026), CMake 4.2.3-msvc3, Ninja 1.13.2, Python 3.14.4, FFmpeg 9.0.2-full_build. The required measurement filters and libmp3lame were present. A restricted CMake compiler check initially encountered MSVC C1902; a separate build with expanded execution permissions completed without source changes.

**CTest: 10 suites passed, 0 failed, 119.83 s total.**

| Suite | Result | Seconds |
| --- | --- | ---: |
| nyxen-continuous | Passed | 4.66 |
| nyxen-continuous-asan | Passed | 37.02 |
| nyxen-audio | Passed | 1.43 |
| nyxen-dsp | Passed | 0.60 |
| nyxen-rt-source-audit | Passed | 0.12 |
| nyxen-asan-detector | Passed | 0.89 |
| nyxen-cli-integration | Passed | 4.20 |
| nyxen-evaluation | Passed | 35.02 |
| nyxen-output-safety | Passed | 17.79 |
| nyxen-numerical-precision | Passed | 18.07 |

Four synthetic MP3 cases and 63 Decimal-reference precision cases passed. The ASan detector test includes an intentional out-of-bounds failure in an isolated child to verify that ASan detects it. Its expected detection is not an error in NyxenCore.

Equivalent commands from an initialized x64 development shell:

```text
cmake -S . -B <build-dir> -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DNYXEN_JUCE_DIR=<checkout>
cmake --build <build-dir> --parallel 2
ctest --test-dir <build-dir> --output-on-failure
```

This result does not validate a fresh dependency acquisition on another machine, cross-platform support, relocated binaries, concurrent audio hosting, physical device callbacks, underruns or perceptual listening results. CMake does not enforce the documented JUCE revision. The compiled safe-gain helper uses configure-time paths and must be rebuilt after relocation. No tests were rerun for this documentation-only change.
