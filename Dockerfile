# The whole toolchain: JieLi's clang for the FM-1 (x86-64 Linux only), clang/lld for the
# WebAssembly emulator, python for the packager, and the formatters. Nothing else is needed.
# It also has JieLi's AC79 SDK, for fm1-base / felucca-bsp builds (foma doesn't use it).
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

# JieLi's AC79 SDK (Apache-2.0, its LICENSE kept), pinned to AC79NN_SDK_V1.2.1_2023-12-13; JieLi took
# their repo down, so it comes from a mirror (the commit pins the content)
ARG AC79_SDK_URL=https://github.com/amitv87/fw-AC79_AIoT_SDK
ARG AC79_SDK_REV=d179b4484759423312073f5fbb232501aa491047
RUN apt-get update \
  && apt-get install -y --no-install-recommends git \
  && git init -q /opt/jieli/ac79-sdk \
  && cd /opt/jieli/ac79-sdk \
  && git fetch -q --depth 1 --filter=blob:none "$AC79_SDK_URL" "$AC79_SDK_REV" \
  && git sparse-checkout set --no-cone /LICENSE /README.md /cpu/wl82/ /include_lib/ /apps/include_lib/ \
  && git checkout -q FETCH_HEAD \
  && rm -rf .git \
  && test -f cpu/wl82/tools/uboot.boot \
  && apt-get purge -y git && apt-get autoremove -y && rm -rf /var/lib/apt/lists/*

ENV JIELI_TOOLCHAIN=/opt/jieli/toolchain \
  AC79_SDK=/opt/jieli/ac79-sdk \
  NPM_CONFIG_CACHE=/tmp/npm \
  HOME=/tmp \
  PYTHONUNBUFFERED=1
WORKDIR /work
