# Build/run environment for the click-to-talk simulator -- this host has no
# sudo access to apt-get install libmosquitto-dev/libncurses-dev directly,
# so the build happens in a throwaway container instead. Joins the compose
# stack's network at run time to reach `mosquitto`/`server` by service name.
FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential libmosquitto-dev libncurses-dev python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /sim
