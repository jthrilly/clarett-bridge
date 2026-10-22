#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
if command -v xcrun >/dev/null 2>&1; then
  compiler=(xcrun clang++)
else
  compiler=("${CXX:-clang++}")
fi
for suite in Protocol AudioRing Monitor SampleRate Volume; do
  "${compiler[@]}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I Shared "Tests/${suite}Tests.cpp" -o "build/tests/${suite}-tests"
  "build/tests/${suite}-tests"
done
