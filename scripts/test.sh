#!/bin/zsh
set -euo pipefail
cd "${0:A:h:h}"
mkdir -p build/tests
xcrun clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I Shared Tests/ProtocolTests.cpp -o build/tests/protocol-tests
build/tests/protocol-tests
xcrun clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I Shared Tests/AudioRingTests.cpp -o build/tests/audio-ring-tests
build/tests/audio-ring-tests
xcrun clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I Shared Tests/MonitorTests.cpp -o build/tests/monitor-tests
build/tests/monitor-tests
