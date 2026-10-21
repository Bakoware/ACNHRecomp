#!/bin/bash
# Run Animal Crossing: New Horizons on a Mac (Apple Silicon, Vulkan on Metal through MoltenVK).
# usage: tools/acnh_mac.sh [quality|balanced|performance] [build dir]
#   quality      docked 1080p, plain scaling to the window
#   balanced     the game renders 720p (handheld mode) and FSR 1 upscales to the window  [default]
#   performance  as balanced with softer FSR sharpening (NX_FSR_SHARP=1.0)
# Extra environment still applies (NX_FSR_SHARP, NX_MEM_LIMIT_GB, NX_LOG, NX_MUTE, NX_HEADLESS).
cd "$(dirname "$0")/.." || exit 1
MODE=${1:-balanced}
BUILD=${2:-private/acnh/build}
case "$MODE" in
  quality) ;;
  balanced) export NX_HANDHELD=1 NX_FSR=1 ;;
  performance) export NX_HANDHELD=1 NX_FSR=1 NX_FSR_SHARP=${NX_FSR_SHARP:-1.0} ;;
  *) echo "unknown preset: $MODE"; exit 1 ;;
esac
export NX_LOG=${NX_LOG:-private/acnh/nx_play.log}
exec "$BUILD/acnh"
