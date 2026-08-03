"""TTS sidecar: Vietnamese text-to-speech via VieNeu-TTS v3 Turbo (ONNX
backend, torch-free CPU inference) -- 14 built-in preset voices (7 male,
7 female) across three regions and speaking styles, selectable by name, no
reference-audio cloning needed. Replaced the earlier facebook/mms-tts-vie
backend (single-speaker, more robotic, no voice choice) after the user
asked for a more natural-sounding female voice option.

Two endpoints: GET /voices (list presets, for the admin system-config
page's voice picker) and POST /synthesize (text + voice -> WAV file).

Model files are pre-downloaded on the HOST into ./vieneu-hf-cache (see
download_vieneu_model.py -- same in-container CDN-egress workaround as the
asr/ sidecar) and bind-mounted read-only; HF_HUB_CACHE (set in
docker-compose.yml) points vieneu's own huggingface_hub calls at that
bind-mount so no network access happens at container startup.
"""

import io
import logging
import wave
from typing import Any

import soxr
from fastapi import FastAPI
from fastapi.responses import JSONResponse, Response
from pydantic import BaseModel
from vieneu import Vieneu

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("tts")

# The rest of the voice pipeline (LlmVoicePipeline's Opus re-encode, and the
# firmware's SynoraVoiceSession Opus decoder, which is hardcoded to this
# rate) is fixed at 16kHz -- VieNeu-TTS v3 Turbo's native 48kHz output is
# resampled down before returning so that contract doesn't have to change
# just because the TTS backend did. soxr is already a transitive vieneu
# dependency (librosa's default resampler), no new install needed.
OUTPUT_SAMPLE_RATE_HZ = 16000

app = FastAPI(title="Synora TTS")
# Vieneu() is a factory function (returns a BaseVieneuTTS subclass instance
# depending on `mode`), not a type -- can't use it as a type annotation.
tts: Any = None


@app.on_event("startup")
def load_model():
    global tts
    logger.info("Loading VieNeu-TTS v3 Turbo (ONNX/CPU)...")
    tts = Vieneu()  # mode="v3turbo" (default), backend="auto" -> ONNX on CPU
    voices = tts.list_preset_voices()
    logger.info("Model loaded. %d preset voices: %s", len(voices), [v[1] for v in voices])


@app.get("/health")
def health():
    return {"status": "ok", "model_loaded": tts is not None}


@app.get("/voices")
def voices():
    assert tts is not None, "Model not loaded yet"
    return [
        {"id": voice_id, "description": tts.get_preset_voice(voice_id)["description"],
         "gender": tts.get_preset_voice(voice_id)["gender"]}
        for _, voice_id in tts.list_preset_voices()
    ]


class SynthesizeRequest(BaseModel):
    text: str
    voice: str | None = None  # preset voice id, e.g. "Trúc Ly" -- None uses the model's default


@app.post("/synthesize")
def synthesize(request: SynthesizeRequest):
    """Returns a mono 16-bit PCM WAV file at OUTPUT_SAMPLE_RATE_HZ (16000 Hz --
    resampled down from VieNeu-TTS v3 Turbo's native 48000 Hz)."""
    assert tts is not None, "Model not loaded yet"

    text = request.text.strip()
    if not text:
        return Response(status_code=400, content="text must not be empty")

    # Each preset voice has its own natural speaking style (e.g. "doc_truyen"
    # for a storytelling voice) baked into voices_v3_turbo.json -- infer()
    # doesn't apply that automatically from `voice` alone, so it's looked up
    # and passed through explicitly rather than always using the "tu_nhien"
    # default regardless of which voice was chosen.
    style = "tu_nhien"
    if request.voice:
        try:
            style = tts.get_preset_voice(request.voice).get("style", style)
        except ValueError as e:
            return JSONResponse(status_code=400, content={"detail": str(e)})

    waveform = tts.infer(text, voice=request.voice, style=style)  # float32 @ 48kHz
    waveform = soxr.resample(waveform, tts.sample_rate, OUTPUT_SAMPLE_RATE_HZ)

    pcm = (waveform.clip(-1.0, 1.0) * 32767.0).astype("int16")
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(OUTPUT_SAMPLE_RATE_HZ)
        w.writeframes(pcm.tobytes())

    logger.info("Synthesized %r (voice=%s, style=%s) -> %d samples", text, request.voice, style, pcm.shape[0])
    return Response(content=buf.getvalue(), media_type="audio/wav")
