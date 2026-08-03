#!/bin/sh
# Builds the simulator's docker image (once; cached after) and runs it
# interactively, joined to the docker-compose stack's network so it can
# reach `mosquitto`/`server` by service name. Requires the stack to be up
# (docker compose up -d from the repo root) first.
#
# Usage: ./run.sh [device-id]
#   No device-id -- activates a fresh simulated device on every run.
#   With device-id -- reuses an existing one (skips the activation call).
set -e
cd "$(dirname "$0")"

docker build -t synora-sim .

NETWORK=$(docker network ls --format '{{.Name}}' | grep -m1 synora_default || echo synora_default)

docker run -it --rm \
    --network "$NETWORK" \
    -v "$(pwd)/..:/project:ro" \
    -w /sim \
    synora-sim \
    sh -c "g++ -std=c++17 -O0 -g \
        -I/project/managed_components/espressif__cjson/cJSON \
        -o /tmp/sim /project/sim/main.cc /project/sim/host_mqtt.cc \
        /project/main/boards/synora/voice_messages.cc \
        /project/managed_components/espressif__cjson/cJSON/cJSON.c \
        -lmosquitto -lncurses -lpthread \
      && /tmp/sim $*"
