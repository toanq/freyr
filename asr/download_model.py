#!/usr/bin/env python3
"""One-off host-side downloader for vinai/PhoWhisper-small, with a socket
timeout + resume-on-stall retry loop. Run on the HOST, not inside the asr
container -- the in-container download of this same checkpoint stalled
completely (silent hang, not just slow) after ~67MB, matching this
environment's known CDN-egress issue (see feedback_docker_network_egress
memory: some CDNs hang forever from inside containers). Downloading here and
bind-mounting the result into the container (see docker-compose.yml) avoids
that entirely.
"""
import os
import socket
import sys
import time
import urllib.error
import urllib.request

FILES = [
    ".gitattributes",
    "README.md",
    "added_tokens.json",
    "config.json",
    "generation_config.json",
    "merges.txt",
    "normalizer.json",
    "preprocessor_config.json",
    "pytorch_model.bin",
    "special_tokens_map.json",
    "tokenizer.json",
    "tokenizer_config.json",
    "vocab.json",
]

BASE_URL = "https://huggingface.co/vinai/PhoWhisper-small/resolve/main/"
OUT_DIR = os.path.join(os.path.dirname(__file__), "model-src")
STALL_TIMEOUT_S = 30  # a read() blocking longer than this counts as a stall
CHUNK_SIZE = 1024 * 256


def get_expected_size(url: str) -> int:
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": "synora-asr-model-downloader"})
    resp = urllib.request.urlopen(req, timeout=STALL_TIMEOUT_S)
    return int(resp.headers.get("Content-Length", 0))


def download_file(filename: str) -> None:
    out_path = os.path.join(OUT_DIR, filename)
    if os.path.exists(out_path + ".done"):
        print(f"{filename}: already done, skipping")
        return

    url = BASE_URL + filename
    expected_size = get_expected_size(url)
    attempt = 0
    while True:
        attempt += 1
        existing = os.path.getsize(out_path) if os.path.exists(out_path) else 0
        if expected_size and existing == expected_size:
            break  # A previous attempt actually finished; just missing the marker.
        headers = {"User-Agent": "synora-asr-model-downloader"}
        if existing:
            headers["Range"] = f"bytes={existing}-"

        try:
            req = urllib.request.Request(url, headers=headers)
            resp = urllib.request.urlopen(req, timeout=STALL_TIMEOUT_S)
            mode = "ab" if existing else "wb"
            total = existing
            last_report = time.time()
            with open(out_path, mode) as f:
                while True:
                    chunk = resp.read(CHUNK_SIZE)
                    if not chunk:
                        break
                    f.write(chunk)
                    total += len(chunk)
                    if time.time() - last_report > 10:
                        print(f"{filename}: {total / 1024 / 1024:.1f} / {expected_size / 1024 / 1024:.1f} MB")
                        last_report = time.time()
        except (urllib.error.URLError, socket.timeout, ConnectionError, TimeoutError) as e:
            print(f"{filename}: attempt {attempt} failed/stalled ({e}), retrying from byte {existing}...")
            time.sleep(2)
            continue

        final_size = os.path.getsize(out_path)
        if expected_size and final_size != expected_size:
            # The server closed the connection cleanly but short of the full
            # file (what actually happened the first time this ran) -- not
            # an exception, so this check is the only thing that catches it.
            print(f"{filename}: got {final_size} bytes, expected {expected_size} -- retrying from byte {final_size}...")
            time.sleep(2)
            continue
        break

    open(out_path + ".done", "w").close()
    print(f"{filename}: done ({os.path.getsize(out_path) / 1024 / 1024:.1f} MB)")


if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    targets = sys.argv[1:] if len(sys.argv) > 1 else FILES
    for name in targets:
        download_file(name)
    print("All files downloaded.")
