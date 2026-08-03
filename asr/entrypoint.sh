#!/bin/sh
# Converts vinai/PhoWhisper-small (a standard HuggingFace Whisper checkpoint,
# pre-downloaded on the HOST into ./asr/model-src -- see download_model.py --
# and bind-mounted read-only at /model-src) to CTranslate2 format on first
# startup, caching the result in PHOWHISPER_MODEL_DIR (a separate volume) so
# restarts skip this. Converting from the local bind-mount rather than
# letting ct2-transformers-converter fetch the checkpoint itself: that
# in-container download stalled completely partway through (a silent hang,
# not just slow), matching this environment's known CDN-egress issue with
# large files from inside containers.
set -e

MODEL_DIR="${PHOWHISPER_MODEL_DIR:-/models/phowhisper-small-ct2}"
MODEL_SRC="${PHOWHISPER_MODEL_SRC:-/model-src}"

# asr/model-src is gitignored (host-downloaded, not committed -- see
# download_model.py's own doc comment on why this can't just happen
# in-container). A fresh clone won't have it yet, which would otherwise fail
# deep inside ct2-transformers-converter with a confusing "no config.json"
# error -- fail fast here instead with the actual fix.
if [ ! -f "$MODEL_DIR/model.bin" ] && [ ! -f "$MODEL_SRC/config.json" ]; then
    echo "ERROR: $MODEL_SRC has no model checkpoint (missing config.json)." >&2
    echo "Run 'python3 asr/download_model.py' on the HOST first, then restart this container." >&2
    exit 1
fi

if [ ! -f "$MODEL_DIR/model.bin" ]; then
    echo "No cached CTranslate2 model at $MODEL_DIR -- converting from $MODEL_SRC (one-time, several minutes)..."
    mkdir -p "$MODEL_DIR"
    ct2-transformers-converter --model "$MODEL_SRC" --output_dir "$MODEL_DIR" --quantization int8 --force
    echo "Conversion done."
else
    echo "Using cached CTranslate2 model at $MODEL_DIR"
fi

exec uvicorn app:app --host 0.0.0.0 --port 8000
