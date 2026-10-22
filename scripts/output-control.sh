#!/bin/zsh
set -euo pipefail
cd "${0:A:h:h}"
mkdir -p build/tools
xcrun clang++ -std=c++17 -Wall -Wextra -Werror -I Shared Tools/OutputControl.cpp \
  -framework CoreAudio -framework CoreFoundation -framework IOKit -o build/tools/output-control
build/tools/output-control "$@"
