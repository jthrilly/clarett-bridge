#!/bin/zsh
set -euo pipefail
cd "${0:A:h:h}"
mode=${1:-audio}
if [[ "$mode" != attach && "$mode" != mailbox && "$mode" != audio ]]; then
  print -u2 'Usage: scripts/build.sh [attach|mailbox|audio] [additional xcodebuild arguments]'
  exit 2
fi
if (( $# )); then shift; fi
version=1
if [[ "$mode" == mailbox ]]; then version=4; fi
if [[ "$mode" == audio ]]; then version=17; fi
version=${CLARETT_BUILD_VERSION:-$version}
python3 scripts/generate-project.py "$mode"
xcodebuild -project ClarettProbe.xcodeproj -scheme ClarettProbe \
  -configuration Debug -destination 'platform=macOS,arch=arm64' \
  -derivedDataPath "build/$mode" CLARETT_PROBE_MODE="$mode" \
  CLARETT_DRIVER_VERSION="$version" CLARETT_HOST_VERSION="${CLARETT_HOST_VERSION:-18}" "$@" build
