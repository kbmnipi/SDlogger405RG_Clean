"""V2-2 / V2-3 acceptance fixtures for LOSSY MODE (Milestone 2b).

What the firmware now leaves on the card after a card-GC stall (src/main.c
V2 LOSSY MODE block + src/sd_writer.c vibl_recover()):

  * the stalled chunk is DROPPED - it leaves NO bytes in the stream, because
    vibl_recover() rewinds fptr to vibl_data_end and the next chunk overwrites
    the dead range byte-for-byte (both are 6 + 2048*6 = 12 294 B);
  * main.c calls vibl_mark_gap(lost_chunks * 2048), so next_chunk_seq jumps and
    the hole is VISIBLE to parser_vibl.py as "Gap detected!";
  * recording continues, the trailer is still written at the real payload end
    and its file_crc32 still matches the surviving payload byte-for-byte
    (vibl_stall() rolls the running CRC back over the abandoned chunk).

Fixtures
--------
1. SYNTH_LOSSY_GAP.VIB          - 96 written chunks, holes at seq 30, 55, 56
   plus one INVISIBLE trailing hole at 99 (a stall at the very end of a session
   has no following chunk to bracket it - §10.5 item 1 of the milestone doc).
   Expected: 96 chunks, Gaps Found: Yes, 3 gap events, ✅ MATCH, exit status 0.
2. SYNTH_LOSSY_HEADGAP.VIB      - the session's FIRST chunk stalled, so the
   first written header carries seq = 1. The old parser compared only adjacent
   chunks and could not see a head hole; V2-2 added that branch. Exit 0.
3. SYNTH_LOSSY_GAP_NOTRAILER.VIB- same idea, power cut after a recovered stall:
   gaps recorded, no trailer. Expected "No trailer found (power loss/error)",
   NO "File CRC32" line, gaps reported, exit status 0.

Usage: python test/mk_synth_lossy_gap.py [output_dir]
"""
import os
import struct
import sys

HDR = 256
CHUNK_SAMPLES = 2048
CHUNK_BYTES = 6 + CHUNK_SAMPLES * 6            # 12 294
START_TS = 1700000000123

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


def header(nchunks_written, closed=False):
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
    struct.pack_into('<Q', hdr, 39, START_TS)        # start_ts
    # total_records counts records that ACTUALLY landed on the card (total_records
    # in sd_writer.c is advanced only after a successful commit), so dropped
    # chunks are NOT in it: the parser must match it against the chunks it found.
    struct.pack_into('<I', hdr, 47,
                     nchunks_written * CHUNK_SAMPLES if closed else 0)
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


def build_stream(seqs):
    """Concatenate only the chunks that actually landed. Dropped seq numbers leave
    no bytes behind - that is exactly what vibl_recover()'s rewind achieves."""
    body = b''.join(chunk(s) for s in seqs)
    file_crc = 0xFFFFFFFF
    for s in seqs:
        file_crc = crc32_update(file_crc, chunk(s))
    return body, file_crc ^ 0xFFFFFFFF


def build(outdir):
    # 1. mid-stream stalls, survived, session closed cleanly ------------------
    # 0..29 written | 30 dropped (1 arena lost) | 31..54 written |
    # 55+56 dropped (both arenas lost) | 57..98 written | 99 dropped at the very
    # end: no following chunk brackets that one, so it is invisible to the parser.
    seqs = list(range(0, 30)) + list(range(31, 55)) + list(range(57, 99))
    body, crc = build_stream(seqs)
    p1 = os.path.join(outdir, 'SYNTH_LOSSY_GAP.VIB')
    with open(p1, 'wb') as f:
        f.write(header(len(seqs), closed=True))
        f.write(body)
        f.write(struct.pack('<QII', START_TS + len(seqs) * 1280,
                            len(seqs) * CHUNK_SAMPLES, crc))
    print('1) %-34s %d B  %d chunks, holes at seq 30 / 55-56 (+ invisible 99)'
          % (p1, os.path.getsize(p1), len(seqs)))
    print('   expected trailer CRC32   : %08X  -> parser must print MATCH' % crc)

    # 2. the FIRST chunk of the session stalled ------------------------------
    seqs = list(range(1, 9))
    body, crc = build_stream(seqs)
    p2 = os.path.join(outdir, 'SYNTH_LOSSY_HEADGAP.VIB')
    with open(p2, 'wb') as f:
        f.write(header(len(seqs), closed=True))
        f.write(body)
        f.write(struct.pack('<QII', START_TS + len(seqs) * 1280,
                            len(seqs) * CHUNK_SAMPLES, crc))
    print('2) %-34s %d B  %d chunks, head hole: first written seq = 1'
          % (p2, os.path.getsize(p2), len(seqs)))
    print('   expected trailer CRC32   : %08X' % crc)

    # 3. gaps recorded, then power loss before vibl_close() ------------------
    seqs = list(range(0, 6)) + list(range(9, 12))   # 6, 7, 8 dropped (2 chunks)
    body, crc = build_stream(seqs)
    p3 = os.path.join(outdir, 'SYNTH_LOSSY_GAP_NOTRAILER.VIB')
    with open(p3, 'wb') as f:
        f.write(header(len(seqs)))                   # record_count stays 0 (open header)
        f.write(body)                                # no trailer at all
    print('3) %-34s %d B  %d chunks, hole at seq 6-8, NO trailer'
          % (p3, os.path.getsize(p3), len(seqs)))
    print('   (payload CRC would be %08X, but the parser must not probe one here)' % crc)

    # 4. BYTE-LEVEL PROOF of the vibl_recover() invariant ----------------------
    #    Simulate the card buffer the way the firmware now writes it:
    #      - a stalled chunk lands PARTIALLY (its 6-byte header + the first 7000
    #        sample bytes) at offset vibl_data_end; vibl_data_end does NOT move;
    #      - vibl_recover() rewinds fptr to vibl_data_end (sd_writer.c:258);
    #      - the next chunk (exactly 12 294 B) is appended at vibl_data_end and
    #        therefore OVERWRITES every dead byte;
    #      - vibl_close() truncates at vibl_data_end and writes the trailer.
    #    Asserted result: the buffer is byte-identical to a gap chain that never
    #    contained the dead bytes at all -> no orphan bytes mid-stream, which is
    #    the claim doc/MILESTONE_2_ROBUSTNESS.md 10.2 item 2 makes.
    attempted = list(range(0, 40))
    dropped = {5, 12, 13, 20}                       # 5 alone; 12+13 as a pair
    dead_partial = 6 + 7000                         # bytes that "made it" before timeout
    card = bytearray(header(0, closed=False))        # record_count = 0 at vibl_open()
    data_end = HDR
    file_crc = 0xFFFFFFFF
    written = []
    for seq in attempted:
        full = chunk(seq)
        if seq in dropped:
            dead = full[:dead_partial]
            if data_end + len(dead) > len(card):
                card.extend(b'\x00' * (data_end + len(dead) - len(card)))
            card[data_end:data_end + len(dead)] = dead      # partial commit
            # vibl_recover(): fptr = data_end, CRC rolled back (vibl_stall)
        else:
            if data_end + len(full) > len(card):
                card.extend(b'\x00' * (data_end + len(full) - len(card)))
            card[data_end:data_end + len(full)] = full      # append / overwrite dead bytes
            data_end += len(full)
            file_crc = crc32_update(file_crc, full)
            written.append(seq)
    # vibl_close() step 1: f_lseek(offsetof(record_count)) -> rewrite record_count
    # and the header CRC in place (sd_writer.c seek to 47, 8-byte write).
    struct.pack_into('<I', card, 47, len(written) * CHUNK_SAMPLES)
    struct.pack_into('<I', card, 51, 0)
    struct.pack_into('<I', card, 51,
                     crc32_update(0xFFFFFFFF, bytes(card[:252])) ^ 0xFFFFFFFF)
    card = card[:data_end]                            # f_truncate() at vibl_close()
    card += struct.pack('<QII', START_TS + len(written) * 1280,
                        len(written) * CHUNK_SAMPLES, file_crc ^ 0xFFFFFFFF)
    clean = header(len(written), closed=True) + b''.join(chunk(s) for s in written) \
        + struct.pack('<QII', START_TS + len(written) * 1280,
                      len(written) * CHUNK_SAMPLES, file_crc ^ 0xFFFFFFFF)
    assert bytes(card) == clean, 'rewind/overwrite invariant BROKEN'
    p4 = os.path.join(outdir, 'SYNTH_LOSSY_OVERWRITE.VIB')
    with open(p4, 'wb') as f:
        f.write(bytes(card))
    print('4) %-34s %d B  %d chunks written, %d stalled chunks left %d B each mid-stream'
          % (p4, os.path.getsize(p4), len(written), len(dropped), dead_partial))
    print('   ASSERT PASS: buffer is byte-identical to the clean gap chain '
          '(dead bytes fully overwritten, size exact)')
    print('   expected trailer CRC32   : %08X' % (file_crc ^ 0xFFFFFFFF))


if __name__ == '__main__':
    build(sys.argv[1] if len(sys.argv) > 1 else 'test')
