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

### Pin map

All signals are 3.3 V logic. Connect grounds between boards.

| Function | Pins | Roles | Notes |
|---|---|---|---|
| Capture inputs D0-D7 | PD0-PD7 | scope | 8 channels, pull-down, sampled at 48 MS/s |
| Built-in test signal | PB3-PB10 (D0=PB3 ... D7=PB10) | all | 1 MHz square waves; wire to PD0-PD7 to self-test |
| USB (PC link) | PA11 (D-), PA12 (D+) | scope | OTG FS CDC |
| UART test output | PA2 (USART2 TX, AF7) | when `FEATURE_UART_TEST` | 500 kbaud 8N1, TX only |
| I2C (OLED) | PD12 (SCL), PD13 (SDA) (I2C4, AF4) | when `FEATURE_OLED_I2C` | SSD1306 at address 0x3C |
| SPI (TFT) | PA5 (SCK), PA7 (MOSI), PA4 (CS), PC4 (DC), PC5 (RES) | SIG_GEN | ST7789V, SPI1 AF5, mode 0 |
| CAN | PD1 (TX), PD0 (RX) (FDCAN1, AF9) | SIG_GEN | through a 3.3 V CAN transceiver |
| LED | PC13 | all | blink rate shows the role |
| DMA debug | PC8 | all | toggles every capture half buffer |
| SWD | PA13, PA14 | all | programming |

PD0 and PD1 are capture inputs in the scope roles and the CAN pins in `SIG_GEN`, so CAN is
only built into `SIG_GEN`. The boot-time pin sweep (PA0-PA10, PB0-PB15) runs before the
peripherals above are started.

To decode a protocol, wire the signal pins of the target board to capture inputs of the scope
board and choose those channels in the app's Protocol menu: UART TX, I2C SCL and SDA, CAN on
the transceiver RXD, SPI clock, MOSI and MISO.

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
    Trigger scan (main loop): rising / falling / either edge on any channel, set by the PC
         |
         v
    Burst: ~4.8 ms window around the trigger (229,376 samples) sent over USB CDC
         |
         v
    PC (decode_burst.py, or the EmbeddedScope Qt6 app)

The sampler free-runs into the ring. The main loop scans each completed
segment for the trigger; on a hit it lets the ring capture more segments
(post-trigger) and stops. The seven newest segments are then uploaded
and the ring is re-armed. If no edge appears for 500 ms it captures anyway and
flags the frame as an auto trigger.

Each burst is raw samples (one byte per sample, bit n = channel Dn), so nothing
is lost to edge-rate limits inside the window: 20.83 ns resolution on all
8 channels at once. In edge mode the trigger sits at roughly 2.05 ms into the window.

`Src/acquisition.c` (the earlier continuous edge-stream engine) is still in the
build but is not started by `main.c`.

---

## Burst Frame Format

All multi-byte fields little-endian:

    Byte 0:       0xE7        magic
    Byte 1:       version     2 (version 1 had a 24 byte header, no device time)
    Byte 2-3:     flags       bit 0 = auto trigger (no real edge seen), bits 8-9 trigger mode, bits 10-12 trigger channel
    Byte 4-7:     sample rate in Hz (48,000,000)
    Byte 8-11:    number of samples that follow
    Byte 12-15:   trigger sample index within the data
    Byte 16-19:   frame sequence number
    Byte 20-23:   reserved (currently carries the boot-time GPIO pin sweep result)
    Byte 24-31:   t0_ns, device time of sample 0 of this burst (version 2)
    Then num_samples bytes: bit n = channel Dn (PD0-PD7)

Time in ns = sample_index * 125 / 6 (20.83 ns per sample).

### Device clock

The sampler is stopped while a burst is uploaded, so the time between bursts is
not in the data. To let the PC place bursts exactly, TIM5 runs as a free-running
240 MHz counter that is never stopped (32 bit, extended to 64 bit in its update
interrupt every 17.9 s). The value is latched when the sampler is armed. TIM2
raises one sample every 5 timer ticks, so sample i is taken at exactly
arm_ticks + 5 * (i + 1) and the header's `t0_ns` is the device time of sample 0
(1 tick = 25/6 ns). The PC then places every burst by `t0_ns` instead of by when
it happened to arrive over USB, which removes the 1 ms-level arrival jitter and
gives exact gaps between bursts. The clock restarts at power-up; if the PC sees
it go backwards it continues the timeline from the end of the previous burst.

### Checking the timing

Measured on a two-board setup (a scope board probing the generator board's I2C
updates, which repeat every 200 ms):

- Precision: the captures of every second update (400 ms of generator time) were
  0.399995 to 0.399996 s apart, a standard deviation of 0.43 us over 74 captures
  (limited by the 1 us resolution of the decoder log). With arrival-time
  placement this wobbled by about 1 ms.
- Relative clock error: comparing the scope timeline with the generator's own
  millisecond counter shown on the OLED gave 10.6 ppm; the spacing of the captures
  gave 10.7 ppm. That is the difference between two independent crystals.
- Absolute accuracy needs an independent reference (a frequency counter on the
  1 MHz test output, an oscilloscope, or a 1 PPS source); two boards can only
  show how much they disagree. The timer value is read just before the sampler
  starts, a constant offset of tens of nanoseconds that is the same for every
  burst, so it does not move bursts relative to each other.

`python3 scripts/decode_burst.py /dev/ttyACM0 -n 20` prints the device time of each
burst and the gap to the previous one.

## Build

Install toolchain:

    sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi cmake ninja-build

Clone and build:

    git clone https://github.com/abhisheeekkk/escope-firmware
    cd escope-firmware
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build -j$(nproc)
    sudo dfu-util -a 0 -s 0x08000000:leave -D build/escope-firmware.bin


Three roles from the same source (`Core/Inc/board_config.h`; the two options are mutually exclusive):

| Role | What it runs | LED |
|------|--------------|-----|
| scope only (`-DSCOPE_ONLY=ON`) | scope + 1 MHz test signal; no OLED/I2C, no UART output. Flash this on the board that probes. | toggles every 500 ms |
| signal generator (`-DSIG_GEN=ON`) | 1 MHz test signal + OLED counter on I2C4 (PD12 SCL / PD13 SDA). No acquisition and USB is never started, so the board never appears on the PC. | toggles every 100 ms |
| scope + generator (both defaults 0) | everything: sends bursts to the PC and also drives the test signal and the OLED | toggles every 500 ms |

A role chosen with a CMake option overrides the defaults in the header. A build
with no option uses the header defaults (check them before flashing). If a board runs
fine but never shows up in `lsusb` or `/dev/ttyACM*`, check its LED: a fast blink
means the signal generator firmware is on it.

Use a separate build folder per role so the binaries do not overwrite each other
(any folder named `build*` is git-ignored):

    cmake -B build-scope -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSCOPE_ONLY=ON
    cmake -B build-gen   -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSIG_GEN=ON
    cmake --build build-scope -j$(nproc)
    sudo dfu-util -a 0 -s 0x08000000:leave -D build-scope/escope-firmware.bin

### OLED I2C speed

`I2C_SPEED_KHZ` in `Core/Inc/board_config.h` sets the I2C4 bus speed of the OLED
(signal generator and scope + generator roles). Use 100, 400 or 1000; the default
is 400. It can also be set on the CMake command line, which overrides the header:

    cmake -B build-gen -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSIG_GEN=ON -DI2C_SPEED_KHZ=100

| Value | Mode | Notes |
|-------|------|-------|
| 100 | standard | |
| 400 | fast | the SSD1306 is rated for this |
| 1000 | fast-mode plus | stronger SCL/SDA drive is enabled; the SSD1306 is only rated for 400, so a module may not work |

Any other value stops the build with a message. The timing registers are worked
out for the 120 MHz I2C4 kernel clock (see the table in `Core/Src/oled.c`). What
is programmed is not what appears on the wire: the peripheral's input sync and
the rise time on the pull-up add about 0.12 us to the high phase and 0.26 us to
the low phase, which is 0.38 us of every clock period. That matters little at
100 kHz but at 1 MHz it is more than a third of the period, so the programmed
phases are shorter than the I2C minimums and the wire still meets them (at 1000
kHz: 0.35 us high and 0.66 us low against minimums of 0.26 and 0.5 us). The real
rate also depends on the pull-ups and wiring, so measure SCL on the scope. The second OLED line shows the speed the firmware was
built with. A full OLED update (cursor write plus 36 data bytes, 46 bytes in total)
takes about 4.2 ms at 100 kHz, about 1.1 ms at 400 kHz and about 0.5 ms at 1000 kHz.

Measured at 400 kHz on the scope board: SCL high about 0.87 us and low about 1.67 us
(a period of about 2.5 us, roughly 398 kHz); 50 of 50 OLED updates decoded with
every byte acknowledged and no cut-off transfers. The first 400 kHz setting
(high phase 0.8 us) measured 390 kHz because the fixed overhead of the I2C
peripheral (input sync and the rise time on the pull-up) is about 0.36 us, not
0.3 us, so the high phase was shortened to 0.75 us. If SCL still reads off by
more than a percent, adjust `OLED_I2C_TIMING` from the measured high and low
phases instead of assuming the overhead; stronger pull-ups lower it.

At 1000 kHz the first setting measured 813 kHz (high 0.46 us, low 0.77 us on the
wire against 0.33 and 0.52 programmed): the same +0.12 / +0.26 us overhead as at
400 kHz. The high and low phases were programmed shorter to 0.225 and 0.40 us
for a predicted 994 kHz.

The clock is deliberately not 50% duty. The I2C specification needs the low phase
longer than the high phase (at 400 kHz: low at least 1.3 us, high at least 0.6 us;
standard mode 4.7 us and 4.0 us; fast-mode plus 0.5 us and 0.26 us). The bus is
open-drain: a device pulls a line low hard and fast, but it only rises through the
pull-up resistor, which is slow. All the data work happens while SCL is low (the
slave may take up to 0.9 us after SCL falls to put its bit or ACK on SDA, SDA has
to settle through its own slow rise, and it needs 100 ns of setup before SCL rises),
while the high phase only has to let the receiver sample. A 50% clock at 400 kHz
would be 1.25 us low, below the minimum, so about 35% high is the correct result.

`FEATURE_UART_TEST` in the same header switches on a "Hello World" stream on
PA2 (USART2 TX, 500 kbaud) in the default build.

Output files in the build folder:

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
  `Test_PWM_Init` (ARR = 12 - 1 gives 20 MHz updates and a 10 MHz signal; the firmware currently ships with ARR = 120 - 1, a 1 MHz signal).
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

## UART test output

`main.c` also drives a UART test signal so the protocol decoder in the PC app
can be checked with no other equipment (`FEATURE_UART_TEST` in `board_config.h`).
USART2 TX is on **PA2** (AF7), TX only, **500,000 baud, 8N1**. The main loop sends
`Hello World` (11 bytes, about 220 us) roughly every 2 ms with a blocking
`HAL_UART_Transmit`, which costs nothing noticeable.

Wire PA2 to any capture input (for example PD0) and select it as TX in the
app's Protocol menu; the baud rate is detected automatically. Each bit is 2 us,
about 96 samples at 48 MS/s.

`MX_USART2_UART_Init()` runs after `Test_PWM_Init()` on purpose: the pin sweep
drives PA0-PA10 as outputs and would otherwise disturb PA2.

---

## Project Structure

    Core/               Application code
      Src/main.c        Entry point, heartbeat, USB init, test signal, pin sweep, UART test output
      Src/burst.c       Ring capture, trigger, burst upload
      Inc/burst.h       Public API
      Src/can_gen.c     FDCAN1 test traffic and DroneCAN node ID server (SIG_GEN role)
      Inc/can_gen.h     Its public API
      Src/tft.c         ST7789V SPI TFT driver, test screen and counter (SIG_GEN role)
      Inc/tft.h         Its pins and public API
      Inc/font5x7.h     5x7 font shared by the OLED and TFT drivers
      Inc/board_config.h  Role and feature selection (SCOPE_ONLY, SIG_GEN, CAN, I2C speed)
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
- [x] Built-in test signal on PB3-PB10
- [x] UART test output on PA2 (500 kbaud) for protocol decoding
- [ ] PC software integration (EmbeddedScope Qt6 app)
- [x] UART protocol decoder with auto baud (PC app)
- [x] I2C protocol decoder (PC app)
- [x] CAN protocol decoder (PC app) with a CAN test node and DroneCAN node ID server in the SIG_GEN firmware
- [x] SPI protocol decoder (PC app)
- [x] Trigger set from the PC over USB: any channel, rising/falling/either edge, plus the pre/post split and Auto/Normal mode (default PD0 rising, Auto)
- [ ] FPGA hybrid V1 (iCE40 + STM32 USB bridge)
- [ ] USB3 ASIC V2 (500 MS/s, 32 channels)

---

## Contributing

Pull requests welcome. The codebase is split into layers so you can contribute
to just the area you care about -- firmware, protocol, PC software, or hardware.

## License

GNU General Public License v3.0 (GPL-3.0). See [LICENSE](LICENSE).

---

## Trigger command (PC to device)

The PC configures the trigger by writing 8 bytes to the CDC port:

    C7 01 mode ch 00 pre auto xor

| Field | Meaning |
|-------|---------|
| mode | 0 rising edge, 1 falling edge, 2 either edge |
| ch | trigger channel 0-7 |
| pre | 0-6 segments (0.68 ms each) kept before the trigger segment; the burst is 7 segments |
| auto | Auto-mode timeout in 10 ms units (50 = 500 ms); 0 = Normal mode: wait for a real trigger, never capture untriggered |
| xor | XOR of the first 7 bytes |

Invalid commands are ignored. The new setting takes effect at the next arm; on
power-up the trigger is PD0 rising with 3 segments of pre-trigger in Auto mode
(500 ms), like a scope's default. Each burst
header's flags word reports the trigger in use (bits 8-9 mode, 10-12
channel). Protocol triggers are not done on the device; the PC decodes
protocols from the captured data.

A second command releases the capture inputs' internal pull-downs, for
open-drain buses such as I2C that bring their own pull-ups:

    C7 02 mask 00 00 00 00 xor

Bit n of `mask` set means PDn has no pull-down; all other inputs keep it. The
power-up default is a pull-down on PD0-PD7 (mask 0).

The OLED counter on I2C4 is updated through a non-blocking path (`Oled_Task`),
so the main loop keeps scanning for the trigger while the display is written.

## SPI TFT (SIG_GEN role)

`FEATURE_TFT_SPI` (enabled in the `SIG_GEN` role) drives a 240x320 ST7789V TFT over SPI1 so
the logic analyser has known SPI traffic to decode. The driver is write only: commands are
sent with the DC line low, then parameters or pixel data with DC high, all inside one chip
select window. SPI mode 0 (clock idles low, data sampled on the rising edge), MSB first, 8
bit frames, 16-bit RGB565 pixels.

| Display pin | STM32 pin | Function |
|---|---|---|
| SCL | PA5 | SPI1 SCK (AF5) |
| SDA | PA7 | SPI1 MOSI (AF5) |
| CS | PA4 | chip select, active low (GPIO) |
| DC | PC4 | low = command, high = data (GPIO) |
| RES | PC5 | reset, active low (GPIO) |
| VCC, GND | 3V3, GND | 3.3 V supply |

The screen shows a title, the real SPI clock, red/green/blue/white bars (colour order
check) and a counter that is redrawn every 250 ms. Only the digits that changed are sent,
so each update is a handful of short transfers (CASET, RASET, RAMWR and pixels) and the main
loop is never blocked for long.

**Clock rate.** `TFT_SPI_KHZ` in `board_config.h`. The SPI1 kernel clock is PLL1Q (64 MHz) and
the prescaler divides it by 2, 4 ... 256, so the rate is 64 MHz / 2^n:

| `TFT_SPI_KHZ` | Bit time | Samples per bit at 48 MS/s | Ideal full-screen fill (153.6 KB) |
|---|---|---|---|
| 250 | 4 us | 192 | 4.9 s |
| 500 (default) | 2 us | 96 | 2.5 s |
| 1000 | 1 us | 48 | 1.2 s |
| 2000 | 500 ns | 24 | 0.61 s |
| 4000 | 250 ns | 12 | 0.31 s |
| 8000 | 125 ns | 6 | 0.15 s |
| 16000 | 62.5 ns | 3 | 77 ms |
| 32000 | 31.25 ns | 1.5 | 38 ms |

The ST7789V datasheet allows about 15 MHz for writes, so 16000 is at the limit and 32000 is
beyond it, although many modules still work. For decoding with this analyser (48 MS/s) stay
at 8000 or below; above that there are too few samples per clock. Start at 250 or 500, check
SCK on the scope, then step up. `Tft_ActualKhz()` returns the rate the firmware computed.

To decode it, wire PA5 and PA7 to two capture inputs of the scope board and choose
Protocol > SPI in the app (CLK and MOSI; the display has no MISO line). The mode and clock rate
are detected.

CS, DC and RES use the slowest edges (they change rarely) so they do not couple onto the neighbouring SCK
wire (PA4 and PA5 are adjacent pins). The edge speed of SCK and MOSI follows the clock rate (low up to 2 MHz, medium up to 8 MHz, high at
16 MHz, very high at 32 MHz) so slow clocks get slow, quiet edges on jumper wires.

`TFT_INVERT` (default 1) turns colour inversion on, which most IPS modules need; `TFT_BGR`
swaps red and blue if the bars come out in the wrong order. A display cannot be detected over
this write-only interface, so a missing display is not reported.

## CAN node (SIG_GEN role)

`FEATURE_CAN_TEST` (enabled in the `SIG_GEN` role) runs FDCAN1 on PD0 (RX) and PD1
(TX), alternate function 9, with the kernel clock taken from the 8 MHz HSE so the bit
timing is exact. PD0 and PD1 are scope inputs in the other roles, so CAN is only built
into `SIG_GEN`. The board is a classic CAN 2.0A/B node; CAN FD is not used.

**Wiring.** The pins are 3.3 V logic. Connect a 3.3 V CAN transceiver: PD1 to TXD, PD0
to RXD, CANH and CANL to the bus, common ground, 120 ohm termination at each end of the
bus. Probe the transceiver RXD with a scope channel to see the bus (including ACKs).
Never connect a scope channel to CANH or CANL.

**Settings** (macros in `Core/Inc/board_config.h`, no build flags):

| Macro | Default | Meaning |
|---|---|---|
| `CAN_BITRATE_KBPS` | 1000 | 125, 250, 500 or 1000 |
| `CAN_LOOPBACK` | 0 | 1 = external loopback (frames on TX with no other node), 0 = normal node |
| `CAN_GEN_TX` | 0 | 1 = send test frames (a standard frame 0x123, a DroneCAN NodeStatus style frame, 0x7FF, a remote frame 0x321, every 50 ms); 0 = listen only |
| `CAN_DNA_SERVER` | 1 | run a DroneCAN dynamic node ID server |

In listen-only mode the node still acknowledges valid frames from other nodes; the ACK
does not depend on the receive filter, and a sender with no acknowledging node would
retransmit forever.

**DroneCAN node ID server.** A device without a node ID sends anonymous allocation
requests (message type 1, source node 0) and repeats them until answered. The board
answers the three-stage exchange (6 + 6 + 4 bytes of the 16 byte unique ID), grants node
20 (up to four devices are remembered by unique ID), and announces NodeStatus as node 1
once a second. Replies longer than 7 bytes are sent as multi-frame transfers with the
transfer CRC. The format and the data type signature used for that CRC are taken from
the DroneCAN specification and have been exercised against one DroneCAN device only.

Decode the traffic with the PC app (Protocol > CAN).

