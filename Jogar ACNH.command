#!/bin/bash
# Double-click in Finder (or run it) to play on a Mac: rebuilds what changed in the runtime (seconds; nothing when
# up to date), then starts the game with the settings of private/acnh/config.ini. A copy on the desktop works too:
# it finds the repository from the path below, written by tools/install_mac_shortcut.sh.
REPO="$(cd "$(dirname "$0")" && pwd)"
[ -f "$REPO/tools/acnh_mac.sh" ] || REPO="$(cat "$HOME/.acnhrecomp_repo" 2>/dev/null)"
cd "$REPO" || { echo "ACNH Recomp folder not found ($REPO)"; read -r -p "Press Enter to close"; exit 1; }
BUILD=private/acnh/build

if [ ! -x "$BUILD/acnh" ]; then
  echo "The game is not built yet: run the setup first (README, Installation)."
  read -r -p "Press Enter to close"
  exit 1
fi

if command -v ninja >/dev/null && [ -f "$BUILD/build.ninja" ]; then
  # Another build in progress (tools and agents share private/build.lock): wait for it.
  # A lock older than 2 hours is a leftover of a build that was killed.
  [ -n "$(find private/build.lock -maxdepth 0 -mmin +120 2>/dev/null)" ] && rmdir private/build.lock 2>/dev/null
  until mkdir private/build.lock 2>/dev/null; do echo "Waiting for another build to finish..."; sleep 5; done
  trap 'rmdir private/build.lock 2>/dev/null' EXIT
  echo "Updating the build..."
  if ! ninja -C "$BUILD" acnh > private/acnh/logs/play_build.log 2>&1; then
    tail -20 private/acnh/logs/play_build.log
    echo
    echo "The build failed (full output: private/acnh/logs/play_build.log)."
    read -r -p "Press Enter to close"
    exit 1
  fi
  rmdir private/build.lock 2>/dev/null
  trap - EXIT
fi

echo "Starting Animal Crossing: New Horizons..."
tools/acnh_mac.sh
status=$?
if [ $status -ne 0 ]; then
  echo
  echo "The game closed with an error ($status). Settings > About > Create problem report, or run:"
  echo "  NX_REPORT=1 $BUILD/acnh"
  read -r -p "Press Enter to close"
fi
