#!/bin/zsh
set -euo pipefail
cd "${0:A:h:h}"
mkdir -p build/tools
xcrun clang++ -std=c++17 -Wall -Wextra -Werror Tools/AudioConfigure.cpp \
  -framework CoreAudio -framework CoreFoundation -o build/tools/audio-configure
build/tools/audio-configure "$@"
