#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

WORKSPACE_FOLDER="${WORKSPACE_FOLDER:-${ROOT_DIR}}"
WORKSPACE_ROOT="${WORKSPACE_FOLDER}/workspace"
CONFIG_ROOT="${WORKSPACE_ROOT}/config"
PROJECT_DIR="${WORKSPACE_ROOT}/ghidra/projects"
ENV_SCRIPT="${NX_REVERSE_ENV_SCRIPT:-${WORKSPACE_ROOT}/toolchain-env.sh}"

TOOLCHAIN_PREFIX="${NX_REVERSE_TOOLCHAIN_PREFIX:-/opt/toolchain}"
GHIDRA_DIR="${NX_REVERSE_GHIDRA_DIR:-${TOOLCHAIN_PREFIX}/ghidra}"
JDK_DIR="${NX_REVERSE_JDK_DIR:-${TOOLCHAIN_PREFIX}/jdk}"
CARGO_HOME="${NX_REVERSE_CARGO_HOME:-${TOOLCHAIN_PREFIX}/cargo}"
RUSTUP_HOME="${NX_REVERSE_RUSTUP_HOME:-${TOOLCHAIN_PREFIX}/rustup}"
BIN_DIR="${NX_REVERSE_BIN_DIR:-${TOOLCHAIN_PREFIX}/bin}"
XDG_CONFIG_HOME_DIR="${NX_REVERSE_XDG_CONFIG_HOME:-${CONFIG_ROOT}}"

export JAVA_HOME="${JDK_DIR}"
export CARGO_HOME
export RUSTUP_HOME
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME_DIR}"
export PATH="${BIN_DIR}:${CARGO_HOME}/bin:${PATH}"

mkdir -p "${PROJECT_DIR}" "${CONFIG_ROOT}" "${XDG_CONFIG_HOME_DIR}" "$HOME/.config"

for name in ghidra ghidra-cli; do
    target="${XDG_CONFIG_HOME_DIR}/${name}"
    link="$HOME/.config/${name}"
    mkdir -p "$target"
    if [ -e "$link" ] && [ ! -L "$link" ]; then
        cp -a "$link/." "$target/" 2>/dev/null || true
        rm -rf "$link"
    fi
    ln -sfn "$target" "$link"
done

cat > "${ENV_SCRIPT}" <<EOF
export NX_REVERSE_TOOLCHAIN_PREFIX="${TOOLCHAIN_PREFIX}"
export WORKSPACE_FOLDER="${WORKSPACE_FOLDER}"
export JAVA_HOME="${JDK_DIR}"
export CARGO_HOME="${CARGO_HOME}"
export RUSTUP_HOME="${RUSTUP_HOME}"
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME_DIR}"
export PATH="${BIN_DIR}:${CARGO_HOME}/bin:\${PATH}"
EOF

if command -v ghidra-cli >/dev/null 2>&1; then
    ghidra-cli init >/dev/null 2>&1 || true
    ghidra-cli config set ghidra_install_dir "${GHIDRA_DIR}" >/dev/null 2>&1 || true
    ghidra-cli config set ghidra_project_dir "${PROJECT_DIR}" >/dev/null 2>&1 || true
fi
