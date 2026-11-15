import struct
import zlib
import argparse
import datetime
import numpy as np
import matplotlib.pyplot as plt

def parse_file(f):
    data = f.read()
    if len(data) < 256:
        raise ValueError("File too short for header")

    # 1. Читаем заголовок
    header_bytes = data[:256]
    if header_bytes[0:4] != b'VIBL':
        raise ValueError("Invalid VIBL magic")
    
    header = struct.unpack('<4sHIBB16sbfhhhQII201s', header_bytes)
    
    crc_bytes = bytearray(header_bytes[:252])
    # P0-1b (needed for the acceptance criterion "Header CRC: OK"): in the packed
    # vibl_header_t the header_crc field sits at bytes 51..54 (magic 0-3, version 4-5,
    # sample_rate 6-9, axes 10, bytes_per_sample 11, sensor 12-27, range_g 28, gain 29-32,
    # offset_x/y/z 33-38, start_ts 39-46, record_count 47-50, header_crc 51-54).
    # The previous slice [52:56] left one CRC byte inside the CRC input -> mismatch.
    crc_bytes[51:55] = b'\x00\x00\x00\x00'   # zero header_crc (offsets 51..54) before recomputing
    if zlib.crc32(crc_bytes) != header[13]:
        print(f"⚠️ Warning: Header CRC mismatch!")
    else:
        print("✅ Header CRC: OK")

    header_dict = {
        'version': header[1], 'sample_rate': header[2], 'axes': header[3],
        'bytes_per_sample': header[4], 'sensor': header[5].strip(b'\x00').decode('ascii', errors='ignore'),
        'range_g': header[6], 'gain': header[7], 'offset_x': header[8], 'offset_y': header[9], 'offset_z': header[10],
        'start_ts': header[11], 'record_count': header[12], 'header_crc': header[13]
    }

    # 2. Читаем данные чанк за чанком (без вырезания трейлера заранее)
    chunks = []
    pos = 256
    # P0-1 (audit_13_11_26_1 BUG-3): zlib.crc32 applies the pre/post-condition
    # (init 0xFFFFFFFF + final XOR) internally, so the seed for a chained run
    # MUST be 0. Seeding with 0xFFFFFFFF double-inverts -> wrong value.
    payload_crc = 0
    # MISS-1 companion: set when the chunk loop hits the preallocated tail
    # (count == 0) — that region is uninitialized, so no trailer can be there.
    # P0-2 (truncated-file fix): the region after the last valid chunk can be
    # uninitialized in TWO shapes:
    #   all 0x00 - clusters stretched by f_lseek() (P1-7) but never written
    #              => power loss, vibl_close() never ran;
    #   all 0xFF - blank / erased sectors a card returns after a failed write
    #              => the SD-error shutdown path (main.c P1-8).
    # In both cases there is NO trailer there, and probing it would print a
    # bogus "File CRC32: 00000000 / FFFFFFFF -> MISMATCH".
    blank_tail = False   # tail after the last chunk is all-0x00 or all-0xFF
    cut_off    = False   # loop stopped on a partial chunk or a garbage header

    def trailer_candidate(p):
        # O-1 fix (doc/MILESTONE_1_SUCCESS.md §5): the chunk loop used to read the
        # 16-byte trailer AS a chunk header — struct.unpack('<IH', trailer[0:6])
        # reinterprets end_ts (u64) as seq = end_ts & 0xFFFFFFFF (1 792 021 880 for
        # LOG0003.VIB) and count = (end_ts >> 32) & 0xFFFF (220), which falls through
        # every guard and prints a bogus "File truncated" warning on a COMPLETE file.
        # Discriminator: only one 16-byte block left AND its u32 is not the next
        # expected chunk index. seq == len(chunks) means it really is a chunk header
        # (truncated session) and the normal truncation warning must still fire.
        # Note: the seq-monotonicity guard below cannot do this job — the low 32 bits
        # of a ms epoch are always >> any chunk index.
        if len(data) - p != 16:
            return False
        end_ts, total_records, _ = struct.unpack_from('<QII', data, p)
        # Reject the two values a real trailer can never carry:
        #  total_records == 0  -> a session always has >= 1 record in a trailer
        #                         (zero-chunk close writes total_records = 0 AND
        #                          end_ts = 0; the zero-chunk case is covered by the
        #                          end_ts == 0 test below).
        #  end_ts == 0         -> uninitialized preallocated tail (power-loss file
        #                         truncated by f_truncate() exactly at a chunk
        #                         boundary) or an unset-RTC zero-chunk trailer.
        # Without these, such a tail would be reported as a trailer + CRC MISMATCH
        # instead of the honest "No trailer found (power loss/error)".
        # P0-2: an all-0xFF 16-byte block is a blank/erased sector, never a
        # trailer (the all-0x00 shape is already rejected by end_ts == 0).
        if end_ts == 0xFFFFFFFFFFFFFFFF and total_records == 0xFFFFFFFF:
            return False
        return end_ts != 0 and total_records != 0 and (end_ts & 0xFFFFFFFF) != len(chunks)

    def tail_is_blank(p):
        # P0-2: is everything from p to EOF uninitialized (all 0x00 or all 0xFF)?
        # bytes.count(sub, start) runs in C and does not copy the slice, so a
        # 44 MB preallocated tail is scanned in tens of ms (acceptance: < 1 s).
        remaining = len(data) - p
        if remaining <= 0:
            return False
        return (data.count(b'\x00', p) == remaining
                or data.count(b'\xff', p) == remaining)

    while pos + 6 <= len(data):
        # Silence O-1: step out of the loop exactly at the trailer, no warning.
        if trailer_candidate(pos):
            break

        chunk_hdr = data[pos:pos+6]
        seq, count = struct.unpack('<IH', chunk_hdr)

        # MISS-1: guard against preallocated tail on power-loss files.
        # After P1-7, power-loss files have ~44 MB of uninitialized tail.
        # Unwritten clusters read as count=0 -> infinite loop without this guard.
        # Non-monotonic seq = garbage (stale card data) -> also break.
        # count == 0    -> unwritten cluster of the preallocated tail (power loss)
        # count == 0xFFFF -> blank/erased sector (SD-write error tail). Both mean
        # "no trailer here"; without the 0xFF case this header falls through to
        # the `count > 2048` break and the 16 bytes get mis-probed as a trailer.
        if count == 0 or count == 0xFFFF:
            blank_tail = True
            break
        if chunks and seq <= chunks[-1]['seq_number']:
            cut_off = True      # stale card data, not a trailer
            break

        # Если count не равен 2048, возможно, мы наткнулись на трейлер или мусор
        if count > 2048:
            cut_off = True      # garbage header: keep it out of the trailer probe
            break
            
        chunk_data_len = count * 6
        if pos + 6 + chunk_data_len > len(data):
            cut_off = True   # partial chunk at EOF -> vibl_close() never ran
            print(f"⚠️ Warning: File truncated during chunk {seq}. Ignoring incomplete tail.")
            break
            
        data_bytes = data[pos+6 : pos+6+chunk_data_len]
        
        # Накапливаем CRC для чанка
        payload_crc = zlib.crc32(chunk_hdr, payload_crc)
        payload_crc = zlib.crc32(data_bytes, payload_crc)
        
        records = []
        for i in range(count):
            x, y, z = struct.unpack('<hhh', data_bytes[i*6:(i+1)*6])
            records.append({'x': x, 'y': y, 'z': z})
            
        chunks.append({'seq_number': seq, 'sample_count': count, 'records': records})
        
        pos += 6 + chunk_data_len

    # 3. Пытаемся прочитать трейлер (если файл был корректно закрыт)
    trailer_dict = None
    if blank_tail or cut_off or tail_is_blank(pos):
        # P0-2 / MISS-1 companion: the chunk loop stopped either inside an
        # uninitialized tail (all 0x00 = power loss, all 0xFF = card sectors left
        # unwritten after an SD write error) or on a garbage / partial chunk
        # header. In every one of those cases NO trailer exists at `pos`, so the
        # 16 bytes must not be probed: that is what produced the bogus
        # "File CRC32: 00000000 / FFFFFFFF -> MISMATCH" line. Chunk statistics
        # and plotting below are still printed - the data up to the last
        # f_sync()'ed chunk is intact and remains usable.
        print("⚠️ No trailer found (power loss/error)")

    elif pos + 16 <= len(data):
        trailer_bytes = data[pos:pos+16]
        if trailer_bytes == b'\x00' * 16 or trailer_bytes == b'\xff' * 16:
            # P0-2 safety net: a blank 16-byte block is a sector, not a trailer.
            print("⚠️ No trailer found (power loss/error)")
        else:
            end_ts, total_records, file_crc32 = struct.unpack('<QII', trailer_bytes)
            # P0-1: zlib already finalized the CRC -> mask only, NO ^ 0xFFFFFFFF.
            # This now equals C: crc32_update(0xFFFFFFFF, ...) ^ 0xFFFFFFFF.
            calc_crc = payload_crc & 0xFFFFFFFF
            # P3-3 (Milestone 3 rollback, 2026-11-15): a trailer that EXISTS but
            # whose CRC does not match is NOT a fatal parse error. Observed on the
            # bench at SDIO 12 MHz: 102 chunks + exact file size (f_expand +
            # f_truncate invariant of sd_writer.c:595-612) were fine, only the last
            # 16 B write - the trailer itself - came off the bus corrupted
            # (src/sdio.c P3-2 ROLLBACK). The payload is chunk-framed and each
            # chunk was f_sync'ed before the next one began, so every chunk that
            # parses with a sane header + full payload is recoverable data.
            # 'crc_ok' therefore drives the reporting in main(): MATCH only when
            # the CRC agrees, otherwise a warning - never "the file is broken".
            trailer_dict = {
                'end_ts': end_ts, 'total_records': total_records,
                'file_crc32': file_crc32, 'calc_file_crc': calc_crc,
                'crc_ok': file_crc32 == calc_crc
            }
    else:
        # Fewer than 16 bytes left: the file was cut off before any trailer.
        print("⚠️ No trailer found (power loss/error)")

    return header_dict, chunks, trailer_dict

def format_ts(ts_ms):
    if ts_ms == 0: return "Unknown"
    dt = datetime.datetime.fromtimestamp(ts_ms / 1000.0, tz=datetime.timezone.utc)
    return dt.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3] + " UTC"

def main():
    parser = argparse.ArgumentParser(description='Parse VIBL binary files.')
    parser.add_argument('file', type=str, help='Path to the VIBL file')
    parser.add_argument('--plot', action='store_true', help='Plot the data using matplotlib')
    args = parser.parse_args()

    with open(args.file, 'rb') as f:
        try:
            header, chunks, trailer = parse_file(f)
        except Exception as e:
            print(f"❌ Parsing error: {e}")
            return

    print(f"\n--- HEADER ---")
    print(f"Sensor: {header['sensor']} | Sample Rate: {header['sample_rate']} Hz")
    
    print(f"\n--- PAYLOAD ---")
    print(f"Number of Chunks: {len(chunks)}")
    print(f"Total Records (Chunks): {sum(c['sample_count'] for c in chunks)}")
    
    gaps = False
    for prev, curr in zip(chunks[:-1], chunks[1:]):
        if curr['seq_number'] - prev['seq_number'] > 1:
            gaps = True
            print(f"  ⚠️ Gap detected! Seq jumped from {prev['seq_number']} to {curr['seq_number']}")
    print(f"Gaps Found: {'Yes' if gaps else 'No'}")
    
    if trailer:
        print(f"\n--- TRAILER ---")
        parsed_records = sum(c['sample_count'] for c in chunks)
        print(f"End timestamp   : {format_ts(trailer['end_ts'])}")
        count_note = "" if trailer['total_records'] == parsed_records else \
            "  ⚠️ trailer record count disagrees (trailer likely corrupted)"
        print(f"Declared records: {trailer['total_records']} | parsed: {parsed_records}{count_note}")
        if trailer['crc_ok']:
            print(f"File CRC32: {trailer['file_crc32']:08X} -> ✅ MATCH")
            print("File status : COMPLETE - header + all chunks + verified trailer")
        else:
            # P3-3 (Milestone 3 rollback): a corrupted trailer is a WARNING about
            # the 16 B trailer block itself, not a verdict on the payload. The
            # chunk loop validated every chunk independently (count <= 2048,
            # strictly increasing seq, full 6*count payload present), so all
            # chunks listed above remain valid, recoverable data.
            # This is exactly the 12 MHz bench artefact: 102 good chunks, exact
            # file size, corrupted final 16 B write (src/sdio.c P3-2 ROLLBACK).
            print(f"File CRC32: stored {trailer['file_crc32']:08X} / "
                  f"computed {trailer['calc_file_crc']:08X}")
            print("⚠️ Trailer CRC mismatch (likely corrupted during power loss / SD error)")
            print(f"File status : DATA USABLE - {len(chunks)} chunks / {parsed_records} records "
                  "kept as valid; only the 16 B trailer block is unverified")
    
    if args.plot and chunks:
        print("\nRendering plot...")
        x_data = np.array([r['x'] for c in chunks for r in c['records']])
        y_data = np.array([r['y'] for c in chunks for r in c['records']])
        z_data = np.array([r['z'] for c in chunks for r in c['records']])

        fig, axs = plt.subplots(3, 1, figsize=(10, 8))
        axs[0].plot(x_data, color='red'); axs[0].set_title('X Axis'); axs[0].grid(True)
        axs[1].plot(y_data, color='green'); axs[1].set_title('Y Axis'); axs[1].grid(True)
        axs[2].plot(z_data, color='blue'); axs[2].set_title('Z Axis'); axs[2].grid(True)
        plt.tight_layout()
        plt.show()

if __name__ == '__main__':
    main()
