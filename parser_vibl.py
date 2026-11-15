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
    power_loss_tail = False

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
        # instead of the honest "No trailer found (power loss)".
        return end_ts != 0 and total_records != 0 and (end_ts & 0xFFFFFFFF) != len(chunks)

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
        if count == 0:
            power_loss_tail = True
            break
        if chunks and seq <= chunks[-1]['seq_number']:
            break

        # Если count не равен 2048, возможно, мы наткнулись на трейлер или мусор
        if count > 2048:
            break
            
        chunk_data_len = count * 6
        if pos + 6 + chunk_data_len > len(data):
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
    if power_loss_tail:
        # MISS-1 companion: the chunk loop stopped inside the preallocated
        # tail (count == 0), so the 16 bytes at `pos` are uninitialized
        # zeros, NOT a trailer. Probing them would print a bogus
        # "File CRC32: 00000000 -> MISMATCH".
        print("⚠️ No trailer found (power loss)")

    elif pos + 16 <= len(data):
        trailer_bytes = data[pos:pos+16]
        end_ts, total_records, file_crc32 = struct.unpack('<QII', trailer_bytes)
        # P0-1: zlib already finalized the CRC -> mask only, NO ^ 0xFFFFFFFF.
        # This now equals C: crc32_update(0xFFFFFFFF, ...) ^ 0xFFFFFFFF.
        calc_crc = payload_crc & 0xFFFFFFFF
        trailer_dict = {
            'end_ts': end_ts, 'total_records': total_records,
            'file_crc32': file_crc32, 'calc_file_crc': calc_crc
        }
    else:
        print("⚠️ Warning: No trailer found (file was cut off / power loss)")

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
        print(f"File CRC32: {trailer['file_crc32']:08X} -> {'✅ MATCH' if trailer['file_crc32'] == trailer['calc_file_crc'] else '❌ MISMATCH'}")
    
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
