#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./usb_port_check.sh [PORT] [--recover]
# Example:
#   ./usb_port_check.sh /dev/ttyUSB0
#   ./usb_port_check.sh /dev/ttyUSB0 --recover

PORT="${1:-/dev/ttyUSB0}"
DO_RECOVER=0
if [[ "${2:-}" == "--recover" ]]; then
  DO_RECOVER=1
fi

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
echo "usb_port_check: $(date '+%F %T')"
echo "port: $PORT"
echo "========================================"

if [[ ! -e "$PORT" ]]; then
  echo "STATUS: MISSING ($PORT not found)"
  exit 2
fi

echo "[1] Device node"
ls -l "$PORT" || true

echo "[2] Udev properties"
udevadm info -q property -n "$PORT" 2>/dev/null | \
  rg -n "DEVNAME|ID_VENDOR_ID|ID_MODEL_ID|ID_SERIAL|ID_USB_DRIVER" || true

TTY_NAME="${PORT##*/}"
DEV_PATH="$(readlink -f "/sys/class/tty/${TTY_NAME}/device" 2>/dev/null || true)"
DRIVER_PATH="$(readlink -f "/sys/class/tty/${TTY_NAME}/device/driver" 2>/dev/null || true)"
DRIVER_NAME="$(basename "$DRIVER_PATH" 2>/dev/null || true)"
USB_IFACE="$(basename "$(dirname "$DEV_PATH")" 2>/dev/null || true)"

echo "[3] Sysfs"
echo "device_path: ${DEV_PATH:-unknown}"
echo "driver: ${DRIVER_NAME:-unknown}"
echo "usb_iface: ${USB_IFACE:-unknown}"

echo "[4] Port users"
fuser -v "$PORT" 2>/dev/null || echo "(none)"

echo "[5] Mounted USB disks"
lsblk -o NAME,TRAN,FSTYPE,SIZE,MOUNTPOINT | sed -n '1,200p' | rg -i "NAME|usb|sd[a-z]|mmc" || true
findmnt -rn -S /dev/sd\* -o SOURCE,TARGET,FSTYPE,OPTIONS || true

echo "[6] Recent kernel messages (USB/CH341/ttyUSB)"
dmesg | tail -n 120 | rg -i "ttyUSB|ch341|usb .*error|-110|timeout|disconnect|attached" || true

if [[ "$DO_RECOVER" -eq 1 ]]; then
  echo "[7] Recovery"
  pkill -f "idf.py.*monitor|idf_monitor.py|picocom|minicom|screen" >/dev/null 2>&1 || true
  sudo fuser -k "$PORT" >/dev/null 2>&1 || true

  if [[ "$DRIVER_NAME" == "ch341-uart" && -n "$USB_IFACE" ]]; then
    if sudo sh -c "printf '%s' '$USB_IFACE' > /sys/bus/usb-serial/drivers/ch341-uart/unbind"; then
      sleep 1
      if sudo sh -c "printf '%s' '$USB_IFACE' > /sys/bus/usb-serial/drivers/ch341-uart/bind"; then
        echo "rebind: OK"
      else
        echo "rebind: bind failed"
      fi
    else
      echo "rebind: unbind failed, try module reload"
      sudo modprobe -r ch341 usbserial || true
      sleep 1
      sudo modprobe usbserial || true
      sudo modprobe ch341 || true
    fi
  else
    echo "skip rebind (driver=$DRIVER_NAME)"
  fi

  echo "waiting for port re-enumeration..."
  NEW_PORT=""
  for _ in $(seq 1 15); do
    sleep 1
    if NEW_PORT="$(find_port)"; then break; fi
  done
  if [[ -n "$NEW_PORT" ]]; then
    echo "RECOVERY STATUS: PORT PRESENT ($NEW_PORT)"
    PORT="$NEW_PORT"
  else
    echo "RECOVERY STATUS: PORT MISSING (no ttyUSB/ttyACM found)"
    exit 3
  fi
fi

echo "DONE"
