FROM devkitpro/devkita64:latest

ARG CUSER=devcontainer
ARG TOOLCHAIN_PREFIX=/opt/toolchain

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    gpg \
 && install -d -m 0755 /etc/apt/keyrings \
 && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
    | gpg --dearmor -o /etc/apt/keyrings/llvm.gpg \
 && echo "deb [signed-by=/etc/apt/keyrings/llvm.gpg] https://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-16 main" \
    > /etc/apt/sources.list.d/llvm-16.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
    bash \
    bison \
    build-essential \
    ca-certificates \
    ccache \
    clang-16 \
    clang-format-16 \
    cppcheck \
    curl \
    file \
    flex \
    git \
    libclang-rt-16-dev \
    jq \
    openssh-client \
    python3 \
    python3-pip \
    ripgrep \
    tar \
    unzip \
    xxd \
    zip \
 && apt-get -y autoremove --purge \
 && apt-get -y clean \
 && ln -sf /usr/bin/clang-16 /usr/local/bin/clang \
 && ln -sf /usr/bin/clang++-16 /usr/local/bin/clang++ \
 && ln -sf /usr/bin/clang-format-16 /usr/local/bin/clang-format \
 && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

RUN adduser --disabled-password "$CUSER" \
 && usermod -aG sudo "$CUSER" \
 && sed -ri '/^%sudo[[:space:]]+ALL=/c\%sudo ALL=(ALL:ALL) NOPASSWD: ALL' /etc/sudoers \
 && mkdir -p "$TOOLCHAIN_PREFIX" /src \
 && chown -R "$CUSER:$CUSER" "$TOOLCHAIN_PREFIX" /src

USER ${CUSER}
ENV HOME=/home/${CUSER}
ENV NX_REVERSE_TOOLCHAIN_PREFIX=${TOOLCHAIN_PREFIX}

COPY ./toolchain/ /src/toolchain/

WORKDIR /src
RUN toolchain/install.sh \
 && rm -rf \
    "${TOOLCHAIN_PREFIX}/cache" \
    "${TOOLCHAIN_PREFIX}/src" \
    "${TOOLCHAIN_PREFIX}/cargo/registry" \
    "${TOOLCHAIN_PREFIX}/cargo/git"
