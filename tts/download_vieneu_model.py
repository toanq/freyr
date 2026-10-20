#!/usr/bin/env python3
"""One-off host-side downloader for VieNeu-TTS v3 Turbo's ONNX/CPU model
files, with a retry loop for this environment's known CDN-egress issue (see
feedback_docker_network_egress memory: some downloads hang/stall from
inside containers -- downloading here and bind-mounting the result avoids
that entirely, same reasoning as asr/'s and the previous tts/'s own
downloaders).

Uses huggingface_hub's own hf_hub_download(..., cache_dir=...) rather than
plain HTTP requests (unlike the other download_model.py scripts in this
repo) so the resulting directory is byte-for-byte the same cache layout
hf_hub_download expects to find later -- see app.py, which points
HF_HUB_CACHE at this directory's bind-mount inside the container. Exact
filenames/repos below are read straight from vieneu's own
_v3_turbo_engine/onnx_runtime_lite.py source (_GRAPH_FILES/_CODEC_FILES),
not guessed -- pip install vieneu on the host to inspect it yourself if
this ever needs updating for a new vieneu version.
"""
import os
import shutil
import sys
import time

CACHE_DIR = os.path.join(os.path.dirname(__file__), "vieneu-hf-cache")

BACKBONE_REPO = "pnnbao-ump/VieNeu-TTS-v3-Turbo"
BACKBONE_SUBFOLDER = "onnx_int8"  # int8 quantized graphs -- vieneu's default backend=auto CPU path
BACKBONE_FILES = [
    "vieneu_prefill.onnx",
    "vieneu_decode_step.onnx",
    "vieneu_acoustic_cached.onnx",
    "vieneu_backbone_shared.data",
    "vieneu_v3_heads.npz",
    "config.json",
    "tokenizer.json",
]

CODEC_REPO = "OpenMOSS-Team/MOSS-Audio-Tokenizer-Nano-ONNX"
CODEC_FILES = [
    "moss_audio_tokenizer_decode_full.onnx",
    "moss_audio_tokenizer_decode_shared.data",
    "moss_audio_tokenizer_decode_step.onnx",
    "codec_browser_onnx_meta.json",
    "moss_audio_tokenizer_encode.onnx",
    "moss_audio_tokenizer_encode.data",
]

MAX_ATTEMPTS = 5
RETRY_DELAY_S = 3


def materialize_symlinks(root):
    """Replace every symlink under root with a real copy of its target.

    hf_hub_download's cache layout stores each file's actual bytes under
    blobs/<hash-prefix>/<hash> and symlinks snapshots/<revision>/<filename>
    to it -- normal and harmless for most consumers, but onnxruntime's
    external-data loader rejects it: a multi-file ONNX model (e.g.
    vieneu_prefill.onnx + vieneu_backbone_shared.data) has its files land
    under *different* blob hash-prefix directories, and onnxruntime's
    security check requires external-data references to resolve inside the
    same directory as the main model file -- "External data path escapes
    model directory" (confirmed live). De-symlinking here so the snapshot
    directory holds real, co-located files fixes this; the orphaned blobs/
    directory is left in place (harmless extra disk use, not worth the
    complexity of reference-counted cleanup for a handful of model files).
    """
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            path = os.path.join(dirpath, name)
            if os.path.islink(path):
                target = os.path.realpath(path)
                os.remove(path)
                shutil.copyfile(target, path)
                print(f"materialized {path} (was -> {target})")


def download_all(repo, files, subfolder=None):
    from huggingface_hub import hf_hub_download
    from huggingface_hub.utils import HfHubHTTPError

    for fn in files:
        for attempt in range(1, MAX_ATTEMPTS + 1):
            try:
                path = hf_hub_download(repo, fn, subfolder=subfolder, cache_dir=CACHE_DIR)
                print(f"{repo}/{fn}: done -> {path}")
                break
            except (HfHubHTTPError, OSError) as e:
                # config.json/codec_browser_onnx_meta.json are treated as
                # optional metadata by vieneu's own _fetch() (see
                # onnx_runtime_lite.py) -- a real 404 there isn't fatal.
                if fn.endswith(".json") and attempt == MAX_ATTEMPTS:
                    print(f"{repo}/{fn}: optional file failed after {attempt} attempts ({e}), skipping")
                    break
                if attempt == MAX_ATTEMPTS:
                    print(f"{repo}/{fn}: failed after {attempt} attempts ({e})")
                    raise
                print(f"{repo}/{fn}: attempt {attempt} failed ({e}), retrying...")
                time.sleep(RETRY_DELAY_S)


if __name__ == "__main__":
    try:
        import huggingface_hub  # noqa: F401
    except ImportError:
        print("huggingface_hub not installed -- run: pip install --user huggingface_hub", file=sys.stderr)
        sys.exit(1)

    os.makedirs(CACHE_DIR, exist_ok=True)
    download_all(BACKBONE_REPO, BACKBONE_FILES, subfolder=BACKBONE_SUBFOLDER)
    download_all(CODEC_REPO, CODEC_FILES)
    materialize_symlinks(CACHE_DIR)
    print("All files downloaded.")
