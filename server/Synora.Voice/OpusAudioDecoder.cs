using Concentus.Enums;
using Concentus.Structs;

namespace Synora.Voice;

/// <summary>
/// Decodes the device's captured Opus frames (mono, fixed sample rate/frame
/// duration -- see the firmware's SynoraVoiceSession) into PCM16, wraps
/// PCM16 into a minimal WAV file for the ASR sidecar's upload endpoint (and
/// parses one back, for the TTS sidecar's response / the admin STT test's
/// upload), and encodes PCM16 into Opus frames the device's SynoraVoiceSession
/// can decode (the TTS reply leg).
/// </summary>
public static class OpusAudioDecoder
{
    public static short[] DecodeToPcm16(IEnumerable<byte[]> opusFrames, int sampleRateHz, int frameDurationMs)
    {
        var decoder = new OpusDecoder(sampleRateHz, 1);
        var maxFrameSamples = sampleRateHz / 1000 * frameDurationMs;
        var pcm = new List<short>();
        var scratch = new short[maxFrameSamples];

        foreach (var frame in opusFrames)
        {
            var decoded = decoder.Decode(frame, 0, frame.Length, scratch, 0, maxFrameSamples, false);
            if (decoded > 0)
            {
                pcm.AddRange(scratch.AsSpan(0, decoded).ToArray());
            }
        }

        return pcm.ToArray();
    }

    /// <summary>
    /// Splits pcmSamples into frameDurationMs-long chunks and Opus-encodes
    /// each one -- the reverse of DecodeToPcm16, for sending a TTS reply
    /// back to the device in the same per-frame shape SynoraVoiceSession's
    /// OnAudioDown() expects. The final chunk is zero-padded to a full frame
    /// if pcmSamples isn't an exact multiple of the frame size -- Opus
    /// frames are fixed-duration, a short last frame isn't valid.
    /// </summary>
    public static List<byte[]> EncodeToOpusFrames(short[] pcmSamples, int sampleRateHz, int frameDurationMs)
    {
        var encoder = new OpusEncoder(sampleRateHz, 1, OpusApplication.OPUS_APPLICATION_VOIP);
        var frameSamples = sampleRateHz / 1000 * frameDurationMs;
        var frames = new List<byte[]>();
        var scratch = new byte[4000]; // generous upper bound for one encoded Opus frame

        for (int offset = 0; offset < pcmSamples.Length; offset += frameSamples)
        {
            var count = Math.Min(frameSamples, pcmSamples.Length - offset);
            short[] chunk;
            if (count < frameSamples)
            {
                chunk = new short[frameSamples];
                Array.Copy(pcmSamples, offset, chunk, 0, count);
            }
            else
            {
                chunk = pcmSamples.AsSpan(offset, frameSamples).ToArray();
            }

            var encodedLength = encoder.Encode(chunk, 0, frameSamples, scratch, 0, scratch.Length);
            var frame = new byte[encodedLength];
            Array.Copy(scratch, frame, encodedLength);
            frames.Add(frame);
        }

        return frames;
    }

    public static byte[] BuildWav(short[] pcmSamples, int sampleRateHz)
    {
        const int channels = 1;
        const int bitsPerSample = 16;
        int byteRate = sampleRateHz * channels * bitsPerSample / 8;
        int blockAlign = channels * bitsPerSample / 8;
        int dataSize = pcmSamples.Length * sizeof(short);

        using var ms = new MemoryStream();
        using var w = new BinaryWriter(ms);

        w.Write("RIFF"u8.ToArray());
        w.Write(36 + dataSize);
        w.Write("WAVE"u8.ToArray());
        w.Write("fmt "u8.ToArray());
        w.Write(16); // fmt chunk size
        w.Write((short)1); // PCM
        w.Write((short)channels);
        w.Write(sampleRateHz);
        w.Write(byteRate);
        w.Write((short)blockAlign);
        w.Write((short)bitsPerSample);
        w.Write("data"u8.ToArray());
        w.Write(dataSize);
        foreach (var sample in pcmSamples)
        {
            w.Write(sample);
        }

        return ms.ToArray();
    }

    /// <summary>
    /// Parses a mono 16-bit PCM WAV file (the shape a browser-based admin
    /// STT test upload is expected to provide) into raw samples. Throws
    /// <see cref="InvalidDataException"/> on anything else (wrong channel
    /// count/bit depth/sample rate, missing chunks) -- this is a test-tool
    /// input path, not a general-purpose WAV reader, so a clear rejection
    /// beats silently resampling/downmixing something unexpected.
    /// </summary>
    public static short[] ParseWav16Mono(byte[] wavBytes, int expectedSampleRateHz)
    {
        using var ms = new MemoryStream(wavBytes);
        using var r = new BinaryReader(ms);

        if (new string(r.ReadChars(4)) != "RIFF")
        {
            throw new InvalidDataException("Not a RIFF file.");
        }
        r.ReadInt32(); // overall file size, unused
        if (new string(r.ReadChars(4)) != "WAVE")
        {
            throw new InvalidDataException("Not a WAVE file.");
        }

        short channels = 0;
        short bitsPerSample = 0;
        int sampleRate = 0;
        byte[]? data = null;

        while (ms.Position < ms.Length)
        {
            var chunkId = new string(r.ReadChars(4));
            var chunkSize = r.ReadInt32();
            if (chunkId == "fmt ")
            {
                r.ReadInt16(); // audio format (1 = PCM)
                channels = r.ReadInt16();
                sampleRate = r.ReadInt32();
                r.ReadInt32(); // byte rate, unused
                r.ReadInt16(); // block align, unused
                bitsPerSample = r.ReadInt16();
                if (chunkSize > 16)
                {
                    r.ReadBytes(chunkSize - 16); // skip any extra fmt fields
                }
            }
            else if (chunkId == "data")
            {
                data = r.ReadBytes(chunkSize);
            }
            else
            {
                r.ReadBytes(chunkSize);
            }
            if (chunkSize % 2 == 1 && ms.Position < ms.Length)
            {
                r.ReadByte(); // chunks are word-aligned
            }
        }

        if (data is null)
        {
            throw new InvalidDataException("No data chunk found.");
        }
        if (channels != 1)
        {
            throw new InvalidDataException($"Expected mono audio, got {channels} channel(s).");
        }
        if (bitsPerSample != 16)
        {
            throw new InvalidDataException($"Expected 16-bit PCM, got {bitsPerSample}-bit.");
        }
        if (sampleRate != expectedSampleRateHz)
        {
            throw new InvalidDataException($"Expected {expectedSampleRateHz} Hz, got {sampleRate} Hz.");
        }

        var samples = new short[data.Length / 2];
        Buffer.BlockCopy(data, 0, samples, 0, samples.Length * 2);
        return samples;
    }
}
