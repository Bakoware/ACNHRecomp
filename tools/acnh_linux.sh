#!/bin/bash
# Run Animal Crossing: New Horizons on Linux (or macOS) with a preset. usage: tools/acnh_linux.sh [preset] [build dir]
#   quality      docked 1080p, plain scaling to the window                                          [default]
#   balanced     the game renders 720p (handheld mode) and FSR 1 upscales to the window
#   intel        integrated GPU (Intel Iris Xe / UHD, Mesa ANV): handheld 720p, pipelined submits, audio reverb off
#   lightest     intel plus FSR 1
# The audio reverb (I3DL2, rendered on the CPU) costs about one core; NX_AUDIO_REVERB=0 drops it in any preset.
# Extra environment still applies (NX_FULLSCREEN=1, NX_RESOLUTION=WxH, NX_MUTE=1, NX_MEM_LIMIT_GB, NX_LOG, NX_HEADLESS=1).
cd "$(dirname "$0")/.." || exit 1
MODE=${1:-quality}
BUILD=${2:-private/acnh/build}
case "$MODE" in
  quality) ;;
  balanced) export NX_HANDHELD=1 NX_FSR=1 ;;
  intel) export NX_GPU=${NX_GPU:-integrated} NX_HANDHELD=1 NX_PIPELINED=${NX_PIPELINED:-2} NX_AUDIO_REVERB=${NX_AUDIO_REVERB:-0} ;;
  lightest) export NX_GPU=${NX_GPU:-integrated} NX_HANDHELD=1 NX_PIPELINED=${NX_PIPELINED:-2} NX_FSR=1 NX_AUDIO_REVERB=${NX_AUDIO_REVERB:-0} ;;
  *) echo "unknown preset: $MODE"; exit 1 ;;
esac
export NX_LOG=${NX_LOG:-private/acnh/nx_play.log}
export NX_MEM_LIMIT_GB=${NX_MEM_LIMIT_GB:-10}
exec "$BUILD/acnh"
