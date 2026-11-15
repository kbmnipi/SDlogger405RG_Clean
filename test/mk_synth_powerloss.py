"""MISS-1 acceptance: synthetic POWER-LOSS file (preallocated tail, no trailer).

Models exactly what P1-7 leaves on the card after power loss:
  - header 256 B (valid VIBL, header_crc computed like vibl_close() step 1);
  - nchunks real chunks written and f_sync()'d;
  - the rest of the PREALLOC target (256 + 3600*12294 = 44,258,976 B) filled
    with zeros = the uninitialized tail FatFs already allocated via f_lseek;
  - NO trailer (vibl_close() never ran).

Before MISS-1 the parser looped count==0 -> chunk_data_len 0 -> pos += 6
~7.3M times and built a list of ~7.3M empty chunks (OOM on PC).

Usage: python test/mk_synth_powerloss.py [out_path]
"""
import os
import struct
import sys

HDR = 256
CHUNK_SAMPLES = 2048
CHUNK_BYTES = 6 + CHUNK_SAMPLES * 6            # 12294
PREALLOC = HDR + 3600 * CHUNK_BYTES            # 44,258,976

# port of crc32_build_table()/crc32_update() from sd_writer.c
_tbl = []
for _i in range(256):
    _c = _i
    for _k in range(8):
        _c = (0xEDB88320 ^ (_c >> 1)) if (_c & 1) else (_c >> 1)
    _tbl.append(_c)


def crc32_update(crc, data):
    for b in data:
        crc = _tbl[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc & 0xFFFFFFFF


def build(path, nchunks=10, start_ts=1700000000123):
    hdr = bytearray(HDR)
    hdr[0:4] = b'VIBL'
    struct.pack_into('<H', hdr, 4, 1)                # version
    struct.pack_into('<I', hdr, 6, 1600)             # sample_rate
    hdr[10] = 3                                      # axes
    hdr[11] = 6                                      # bytes_per_sample
    name = b'LIS3DSH\x00'
    hdr[12:12 + len(name)] = name
    hdr[28] = struct.pack('<b', 16)[0]               # range_g
    struct.pack_into('<f', hdr, 29, 100.0)           # gain
    struct.pack_into('<hhh', hdr, 33, 0, 0, 0)       # offset_x/y/z
    struct.pack_into('<Q', hdr, 39, start_ts)        # start_ts
    struct.pack_into('<I', hdr, 47, 0)               # record_count (open-time value)
    struct.pack_into('<I', hdr, 51, 0)               # header_crc
    struct.pack_into('<I', hdr, 51,
                     crc32_update(0xFFFFFFFF, bytes(hdr[:252])) ^ 0xFFFFFFFF)

    body = bytearray()
    seq = 0
    for _ in range(nchunks):
        payload = bytearray()
        for s in range(CHUNK_SAMPLES):
            v = (seq * 1000 + s) & 0x7FFF
            payload += struct.pack('<hhh', v - 1000, v - 2000, v - 3000)
        body += struct.pack('<IH', seq, CHUNK_SAMPLES) + bytes(payload)
        seq += 1

    tail = bytearray(PREALLOC - HDR - len(body))     # zeros: never-written clusters
    with open(path, 'wb') as f:
        f.write(bytes(hdr))
        f.write(bytes(body))
        f.write(bytes(tail))                          # NO trailer written

    size = os.path.getsize(path)
    print('power-loss file   : %s' % path)
    print('file size         : %d B (%.1f MiB) = prealloc target' % (size, size / 1048576.0))
    print('real chunks       : %d (payload ends at offset %d)' % (nchunks, HDR + len(body)))
    print('zeros tail        : %d B (%.1f MiB)' % (len(tail), len(tail) / 1048576.0))
    print('pre-P1-7 iters    : %d (%d B / 6 B per zero-length chunk)'
          % (len(tail) // 6, len(tail)))
    print('trailer           : absent (no vibl_close())')


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join('test', 'SYNTH_POWERLOSS.VIB')
    build(out)
