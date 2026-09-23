/* STM32H743ZI linker script
 * Flash: 2MB at 0x08000000
 * DTCM:  128KB at 0x20000000  (tightly coupled, fastest for stack/data)
 * AXI SRAM (D1): 512KB at 0x24000000  (DMA accessible — use for USB buffers)
 * SRAM1 (D2): 128KB at 0x30000000     (DMA accessible)
 * SRAM2 (D2): 128KB at 0x30020000     (DMA accessible)
 * SRAM3 (D2): 32KB  at 0x30040000
 * SRAM4 (D3): 64KB  at 0x38000000     (accessible from D3 peripherals)
 * ITCM:  64KB  at 0x00000000          (instruction TCM, optional)
 */

ENTRY(Reset_Handler)

_Min_Heap_Size  = 0x400;   /* 1KB heap minimum */
_Min_Stack_Size = 0x800;   /* 2KB stack minimum */

MEMORY
{
    FLASH     (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
    DTCMRAM   (rw)  : ORIGIN = 0x20000000, LENGTH = 128K
    RAM_D1    (rw)  : ORIGIN = 0x24000000, LENGTH = 512K
    RAM_D2    (rw)  : ORIGIN = 0x30000000, LENGTH = 288K
    RAM_D3    (rw)  : ORIGIN = 0x38000000, LENGTH = 64K
    ITCMRAM   (rw)  : ORIGIN = 0x00000000, LENGTH = 64K
}

_estack = ORIGIN(DTCMRAM) + LENGTH(DTCMRAM);  /* Stack starts at top of DTCM */

SECTIONS
{
    /* Vector table and code go to FLASH */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } >FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } >FLASH

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
    .ARM        : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH

    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } >FLASH

    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } >FLASH

    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } >FLASH

    /* Initialised data goes to DTCM RAM, loaded from FLASH */
    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } >DTCMRAM AT>FLASH

    /* BSS (zero-initialised) in DTCM RAM */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } >DTCMRAM

    /* USB buffers in D2 SRAM — required for USB OTG FS DMA access (0x30000000) */
    .usb_buffers (NOLOAD) :
    {
        . = ALIGN(4);
        *(.usb_buffers)
        . = ALIGN(4);
    } >RAM_D2

    /* DMA capture buffers in AXI SRAM D1 */
    .dma_buffers (NOLOAD) :
    {
        . = ALIGN(32);          /* Cache-line aligned for DCache operations */
        *(.dma_buffers)
        . = ALIGN(32);
    } >RAM_D1

    /* User stack in DTCM */
    ._user_heap_stack :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        PROVIDE(_end = .);
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } >DTCMRAM

    /DISCARD/ :
    {
        libc.a(*)
        libm.a(*)
        libgcc.a(*)
    }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}
