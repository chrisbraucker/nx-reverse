#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

PATCH_DIR="${NX_REVERSE_PATCH_DIR:-$SCRIPT_DIR/patches}"
TOOLCHAIN_PREFIX="${NX_REVERSE_TOOLCHAIN_PREFIX:-/opt/toolchain}"

GHIDRA_VERSION="12.1.2"
GHIDRA_BUILD="20260605"
GHIDRA_ZIP="ghidra_${GHIDRA_VERSION}_PUBLIC_${GHIDRA_BUILD}.zip"
GHIDRA_URL="https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_${GHIDRA_VERSION}_build/${GHIDRA_ZIP}"

GHIDRA_DIR="${NX_REVERSE_GHIDRA_DIR:-${TOOLCHAIN_PREFIX}/ghidra}"
JDK_DIR="${NX_REVERSE_JDK_DIR:-${TOOLCHAIN_PREFIX}/jdk}"
CARGO_HOME="${NX_REVERSE_CARGO_HOME:-${TOOLCHAIN_PREFIX}/cargo}"
RUSTUP_HOME="${NX_REVERSE_RUSTUP_HOME:-${TOOLCHAIN_PREFIX}/rustup}"
BIN_DIR="${NX_REVERSE_BIN_DIR:-${TOOLCHAIN_PREFIX}/bin}"
CACHE_DIR="${NX_REVERSE_CACHE_DIR:-${TOOLCHAIN_PREFIX}/cache}"
SRC_DIR="${NX_REVERSE_SRC_DIR:-${TOOLCHAIN_PREFIX}/src}"

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "missing required command: $1" >&2
        exit 1
    }
}

for cmd in bash curl git python3 unzip tar; do
    need_cmd "$cmd"
done

ensure_writable_prefix() {
    local probe_file
    if ! mkdir -p "${TOOLCHAIN_PREFIX}" 2>/dev/null; then
        cat >&2 <<EOF
toolchain prefix is not writable: ${TOOLCHAIN_PREFIX}

Either rerun with elevated privileges or override the prefix, for example:
  NX_REVERSE_TOOLCHAIN_PREFIX="${ROOT_DIR}/workspace/toolchain" \\
    bash .devcontainer/toolchain/install.sh
EOF
        exit 1
    fi
    probe_file="${TOOLCHAIN_PREFIX}/.write-test"
    if ! : >"${probe_file}" 2>/dev/null; then
        cat >&2 <<EOF
toolchain prefix is not writable: ${TOOLCHAIN_PREFIX}

Either rerun with elevated privileges or override the prefix, for example:
  NX_REVERSE_TOOLCHAIN_PREFIX="${ROOT_DIR}/workspace/toolchain" \\
    bash .devcontainer/toolchain/install.sh
EOF
        exit 1
    fi
    rm -f "${probe_file}"
}

download_jdk() {
    local json url tgz extract_dir jdk_extract
    json="$(curl -fsSL -H 'User-Agent: nx-reverse' \
        'https://api.adoptium.net/v3/assets/latest/21/hotspot?architecture=aarch64&heap_size=normal&image_type=jdk&jvm_impl=hotspot&os=linux&vendor=eclipse')"
    url="$(printf '%s' "$json" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d[0]["binary"]["package"]["link"])')"
    tgz="${CACHE_DIR}/${url##*/}"
    [[ -f "$tgz" ]] || curl -L --fail -o "$tgz" "$url"

    extract_dir="${CACHE_DIR}/jdk-extract"
    rm -rf "${extract_dir}" "${JDK_DIR}"
    mkdir -p "${extract_dir}"
    tar -xzf "$tgz" -C "${extract_dir}"
    jdk_extract="$(find "${extract_dir}" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
    mv "${jdk_extract}" "${JDK_DIR}"
    rm -rf "${extract_dir}"
}

install_ghidra() {
    local zip_path extract_dir extracted_root
    zip_path="${CACHE_DIR}/${GHIDRA_ZIP}"
    [[ -f "$zip_path" ]] || curl -L --fail -o "$zip_path" "$GHIDRA_URL"

    extract_dir="${CACHE_DIR}/ghidra-extract"
    rm -rf "${extract_dir}" "${GHIDRA_DIR}"
    mkdir -p "${extract_dir}"
    unzip -q -o "$zip_path" -d "${extract_dir}"
    extracted_root="$(find "${extract_dir}" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
    mv "${extracted_root}" "${GHIDRA_DIR}"
    rm -rf "${extract_dir}"
}

apply_patch_if_needed() {
    local repo="$1"
    local patch_file="$2"
    if git -C "$repo" apply --check "$patch_file" >/dev/null 2>&1; then
        git -C "$repo" apply "$patch_file"
    elif git -C "$repo" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
        :
    else
        echo "patch does not apply cleanly: $patch_file" >&2
        exit 1
    fi
}

install_rustup() {
    export CARGO_HOME RUSTUP_HOME
    if [[ ! -x "${CARGO_HOME}/bin/rustup" ]]; then
        curl https://sh.rustup.rs -sSf | sh -s -- -y --profile minimal --no-modify-path
    fi
    # shellcheck disable=SC1090
    . "${CARGO_HOME}/env"
    rustup toolchain install stable --profile minimal >/dev/null
    rustup default stable >/dev/null
}

build_ghidra_cli() {
    local repo="${SRC_DIR}/ghidra-cli"
    rm -rf "$repo"
    git clone --depth 1 https://github.com/akiselev/ghidra-cli.git "$repo" >/dev/null
    apply_patch_if_needed "$repo" "${PATCH_DIR}/ghidra-cli-g12.patch"

    # shellcheck disable=SC1090
    . "${CARGO_HOME}/env"
    CARGO_INCREMENTAL=0 cargo install --path "$repo" --locked -j1
    if [[ -x "${CARGO_HOME}/bin/ghidra" ]]; then
        rm -f "${CARGO_HOME}/bin/ghidra-cli-bin"
        mv -f "${CARGO_HOME}/bin/ghidra" "${CARGO_HOME}/bin/ghidra-cli-bin"
    fi
}

build_switch_loader() {
    local repo="${SRC_DIR}/ghidra-switch-loader"
    rm -rf "$repo"
    git clone --depth 1 https://github.com/adubbz/ghidra-switch-loader.git "$repo" >/dev/null
    apply_patch_if_needed "$repo" "${PATCH_DIR}/ghidra-switch-loader-g12.patch"

    (
        cd "$repo"
        export JAVA_HOME="$JDK_DIR"
        export GHIDRA_INSTALL_DIR="$GHIDRA_DIR"
        ./gradlew >/dev/null
    )

    mkdir -p "${GHIDRA_DIR}/Ghidra/Extensions"
    unzip -q -o "${repo}/dist/"*.zip -d "${GHIDRA_DIR}/Ghidra/Extensions"
}

build_decompiler_natives() {
    (
        cd "${GHIDRA_DIR}/Ghidra/Features/Decompiler"
        export JAVA_HOME="${JDK_DIR}"
        "${GHIDRA_DIR}/support/gradle/gradlew" \
            --no-daemon \
            -Dorg.gradle.workers.max=1 \
            buildNatives_linux_arm_64 >/dev/null
    )
}

write_wrappers() {
    cat > "${BIN_DIR}/ghidra" <<EOF
#!/usr/bin/env bash
set -euo pipefail
export JAVA_HOME="${JDK_DIR}"
export CARGO_HOME="${CARGO_HOME}"
export RUSTUP_HOME="${RUSTUP_HOME}"
export PATH="${BIN_DIR}:${CARGO_HOME}/bin:\${JAVA_HOME}/bin:\${PATH}"
exec "${GHIDRA_DIR}/ghidraRun" "\$@"
EOF

    cat > "${BIN_DIR}/ghidra-cli" <<EOF
#!/usr/bin/env bash
set -euo pipefail
export JAVA_HOME="${JDK_DIR}"
export CARGO_HOME="${CARGO_HOME}"
export RUSTUP_HOME="${RUSTUP_HOME}"
export PATH="${BIN_DIR}:${CARGO_HOME}/bin:\${JAVA_HOME}/bin:\${PATH}"
exec "${CARGO_HOME}/bin/ghidra-cli-bin" "\$@"
EOF

    cat > "${BIN_DIR}/analyzeHeadless" <<EOF
#!/usr/bin/env bash
set -euo pipefail
export JAVA_HOME="${JDK_DIR}"
export CARGO_HOME="${CARGO_HOME}"
export RUSTUP_HOME="${RUSTUP_HOME}"
export PATH="${BIN_DIR}:${CARGO_HOME}/bin:\${JAVA_HOME}/bin:\${PATH}"
exec "${GHIDRA_DIR}/support/analyzeHeadless" "\$@"
EOF

    chmod +x "${BIN_DIR}/ghidra" "${BIN_DIR}/ghidra-cli" "${BIN_DIR}/analyzeHeadless"
    ln -sf "${JDK_DIR}/bin/java" "${BIN_DIR}/java"
    ln -sf "${JDK_DIR}/bin/javac" "${BIN_DIR}/javac"
}

ensure_writable_prefix
mkdir -p \
    "${BIN_DIR}" \
    "${CARGO_HOME}" \
    "${RUSTUP_HOME}" \
    "${CACHE_DIR}" \
    "${SRC_DIR}"
install_ghidra
download_jdk
install_rustup
build_ghidra_cli
build_switch_loader
build_decompiler_natives
write_wrappers

echo "Installed:"
echo "  Prefix:       ${TOOLCHAIN_PREFIX}"
echo "  Ghidra:       ${GHIDRA_DIR}"
echo "  JDK:          ${JDK_DIR}"
echo "  CARGO_HOME:   ${CARGO_HOME}"
echo "  RUSTUP_HOME:  ${RUSTUP_HOME}"
echo "  Cache:        ${CACHE_DIR}"
echo "  Sources:      ${SRC_DIR}"
echo
echo "Next:"
echo "  bash .devcontainer/toolchain/init-workspace.sh"
echo "  source workspace/toolchain-env.sh"
echo "  ghidra-cli doctor"
