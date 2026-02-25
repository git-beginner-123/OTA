#!/usr/bin/env bash
set -euo pipefail

# Flash by target type:
#   1/GO, 2/CHESS, 3/DICE, 4/GOMOKU, or ALL
#
# Usage:
#   ./flash_variant.sh <target|ALL> [PORT] [--no-monitor]
# Examples:
#   ./flash_variant.sh 1 /dev/ttyACM0
#   ./flash_variant.sh GO /dev/ttyACM0 --no-monitor
#   ./flash_variant.sh ALL /dev/ttyACM0 --no-monitor

TARGET_RAW="${1:-ALL}"
TARGET="${TARGET_RAW^^}"
PORT="${2:-AUTO}"
MONITOR_AFTER_FLASH=1
if [[ "${3:-}" == "--no-monitor" || "${2:-}" == "--no-monitor" ]]; then
  MONITOR_AFTER_FLASH=0
fi

pick_dir() {
  local t="$1"
  case "$t" in
    1|GO) echo "build-go" ;;
    2|CHESS) echo "build-chess" ;;
    3|DICE) echo "build-dice" ;;
    4|GOMOKU) echo "build-gomoku" ;;
    *) return 1 ;;
  esac
}

resolve_port() {
  local p="${1:-AUTO}"
  if [[ "$p" != "AUTO" && -e "$p" ]]; then
    echo "$p"
    return 0
  fi
  local by_id
  by_id="$(ls -1 /dev/serial/by-id/* 2>/dev/null | head -n1 || true)"
  if [[ -n "$by_id" && -e "$by_id" ]]; then
    echo "$by_id"
    return 0
  fi
  local tty
  tty="$(ls -1 /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -n1 || true)"
  if [[ -n "$tty" && -e "$tty" ]]; then
    echo "$tty"
    return 0
  fi
  return 1
}

flash_one() {
  local t="$1"
  local dir
  local resolved_port
  dir="$(pick_dir "$t")"
  resolved_port="$(resolve_port "$PORT")" || {
    echo "No serial port found. Tried /dev/serial/by-id, /dev/ttyUSB*, /dev/ttyACM*" >&2
    exit 1
  }
  [[ -d "$dir" ]] || { echo "Missing build dir: $dir (run build_variants_ota.sh first)" >&2; exit 1; }
  echo "[FLASH] target=$t dir=$dir port=$resolved_port"
  if [[ "$MONITOR_AFTER_FLASH" -eq 1 ]]; then
    idf.py -B "$dir" -p "$resolved_port" flash monitor
  else
    idf.py -B "$dir" -p "$resolved_port" flash
  fi
}

if [[ "$TARGET" == "ALL" ]]; then
  # ALL mode should not auto-open monitor repeatedly.
  MONITOR_AFTER_FLASH=0
  flash_one GO
  flash_one CHESS
  flash_one DICE
  flash_one GOMOKU
  echo "[DONE] flashed ALL targets to ${PORT}"
  exit 0
fi

flash_one "$TARGET"
