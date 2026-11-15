"""P0-2 / P1-8 acceptance fixtures: tails produced by an SD WRITE ERROR (not by
power loss).

Three shapes matter for parser_vibl.py and for the firmware shutdown path:

1. SYNTH_FFTAIL.VIB      - header + 10 real chunks + a tail of 0xFF up to the
   P1-7 preallocation target (44 258 656 B), no trailer. This is what a card
   returns for sectors that were never written after a failed chunk (erase
   state = 1). Before the fix the parser read 0xFF as a chunk header
   (count = 0xFFFF > 2048), broke silently and then mis-probed 16 bytes of 0xFF
   as a trailer -> bogus "File CRC32: FFFFFFFF -> MISMATCH".

2. SYNTH_TRUNC_PARTIAL.VIB - 4 real chunks + the 6-byte header of chunk #4 and
   only 300 of its 12 288 payload bytes, no trailer: the card died mid-chunk
   and vibl_close() never ran. The parser must drop the incomplete tail,
   report 4 chunks and "No trailer found (power loss/error)".

3. SYNTH_SDERROR_CLOSED.VIB - what main.c P1-8 (graceful shutdown) now leaves
   on the card: the partial bytes of the failed chunk are cut off by
   f_truncate(vibl_data_end) inside vibl_close(), so the file is exactly
   header + 4 good chunks + trailer, and the trailer CRC must MATCH the payload.

4. SYNTH_CORRUPT_TRAILER.VIB - the Milestone-3 (12 MHz) bench artefact that
   caused the ClockDiv rollback to 8 MHz: 102 chunks, no gaps, exact size
   (256 + 102*12294 + 16 = 1 254 260 B), but one bit flipped in the 16 B
   trailer's CRC field (a setup/hold violation on D0 during the last write).
   parser_vibl.py must keep every chunk as valid and downgrade the trailer to
   "⚠️ Trailer CRC mismatch (likely corrupted during power loss / SD error)" -
   never a fatal "MISMATCH" verdict on the whole file.

Usage: python test/mk_synth_error_tail.py [output_dir]
"""
import os
import struct
import sys

HDR = 256
CHUNK_SAMPLES = 2048
CHUNK_BYTES = 6 + CHUNK_SAMPLES * 6            # 12294
PREALLOC = HDR + 3600 * CHUNK_BYTES            # 44 258 656

# --- exact port of crc32_build_table()/crc32_update() from sd_writer.c --------
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


def header(nchunks_written, start_ts=1700000000123, closed=False):
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
    record_count = nchunks_written * CHUNK_SAMPLES if closed else 0
    struct.pack_into('<I', hdr, 47, record_count)    # record_count
    struct.pack_into('<I', hdr, 51, 0)               # header_crc
    struct.pack_into('<I', hdr, 51,
                     crc32_update(0xFFFFFFFF, bytes(hdr[:252])) ^ 0xFFFFFFFF)
    return hdr


def chunk(seq, samples=CHUNK_SAMPLES):
    payload = bytearray()
    for s in range(samples):
        v = (seq * 1000 + s) & 0x7FFF
        payload += struct.pack('<hhh', v - 1000, v - 2000, v - 3000)
    return struct.pack('<IH', seq, samples) + bytes(payload)


def build(outdir):
    start_ts = 1700000000123

    # 1. all-0xFF blank tail, no trailer ------------------------------------
    body = b''.join(chunk(i) for i in range(10))
    tail_len = PREALLOC - HDR - len(body)
    p1 = os.path.join(outdir, 'SYNTH_FFTAIL.VIB')
    with open(p1, 'wb') as f:
        f.write(header(10, start_ts))
        f.write(body)
        f.write(b'\xff' * tail_len)
    print('1) %-32s %d B  10 chunks + %d B of 0xFF tail, no trailer'
          % (p1, os.path.getsize(p1), tail_len))

    # 2. card died in the middle of chunk #4, close never ran ----------------
    body = b''.join(chunk(i) for i in range(4))
    partial = struct.pack('<IH', 4, CHUNK_SAMPLES) + chunk(4)[6:306]   # 300 payload B
    p2 = os.path.join(outdir, 'SYNTH_TRUNC_PARTIAL.VIB')
    with open(p2, 'wb') as f:
        f.write(header(4, start_ts))
        f.write(body)
        f.write(partial)
    print('2) %-32s %d B  4 chunks + %d B partial chunk, no trailer'
          % (p2, os.path.getsize(p2), len(partial)))

    # 3. shutdown path result: partial bytes truncated away + trailer --------
    #    vibl_close(): f_lseek(vibl_data_end) + f_truncate() + trailer, and the
    #    running CRC covers only the 4 chunks that actually succeeded.
    file_crc = 0xFFFFFFFF
    for i in range(4):
        file_crc = crc32_update(file_crc, chunk(i))
    p3 = os.path.join(outdir, 'SYNTH_SDERROR_CLOSED.VIB')
    with open(p3, 'wb') as f:
        f.write(header(4, start_ts, closed=True))
        f.write(body)
        f.write(struct.pack('<QII', start_ts + 30000, 4 * CHUNK_SAMPLES,
                            file_crc ^ 0xFFFFFFFF))
    print('3) %-32s %d B  4 chunks + trailer (f_truncate at vibl_data_end)'
          % (p3, os.path.getsize(p3)))
    print('   expected trailer CRC32 : %08X' % (file_crc ^ 0xFFFFFFFF))

    # 4. Milestone-3 bench artefact: whole file fine, TRAILER write corrupted -----
    #    102 chunks landed correctly (256 + 102*12294 + 16 = 1 254 260 B, no gaps),
    #    but the final 16 B (the trailer) was damaged on the bus at ClockDiv = 2
    #    (12 MHz). Modelled as ONE flipped bit in the trailer's file_crc32 field -
    #    a setup/hold violation on D0 is a single-bit event. parser_vibl.py must
    #    keep all 102 chunks as valid data and report the trailer as a WARNING.
    nchunks = 102
    body = b''.join(chunk(i) for i in range(nchunks))
    file_crc = 0xFFFFFFFF
    for i in range(nchunks):
        file_crc = crc32_update(file_crc, chunk(i))
    good_crc = file_crc ^ 0xFFFFFFFF
    bad_crc = good_crc ^ 0x00000001          # 1 bit corrupted by the bus error
    p4 = os.path.join(outdir, 'SYNTH_CORRUPT_TRAILER.VIB')
    with open(p4, 'wb') as f:
        f.write(header(nchunks, start_ts, closed=True))
        f.write(body)
        f.write(struct.pack('<QII', start_ts + nchunks * 1280,
                            nchunks * CHUNK_SAMPLES, bad_crc))
    print('4) %-32s %d B  %d chunks + CORRUPTED trailer (1 bit of file_crc32)'
          % (p4, os.path.getsize(p4), nchunks))
    print('   intact trailer CRC32   : %08X' % good_crc)
    print('   stored (corrupt) CRC32 : %08X  -> parser warns, does not fail' % bad_crc)


if __name__ == '__main__':
    build(sys.argv[1] if len(sys.argv) > 1 else 'test')
