#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
ref=${1:-HEAD}
commit=$(git rev-parse --verify "${ref}^{commit}")
name="clarett-bridge-${commit:0:12}"
mkdir -p build/releases
git archive --format=zip --prefix="$name/" --output="build/releases/$name.zip" "$commit"
git archive --format=tar --prefix="$name/" "$commit" | gzip -n > "build/releases/$name.tar.gz"
printf '%s\n' "build/releases/$name.zip" "build/releases/$name.tar.gz"
