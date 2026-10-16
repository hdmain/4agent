#!/usr/bin/env bash
set -euo pipefail
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-"$source_dir/build-deb"}
version=${2:-1.0.0-1}
cmake -S "$source_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DFOURAGENT_ENABLE_PACKAGING=ON -DFOURAGENT_DEB_VERSION="$version"
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure
(cd -- "$build_dir" && cpack -G DEB)
