# EmbeddedScope Firmware

Open-source digital logic analyzer firmware for the STM32H743VIT6.
Part of the EmbeddedScope project -- a fully open-source, high-performance
digital logic analyzer targeting embedded systems developers.

> 8 channels. 48 MS/s. Triggered USB capture. Completely open source.

---

## Why EmbeddedScope?

Commercial logic analyzers cost hundreds of dollars. EmbeddedScope aims to
deliver professional-grade digital signal capture at hobbyist prices, with
full source code transparency and no vendor lock-in.

- No proprietary software required
- No closed firmware blobs
- Hackable at every layer -- firmware, protocol, and PC software
- Built on proven STM32H7 silicon running at 480 MHz

---

## Hardware

| Parameter | Value |
|-----------|-------|
| MCU | STM32H743VIT6 (LQFP100, Cortex-M7) |
| Clock | 480 MHz (8 MHz HSE via PLL) |
| Digital inputs | PD0-PD7 (8 channels) |
| Sample rate | 48 MS/s via DMA |
| USB | OTG FS CDC (PA11/PA12) -- appears as /dev/ttyACM0 |
| LED | PC13 |
| Flash | 2 MB |
| RAM | 1 MB (512KB AXI + 128KB DTCM + others) |

---

## Architecture

    GPIO PD0-PD7
         |
         v
    TIM2 @ 48 MHz (update event)
         |
         v
    DMA1 Stream0 (double-buffer mode, 8 x 32 KB segments = 256 KB ring in D2 SRAM)
         |
         v
    Edge trigger scan (main loop, PD0 rising by default)
         |
         v
    Burst: ~4.8 ms window around the trigger (229,376 samples) sent over USB CDC
         |
         v
    PC (decode_burst.py, or the EmbeddedScope Qt6 app)

The sampler free-runs into the ring. The main loop scans each completed
segment for the trigger edge; on a hit it lets the ring capture three more
segments (post-trigger) and stops. The seven newest segments are then uploaded
and the ring is re-armed. If no edge appears for 500 ms it captures anyway and
flags the frame as an auto trigger.

Each burst is raw samples (one byte per sample, bit n = channel Dn), so nothing
is lost to edge-rate limits inside the window: 20.83 ns resolution on all
8 channels at once. The trigger sits at roughly 2.05 ms into the window.

`Src/acquisition.c` (the earlier continuous edge-stream engine) is still in the
build but is not started by `main.c`.

---

## Burst Frame Format

All multi-byte fields little-endian:

    Byte 0:       0xE7        magic
    Byte 1:       version     1
    Byte 2-3:     flags       bit 0 = auto trigger (no real edge seen)
    Byte 4-7:     sample rate in Hz (48,000,000)
    Byte 8-11:    number of samples that follow
    Byte 12-15:   trigger sample index within the data
    Byte 16-19:   frame sequence number
    Byte 20-23:   reserved (currently carries the boot-time GPIO pin sweep result)
    Then num_samples bytes: bit n = channel Dn (PD0-PD7)

Time in ns = sample_index * 125 / 6 (20.83 ns per sample).

## Build

Install toolchain:

    sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi cmake ninja-build

Clone and build:

    git clone https://github.com/abhisheeekkk/escope-firmware
    cd escope-firmware
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build -j$(nproc)
    sudo dfu-util -a 0 -s 0x08000000:leave -D build/escope-firmware.bin


Output files in build/:

    escope-firmware.bin   -- flash this
    escope-firmware.hex   -- alternative format
    escope-firmware.elf   -- debug with GDB

---

## Flash

Enter DFU mode -- pull BOOT0 high, press RESET:

    sudo dfu-util -a 0 -s 0x08000000:leave -D build/escope-firmware.bin

Normal boot -- pull BOOT0 low, press RESET. The board enumerates as a USB CDC
device (it can take about 10 seconds). Verify with the decoder:

    ls /dev/ttyACM*
    python3 scripts/decode_burst.py /dev/ttyACM0

---

## Decode bursts

    pip3 install pyserial
    python3 scripts/decode_burst.py /dev/ttyACM0            # print per-channel stats
    python3 scripts/decode_burst.py /dev/ttyACM0 -s -n 1    # also save .bin + .vcd (GTKWave / PulseView)

The port number can change after a reset; check `ls /dev/ttyACM*`. Enumeration
can take about 10 seconds after flashing.

Example with the built-in test signal (see below):

    --- Burst seq=8 ---
      229376 samples = 4.779 ms, trigger at sample 98310 (2.048 ms into the window)
      D0: 47786 rising, 47787 falling, period 100.0 ns (10000.000 kHz), duty 45.0%
      ...

Edge counts differ by one between bursts because the 4.779 ms window holds a
non-integer number of periods. Duty is quantized to whole samples, so at
10 MHz (4.8 samples per period) it reads 41-62% for a true 50%.

The older `scripts/decode_escope.py` decodes the edge-chunk format from
`acquisition.c`.

---

## Built-in test signal

`main.c` generates a test signal so the analyzer can be checked with no other
equipment. Short PB3-PB10 to PD0-PD7 (D0=PB3 ... D7=PB10):

- TIM3 requests DMA1 Stream1 on every update; each request copies one word
  from a small table into `GPIOB->BSRR`. No CPU or interrupt is involved, so
  edges are jitter-free.
- The table has two entries (all high, all low), so every channel is a 50%
  square wave at TIM3 rate / 2. The rate is set by `TIM3->ARR` in
  `Test_PWM_Init` (ARR = 12 - 1 gives 20 MHz updates and a 10 MHz signal).
  Asking for more (ARR = 6 - 1, a 20 MHz signal) does not work: the DMA cannot
  write GPIOB->BSRR that fast and the output comes out at 12 MHz (4 samples
  per period).
- The table sits in `.dma_buffers`, which the linker does not zero. It is
  cleared explicitly at startup; without that, stale RAM bits corrupt
  individual pins.

Verified on hardware at 1, 5 and 10 MHz on all 8 channels. 10 MHz is close to
the limit of a 48 MS/s sampler (4.8 samples per period).

At boot the firmware also runs a GPIO pin sweep (PA0-PA10, PB0-PB15 driven low
then high and read back). The pins that could not follow are reported in the
reserved word of every burst header, and `decode_burst.py` prints them once.

---

## Project Structure

    Core/               Application code
      Src/main.c        Entry point, heartbeat, USB init, test signal, pin sweep
      Src/burst.c       Ring capture, trigger, burst upload
      Inc/burst.h       Public API
      Src/acquisition.c Earlier edge-stream engine (not started by main)
      Inc/acquisition.h Its public API
    USB_DEVICE/         USB CDC stack (CubeMX generated)
    Middlewares/        ST USB Device Library
    scripts/            Host-side tools
      decode_burst.py   Burst decoder (stats, .bin, .vcd)
      decode_escope.py  Edge chunk decoder (older format)
    CMakeLists.txt      CMake build system
    STM32H743ZITx_FLASH.ld  Linker script

---

## Roadmap

- [x] USB CDC hello world at 480 MHz
- [x] DMA acquisition engine -- 8ch, 48 MS/s
- [x] Edge compression and USB streaming
- [x] Python chunk decoder
- [x] Triggered burst capture (8 ch x 4.8 ms) with VCD export
- [x] Built-in 10 MHz test signal on PB3-PB10
- [ ] PC software integration (EmbeddedScope Qt6 app)
- [ ] Protocol decoders (UART, SPI, I2C)
- [ ] Selectable trigger channel, edge and pre/post-trigger split (currently fixed at PD0 rising)
- [ ] FPGA hybrid V1 (iCE40 + STM32 USB bridge)
- [ ] USB3 ASIC V2 (500 MS/s, 32 channels)

---

## Contributing

Pull requests welcome. The codebase is split into layers so you can contribute
to just the area you care about -- firmware, protocol, PC software, or hardware.

## License

MIT
