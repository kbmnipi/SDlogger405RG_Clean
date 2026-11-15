## 6. Power Management & Reliability

### 6.1 Battery strategy
- **During RECORDING:** Sleep/Stop modes are **not viable** — we sample every 625 µs and Stop-mode wake latency is too long/unpredictable, which would drop samples. Keep the MCU active; the dominant draw is SD write bursts + SPI reads, not CPU idle. Because DMA does the heavy lifting (CPU < ~15% sustained), we can **scale HCLK down** from 168 MHz to ~96–120 MHz during recording to cut dynamic power while still meeting the sampling+write budget.
- **During IDLE:** this is where battery life is won. Gate SPI1/SDIO clocks, then enter **Stop mode** (RAM retained → calibration/state preserved; wake in µs). Use **Standby** only if near-zero draw is required and full re-init on button press is acceptable. A low-frequency RTC tick can optionally drive a very-low-rate heartbeat log while lightly sleeping.
- **Policy:** IDLE = Stop mode (clocks gated); RECORDING = active, possibly clock-scaled. This preserves sample integrity far better than per-sample sleep.

### 6.2 Watchdog strategy (IWDG)
Use the **Independent WDT** — it runs on LSI (~32 kHz), independent of the main clock and still running in Stop mode, so it self-recovers even if the main clock fails (ideal for a battery device).

- **Timeout:** set to comfortably exceed the worst legitimate blocking op. Worst case = one SD write (~200 ms) + margin → **~1–2 s**. A normal 200 ms stall never trips it; a genuine hang (seconds of no progress) does.
- **Feeding:** because we use **non-blocking `HAL_SD` + DMA with completion callbacks**, the main loop stays responsive and feeds IWDG every iteration naturally — there is no long unattended block to cause spurious resets. Any bounded wait loop also feeds the WDT inside it. This eliminates the classic "WDT reset during SD write" failure mode by design.

### 6.3 Graceful recovery (power loss / reset)
On any reset, boot re-initializes and inspects the last file:
- A **clean** file has a valid trailer + `file_crc32`. An **incomplete** file lacks it.
- Recovery rule: keep all chunks whose `seq_number` is contiguous from 0; discard the tail after the first gap/missing chunk. Because data is committed per-chunk with seq numbers, prior data is never corrupted and dropped samples are bounded to at most one buffer (~128 ms) at the instant of power loss.

---

## 7. State Machine & Error Handling

```
        ┌────────┐   boot ok    ┌───────┐  button  ┌────────────┐  stop  ┌───────────┐
        │ BOOT   │────────────►│ IDLE  │─────────►│ RECORDING  │───────►│ FLUSHING  │
        └────────┘             └───────┘◄─────────┴────────────┘       └───────────┘
              │ boot fail            ▲   wake (button/RTC)                 │ clean stop
              ▼                      └─────────────────────────────────────┘
           ┌────────┐  recoverable: re-init on next button press
           │ ERROR  │  unrecoverable: hold until power cycle
           └────────┘
```

| State | Behavior |
|---|---|
| **BOOT** | Init clocks, GPIO, SPI1 + LIS3DSH (read ID reg to confirm presence), SDIO + FATFS mount, start IWDG. Sensor or SD missing → ERROR with a code (LED blink pattern; no display). |
| **IDLE** | MCU in Stop mode; wake on button EXTI or RTC tick. Button debounced here. Lowest power state. |
| **RECORDING** | Timer+DMA sampling active, triple-buffer producer-consumer running, SD writer draining buffers, IWDG fed each loop iteration. Button → FLUSHING. |
| **FLUSHING** | Freeze the sampling timer (stop producer), drain all READY/WRITING buffers to SD, write trailer + CRC, `f_sync`, return to IDLE. Guarantees a clean parseable file on user Stop. |
| **ERROR** | Latched fault with code; LED blink pattern encodes it. Recoverable (SD removed) → re-init on next button press; unrecoverable (sensor gone) → hold until power cycle. |

### Error handling specifics
- **Buffer overflow** (all 3 buffers full): set a `gap` flag, drop the oldest READY buffer but record its seq so the PC parser sees the gap; **do not reset** — keep logging. Count total dropped samples for the report.
- **SD card removed mid-write:** SDIO DMA error / CMD timeout caught in callback → ERROR(SD_LOST), stop sampling to avoid unbounded overflow, re-init on next button press (or auto-retry with backoff). Already-flushed chunks remain committed via per-chunk FATFS sync.
- **SD write failure (CRC/timeout):** retry the same buffer up to N times; if still failing → ERROR. Never silently drop — data is either written or explicitly marked as a gap.

### Button control (non-blocking)
EXTI on the BOOT/User pin with hardware RC debounce + software timestamp debounce in the main loop (ignore re-triggers within ~30 ms; require stable level for ~20–50 ms before accepting). The EXTI only sets a flag; the state machine polls it and applies the transition — **no `delay()` in the handler**. Optional: short-press = start/stop toggle, long-press = force flush + safe stop.

---

## 8. Resource Analysis

### 8.1 RAM budget (D1 SRAM = 128 KB)
| Item | Size | Location |
|---|---|---|
| Triple sensor buffer (3 × 12 KB) | **36 KB** | D1 (DMA-accessible) |
| FATFS working/sector buffer + FS structs | ~12 KB | D1 (SDIO-DMA-backed writes) |
| `HAL_SD` internal buffers | ~4 KB | D1 |
| Stack + heap + globals | ~8–16 KB | D1 |
| **Total** | **~60–70 KB** | fits 128 KB with headroom to scale N up |

CCM RAM (64 KB) reserved for non-DMA data / large lookup tables only.

### 8.2 DMA channels
- **SPI1_RX → DMA2_Stream0_Channel3** — sensor capture (one generic stream).
- **SDIO TX/RX → dedicated SDIO DMA engine** — consumes **no** generic DMA stream and offloads the CPU entirely during card writes. This frees all other generic streams for future use.

### 8.3 CPU load estimate @ 1600 Hz
| Activity | Cost |
|---|---|
| Timer ISR (arm/verify DMA, bump index) | few hundred cycles/tick → **<0.5%** |
| SPI RX DMA-complete handler (int16 convert + append; FPU for calibration) | handful of instructions/sample → **<2%** |
| SD writer (main loop) | mostly waiting on completion flags; spikes to ~5–10% during a burst, near-zero sustained |
| **Total sustained** | **well under ~15%** → large margin; can run at reduced clock for power savings |

### 8.4 Key design invariants (checklist)
- [ ] All DMA buffers placed in **D1 SRAM**, never CCM.
- [ ] Sampling driven by **Timer2 @ exact 1600 Hz** + SPI1 RX DMA — not main-loop timing.
- [ ] SD writes are **non-blocking + SDIO-DMA**; IWDG fed every loop iteration (no long unattended blocks).
- [ ] Triple buffering gives **~384 ms** worst-case stall absorption (> 200 ms SD stall) → no loss under normal stalls.
- [ ] Chunked file format with seq numbers + trailer CRC → crash-safe, gap-detectable, PC-parseable.
- [ ] IDLE = Stop mode (battery); RECORDING = active/clock-scaled (sample integrity).
