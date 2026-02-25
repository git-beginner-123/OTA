#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME="$(basename "$0")"

usage() {
  cat <<EOF
Usage:
  $SCRIPT_NAME collect [--project DIR] [--out DIR] [--name LABEL]
  $SCRIPT_NAME compare SNAPSHOT_A SNAPSHOT_B [--out REPORT]

Commands:
  collect   Collect ESP-IDF build-related snapshot for current device.
  compare   Compare two collected snapshots and print/write a report.

Examples:
  $SCRIPT_NAME collect --project . --out ./idf_snapshots --name devA
  $SCRIPT_NAME compare ./idf_snapshots/devA_20260221_101010 ./idf_snapshots/devB_20260221_111010
  $SCRIPT_NAME compare snapA snapB --out idf_diff_report.txt
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

ts_now() {
  date +"%Y%m%d_%H%M%S"
}

safe_run() {
  local title="$1"
  shift
  {
    echo "### $title"
    if "$@" >/tmp/idf_diff_cmd.out 2>/tmp/idf_diff_cmd.err; then
      cat /tmp/idf_diff_cmd.out
      if [ -s /tmp/idf_diff_cmd.err ]; then
        echo "[stderr]"
        cat /tmp/idf_diff_cmd.err
      fi
    else
      echo "[command failed] $*"
      if [ -s /tmp/idf_diff_cmd.out ]; then
        cat /tmp/idf_diff_cmd.out
      fi
      if [ -s /tmp/idf_diff_cmd.err ]; then
        echo "[stderr]"
        cat /tmp/idf_diff_cmd.err
      fi
    fi
    echo
  }
}

copy_if_exists() {
  local src="$1"
  local dst="$2"
  if [ -e "$src" ]; then
    mkdir -p "$(dirname "$dst")"
    cp -a "$src" "$dst"
  fi
}

collect_snapshot() {
  local project="."
  local out_root="./idf_snapshots"
  local label=""

  while [ $# -gt 0 ]; do
    case "$1" in
      --project)
        project="$2"
        shift 2
        ;;
      --out)
        out_root="$2"
        shift 2
        ;;
      --name)
        label="$2"
        shift 2
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        die "Unknown option for collect: $1"
        ;;
    esac
  done

  [ -d "$project" ] || die "Project directory not found: $project"
  mkdir -p "$out_root"

  local host ts snap_name snap_dir meta_dir files_dir
  host="$(hostname 2>/dev/null || echo unknown-host)"
  ts="$(ts_now)"
  if [ -n "$label" ]; then
    snap_name="${label}_${ts}"
  else
    snap_name="${host}_${ts}"
  fi
  snap_dir="$(cd "$out_root" && pwd)/$snap_name"
  meta_dir="$snap_dir/meta"
  files_dir="$snap_dir/files"

  mkdir -p "$meta_dir" "$files_dir"

  local abs_project
  abs_project="$(cd "$project" && pwd)"

  {
    echo "snapshot_name=$snap_name"
    echo "created_at=$(date -Iseconds)"
    echo "hostname=$host"
    echo "project_dir=$abs_project"
    echo "user=$(id -un 2>/dev/null || true)"
    echo "shell=${SHELL:-}"
  } >"$meta_dir/snapshot_info.txt"

  {
    safe_run "uname -a" uname -a
    safe_run "os-release" bash -lc 'cat /etc/os-release 2>/dev/null || true'
    safe_run "idf.py --version" idf.py --version
    safe_run "python3 --version" python3 --version
    safe_run "python --version" python --version
    safe_run "cmake --version" cmake --version
    safe_run "ninja --version" ninja --version
    safe_run "idf_tools.py --version" idf_tools.py --version
    safe_run "IDF environment variables" bash -lc 'env | rg "^IDF_" || true'
    safe_run "PATH" bash -lc 'echo "$PATH"'
    safe_run "toolchain xtensa-esp32-elf-gcc" bash -lc 'command -v xtensa-esp32-elf-gcc && xtensa-esp32-elf-gcc --version | head -n 1'
    safe_run "toolchain xtensa-esp32s2-elf-gcc" bash -lc 'command -v xtensa-esp32s2-elf-gcc && xtensa-esp32s2-elf-gcc --version | head -n 1'
    safe_run "toolchain xtensa-esp32s3-elf-gcc" bash -lc 'command -v xtensa-esp32s3-elf-gcc && xtensa-esp32s3-elf-gcc --version | head -n 1'
    safe_run "toolchain riscv32-esp-elf-gcc" bash -lc 'command -v riscv32-esp-elf-gcc && riscv32-esp-elf-gcc --version | head -n 1'
  } >"$meta_dir/environment.txt"

  {
    echo "### project git status"
    (cd "$abs_project" && git rev-parse --is-inside-work-tree >/dev/null 2>&1 && {
      git rev-parse HEAD
      git status --short
      git remote -v
    }) || echo "not a git repository"
    echo

    if [ -n "${IDF_PATH:-}" ]; then
      echo "### IDF_PATH=$IDF_PATH"
      (cd "$IDF_PATH" && git rev-parse --is-inside-work-tree >/dev/null 2>&1 && {
        git rev-parse HEAD
        git describe --tags --always
        git status --short
      }) || echo "IDF_PATH is not a git repository"
    else
      echo "IDF_PATH is not set"
    fi
  } >"$meta_dir/git_info.txt"

  local rel
  for rel in \
    sdkconfig \
    sdkconfig.defaults \
    sdkconfig.old \
    sdkconfig.bak \
    dependencies.lock \
    partitions.csv \
    CMakeLists.txt \
    main/idf_component.yml \
    build/config/sdkconfig.h \
    build/config/sdkconfig.cmake \
    build/project_description.json \
    build/flasher_args.json \
    build/config.env
  do
    copy_if_exists "$abs_project/$rel" "$files_dir/$rel"
  done

  if [ -d "$abs_project/managed_components" ]; then
    mkdir -p "$files_dir"
    (cd "$abs_project" && find managed_components -maxdepth 2 -mindepth 1 -type d | sort) >"$meta_dir/managed_components_dirs.txt" || true
  fi

  {
    echo "# sha256 files in snapshot"
    (cd "$snap_dir" && find . -type f ! -name "hashes.sha256" | sort | while read -r f; do
      sha256sum "$f"
    done)
  } >"$snap_dir/hashes.sha256"

  echo "Snapshot created: $snap_dir"
}

diff_file() {
  local left="$1"
  local right="$2"
  local title="$3"
  local report="$4"
  echo "## $title" >>"$report"
  if [ -f "$left" ] && [ -f "$right" ]; then
    if diff -u "$left" "$right" >>"$report"; then
      echo "(no differences)" >>"$report"
    fi
  elif [ -f "$left" ] || [ -f "$right" ]; then
    echo "(missing on one side)" >>"$report"
    echo "left:  $left" >>"$report"
    echo "right: $right" >>"$report"
  else
    echo "(missing on both sides)" >>"$report"
  fi
  echo >>"$report"
}

compare_snapshot() {
  [ $# -ge 2 ] || die "compare needs SNAPSHOT_A SNAPSHOT_B"
  local snap_a="$1"
  local snap_b="$2"
  shift 2

  local out_report=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --out)
        out_report="$2"
        shift 2
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        die "Unknown option for compare: $1"
        ;;
    esac
  done

  [ -d "$snap_a" ] || die "Snapshot not found: $snap_a"
  [ -d "$snap_b" ] || die "Snapshot not found: $snap_b"

  if [ -z "$out_report" ]; then
    out_report="./idf_diff_report_$(ts_now).txt"
  fi

  : >"$out_report"
  {
    echo "ESP-IDF Snapshot Diff Report"
    echo "generated_at: $(date -Iseconds)"
    echo "snapshot_a: $(cd "$snap_a" && pwd)"
    echo "snapshot_b: $(cd "$snap_b" && pwd)"
    echo
  } >>"$out_report"

  diff_file "$snap_a/meta/snapshot_info.txt" "$snap_b/meta/snapshot_info.txt" "Snapshot Info" "$out_report"
  diff_file "$snap_a/meta/environment.txt" "$snap_b/meta/environment.txt" "Environment" "$out_report"
  diff_file "$snap_a/meta/git_info.txt" "$snap_b/meta/git_info.txt" "Git Info" "$out_report"
  diff_file "$snap_a/meta/managed_components_dirs.txt" "$snap_b/meta/managed_components_dirs.txt" "Managed Components Dirs" "$out_report"

  local rel
  for rel in \
    sdkconfig \
    sdkconfig.defaults \
    dependencies.lock \
    partitions.csv \
    CMakeLists.txt \
    main/idf_component.yml \
    build/config/sdkconfig.h \
    build/config/sdkconfig.cmake \
    build/project_description.json \
    build/flasher_args.json \
    build/config.env
  do
    diff_file "$snap_a/files/$rel" "$snap_b/files/$rel" "File: $rel" "$out_report"
  done

  echo "Report generated: $out_report"
}

main() {
  [ $# -gt 0 ] || {
    usage
    exit 1
  }

  local cmd="$1"
  shift
  case "$cmd" in
    collect)
      collect_snapshot "$@"
      ;;
    compare)
      compare_snapshot "$@"
      ;;
    -h|--help|help)
      usage
      ;;
    *)
      die "Unknown command: $cmd"
      ;;
  esac
}

main "$@"
