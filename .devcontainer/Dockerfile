FROM devkitpro/devkita64:20260219@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282 AS base

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
 && echo "deb [signed-by=/etc/apt/keyrings/llvm.gpg] https://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-21 main" \
    > /etc/apt/sources.list.d/llvm-21.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
    bash \
    bison \
    build-essential \
    ca-certificates \
    ccache \
    clang-21 \
    clang-format-21 \
    clang-tidy-21 \
    cppcheck \
    curl \
    file \
    flex \
    gdb-multiarch \
    git \
    libclang-rt-21-dev \
    jq \
    openssh-client \
    python3 \
    python3-pip \
    ripgrep \
    tar \
    unzip \
    vim \
    xxd \
    zip \
 && apt-get -y autoremove --purge \
 && apt-get -y clean \
 && ln -sf /usr/bin/clang-21 /usr/local/bin/clang \
 && ln -sf /usr/bin/clang++-21 /usr/local/bin/clang++ \
 && ln -sf /usr/bin/clang-format-21 /usr/local/bin/clang-format \
 && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

RUN adduser --disabled-password "$CUSER" \
 && usermod -aG sudo "$CUSER" \
 && sed -ri '/^%sudo[[:space:]]+ALL=/c\%sudo ALL=(ALL:ALL) NOPASSWD: ALL' /etc/sudoers \
 && mkdir -p "$TOOLCHAIN_PREFIX" /src

ENV NX_REVERSE_TOOLCHAIN_PREFIX=${TOOLCHAIN_PREFIX}
ENV PATH=/opt/devkitpro/devkitA64/bin:${PATH}

FROM base AS ghidra-toolchain

WORKDIR /src
COPY ./toolchain/install.sh /src/toolchain/install.sh
COPY ./toolchain/patches/ /src/toolchain/patches/
RUN toolchain/install.sh \
 && rm -rf \
    "${TOOLCHAIN_PREFIX}/cache" \
    "${TOOLCHAIN_PREFIX}/src" \
    "${TOOLCHAIN_PREFIX}/cargo/registry" \
    "${TOOLCHAIN_PREFIX}/cargo/git"

FROM base AS devcontainer

ARG CUSER=devcontainer
ARG TOOLCHAIN_PREFIX=/opt/toolchain
ARG GO_VERSION=1.26.5
ARG GO_SHA256=fe4789e92b1f33358680864bbe8704289e7bb5fc207d80623c308935bd696d49

COPY --from=ghidra-toolchain ${TOOLCHAIN_PREFIX} ${TOOLCHAIN_PREFIX}

RUN curl -fsSLo /tmp/go.tar.gz "https://go.dev/dl/go${GO_VERSION}.linux-arm64.tar.gz" \
 && echo "${GO_SHA256}  /tmp/go.tar.gz" | sha256sum --check --status \
 && tar -C /usr/local -xzf /tmp/go.tar.gz \
 && rm -f /tmp/go.tar.gz \
 && install -d -o "$CUSER" -g "$CUSER" /go

ENV GOROOT=/usr/local/go
ENV GOPATH=/go
ENV PATH=${GOROOT}/bin:${GOPATH}/bin:${PATH}

WORKDIR /src
