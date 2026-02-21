#!/usr/bin/env bash
set -euo pipefail

# Single-source conversion:
# input:  Lunar_Eclipse_Essentials_UPDATED_YouTube.webm (project root)
# range:  12s..26s
# output: MJPEG_OUT/lunar_eclipse_240.mjpeg

IN_FILE="${IN_FILE:-Lunar_Eclipse_Essentials_UPDATED_YouTube.webm}"
OUT_FILE="${OUT_FILE:-MJPEG_OUT/lunar_eclipse_240.mjpeg}"
START_SEC="${START_SEC:-12}"
END_SEC="${END_SEC:-26}"
FPS="${FPS:-10}"
MAX_FRAME_BYTES="${MAX_FRAME_BYTES:-10240}"

mkdir -p MJPEG_OUT

if [ ! -f "${IN_FILE}" ]; then
  echo "ERR: input not found: ${IN_FILE}" >&2
  exit 1
fi

check_mjpeg() {
  python3 - "$1" <<'PY'
import sys
p = sys.argv[1]
b = open(p, "rb").read()
i = 0
mx = 0
ok = 0
checked = False
while True:
    s = b.find(b"\xff\xd8", i)
    if s < 0: break
    e = b.find(b"\xff\xd9", s + 2)
    if e < 0: break
    sz = e + 2 - s
    if sz > mx: mx = sz
    if not checked:
        f = b[s:e+2]
        k = 2
        while k + 8 < len(f):
            if f[k] != 0xFF:
                k += 1
                continue
            while k + 1 < len(f) and f[k + 1] == 0xFF:
                k += 1
            if k + 1 >= len(f): break
            m = f[k + 1]
            k += 2
            if m in (0xD8, 0xD9, 0x01) or 0xD0 <= m <= 0xD7:
                continue
            if k + 1 >= len(f): break
            seglen = (f[k] << 8) | f[k + 1]
            if seglen < 2 or k + seglen > len(f): break
            if m == 0xC0 and seglen >= 17:
                nf = f[k + 7]
                if nf == 3:
                    hv0 = f[k + 9]
                    hv1 = f[k + 12]
                    hv2 = f[k + 15]
                    y = ((hv0 >> 4), (hv0 & 0x0F))
                    cb = ((hv1 >> 4), (hv1 & 0x0F))
                    cr = ((hv2 >> 4), (hv2 & 0x0F))
                    if (y, cb, cr) in [((2,2),(1,1),(1,1)), ((2,1),(1,1),(1,1)), ((1,1),(1,1),(1,1))]:
                        ok = 1
                elif nf == 1:
                    ok = 1
                checked = True
                break
            k += seglen
    i = e + 2
print(mx, ok)
PY
}

echo "Input:  ${IN_FILE}"
echo "Range:  ${START_SEC}s..${END_SEC}s"
echo "Output: ${OUT_FILE}"

# Try ffmpeg first.
ffmpeg -y -hide_banner -loglevel error \
  -ss "${START_SEC}" -to "${END_SEC}" -i "${IN_FILE}" \
  -vf "fps=${FPS},scale=240:240:force_original_aspect_ratio=decrease:flags=lanczos,pad=240:240:(ow-iw)/2:(oh-ih)/2:black,format=yuvj420p" \
  -c:v mjpeg -pix_fmt yuvj420p -q:v 22 -an -f mjpeg "${OUT_FILE}"

read -r max_frame samp_ok <<<"$(check_mjpeg "${OUT_FILE}")"

if [ "${samp_ok}" -ne 1 ] || [ "${max_frame}" -gt "${MAX_FRAME_BYTES}" ]; then
  echo "WARN: ffmpeg output not TJpgDec-friendly (samp_ok=${samp_ok}, max_frame=${max_frame}), fallback Pillow"

  # Decode selected range to temporary PNG frames, then encode baseline JPEG 4:4:4.
  TMP_DIR="$(mktemp -d)"
  trap 'rm -rf "${TMP_DIR}"' EXIT

  ffmpeg -y -hide_banner -loglevel error \
    -ss "${START_SEC}" -to "${END_SEC}" -i "${IN_FILE}" \
    -vf "fps=${FPS},scale=240:240:force_original_aspect_ratio=decrease:flags=lanczos,pad=240:240:(ow-iw)/2:(oh-ih)/2:black" \
    "${TMP_DIR}/frame_%05d.png"

  pyq=78
  while :; do
    python3 - "${TMP_DIR}" "${OUT_FILE}" "${pyq}" <<'PY'
import io, os, sys
from PIL import Image
in_dir, out_file, q = sys.argv[1], sys.argv[2], int(sys.argv[3])
files = sorted([f for f in os.listdir(in_dir) if f.startswith("frame_") and f.endswith(".png")])
with open(out_file, "wb") as out:
    for fn in files:
        im = Image.open(os.path.join(in_dir, fn)).convert("RGB")
        bio = io.BytesIO()
        im.save(bio, format="JPEG", quality=q, subsampling=0, optimize=False, progressive=False)
        out.write(bio.getvalue())
PY

    read -r max_frame samp_ok <<<"$(check_mjpeg "${OUT_FILE}")"
    if [ "${samp_ok}" -eq 1 ] && [ "${max_frame}" -le "${MAX_FRAME_BYTES}" ]; then
      break
    fi
    if [ "${pyq}" -le 42 ]; then
      break
    fi
    pyq=$((pyq - 6))
  done
fi

read -r max_frame samp_ok <<<"$(check_mjpeg "${OUT_FILE}")"
if [ "${samp_ok}" -ne 1 ]; then
  echo "ERR: output sampling still unsupported for TJpgDec" >&2
  exit 1
fi

echo "OK: ${OUT_FILE}"
echo "max_frame=${max_frame}B  samp_ok=${samp_ok}"
ls -lh "${OUT_FILE}"
