OUTPUT_FORMAT("elf32-littleriscv")
/*
 * ESP32-P4 linker script for LK.
 * Code/rodata in flash XIP at 0x40005000 (loaded by SBL), data/bss in L2MEM.
 * PSRAM: pages compartidas SDMMC (IDMAC) al inicio, resto para heap PSRAM.
 */

ENTRY(_start)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x40005000, LENGTH = 16M
    RAM   (rwx) : ORIGIN = 0x4FF00000, LENGTH = 768K
    /* Ultimos 32K reservados al core LP (ulp.ld), como en Xinu. */
    PSRAM (rwx) : ORIGIN = 0x48000000, LENGTH = 32M - 32K
}

SECTIONS
{
    . = ORIGIN(FLASH);

    .text : {
        KEEP(*(.text.boot))
        *(.text .text*)
        *(.gnu.linkonce.t.*)
    } > FLASH

    . = ALIGN(4);
    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.* .gnu.linkonce.r.*)
        __rodata_end = .;
    } > FLASH

    . = ALIGN(4);
    .fs_impl : {
        __start_fs_impl = .;
        KEEP(*(.fs_impl))
        __stop_fs_impl = .;
    } > FLASH

    . = ALIGN(4);
    .commands : {
        __commands_start = .;
        KEEP(*(.commands))
        __commands_end = .;
    } > FLASH

    . = ALIGN(4);
    .apps : {
        __apps_start = .;
        KEEP(*(.apps))
        __apps_end = .;
    } > FLASH

    . = ALIGN(4);
    .lk_init : {
        __lk_init_start = .;
        KEEP(*(.lk_init))
        __lk_init_end = .;
    } > FLASH

    . = ALIGN(4);
    /* LMA de .data: debe quedar en FLASH (aqui el location counter aun esta
     * en la region FLASH).  Si se define despues de .l2reserve, el linker lo
     * evalua en RAM y el start.S copiaria .data desde RAM sobre si mismo. */
    __data_start_rom = .;

    /* Los primeros 128 KB de HP_L2MEM no son RAM: estan consumidos por el
     * array de datos de la cache L2 (medido en placa en Xinu).  Cualquier
     * escritura con cache que se desaloja en esa zona se pierde, y corrompe
     * .data/.bss/heap (de ahi los asserts de magic en thread/event).  Reservar
     * el rango deja todo el estado del kernel por encima de 0x4FF20000. */
    .l2reserve (NOLOAD) : ALIGN(4096)
    {
        __l2reserve_start = .;
        . = . + 0x20000;
        __l2reserve_end = .;
    } > RAM

    .data : AT(__data_start_rom) {
        __data_start = .;
        *(.data .data.* .gnu.linkonce.d.*)
        __ctor_list = .;
        KEEP(*(.ctors .init_array*))
        __ctor_end = .;
        __dtor_list = .;
        KEEP(*(.dtors .fini_array*))
        __dtor_end = .;
        . = ALIGN(4);
        /* sdata in same load image */
        __global_pointer$ = . + 0x800;
        *(.srodata.cst16) *(.srodata.cst8) *(.srodata.cst4) *(.srodata.cst2) *(.srodata .srodata.*)
        *(.sdata .sdata.* .gnu.linkonce.s.*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM

    . = ALIGN(8);
    __bss_start = .;
    .sbss (NOLOAD) : {
        *(.dynsbss)
        *(.sbss .sbss.* .gnu.linkonce.sb.*)
        *(.scommon)
    } > RAM
    .bss (NOLOAD) : {
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
    } > RAM
    . = ALIGN(8);
    __bss_end = .;

    . = ALIGN(16);
    _end = .;

    _end_of_ram = ORIGIN(RAM) + LENGTH(RAM);

    /* Paginas compartidas para el audio (AHB-DMA <-> CPU), como Xinu .audioshare.
     * El CPU accede por el alias directo 0x88 (sin cache) y en los descriptores
     * anota la forma 0x48 que acepta el motor.  Paginas de 4KB separadas para
     * que ningun malloc deje su cabecera en la misma linea de cache. */
    .audioshare (NOLOAD) : ALIGN(4096)
    {
        _audio_desc_page = .;
        . = . + 4096;
        _audio_buf_page = .;
        . = . + 8192;
        _audio_stream_desc_page = .;
        . = . + 4096;
        _audio_stream_buf_page = .;
        . = . + 65536;
        _audio_in_desc_page = .;
        . = . + 4096;
        _audio_in_buf_page = .;
        . = . + 65536;
    } > PSRAM

    /* Páginas compartidas CPU/IDMAC del SDMMC en PSRAM (como Xinu .sdshare):
     * CPU accede por alias no-cacheado (+0x40000000), IDMAC por dirección bus 0x48.. */
    .sdshare (NOLOAD) : ALIGN(4096)
    {
        _sd_desc_page = .;
        . = . + 4096;
        _sd_block_page = .;
        . = . + 4096;
    } > PSRAM

    /* El heap PSRAM arranca por encima de TODAS las paginas compartidas
     * (audio + SD), igual que en Xinu: _psram_free_start se define desde
     * _sd_block_page, que ya viene despues de .audioshare. */
    _psram_free_start = _sd_block_page + 4096;
    _psram_free_end   = ORIGIN(PSRAM) + LENGTH(PSRAM);

    /DISCARD/ : { *(.comment .note .eh_frame) }
}
