#!/usr/bin/env bash
# Translates the shaders a run found without a SPIR-V translation (NX_DUMP_SHADER_MISSES=1 writes their code and
# control sections to <data root>/shader_misses/) and adds them to <data root>/spirv with their results.tsv rows.
# A program bound with such a stage draws without it: a missing fragment shader writes depth only and the deferred
# lighting shows a black silhouette (ACNH 3.0.2: 15 Park_UBER fragment variants the offline extraction does not
# reach, on character faces, the Nook Stop and others; the log says "no <hash>.spv" and, per draw, "stage(s) without
# a translation").
#   usage: tools/translate_shader_misses.sh [data root]      (default: private/acnh)
#   env:   DOTNET=<dotnet>   (default: private/third_party/dotnet/dotnet, else dotnet on PATH)
#          HARNESS=<spirv_harness.dll>  (default: private/build/spirv_harness/bin/Release/net10.0/spirv_harness.dll)
set -u
cd "$(dirname "$0")/.." || exit 1
W=${1:-private/acnh}
M=$W/shader_misses
[ -d "$M" ] && ls "$M"/code_*.bin > /dev/null 2>&1 || { echo "no $M/code_*.bin: run the game with NX_DUMP_SHADER_MISSES=1"; exit 1; }
DOTNET=${DOTNET:-}
if [ -z "$DOTNET" ]; then
  if [ -x private/third_party/dotnet/dotnet ]; then DOTNET=private/third_party/dotnet/dotnet; else DOTNET=dotnet; fi
fi
HARNESS=${HARNESS:-private/build/spirv_harness/bin/Release/net10.0/spirv_harness.dll}
VAL=$(command -v spirv-val || echo /usr/bin/spirv-val)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
# A one-group extraction tree (the batch reads manifest.tsv: hash in column 11, code and control files in 12, 13).
G=$T/in/runtime_misses
mkdir -p "$G" "$T/out"
{
  printf 'id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\tconst_size\t'
  printf 'code_sha1_16\tcode_file\tctrl_file\n'
  for f in "$M"/code_*.bin; do
    h=$(basename "$f" .bin); h=${h#code_}
    [ -e "$W/spirv/$h.spv" ] && continue  # translated by an earlier run
    [ -e "$M/ctrl_$h.bin" ] || continue
    cp "$f" "$M/ctrl_$h.bin" "$G/"
    printf 'runtime/%s\t-\t0\t0\t0\t0x880\t0\t0\t0\t0\t%s\tcode_%s.bin\tctrl_%s.bin\n' "$h" "$h" "$h" "$h"
  done
} > "$G/manifest.tsv"
n=$(($(wc -l < "$G/manifest.tsv") - 1))
[ "$n" -gt 0 ] || { echo "nothing new to translate"; exit 0; }
echo "$n shaders to translate"
"$DOTNET" "$HARNESS" --batch "$T/in" --out "$T/out" --jobs 4 --spirv-val "$VAL" | tail -n 3
mkdir -p "$W/spirv"
added=0
while IFS=$'\t' read -r h rest; do
  [ "$h" = hash ] && continue
  [ -s "$T/out/$h.spv" ] || { echo "$h: not translated ($(echo "$rest" | cut -f3))"; continue; }
  cp "$T/out/$h.spv" "$W/spirv/"
  grep -q "^$h	" "$W/spirv/results.tsv" 2>/dev/null || printf '%s\t%s\n' "$h" "$rest" >> "$W/spirv/results.tsv"
  added=$((added + 1))
done < "$T/out/results.tsv"
echo "$added added to $W/spirv"
