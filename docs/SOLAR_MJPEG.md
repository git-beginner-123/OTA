# SOLAR MJPEG Stable Configuration

Last verified: 2026-02-16

This project has one stable pipeline for SOLAR clip playback. Use this profile to avoid:
- `JDR_FMT3` decode failures
- center-crop content loss
- over-bright/washed output

## 1) Transcode (PC side)

Use:
- script: `build_space_total_keep_source.sh`
- default pixel format: `yuv420p` (critical for TJpgDec compatibility)
- scaling: keep aspect ratio + pad to `240x240`
- no center crop

Current key defaults in script:
- `PIX_FMT=yuv420p`
- `EQ_FILTER=eq=brightness=0.03:contrast=1.12:gamma=1.06:saturation=1.05`
- filter chain:
  - `scale=240:240:force_original_aspect_ratio=decrease`
  - `pad=240:240:(ow-iw)/2:(oh-ih)/2:black`

Recommended natural look command:

```bash
IN=big_720p30.mp4 \
PIX_FMT=yuv420p \
EQ_FILTER='eq=brightness=0.00:contrast=1.00:gamma=1.00:saturation=1.00' \
./build_space_total_keep_source.sh
```

Output file:
- `mjpeg_out/space_total.mjpeg`

## 2) Playback constraints (device side)

`main/experiments/exp_solar.c` uses ESP ROM TJpgDec.

Important:
- `SOLAR_BRIGHTNESS_BOOST=0` (do not double-boost by default)
- decoder only accepts specific JPEG sampling
- known good from ffmpeg profile above: `Y=2x2 Cb=1x1 Cr=1x1` (`yuv420p/yuvj420p`)

Known bad in this project:
- `Y=2x2 Cb=1x2 Cr=1x2` -> `jd_prepare err=8 (JDR_FMT3)`

## 3) Transfer to SPIFFS payload

```bash
cp -f mjpeg_out/space_total.mjpeg build/spiffs_payload/space_total.mjpeg
```

Optional verify:

```bash
stat -c '%n %s' mjpeg_out/space_total.mjpeg build/spiffs_payload/space_total.mjpeg
sha256sum mjpeg_out/space_total.mjpeg build/spiffs_payload/space_total.mjpeg
```

## 4) Flash

Flash app + SPIFFS + monitor:

```bash
idf.py -p /dev/ttyUSB0 -b 115200 flash spiffs-flash monitor
```

SPIFFS only:

```bash
idf.py -p /dev/ttyUSB0 -b 115200 spiffs-flash monitor
```

## 5) Quick troubleshooting

If image decode fails:
1. Check log for `JDR_FMT3`.
2. Re-transcode with `PIX_FMT=yuv420p`.
3. Avoid `yuv422p/yuvj422p/yuv444p/yuvj444p` in this pipeline.

If framing looks zoomed/cut:
1. Ensure transcode uses `scale + pad`, not `crop`.
2. Rebuild with `build_space_total_keep_source.sh` current defaults.

If serial flash fails with `/dev/ttyUSB0` busy/not found:
1. Check actual device: `ls -l /dev/ttyUSB* /dev/ttyACM*`
2. Kill monitor tools: `pkill -f "idf.py.*monitor|python.*miniterm|minicom|picocom|screen"`
3. Retry flash.
