using System.Buffers.Binary;

namespace Synora.Protocol;

/// <summary>
/// Binary audio framing for the xiaozhi WebSocket protocol. Mirrors the firmware
/// structs <c>BinaryProtocol2</c>/<c>BinaryProtocol3</c> (see the device
/// <c>main/protocols/protocol.h</c> / <c>websocket_protocol.cc</c>). All
/// multi-byte fields are big-endian (network order), matching the device's
/// <c>htons</c>/<c>htonl</c> usage.
///
/// Layouts:
///   v2 (16-byte header): version u16 | type u16 | reserved u32 | timestamp u32 | payload_size u32 | payload
///   v3 (4-byte header):  type u8 | reserved u8 | payload_size u16 | payload
///   v0/v1:               raw payload (no header)
/// </summary>
public static class XiaozhiBinaryFrame
{
    public const int V2HeaderSize = 16;
    public const int V3HeaderSize = 4;

    /// <summary>Message type byte: Opus audio.</summary>
    public const byte TypeOpus = 0;
    /// <summary>Message type byte: JSON (unused on the binary channel today).</summary>
    public const byte TypeJson = 1;

    /// <summary>Wrap an Opus payload in a frame for the negotiated protocol version.</summary>
    public static byte[] Encode(int version, ReadOnlySpan<byte> payload, uint timestamp = 0, byte type = TypeOpus)
    {
        switch (version)
        {
            case 2:
            {
                var buf = new byte[V2HeaderSize + payload.Length];
                var s = buf.AsSpan();
                BinaryPrimitives.WriteUInt16BigEndian(s[0..], (ushort)version);
                BinaryPrimitives.WriteUInt16BigEndian(s[2..], type);
                BinaryPrimitives.WriteUInt32BigEndian(s[4..], 0u); // reserved
                BinaryPrimitives.WriteUInt32BigEndian(s[8..], timestamp);
                BinaryPrimitives.WriteUInt32BigEndian(s[12..], (uint)payload.Length);
                payload.CopyTo(s[V2HeaderSize..]);
                return buf;
            }
            case 3:
            {
                var buf = new byte[V3HeaderSize + payload.Length];
                var s = buf.AsSpan();
                s[0] = type;
                s[1] = 0; // reserved
                BinaryPrimitives.WriteUInt16BigEndian(s[2..], (ushort)payload.Length);
                payload.CopyTo(s[V3HeaderSize..]);
                return buf;
            }
            default: // v0/v1: raw Opus, no header
                return payload.ToArray();
        }
    }

    /// <summary>
    /// Extract the Opus payload (and timestamp, v2 only) from a received frame.
    /// Returns false on a short/malformed frame.
    /// </summary>
    public static bool TryDecode(int version, ReadOnlySpan<byte> frame, out ReadOnlySpan<byte> payload, out uint timestamp)
    {
        payload = default;
        timestamp = 0;
        switch (version)
        {
            case 2:
                if (frame.Length < V2HeaderSize) return false;
                timestamp = BinaryPrimitives.ReadUInt32BigEndian(frame[8..]);
                var size2 = BinaryPrimitives.ReadUInt32BigEndian(frame[12..]);
                if ((long)V2HeaderSize + size2 > frame.Length) return false;
                payload = frame.Slice(V2HeaderSize, (int)size2);
                return true;
            case 3:
                if (frame.Length < V3HeaderSize) return false;
                int size3 = BinaryPrimitives.ReadUInt16BigEndian(frame[2..]);
                if (V3HeaderSize + size3 > frame.Length) return false;
                payload = frame.Slice(V3HeaderSize, size3);
                return true;
            default: // v0/v1: whole frame is the payload
                payload = frame;
                return true;
        }
    }
}
