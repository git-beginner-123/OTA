#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./flash_safe.sh [PORT] [--no-monitor]
# Example:
#   ./flash_safe.sh /dev/ttyUSB0
#   ./flash_safe.sh /dev/ttyUSB0 --no-monitor

PORT="${1:-/dev/ttyUSB0}"
MONITOR_AFTER_FLASH=1
if [[ "${2:-}" == "--no-monitor" ]]; then
  MONITOR_AFTER_FLASH=0
fi
START_TS="$(date '+%F %T')"

find_port() {
  local p=""
  if [[ -e "$PORT" ]]; then
    echo "$PORT"
    return 0
  fi
  p="$(ls -1 /dev/serial/by-id/* 2>/dev/null | head -n1 || true)"
  if [[ -n "$p" && -e "$p" ]]; then
    echo "$p"
    return 0
  fi
  p="$(ls -1 /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -n1 || true)"
  if [[ -n "$p" && -e "$p" ]]; then
    echo "$p"
    return 0
  fi
  return 1
}

echo "========================================"
echo "flash_safe: start  $START_TS"
echo "flash_safe: port   $PORT"
echo "========================================"

if ! PORT_FOUND="$(find_port)"; then
  echo "WARN: initial port not found: $PORT" >&2
  echo "INFO: waiting for USB serial re-enumeration..." >&2
  for _ in $(seq 1 15); do
    sleep 1
    if PORT_FOUND="$(find_port)"; then break; fi
  done
fi
if [[ -z "${PORT_FOUND:-}" ]]; then
  echo "ERROR: serial port not found after wait" >&2
  exit 1
fi
PORT="$PORT_FOUND"

echo "[1/4] Releasing serial port users: $PORT"
sudo fuser -k "$PORT" >/dev/null 2>&1 || true
pkill -f "idf.py monitor|picocom|minicom|screen|putty|moserial" >/dev/null 2>&1 || true

DRIVER_PATH="$(readlink -f "/sys/class/tty/${PORT##*/}/device/driver" 2>/dev/null || true)"
DRIVER_NAME="$(basename "$DRIVER_PATH" 2>/dev/null || true)"

if [[ "$DRIVER_NAME" == "ch341-uart" ]]; then
  IFACE="$(basename "$(dirname "$(readlink -f "/sys/class/tty/${PORT##*/}/device")")")"
  if [[ -n "$IFACE" ]]; then
    echo "[2/4] Rebinding ch341-uart: $IFACE"
    if sudo sh -c "printf '%s' '$IFACE' > /sys/bus/usb-serial/drivers/ch341-uart/unbind"; then
      sleep 1
      if sudo sh -c "printf '%s' '$IFACE' > /sys/bus/usb-serial/drivers/ch341-uart/bind"; then
        sleep 1
      else
        echo "WARN: bind failed, continue without rebind" >&2
      fi
    else
      echo "WARN: unbind failed (permission/policy), continue without rebind" >&2
    fi
  fi
else
  echo "[2/4] Skip rebind (driver: ${DRIVER_NAME:-unknown})"
fi

echo "[3/4] Verifying port exists"
if [[ ! -e "$PORT" ]]; then
  echo "WARN: configured port disappeared, waiting and auto-detecting..." >&2
  PORT_FOUND=""
  for _ in $(seq 1 15); do
    sleep 1
    if PORT_FOUND="$(find_port)"; then break; fi
  done
  if [[ -z "$PORT_FOUND" ]]; then
    echo "ERROR: serial port disappeared after recovery and did not return" >&2
    exit 1
  fi
  PORT="$PORT_FOUND"
  echo "INFO: switched to detected port: $PORT"
fi

echo "[4/4] Flashing via idf.py"
if idf.py -p "$PORT" flash; then
  END_TS="$(date '+%F %T')"
  echo "========================================"
  echo "flash_safe: SUCCESS  $END_TS"
  echo "flash_safe: flashed  $PORT"
  if [[ "$MONITOR_AFTER_FLASH" -eq 1 ]]; then
    echo "flash_safe: monitor  ON (exit with Ctrl+])"
  else
    echo "next: idf.py -p $PORT monitor"
  fi
  echo "========================================"
else
  END_TS="$(date '+%F %T')"
  echo "========================================" >&2
  echo "flash_safe: FAILED   $END_TS" >&2
  echo "flash_safe: port     $PORT" >&2
  echo "========================================" >&2
  exit 1
fi

if [[ "$MONITOR_AFTER_FLASH" -eq 1 ]]; then
  echo "[5/5] Opening monitor"
  exec idf.py -p "$PORT" monitor
fi
