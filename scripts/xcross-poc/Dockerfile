FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    PATH="/opt/flutter/bin:/root/.local/bin:/opt/swift/usr/bin:${PATH}"

RUN apt-get update && apt-get install -y --no-install-recommends \
    curl git unzip xz-utils ca-certificates clang lld llvm \
    python3 python3-venv pipx usbmuxd usbutils libimobiledevice-utils \
    binutils libcurl4-openssl-dev libxml2-dev libz3-dev libncurses-dev \
    sudo \
    && rm -rf /var/lib/apt/lists/*

# Swift toolchain, straight from swift.org (swiftly's installer does not
# recognize Ubuntu 24.04.x point releases yet; the tarball works fine)
RUN curl -fsSL -o /tmp/swift.tar.gz \
    https://download.swift.org/swift-6.4.2-release/ubuntu2404/swift-6.4.2-RELEASE/swift-6.4.2-RELEASE-ubuntu24.04.tar.gz \
    && mkdir -p /opt/swift \
    && tar -xzf /tmp/swift.tar.gz -C /opt/swift --strip-components=1 \
    && rm /tmp/swift.tar.gz

# Flutter SDK, pinned to 3.47.6: the newest release xcross_gen_snapshot
# (the Linux/Windows iOS AOT compiler xcross needs) has published for.
RUN git clone --depth 1 -b 3.47.6 https://github.com/flutter/flutter.git /opt/flutter

# xcross
RUN curl -fsSL https://raw.githubusercontent.com/arxdeus/xcross/main/install.sh | sh

RUN pipx ensurepath

WORKDIR /work
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]
