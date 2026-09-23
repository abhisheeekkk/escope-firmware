# EmbeddedScope Firmware

STM32H743VIT6 firmware for the EmbeddedScope digital logic analyzer.

## Hardware
- MCU: STM32H743VIT6 (LQFP100)
- HSE: 24 MHz crystal
- SYSCLK: 480 MHz (PLL1)
- USB: OTG FS on PA11/PA12 (CDC Virtual COM Port)
- LED: PC13
- Digital inputs: PD0-PD7 (8 channels, 48 MS/s)

## Build

Requirements:

    sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi cmake ninja-build

Build:

    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build -j$(nproc)

Flash via DFU (BOOT0 high + reset to enter DFU mode):

    sudo dfu-util -a 0 -s 0x08000000:leave -D build/escope-firmware.bin

Verify (BOOT0 low + reset, then):

    cat /dev/ttyACM0
    EmbeddedScope | uptime 1000ms | sysclk 480MHz

## Roadmap
- [x] USB CDC hello world at 480MHz
- [ ] DMA acquisition engine (PD0-PD7, 48 MS/s)
- [ ] Edge compression and USB streaming
- [ ] PC software integration (EmbeddedScope Qt6 app)
- [ ] FPGA hybrid (Phase 2)

## Project Structure

    Core/           -- Application code (main.c, clock config, interrupts)
    USB_DEVICE/     -- USB CDC stack (CubeMX generated + customised)
    Middlewares/    -- ST USB Device Library
    CMakeLists.txt  -- CMake build system
    STM32H743ZITx_FLASH.ld -- Linker script
