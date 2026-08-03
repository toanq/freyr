// Records mono mic audio via Web Audio API and packages it as a 16000 Hz
// 16-bit PCM WAV Blob -- the exact format the admin STT test endpoint (and
// OpusAudioDecoder.ParseWav16Mono server-side) requires. MediaRecorder isn't
// used here: it outputs a compressed container (webm/opus, browser-
// dependent), not raw PCM, so building the WAV by hand from an
// AudioContext tap is the simplest way to get a format the server accepts
// without adding a server-side transcoding step just for this test tool.
//
// Uses the deprecated ScriptProcessorNode rather than an AudioWorkletNode --
// simpler (no separate worklet module to load/bundle) and this is an
// admin-only debug tool, not a production audio path, so the deprecation
// doesn't matter in practice.

export type WavRecorder = {
  stop: () => Promise<Blob>
}

const TARGET_SAMPLE_RATE = 16000

export async function startWavRecording(): Promise<WavRecorder> {
  const stream = await navigator.mediaDevices.getUserMedia({ audio: { channelCount: 1 } })
  const audioContext = new AudioContext()
  const source = audioContext.createMediaStreamSource(stream)
  const processor = audioContext.createScriptProcessor(4096, 1, 1)
  const chunks: Float32Array[] = []

  processor.onaudioprocess = (e) => {
    chunks.push(new Float32Array(e.inputBuffer.getChannelData(0)))
  }
  source.connect(processor)
  processor.connect(audioContext.destination)

  return {
    stop: async () => {
      processor.disconnect()
      source.disconnect()
      stream.getTracks().forEach((t) => t.stop())

      const merged = mergeChunks(chunks)
      const resampled = resample(merged, audioContext.sampleRate, TARGET_SAMPLE_RATE)
      const pcm16 = floatTo16BitPcm(resampled)
      await audioContext.close()
      return buildWavBlob(pcm16, TARGET_SAMPLE_RATE)
    },
  }
}

function mergeChunks(chunks: Float32Array[]): Float32Array {
  const total = chunks.reduce((sum, c) => sum + c.length, 0)
  const merged = new Float32Array(total)
  let offset = 0
  for (const chunk of chunks) {
    merged.set(chunk, offset)
    offset += chunk.length
  }
  return merged
}

// Nearest-neighbor resampling -- not audiophile-grade, but the target here
// is "the ASR model can transcribe it", not high-fidelity playback, and
// this is a debug tool.
function resample(samples: Float32Array, fromRate: number, toRate: number): Float32Array {
  if (fromRate === toRate) return samples
  const ratio = fromRate / toRate
  const newLength = Math.round(samples.length / ratio)
  const out = new Float32Array(newLength)
  for (let i = 0; i < newLength; i++) {
    out[i] = samples[Math.min(samples.length - 1, Math.floor(i * ratio))]
  }
  return out
}

function floatTo16BitPcm(samples: Float32Array): Int16Array {
  const out = new Int16Array(samples.length)
  for (let i = 0; i < samples.length; i++) {
    const s = Math.max(-1, Math.min(1, samples[i]))
    out[i] = s < 0 ? s * 0x8000 : s * 0x7fff
  }
  return out
}

function buildWavBlob(pcm16: Int16Array, sampleRateHz: number): Blob {
  const buffer = new ArrayBuffer(44 + pcm16.length * 2)
  const view = new DataView(buffer)

  const writeString = (offset: number, str: string) => {
    for (let i = 0; i < str.length; i++) view.setUint8(offset + i, str.charCodeAt(i))
  }

  writeString(0, 'RIFF')
  view.setUint32(4, 36 + pcm16.length * 2, true)
  writeString(8, 'WAVE')
  writeString(12, 'fmt ')
  view.setUint32(16, 16, true) // fmt chunk size
  view.setUint16(20, 1, true) // PCM
  view.setUint16(22, 1, true) // mono
  view.setUint32(24, sampleRateHz, true)
  view.setUint32(28, sampleRateHz * 2, true) // byte rate (rate * channels * bytesPerSample)
  view.setUint16(32, 2, true) // block align
  view.setUint16(34, 16, true) // bits per sample
  writeString(36, 'data')
  view.setUint32(40, pcm16.length * 2, true)
  for (let i = 0; i < pcm16.length; i++) {
    view.setInt16(44 + i * 2, pcm16[i], true)
  }

  return new Blob([buffer], { type: 'audio/wav' })
}
