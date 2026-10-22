#!/usr/bin/env bash
# Builds (the first time, or after an update of the repository) and opens the launcher: the program that sets the
# game up from your files and starts it (README, "Installation"). Linux and macOS.
#   usage: tools/launcher.sh            env: BUILD=<build folder> (default build/launcher)
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${BUILD:-build/launcher}
case "$BUILD" in /*) ;; *) BUILD=$PWD/$BUILD ;; esac
GEN=()
command -v ninja >/dev/null && GEN=(-G Ninja)
EXTRA=()
[ "$(uname)" = Darwin ] && EXTRA+=(-DCMAKE_PREFIX_PATH="$(brew --prefix 2>/dev/null || echo /opt/homebrew)")
# NX_GEN_DIR points at an empty folder: the launcher never needs the generated game code.
[ -f "$BUILD/CMakeCache.txt" ] ||
  cmake -S . -B "$BUILD" ${GEN[@]+"${GEN[@]}"} -DNX_GAME=acnh -DNX_GEN_DIR="$BUILD/no-gen" \
        -DCMAKE_BUILD_TYPE=Release ${EXTRA[@]+"${EXTRA[@]}"} >/dev/null
cmake --build "$BUILD" --target acnhrecomp_launcher
exec "$BUILD/acnhrecomp_launcher" "$@"
