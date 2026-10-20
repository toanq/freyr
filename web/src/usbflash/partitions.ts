// ESP-IDF binary partition table: 32-byte entries starting at 0x8000, each
// magic 0xAA 0x50 (0x50AA little-endian), type, subtype, offset, size, label.
// An MD5 entry (0xEB 0xEB) or 0xFF padding ends the table.
export const PARTITION_TABLE_OFFSET = 0x8000
export const PARTITION_TABLE_SIZE = 0xc00

export type Partition = { label: string; type: number; subtype: number; offset: number; size: number }

const TYPE_APP = 0x00
const TYPE_DATA = 0x01
const SUBTYPE_OTA_0 = 0x10
const SUBTYPE_DATA_OTA = 0x00
const SUBTYPE_DATA_NVS = 0x02

export function parsePartitionTable(bytes: Uint8Array): Partition[] {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  const parts: Partition[] = []
  for (let off = 0; off + 32 <= bytes.length; off += 32) {
    if (view.getUint16(off, true) !== 0x50aa) break
    const labelBytes = bytes.subarray(off + 12, off + 28)
    const end = labelBytes.indexOf(0)
    parts.push({
      type: bytes[off + 2],
      subtype: bytes[off + 3],
      offset: view.getUint32(off + 4, true),
      size: view.getUint32(off + 8, true),
      label: new TextDecoder().decode(end === -1 ? labelBytes : labelBytes.subarray(0, end)),
    })
  }
  return parts
}

export type UpdateLayout = { ota0: Partition; otadata: Partition; nvs: Partition | null }

// The two regions a keep-settings update writes. Refuses (null) rather than
// guessing when the table isn't the shape this firmware uses.
export function findUpdateLayout(parts: Partition[]): UpdateLayout | null {
  const ota0 = parts.find((p) => p.type === TYPE_APP && p.subtype === SUBTYPE_OTA_0)
  const otadata = parts.find((p) => p.type === TYPE_DATA && p.subtype === SUBTYPE_DATA_OTA)
  const nvs = parts.find((p) => p.type === TYPE_DATA && p.subtype === SUBTYPE_DATA_NVS) ?? null
  return ota0 && otadata ? { ota0, otadata, nvs } : null
}

export function overlaps(aOffset: number, aSize: number, b: Partition): boolean {
  return aOffset < b.offset + b.size && b.offset < aOffset + aSize
}
