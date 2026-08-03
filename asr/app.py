"""ASR sidecar: Vietnamese speech-to-text via faster-whisper + PhoWhisper-small.

One endpoint (POST /transcribe) takes a WAV file and returns its transcript.
The CTranslate2-converted model is produced on first startup (see entrypoint.sh)
and cached in a mounted volume, so subsequent restarts skip the conversion.
"""

import logging
import os
import tempfile

from fastapi import FastAPI, UploadFile
from faster_whisper import WhisperModel

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("asr")

MODEL_DIR = os.environ.get("PHOWHISPER_MODEL_DIR", "/models/phowhisper-small-ct2")
# int8 on CPU: ~4x faster, ~4x less memory than float32, negligible accuracy
# loss for a checkpoint this size -- see faster-whisper's own quantization docs.
COMPUTE_TYPE = os.environ.get("ASR_COMPUTE_TYPE", "int8")

app = FastAPI(title="Synora ASR")
model: WhisperModel | None = None


@app.on_event("startup")
def load_model():
    global model
    logger.info("Loading PhoWhisper-small (CTranslate2) from %s ...", MODEL_DIR)
    model = WhisperModel(MODEL_DIR, device="cpu", compute_type=COMPUTE_TYPE)
    logger.info("Model loaded.")


@app.get("/health")
def health():
    return {"status": "ok", "model_loaded": model is not None}


@app.post("/transcribe")
async def transcribe(file: UploadFile):
    """Accepts a WAV file, returns {"text": "..."}. Always forces Vietnamese
    (no language auto-detect) -- Synora is a Vietnamese-only assistant, and
    skipping detection is also faster."""
    assert model is not None, "Model not loaded yet"

    # faster-whisper's transcribe() wants a file path or file-like object with
    # seek support; the upload's SpooledTemporaryFile usually qualifies, but a
    # real temp file sidesteps any edge case with how it was received.
    with tempfile.NamedTemporaryFile(suffix=".wav") as tmp:
        tmp.write(await file.read())
        tmp.flush()

        segments, _info = model.transcribe(tmp.name, language="vi", beam_size=5)
        text = " ".join(segment.text.strip() for segment in segments).strip()

    logger.info("Transcribed %d bytes -> %r", file.size or 0, text)
    return {"text": text}
