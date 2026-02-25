#!/usr/bin/env bash
set -euo pipefail

# Build firmware variants and prepare OTA artifacts with numeric app type:
#   1=GO, 2=CHESS, 3=DICE, 4=GOMOKU
#
# Output per target:
#   ota/<slug>/latest.bin
#   ota/<slug>/app-<version>.bin
#   ota/<slug>/app-<version>-t<type>.bin
#   plus corresponding sha256 files
#
# Also writes:
#   ota/targets.txt  (type map)

PROJECT_BIN="game_console_idf61_ili9341.bin"
TARGET_FILTER_RAW="${1:-ALL}"
TARGET_FILTER="${TARGET_FILTER_RAW^^}"
VERSION_OVERRIDE="${2:-}"

detect_version() {
  if [[ -n "${VERSION_OVERRIDE}" ]]; then
    echo "${VERSION_OVERRIDE}"
    return 0
  fi
  if command -v git >/dev/null 2>&1; then
    local v
    v="$(git describe --tags --always --dirty 2>/dev/null || true)"
    if [[ -n "$v" ]]; then
      echo "$v"
      return 0
    fi
  fi
  echo "v0.0.0-local"
}

VERSION="$(detect_version)"

is_selected() {
  local type_id="$1"
  local slug="$2"
  local variant="$3"
  local slug_u="${slug^^}"
  local variant_u="${variant^^}"
  [[ "${TARGET_FILTER}" == "ALL" || "${TARGET_FILTER}" == "${type_id}" || "${TARGET_FILTER}" == "${slug_u}" || "${TARGET_FILTER}" == "${variant_u}" ]]
}

build_one() {
  local variant="$1"
  local slug="$2"
  local type_id="$3"
  local bdir="build-${slug}"
  echo "[BUILD] type=${type_id} variant=${variant} dir=${bdir} ver=${VERSION}"
  idf.py -B "${bdir}" -DAPP_VARIANT="${variant}" build

  local src_bin="${bdir}/${PROJECT_BIN}"
  [[ -f "${src_bin}" ]] || { echo "missing ${src_bin}" >&2; exit 1; }

  local out_dir="ota/${slug}"
  mkdir -p "${out_dir}"
  cp -f "${src_bin}" "${out_dir}/latest.bin"
  cp -f "${src_bin}" "${out_dir}/app-${VERSION}.bin"
  cp -f "${src_bin}" "${out_dir}/app-${VERSION}-t${type_id}.bin"
  sha256sum "${out_dir}/latest.bin" > "${out_dir}/latest.sha256"
  sha256sum "${out_dir}/app-${VERSION}.bin" > "${out_dir}/app-${VERSION}.sha256"
  sha256sum "${out_dir}/app-${VERSION}-t${type_id}.bin" > "${out_dir}/app-${VERSION}-t${type_id}.sha256"
}

mkdir -p ota
cat > ota/targets.txt <<EOF
1 GO go
2 CHESS chess
3 DICE dice
4 GOMOKU gomoku
EOF

if is_selected 1 go GO; then
  build_one GO go 1
fi
if is_selected 2 chess CHESS; then
  build_one CHESS chess 2
fi
if is_selected 3 dice DICE; then
  build_one DICE dice 3
fi
if is_selected 4 gomoku GOMOKU; then
  build_one GOMOKU gomoku 4
fi

echo
echo "[DONE] Generated:"
echo "  ota/go/*"
echo "  ota/chess/*"
echo "  ota/dice/*"
echo "  ota/gomoku/*"
echo "  ota/targets.txt"
echo
echo "Filter examples:"
echo "  ./build_variants_ota.sh 1"
echo "  ./build_variants_ota.sh GO"
echo "  ./build_variants_ota.sh ALL"
echo "  ./build_variants_ota.sh ALL v1.2.8   # optional version override for artifact file names"
echo
echo "Flash hints:"
echo "  ./flash_variant.sh <type|name|ALL> /dev/ttyACM0 [--no-monitor]"
echo "  type map: 1=GO 2=CHESS 3=DICE 4=GOMOKU"
echo "  examples:"
echo "    ./flash_variant.sh 1 /dev/ttyACM0"
echo "    ./flash_variant.sh GO /dev/ttyACM0 --no-monitor"
echo "    ./flash_variant.sh ALL /dev/ttyACM0 --no-monitor"
echo
echo "Direct idf.py flash:"
echo "  idf.py -B build-go -p /dev/ttyACM0 flash monitor"
echo "  idf.py -B build-chess -p /dev/ttyACM0 flash monitor"
echo "  idf.py -B build-dice -p /dev/ttyACM0 flash monitor"
echo "  idf.py -B build-gomoku -p /dev/ttyACM0 flash monitor"
