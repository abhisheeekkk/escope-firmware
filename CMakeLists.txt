cmake_minimum_required(VERSION 3.22)

# Must be set before project() — tells CMake we're cross-compiling
set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR ARM)

# Toolchain (arm-none-eabi must be on PATH)
set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY      arm-none-eabi-objcopy)
set(CMAKE_SIZE         arm-none-eabi-size)

# Prevent CMake from trying to link test executables for the cross-compiler
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(escope-firmware C CXX ASM)

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)

# ── Target CPU flags ──────────────────────────────────────────────────────────
# STM32H743 = Cortex-M7 with FPU (double precision)
set(CPU_FLAGS
    -mcpu=cortex-m7
    -mthumb
    -mfpu=fpv5-d16
    -mfloat-abi=hard
)

# ── Compiler definitions ──────────────────────────────────────────────────────
add_compile_definitions(
    STM32H743xx          # Device family define — selects stm32h743xx.h
    USE_HAL_DRIVER       # Enables the HAL layer
    HSE_VALUE=24000000U   # 8 MHz external crystal (Nucleo-H743ZI default)
    HSE_STARTUP_TIMEOUT=100U
    LSE_STARTUP_TIMEOUT=5000U
    LSE_VALUE=32768U
    EXTERNAL_CLOCK_VALUE=12288000U
    HSI_VALUE=64000000U
    CSI_VALUE=4000000U
    HSI48_VALUE=48000000U
    VDD_VALUE=3300U
)

# ── Compile flags ─────────────────────────────────────────────────────────────
add_compile_options(
    ${CPU_FLAGS}
    -Wall
    -Wextra
    -fdata-sections
    -ffunction-sections
)
# Build-type specific flags
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-Og -g3)
    add_compile_definitions(DEBUG)
else()
    add_compile_options(-O2)
    add_compile_definitions(NDEBUG)
endif()

# ── Linker flags ──────────────────────────────────────────────────────────────
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/STM32H743ZITx_FLASH.ld)

add_link_options(
    ${CPU_FLAGS}
    -T${LINKER_SCRIPT}
    -Wl,--gc-sections           # Remove unused sections (critical for size)
    -Wl,-Map=${PROJECT_NAME}.map,--cref
    -Wl,--print-memory-usage
    -specs=nano.specs           # Newlib-nano: smaller printf/malloc
    -specs=nosys.specs          # No OS syscalls (bare-metal)
    -lc -lm                     # Standard C and math
)

# ── Include paths ─────────────────────────────────────────────────────────────
include_directories(
    Core/Inc
    Drivers/STM32H7xx_HAL_Driver/Inc
    Drivers/STM32H7xx_HAL_Driver/Inc/Legacy
    Drivers/CMSIS/Device/ST/STM32H7xx/Include
    Drivers/CMSIS/Include
    USB_DEVICE/App
    USB_DEVICE/Target
    Middlewares/ST/STM32_USB_Device_Library/Core/Inc
    Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc
)

# ── Source files ──────────────────────────────────────────────────────────────
file(GLOB_RECURSE HAL_SOURCES
    "Drivers/STM32H7xx_HAL_Driver/Src/*.c"
)
# Exclude template files (they need manual instantiation)
list(FILTER HAL_SOURCES EXCLUDE REGEX ".*_template\\.c$")

file(GLOB USB_CORE_SOURCES
    "Middlewares/ST/STM32_USB_Device_Library/Core/Src/*.c"
)
list(FILTER USB_CORE_SOURCES EXCLUDE REGEX ".*_template\.c$")

file(GLOB USB_CDC_SOURCES
    "Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/*.c"
)
list(FILTER USB_CDC_SOURCES EXCLUDE REGEX ".*_template\.c$")

set(CORE_SOURCES
    Core/Src/main.c
    Core/Src/stm32h7xx_it.c
    Core/Src/stm32h7xx_hal_msp.c
    Core/Src/system_stm32h7xx.c
    Core/Src/syscalls.c
    Core/Src/sysmem.c
)

set(USB_APP_SOURCES
    USB_DEVICE/App/usb_device.c
    USB_DEVICE/App/usbd_desc.c
    USB_DEVICE/App/usbd_cdc_if.c
    USB_DEVICE/Target/usbd_conf.c
)

set(STARTUP_FILE Core/Src/startup_stm32h743zitx.s)

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(${PROJECT_NAME}.elf
    ${CORE_SOURCES}
    ${USB_APP_SOURCES}
    ${HAL_SOURCES}
    ${USB_CORE_SOURCES}
    ${USB_CDC_SOURCES}
    ${STARTUP_FILE}
)

# ── Post-build: generate .hex and .bin ───────────────────────────────────────
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}.elf>
    COMMENT "Generating ${PROJECT_NAME}.hex and ${PROJECT_NAME}.bin"
)
