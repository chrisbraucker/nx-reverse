FROM devkitpro/devkita64:latest

ARG CUSER=devcontainer
ARG TOOLCHAIN_PREFIX=/opt/toolchain

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
    bash \
    bison \
    build-essential \
    ca-certificates \
    ccache \
    clang \
    clang-format \
    cppcheck \
    curl \
    file \
    flex \
    git \
    libclang-rt-dev \
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
 && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

RUN adduser --disabled-password "$CUSER" \
 && usermod -aG sudo "$CUSER" \
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
