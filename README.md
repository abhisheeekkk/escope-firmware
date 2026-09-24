# EmbeddedScope Firmware

Open-source digital logic analyzer firmware for the STM32H743VIT6.
Part of the EmbeddedScope project -- a fully open-source, high-performance
digital logic analyzer targeting embedded systems developers.

> 8 channels. 48 MS/s. USB streaming. Completely open source.

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
    DMA1 Stream0 (circular, ping-pong 4096 samples/half)
         |
         v
    Edge detection (XOR adjacent samples)
         |
         v
    USB CDC packet (0xE5 magic, seq, count, edges)
         |
         v
    PC (EmbeddedScope Qt6 app or decode_escope.py)

Each DMA half-transfer completes every 85 us.
At idle (no transitions), zero bytes are sent over USB.
On a 1 MHz square wave, throughput is ~120 KB/s -- well within USB FS limits.

---

## Packet Format

    Byte 0:     0xE5        magic
    Byte 1:     seq         packet sequence (wraps at 255)
    Byte 2-3:   count       number of edge records (big-endian)
    Then count x 5 bytes:
      Bytes 0-3: timestamp_ns  (uint32 big-endian, ns since capture start)
      Byte  4:   channel(6:0) | level(7)

---

## Build

Install toolchain:

    sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi cmake ninja-build

Clone and build:

    git clone https://github.com/abhisheeekkk/escope-firmware
    cd escope-firmware
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build -j$(nproc)

Output files in build/:

    escope-firmware.bin   -- flash this
    escope-firmware.hex   -- alternative format
    escope-firmware.elf   -- debug with GDB

---

## Flash

Enter DFU mode -- pull BOOT0 high, press RESET:

    sudo dfu-util -a 0 -s 0x08000000:leave -D build/escope-firmware.bin

Normal boot -- pull BOOT0 low, press RESET. Verify:

    cat /dev/ttyACM0
    EmbeddedScope | uptime 1000ms | PD=0x00 | ...

---

## Decode packets

    pip3 install pyserial
    python3 scripts/decode_escope.py /dev/ttyACM0

Output when toggling PD0:

    --- Packet seq=0 edges=1 ---
      D0 -> HIGH @ 1234.567 us
    --- Packet seq=1 edges=1 ---
      D0 -> LOW  @ 2345.678 us

---

## Project Structure

    Core/               Application code
      Src/main.c        Entry point, heartbeat, USB init
      Src/acquisition.c DMA engine, edge detection, packet TX
      Inc/acquisition.h Public API
    USB_DEVICE/         USB CDC stack (CubeMX generated)
    Middlewares/        ST USB Device Library
    scripts/            Host-side tools
      decode_escope.py  Edge packet decoder
    CMakeLists.txt      CMake build system
    STM32H743ZITx_FLASH.ld  Linker script

---

## Roadmap

- [x] USB CDC hello world at 480 MHz
- [x] DMA acquisition engine -- 8ch, 48 MS/s
- [x] Edge compression and USB streaming
- [x] Python packet decoder
- [ ] PC software integration (EmbeddedScope Qt6 app)
- [ ] Protocol decoders (UART, SPI, I2C)
- [ ] Hardware trigger
- [ ] FPGA hybrid V1 (iCE40 + STM32 USB bridge)
- [ ] USB3 ASIC V2 (500 MS/s, 32 channels)

---

## Contributing

Pull requests welcome. The codebase is split into layers so you can contribute
to just the area you care about -- firmware, protocol, PC software, or hardware.

## License

MIT
