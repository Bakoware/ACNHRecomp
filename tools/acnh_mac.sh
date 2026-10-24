#!/bin/bash
# Run Animal Crossing: New Horizons on a Mac (Apple Silicon, Vulkan on Metal through MoltenVK).
# usage: tools/acnh_mac.sh [quality|balanced|performance] [build dir]
#   quality      docked 1080p, plain scaling to the window
#   balanced     the game renders 720p (handheld mode) and FSR 1 upscales to the window  [default]
#   performance  as balanced with softer FSR sharpening (NX_FSR_SHARP=1.0) and the audio reverb off
# Extra environment still applies (NX_FSR_SHARP, NX_MEM_LIMIT_GB, NX_LOG, NX_MUTE, NX_HEADLESS).
cd "$(dirname "$0")/.." || exit 1
BUILD="private/acnh/build"
if [ -n "$1" ]; then
  case "$1" in
    quality)
      export NX_HANDHELD=0 NX_FSR=0
      BUILD=${2:-$BUILD}
      ;;
    balanced)
      export NX_HANDHELD=1 NX_FSR=1
      BUILD=${2:-$BUILD}
      ;;
    performance)
      export NX_HANDHELD=1 NX_FSR=1 NX_FSR_SHARP=${NX_FSR_SHARP:-1.0} NX_AUDIO_REVERB=${NX_AUDIO_REVERB:-0}
      BUILD=${2:-$BUILD}
      ;;
    *)
      if [ -d "$1" ]; then
        BUILD="$1"
      else
        echo "unknown preset: $1"
        exit 1
      fi
      ;;
  esac
fi
export NX_LOG=${NX_LOG:-private/acnh/nx_play.log}
exec "$BUILD/acnh"
