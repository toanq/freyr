#!/bin/bash
# Starts minicom on the ESP32's serial console and mirrors everything it
# prints to a log file at the same time (minicom's own -C capture option),
# so a second terminal can `tail -f` that file live for as long as minicom
# stays open -- neither side blocks or competes with the other for the port.
#
# Usage: scripts/monitor_esp32.sh [port] [baud]
#   port defaults to /dev/ttyACM0 (this board's USB-Serial-JTAG), baud to 115200.

set -euo pipefail

PORT="${1:-/dev/ttyACM0}"
BAUD="${2:-115200}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="$SCRIPT_DIR/../logs"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/esp32-$(date +%Y%m%d-%H%M%S).log"

echo "Monitoring $PORT @ $BAUD baud"
echo "Logging to $LOG_FILE"
echo "Tail it live from another terminal with:"
echo "  tail -f \"$LOG_FILE\""
echo

exec minicom -b "$BAUD" -D "$PORT" -C "$LOG_FILE"
