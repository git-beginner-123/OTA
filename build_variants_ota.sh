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
VERSION_SOURCE="unknown"
MODE_RAW="${3:-AUTO}"
MODE="${MODE_RAW^^}"

PUBLIC_OTA_REPO_URL="${PUBLIC_OTA_REPO_URL:-git@github.com:git-beginner-123/OTA.git}"
PUBLIC_OTA_REPO_DIR="${PUBLIC_OTA_REPO_DIR:-/tmp/game_ota_public_repo}"
PUBLIC_OTA_SUBDIR="${PUBLIC_OTA_SUBDIR:-ota}"

PUSH_PRIVATE=0
PUSH_PUBLIC=1

case "${MODE}" in
  ""|AUTO)
    PUSH_PRIVATE=0
    PUSH_PUBLIC=1
    ;;
  PUSH|BOTH)
    PUSH_PRIVATE=1
    PUSH_PUBLIC=1
    ;;
  PUBLIC|OTA)
    PUSH_PRIVATE=0
    PUSH_PUBLIC=1
    ;;
  LOCAL|NOREMOTE)
    PUSH_PRIVATE=0
    PUSH_PUBLIC=0
    ;;
  *)
    echo "[WARN] unknown mode '${MODE_RAW}', fallback to AUTO"
    PUSH_PRIVATE=0
    PUSH_PUBLIC=1
    ;;
esac

detect_version() {
  if [[ -n "${VERSION_OVERRIDE}" ]]; then
    VERSION_SOURCE="override(arg2)"
    echo "${VERSION_OVERRIDE}"
    return 0
  fi
  if command -v git >/dev/null 2>&1; then
    local v
    v="$(git describe --tags --always --dirty 2>/dev/null || true)"
    if [[ -n "$v" ]]; then
      VERSION_SOURCE="git describe --tags --always --dirty"
      echo "$v"
      return 0
    fi
  fi
  VERSION_SOURCE="fallback"
  echo "v0.0.0-local"
}

VERSION="$(detect_version)"
echo "[INFO] VERSION=${VERSION} (source: ${VERSION_SOURCE})"

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
  local -a build_args=()
  if [[ -n "${VERSION_OVERRIDE}" ]]; then
    build_args+=("-DPROJECT_VER=${VERSION_OVERRIDE}")
  fi
  echo "[BUILD] type=${type_id} variant=${variant} dir=${bdir} ver=${VERSION}"
  idf.py -B "${bdir}" -DAPP_VARIANT="${variant}" "${build_args[@]}" build

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

maybe_push_private_repo() {
  if [[ "${PUSH_PRIVATE}" -ne 1 ]]; then
    return 0
  fi

  if ! command -v git >/dev/null 2>&1; then
    echo "[WARN] git not found, skip private push"
    return 0
  fi

  local branch
  branch="$(git branch --show-current 2>/dev/null || true)"
  if [[ -z "${branch}" ]]; then
    echo "[WARN] cannot detect branch, skip private push"
    return 0
  fi

  git add ota
  if git diff --cached --quiet; then
    echo "[PUSH] private repo ota unchanged, nothing to commit"
    return 0
  fi

  git commit -m "ota: update bins (${VERSION})"
  git push origin "${branch}"
  echo "[PUSH] private repo pushed: origin/${branch}"
}

publish_to_public_ota_repo() {
  if [[ "${PUSH_PUBLIC}" -ne 1 ]]; then
    return 0
  fi

  if ! command -v git >/dev/null 2>&1; then
    echo "[WARN] git not found, skip public ota publish"
    return 0
  fi

  local repo_dir="${PUBLIC_OTA_REPO_DIR}"
  local repo_url="${PUBLIC_OTA_REPO_URL}"
  local subdir="${PUBLIC_OTA_SUBDIR}"
  local branch
  local has_head=0
  local remote_branch_exists=0

  echo "[PUBLIC] sync ota/* to ${repo_url} (${repo_dir}/${subdir})"

  if [[ ! -d "${repo_dir}/.git" ]]; then
    rm -rf "${repo_dir}"
    git clone "${repo_url}" "${repo_dir}"
  else
    git -C "${repo_dir}" remote set-url origin "${repo_url}"
  fi

  if git -C "${repo_dir}" rev-parse --verify HEAD >/dev/null 2>&1; then
    has_head=1
  fi

  if [[ "${has_head}" -eq 1 ]]; then
    branch="$(git -C "${repo_dir}" branch --show-current 2>/dev/null || true)"
    if [[ -z "${branch}" ]]; then
      branch="$(git -C "${repo_dir}" symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null | sed 's#^origin/##')"
    fi
    if [[ -z "${branch}" ]]; then
      branch="main"
    fi
    if git -C "${repo_dir}" fetch origin --prune; then
      if git -C "${repo_dir}" show-ref --verify --quiet "refs/remotes/origin/${branch}"; then
        remote_branch_exists=1
      elif git -C "${repo_dir}" show-ref --verify --quiet "refs/remotes/origin/main"; then
        branch="main"
        remote_branch_exists=1
      elif git -C "${repo_dir}" show-ref --verify --quiet "refs/remotes/origin/master"; then
        branch="master"
        remote_branch_exists=1
      else
        echo "[PUBLIC] no remote branch found, will continue with local ${branch}"
      fi
    else
      echo "[PUBLIC] fetch failed, continue with local ${branch}"
    fi
    git -C "${repo_dir}" checkout "${branch}"
    if git -C "${repo_dir}" show-ref --verify --quiet "refs/remotes/origin/${branch}"; then
      git -C "${repo_dir}" merge --ff-only "origin/${branch}"
    fi
  else
    branch="main"
    echo "[PUBLIC] empty repo detected, bootstrap branch ${branch}"
    git -C "${repo_dir}" checkout --orphan "${branch}"
    git -C "${repo_dir}" rm -rf . >/dev/null 2>&1 || true
  fi

  verify_public_remote_head() {
    local repo="$1"
    local br="$2"
    local local_head
    local remote_head
    local_head="$(git -C "${repo}" rev-parse HEAD)"
    remote_head="$(git -C "${repo}" ls-remote --heads origin "${br}" | awk 'NR==1{print $1}')"
    if [[ -z "${remote_head}" ]]; then
      echo "[PUBLIC] verify failed: cannot read origin/${br} head" >&2
      return 1
    fi
    if [[ "${local_head}" != "${remote_head}" ]]; then
      echo "[PUBLIC] verify failed: local ${local_head} != origin/${br} ${remote_head}" >&2
      return 1
    fi
    echo "[PUBLIC] verify ok: origin/${br}=${remote_head}"
  }

  rm -rf "${repo_dir}/${subdir}"
  cp -a ota "${repo_dir}/${subdir}"

  git -C "${repo_dir}" add "${subdir}"
  if git -C "${repo_dir}" diff --cached --quiet; then
    local ahead=0
    echo "[PUBLIC] ota unchanged, nothing to commit"
    if [[ "${remote_branch_exists}" -eq 0 ]]; then
      echo "[PUBLIC] first push for branch ${branch}"
      git -C "${repo_dir}" push -u origin "${branch}"
      verify_public_remote_head "${repo_dir}" "${branch}"
      return 0
    fi
    ahead="$(git -C "${repo_dir}" rev-list --right-only --count "origin/${branch}...${branch}")"
    if [[ "${ahead}" -gt 0 ]]; then
      echo "[PUBLIC] local branch ahead by ${ahead}, push pending commits"
      git -C "${repo_dir}" push origin "${branch}"
    fi
    verify_public_remote_head "${repo_dir}" "${branch}"
    return 0
  fi

  git -C "${repo_dir}" commit -m "ota: update bins (${VERSION})"
  if [[ "${has_head}" -eq 1 && "${remote_branch_exists}" -eq 1 ]]; then
    git -C "${repo_dir}" push origin "${branch}"
  else
    git -C "${repo_dir}" push -u origin "${branch}"
  fi
  verify_public_remote_head "${repo_dir}" "${branch}"
  echo "[PUBLIC] pushed ota to ${repo_url}@${branch}"
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

maybe_push_private_repo
publish_to_public_ota_repo

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
echo "  ./build_variants_ota.sh ALL v1.2.8         # default: publish ota to PUBLIC repo"
echo "  ./build_variants_ota.sh ALL v1.2.8 PUSH    # publish to PUBLIC + private current repo"
echo "  ./build_variants_ota.sh ALL v1.2.8 LOCAL   # no git push, local build only"
echo
echo "Flash hints:"
echo "  ./flash_variant.sh <type|name|ALL> /dev/ttyUSB0 [--no-monitor]"
echo "  type map: 1=GO 2=CHESS 3=DICE 4=GOMOKU"
echo "  examples:"
echo "    ./flash_variant.sh 1 /dev/ttyUSB0"
echo "    ./flash_variant.sh GO /dev/ttyUSB0 --no-monitor"
echo "    ./flash_variant.sh ALL /dev/ttyUSB0 --no-monitor"
echo
echo "Direct idf.py flash:"
echo "  idf.py -B build-go -p /dev/ttyUSB0 flash monitor"
echo "  idf.py -B build-chess -p /dev/ttyUSB0 flash monitor"
echo "  idf.py -B build-dice -p /dev/ttyUSB0 flash monitor"
echo "  idf.py -B build-gomoku -p /dev/ttyUSB0 flash monitor"
