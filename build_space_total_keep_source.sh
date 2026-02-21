#!/usr/bin/env bash
set -euo pipefail

# Clean rebuild script (keeps source file).
# Output is always: mjpeg_out/space_total.mjpeg
#
# Usage:
#   ./build_space_total_keep_source.sh
#   IN=big_720p30.mp4 LIMIT_BYTES=$((6*1024*1024)) ./build_space_total_keep_source.sh
#   FPS_LIST="10 8 6" Q_LIST="10 12 14 18" ./build_space_total_keep_source.sh

IN="${IN:-big_720p30.mp4}"
OUT="mjpeg_out/space_total.mjpeg"
LIMIT_BYTES="${LIMIT_BYTES:-6291456}"
FPS_LIST="${FPS_LIST:-10 8 6 5 4}"
Q_LIST="${Q_LIST:-2 4 6 8 10 12 14 16 18 22 26 30 34}"
PIX_FMT="${PIX_FMT:-yuv420p}"
# FRAME_MODE:
#   cover   -> center-crop to square then scale (subject larger, clearer)
#   contain -> keep full frame + pad (no cut, subject smaller)
FRAME_MODE="${FRAME_MODE:-contain}"
# Visible content size before final 240x240 canvas pad.
# 240 means full canvas (no side border). Smaller values add border and make subject smaller.
CONTENT_SIZE="${CONTENT_SIZE:-240}"
PAD_COLOR="${PAD_COLOR:-black}"
# Additional zoom after framing. 1.00 = no zoom.
# Example: 1.08 gives a mild 8% zoom-in.
ZOOM="${ZOOM:-1.00}"
# Keep enhancement mild here; avoid over-boost because playback side may still do panel-level correction.
EQ_FILTER="${EQ_FILTER:-eq=brightness=0.03:contrast=1.12:gamma=1.06:saturation=1.05}"
SHARP_FILTER="${SHARP_FILTER:-unsharp=5:5:0.6:5:5:0.0}"

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ERR: ffmpeg not found"
  exit 1
fi

if [[ ! -f "$IN" ]]; then
  echo "ERR: input not found: $IN"
  exit 1
fi

mkdir -p mjpeg_out

echo "IN:    $IN"
echo "OUT:   $OUT"
echo "LIMIT: ${LIMIT_BYTES} bytes"
echo "FPS:   ${FPS_LIST}"
echo "Q:     ${Q_LIST}"
echo "PIXFMT:${PIX_FMT}"
echo "MODE:  ${FRAME_MODE}"
echo "CONTENT:${CONTENT_SIZE}"
echo "PAD:   ${PAD_COLOR}"
echo "ZOOM:  ${ZOOM}"
echo "EQ:    ${EQ_FILTER}"
echo "SHARP: ${SHARP_FILTER}"

# Full-duration conversion:
# 1) frame mode:
#    - cover: center-crop to square (better subject detail)
#    - contain: keep full frame + pad
# 2) apply mild enhancement for small LCD
ok=0
best_fps=""
best_q=""
best_size=""
if [[ "${FRAME_MODE}" == "cover" ]]; then
  VF_PREFIX="fps=%s,crop='min(iw,ih)':'min(iw,ih)',scale=${CONTENT_SIZE}:${CONTENT_SIZE}:flags=lanczos,pad=240:240:(ow-iw)/2:(oh-ih)/2:${PAD_COLOR}"
else
  VF_PREFIX="fps=%s,scale=${CONTENT_SIZE}:${CONTENT_SIZE}:force_original_aspect_ratio=decrease:flags=lanczos,pad=240:240:(ow-iw)/2:(oh-ih)/2:${PAD_COLOR}"
fi
ZOOM_FILTER=""
if [[ "${ZOOM}" != "1" && "${ZOOM}" != "1.0" && "${ZOOM}" != "1.00" ]]; then
  ZOOM_FILTER=",scale=iw*${ZOOM}:ih*${ZOOM}:flags=lanczos,crop=240:240"
fi
for FPS in ${FPS_LIST}; do
  for Q in ${Q_LIST}; do
    vf=$(printf "${VF_PREFIX}" "${FPS}")
    ffmpeg -y -hide_banner -loglevel error \
      -i "$IN" \
      -vf "${vf}${ZOOM_FILTER},${SHARP_FILTER},${EQ_FILTER}" \
      -c:v mjpeg -pix_fmt "${PIX_FMT}" -q:v "$Q" -an -f mjpeg "$OUT"

    if stat -c%s "$OUT" >/dev/null 2>&1; then
      sz=$(stat -c%s "$OUT")
    else
      sz=$(wc -c < "$OUT")
    fi
    printf 'try fps=%s q=%s size=%s\n' "$FPS" "$Q" "$sz"
    if [[ "$sz" -le "$LIMIT_BYTES" ]]; then
      ok=1
      best_fps="$FPS"
      best_q="$Q"
      best_size="$sz"
      break 2
    fi
  done
done

if [[ "$ok" -ne 1 ]]; then
  echo "ERR: cannot fit <= ${LIMIT_BYTES} bytes with current search range"
  echo "TIP: increase LIMIT_BYTES or relax FPS_LIST/Q_LIST."
  exit 1
fi

echo "OK: built $OUT (source kept)"
echo "chosen fps=${best_fps} q=${best_q} size=${best_size}"
ls -lh "$OUT"
