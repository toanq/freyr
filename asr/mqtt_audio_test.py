#!/usr/bin/env python3
"""One-off end-to-end test: encodes a WAV file to raw Opus frames (16kHz
mono, 60ms -- matching the firmware's SynoraVoiceSession audio_params
exactly) and publishes them through the real MQTT broker around a
hello/listen-start/listen-stop turn, to verify the ASR sidecar + LlmVoicePipeline
wiring works end-to-end (not just the ASR endpoint in isolation)."""
import json
import subprocess
import sys
import time

import opuslib
import paho.mqtt.client as mqtt

WAV_PATH = sys.argv[1]
DEVICE_ID = sys.argv[2]
MQTT_HOST = "mosquitto"
MQTT_USERNAME = "synora_device"
MQTT_PASSWORD = sys.argv[3] if len(sys.argv) > 3 else "synora_mqtt_dev"

SAMPLE_RATE = 16000
FRAME_MS = 60
FRAME_SAMPLES = SAMPLE_RATE // 1000 * FRAME_MS  # 960

def topic(suffix):
    return f"synora/devices/{DEVICE_ID}/{suffix}"

# Resample to 16kHz mono s16le PCM via ffmpeg.
pcm = subprocess.run(
    ["ffmpeg", "-y", "-i", WAV_PATH, "-ar", str(SAMPLE_RATE), "-ac", "1", "-f", "s16le", "-"],
    capture_output=True, check=True,
).stdout

client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
client.connect(MQTT_HOST, 1883, keepalive=30)
client.loop_start()
time.sleep(1)

hello = {
    "type": "hello", "version": 2, "transport": "mqtt",
    "audio_params": {"format": "opus", "sample_rate": SAMPLE_RATE, "channels": 1, "frame_duration": FRAME_MS},
}
client.publish(topic("voice/control/up"), json.dumps(hello), qos=1)
time.sleep(0.5)
client.publish(topic("voice/control/up"), json.dumps({"type": "listen", "state": "start", "mode": "manual"}), qos=1)
time.sleep(0.5)

encoder = opuslib.Encoder(SAMPLE_RATE, 1, opuslib.APPLICATION_AUDIO)
bytes_per_frame = FRAME_SAMPLES * 2  # 16-bit mono
frame_count = 0
for i in range(0, len(pcm) - bytes_per_frame, bytes_per_frame):
    chunk = pcm[i:i + bytes_per_frame]
    opus_frame = encoder.encode(chunk, FRAME_SAMPLES)
    client.publish(topic("voice/audio/up"), opus_frame, qos=0)
    frame_count += 1
    time.sleep(0.01)  # don't burst-publish faster than real-time audio would

print(f"Published {frame_count} Opus frames ({len(pcm)} PCM bytes)")

time.sleep(0.5)
client.publish(topic("voice/control/up"), json.dumps({"type": "listen", "state": "stop"}), qos=1)
time.sleep(3)  # give the server time to run ASR + LLM
client.loop_stop()
client.disconnect()
print("Done.")
