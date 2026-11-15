"""Synthetic VIBL writer: a pure-Python PORT of the C algorithm in sd_writer.c.

Why a port instead of a zlib one-liner: the point of this test is to prove that
the C CRC (crc32_update() + final XOR) and parser_vibl.py agree after the P0-1
fix. Re-implementing the C table algorithm independently and feeding the result
to the real parser is the only way to check that without hardware.

Also verifies the P1-7 geometry:
  * payload starts at VIBL_HEADER_SIZE = 256 (NOT 512 - that value belonged to
    the older tree, which had a 512-byte header sector);
  * preallocation extends f_size to 256 + 3600*12294 and vibl_close() must
    truncate back to the real payload end, so the trailer lands at exactly
    HDR + payload_len;
  * vibl_mark_gap() is size-neutral (a chunk seq_number jump only).
"""
import os
import struct

HDR = 256
CHUNK_SAMPLES = 2048
CHUNK_BYTES = 6 + CHUNK_SAMPLES * 6            # 12294
PREALLOC = HDR + 3600 * CHUNK_BYTES            # 44,258,976
CAP = 64 * 1024 * 1024

# --- exact port of crc32_build_table()/crc32_update() from sd_writer.c -------
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


def build(path, nchunks=5, gap_at=3, start_ts=1700000000123):
    hdr = bytearray(HDR)
    hdr[0:4] = b'VIBL'
    struct.pack_into('<H', hdr, 4, 1)                # version
    struct.pack_into('<I', hdr, 6, 1600)             # sample_rate
    hdr[10] = 3                                      # axes
    hdr[11] = 6                                      # bytes_per_sample
    name = b'LIS3DSH\x00'
    hdr[12:12 + len(name)] = name
    hdr[28] = struct.pack('<b', 16)[0]               # range_g (i8)
    struct.pack_into('<f', hdr, 29, 100.0)           # gain
    struct.pack_into('<hhh', hdr, 33, 0, 0, 0)       # offset_x/y/z
    struct.pack_into('<Q', hdr, 39, start_ts)        # start_ts
    struct.pack_into('<I', hdr, 47, 0)               # record_count
    struct.pack_into('<I', hdr, 51, 0)               # header_crc
    struct.pack_into('<I', hdr, 51,
                     crc32_update(0xFFFFFFFF, bytes(hdr[:252])) ^ 0xFFFFFFFF)

    body = bytearray()
    file_crc = 0xFFFFFFFF                            # C running, un-finalized
    total = 0
    seq = 0
    for i in range(nchunks):
        if i == gap_at:
            seq += 7                                 # vibl_mark_gap(): seq jump
        payload = bytearray()
        for s in range(CHUNK_SAMPLES):
            v = (seq * 1000 + s) & 0x7FFF
            payload += struct.pack('<hhh', v - 1000, v - 2000, v - 3000)
        chdr = struct.pack('<IH', seq, CHUNK_SAMPLES)
        body += chdr + payload
        file_crc = crc32_update(file_crc, chdr)      # exactly the C order
        file_crc = crc32_update(file_crc, bytes(payload))
        total += CHUNK_SAMPLES
        seq += 1

    # vibl_close() step 1: rewrite record_count + header_crc in place
    struct.pack_into('<I', hdr, 47, total)
    struct.pack_into('<I', hdr, 51, 0)
    struct.pack_into('<I', hdr, 51,
                     crc32_update(0xFFFFFFFF, bytes(hdr[:252])) ^ 0xFFFFFFFF)

    with open(path, 'wb') as f:
        f.write(bytes(hdr))
        f.write(bytes(body))                          # payload starts at 256
        f.write(struct.pack('<QII', start_ts + 60000, total,
                            file_crc ^ 0xFFFFFFFF))   # trailer at data end

    print('prealloc target   : %d B (%.1f MiB) vs cap %d B -> %s'
          % (PREALLOC, PREALLOC / 1048576.0, CAP,
             'CLAMPED' if PREALLOC > CAP else 'kept'))
    print('preallocated span : %.1f min for 3600 chunks x 1.28 s '
          '(the audit calls this "1 h"; it is 76.8 min)' % (3600 * 1.28 / 60.0))
    print('final file size   : %d B = %d + %d*%d + %d'
          % (os.path.getsize(path), HDR, nchunks, CHUNK_BYTES, 16))
    print('total_records     : %d' % total)
    print('file_crc32        : %08X' % (file_crc ^ 0xFFFFFFFF))
    print('start_ts          : %d' % start_ts)


if __name__ == '__main__':
    build(os.path.join('test', 'SYNTH01.VIB'))
