# The whole toolchain: JieLi's clang for the FM-1 (x86-64 Linux only), clang/lld for the
# WebAssembly emulator, python for the packager, and the formatters. Nothing else is needed.
FROM --platform=linux/amd64 debian:bookworm-slim

RUN apt-get update \
  && apt-get install -y --no-install-recommends ca-certificates curl xz-utils make python3 python3-pip clang lld clang-format nodejs npm \
  && pip3 install --no-cache-dir --break-system-packages ruff \
  && rm -rf /var/lib/apt/lists/*

# JieLi's Linux toolchain (clang 4.0.1 for pi32v2)
ARG JIELI_TOOLCHAIN_URL=https://pkgman.jieliapp.com/s/linux-toolchain
RUN mkdir -p /opt/jieli \
  && curl -fL --retry 3 "$JIELI_TOOLCHAIN_URL" | tar -xJ -C /opt/jieli \
  && ln -s "$(ls -d /opt/jieli/jieli-linux-toolchains-* | sort | tail -1)" /opt/jieli/toolchain \
  && /opt/jieli/toolchain/pi32v2/bin/clang --version | head -1

ENV JIELI_TOOLCHAIN=/opt/jieli/toolchain \
  NPM_CONFIG_CACHE=/tmp/npm \
  HOME=/tmp \
  PYTHONUNBUFFERED=1
WORKDIR /work
