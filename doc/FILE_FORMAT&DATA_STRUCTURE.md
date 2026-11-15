## 5. File Format & Data Structure

### 5.1 Storage layer: **FATFS (FatFs) over `HAL_SD`**
Chosen over raw sector writes because it yields a standard, PC-readable filesystem with no bespoke recovery tooling, delegates wear-leveling to the card's FTL, and lets us commit/flush cleanly per chunk. The *payload* is our own compact binary container — **not CSV** (CSV at 1600 Hz × 3 axes would be ~5–8× larger and slow to parse). Binary int16 is small, fast to write, and trivially loaded by numpy/pandas on the PC.

### 5.2 Binary container layout (little-endian)
```
FILE HEADER (fixed, e.g., 256 B):
  magic[4]          = "VIBL"            // identifies type + version
  version           u16   = 1
  sample_rate_hz    u32   = 1600
  num_axes          u8    = 3
  bytes_per_sample  u8    = 6           // int16 x,y,z
  sensor_model[16]         = "LIS3DSH"
  range_g           i8                // ±2/±4/±8/±16 g
  gain              f32               // LSB → g calibration factor
  offset_x/y/z      int16 ×3          // per-axis zero bias (optional)
  start_timestamp   u64               // RTC-based epoch (ms)
  record_count      u32               // total samples in file
  header_crc        u32

PAYLOAD — sequence of self-describing CHUNKS:
  [ chunk_hdr ][ n × record ] ...
    chunk_hdr : seq_number u32 | sample_count u16   // gap detection
    record    : int16 x, int16 y, int16 z           // one sample

FILE TRAILER (written on clean stop):
  end_timestamp   u64
  total_records   u32
  file_crc32      u32               // integrity over payload
```

**Why chunked:** each buffer is written as a chunk carrying its **sequence number**. The PC parser detects dropped data by gaps in `seq_number` — drops are explicit and self-describing, never silently assumed contiguous. This makes the format crash-safe (see §6).

---