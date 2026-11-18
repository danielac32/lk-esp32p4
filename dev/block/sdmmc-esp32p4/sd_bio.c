/*
 * BIO block device wrapper for SD card (slot 0) on ESP32-P4 SDMMC host.
 * Adaptado a LK; lógica HW/protocolo alineada con Xinu (sdHw/sdCard).
 */
#include <lib/bio.h>
#include <lib/fs.h>
#include <lib/partition.h>
#include <lk/err.h>
#include <lk/debug.h>
#include <lk/trace.h>
#include <lk/init.h>
#include <kernel/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdmmc_internal.h"

#define LOCAL_TRACE 0

typedef struct sd_bdev { bdev_t dev; } sd_bdev_t;
static sd_bdev_t sd_dev;
static bool sd_bio_registered;

static ssize_t sd_bio_read_block(struct bdev *bdev, void *buf, bnum_t block, uint count) {
    (void)bdev;
    if (!sd_present || count == 0 || !buf) return ERR_IO;
    uint8_t *dst = (uint8_t *)buf;
    sd_card_lock();
    sdmmc_bus_lock();
    for (uint i = 0; i < count; i++) {
        if (!sd_read_block(block + i)) {
            uint32_t e = sd_hwerr;
            sdmmc_bus_unlock();
            sd_card_unlock();
            printf("sd: read error block %u hwerr=0x%x present=%d hc=%d rdy=%d\n",
                   block + i, e, sd_present, sd_hc, sd_host_ready);
            return ERR_IO;
        }
        sd_block_copy_out(dst, SD_BLOCK_SIZE);
        dst += SD_BLOCK_SIZE;
    }
    sd_host_lba = block + count;
    sdmmc_bus_unlock();
    sd_card_unlock();
    return (ssize_t)count * SD_BLOCK_SIZE;
}

static ssize_t sd_bio_read(bdev_t *bdev, void *buf, off_t offset, size_t len) {
    if (!sd_present || !buf || len == 0) return ERR_IO;
    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0;
    bnum_t block = (bnum_t)(offset / SD_BLOCK_SIZE);
    size_t boff = (size_t)(offset % SD_BLOCK_SIZE);

    sd_card_lock();
    sdmmc_bus_lock();
    while (done < len) {
        if (!sd_read_block(block)) {
            uint32_t e = sd_hwerr;
            sdmmc_bus_unlock();
            sd_card_unlock();
            printf("sd: read err blk %u hwerr=0x%x\n", block, e);
            return done ? (ssize_t)done : (ssize_t)ERR_IO;
        }
        size_t chunk = SD_BLOCK_SIZE - boff;
        if (chunk > len - done) chunk = len - done;
        if (chunk == SD_BLOCK_SIZE) {
            sd_block_copy_out(dst + done, SD_BLOCK_SIZE);
        } else {
            uint8_t tmp[SD_BLOCK_SIZE];
            sd_block_copy_out(tmp, SD_BLOCK_SIZE);
            memcpy(dst + done, tmp + boff, chunk);
        }
        done += chunk;
        block++;
        boff = 0;
    }
    sd_host_lba = block;
    sdmmc_bus_unlock();
    sd_card_unlock();
    return (ssize_t)done;
}

static ssize_t sd_bio_write_block(struct bdev *bdev, const void *buf, bnum_t block, uint count) {
    (void)bdev;
    if (!sd_present || count == 0 || !buf) return ERR_IO;
    const uint8_t *src = (const uint8_t *)buf;
    sd_card_lock();
    for (uint i = 0; i < count; i++) {
        /* Soltar el bus entre bloques para no ahogar al WiFi (ver sd_bio_write) */
        sdmmc_bus_lock();
        sd_block_copy_in(src, SD_BLOCK_SIZE);
        if (!sd_write_block(block + i)) {
            sdmmc_bus_unlock();
            sd_card_unlock();
            printf("sd: write error at block %u (hwerr=0x%x)\n", block + i, sd_hwerr);
            return ERR_IO;
        }
        sd_host_lba = block + i;
        sdmmc_bus_unlock();
        thread_yield();
        src += SD_BLOCK_SIZE;
    }
    sd_card_unlock();
    return (ssize_t)count * SD_BLOCK_SIZE;
}

static ssize_t sd_bio_write(bdev_t *bdev, const void *buf, off_t offset, size_t len) {
    if (!sd_present || !buf || len == 0) return ERR_IO;
    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;
    bnum_t block = (bnum_t)(offset / SD_BLOCK_SIZE);
    size_t boff = (size_t)(offset % SD_BLOCK_SIZE);

    sd_card_lock();
    while (done < len) {
        size_t chunk = SD_BLOCK_SIZE - boff;
        if (chunk > len - done) chunk = len - done;
        /* SOLTAR el bus SDMMC entre bloques: WiFi (esp_hosted) comparte este
         * bus y necesita CMD53 para bombear el C6.  Si mantenemos el lock
         * durante toda una escritura de 21KB, se pierden RX (rx_dropped++)
         * y caen httpd/telnet/UDP (medido en HW).  Re-tomar por bloque y
         * ceder el CPU da turno al wifi_poll. */
        sdmmc_bus_lock();
        if (chunk != SD_BLOCK_SIZE) {
            if (!sd_read_block(block)) {
                uint32_t e = sd_hwerr;
                sdmmc_bus_unlock(); sd_card_unlock();
                printf("sd: rmw read err blk %u hwerr=0x%x\n", block, e);
                return done ? (ssize_t)done : (ssize_t)ERR_IO;
            }
            uint8_t tmp[SD_BLOCK_SIZE];
            sd_block_copy_out(tmp, SD_BLOCK_SIZE);
            memcpy(tmp + boff, src + done, chunk);
            sd_block_copy_in(tmp, SD_BLOCK_SIZE);
        } else {
            sd_block_copy_in(src + done, SD_BLOCK_SIZE);
        }
        if (!sd_write_block(block)) {
            uint32_t e = sd_hwerr;
            sdmmc_bus_unlock(); sd_card_unlock();
            printf("sd: write err blk %u hwerr=0x%x\n", block, e);
            return done ? (ssize_t)done : (ssize_t)ERR_IO;
        }
        sd_host_lba = block;
        sdmmc_bus_unlock();
        /* ceder el CPU para que el hilo de WiFi use el bus */
        thread_yield();
        done += chunk;
        block++;
        boff = 0;
    }
    sd_card_unlock();
    return (ssize_t)done;
}

static int sd_bio_ioctl(struct bdev *bdev, int request, void *argp) {
    (void)bdev; (void)argp;
    switch (request) {
        case BIO_IOCTL_IS_MAPPED:
            if (argp) { *(bool *)argp = false; return NO_ERROR; }
            return ERR_INVALID_ARGS;
        default:
            return ERR_NOT_SUPPORTED;
    }
}

void esp32p4_sdmmc_init(void) {
    printf("sdmmc: [paso1] locks+bus\n");
    sdmmc_bus_init();
    sd_card_lock_init();

    printf("sdmmc: [paso2] pads\n");
    if (!sd_pads_init()) { printf("sdmmc: pads init failed\n"); return; }
    printf("sdmmc: [paso3] controller\n");
    if (!sd_controller_init()) { printf("sdmmc: controller init failed (hwerr=0x%x)\n", sd_hwerr); return; }

    printf("sdmmc: [paso4] identify slot0\n");
    if (!sd_identify()) {
        printf("sdmmc: no SD en slot0 (%s, hwerr=0x%x); host listo para SDIO slot1\n",
               sd_fail ? sd_fail : "?", sd_hwerr);
        return;
    }
    printf("sdmmc: SD presente: %u bloques (%u MB), %s, %u-bit, RCA %u\n",
           sd_blocks, (unsigned)((uint64_t)sd_blocks * SD_BLOCK_SIZE / (1024 * 1024)),
           sd_hc ? "SDHC/SDXC" : "SDSC", sd_width_card, sd_rca);

    bio_initialize_bdev(&sd_dev.dev, "sd0", SD_BLOCK_SIZE, sd_blocks, 0, NULL, BIO_FLAGS_NONE);
    sd_dev.dev.read = sd_bio_read;
    sd_dev.dev.read_block = sd_bio_read_block;
    sd_dev.dev.write = sd_bio_write;
    sd_dev.dev.write_block = sd_bio_write_block;
    sd_dev.dev.ioctl = sd_bio_ioctl;
    bio_register_device(&sd_dev.dev);
    sd_bio_registered = true;
    printf("sdmmc: bio 'sd0' registrado\n");

    /* Montado RW para poder servir PUT/DELETE del httpd y guardar kernel.json. */
    status_t err = fs_mount("/sd", "fat", "sd0", FS_MOUNT_OPTION_NONE);
    if (err == NO_ERROR) printf("sdmmc: FAT montado en /sd (sd0, RW)\n");
    else printf("sdmmc: no se pudo montar FAT en /sd (err %d)\n", err);
}

static void sdmmc_init_hook(uint level) {
    (void)level;
    esp32p4_sdmmc_init();
}

LK_INIT_HOOK(esp32p4_sdmmc, sdmmc_init_hook, LK_INIT_LEVEL_TARGET);
