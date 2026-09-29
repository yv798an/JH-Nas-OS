#!/bin/sh
# 在 Ubuntu x86 上构建并运行单元测试（带 ThreadSanitizer）。
# 注意：这是本机测试，不走交叉工具链，也不需要开发板。
set -e

cmake -S . -B build-host -DNAS_BUILD_TESTS=ON -DNAS_TSAN=ON
cmake --build build-host -j"$(nproc)"
ctest --test-dir build-host --output-on-failure
