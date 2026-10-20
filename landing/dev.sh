#!/bin/bash
# Dev runner for the landing frontend (vite). The CMS API is served by the
# Synora server (/cms) -- run it separately; vite proxies /api and /uploads to
# SYNORA_SERVER_URL (default http://127.0.0.1:5080).
# Ctrl-R restarts, Ctrl-X / Ctrl-C exits.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

FRONTEND_PID=""

start() {
  echo "[dev] starting frontend..."
  setsid npm run dev &
  FRONTEND_PID=$!
}

stop() {
  echo "[dev] stopping..."
  [ -n "$FRONTEND_PID" ] && kill -TERM -- -"$FRONTEND_PID" 2>/dev/null
  [ -n "$FRONTEND_PID" ] && wait "$FRONTEND_PID" 2>/dev/null
  [ -n "$FRONTEND_PID" ] && kill -KILL -- -"$FRONTEND_PID" 2>/dev/null
}

cleanup() {
  stop
  stty "$OLD_STTY" 2>/dev/null
  echo "[dev] bye"
  exit 0
}

OLD_STTY=$(stty -g)
trap cleanup EXIT
stty -icanon -echo -isig min 1 time 0

start
echo "[dev] Ctrl-R restart, Ctrl-X / Ctrl-C exit"

while true; do
  char=$(dd bs=1 count=1 2>/dev/null)
  case "$char" in
    $'\x12') # Ctrl-R
      stop
      start
      echo "[dev] restarted. Ctrl-R restart, Ctrl-X / Ctrl-C exit"
      ;;
    $'\x18' | $'\x03') # Ctrl-X / Ctrl-C
      break
      ;;
  esac
done
