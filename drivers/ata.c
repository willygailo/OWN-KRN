#include <arch/io.h>
#include <drivers/ata.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/sync.h>

#define ATA_PRIMARY_DATA       0x1F0
#define ATA_PRIMARY_ERR        0x1F1
#define ATA_PRIMARY_SECCOUNT   0x1F2
#define ATA_PRIMARY_LBA_LO     0x1F3
#define ATA_PRIMARY_LBA_MID    0x1F4
#define ATA_PRIMARY_LBA_HI     0x1F5
#define ATA_PRIMARY_DRIVE_HEAD 0x1F6
#define ATA_PRIMARY_STATUS     0x1F7
#define ATA_PRIMARY_CMD        0x1F7
#define ATA_PRIMARY_ALTSTATUS  0x3F6

#define ATA_SR_BSY             0x80
#define ATA_SR_DRDY            0x40
#define ATA_SR_DF              0x20
#define ATA_SR_DRQ             0x08
#define ATA_SR_ERR             0x01

#define ATA_CMD_READ_PIO       0x20
#define ATA_CMD_WRITE_PIO      0x30
#define ATA_CMD_CACHE_FLUSH    0xE7
#define ATA_CMD_IDENTIFY       0xEC

static bool       g_ata_present = false;
static uint32_t   g_ata_sectors = 0;
static spinlock_t g_ata_lock = SPINLOCK_INIT;

static void ata_io_delay(void) {
    (void)inb(ATA_PRIMARY_ALTSTATUS);
    (void)inb(ATA_PRIMARY_ALTSTATUS);
    (void)inb(ATA_PRIMARY_ALTSTATUS);
    (void)inb(ATA_PRIMARY_ALTSTATUS);
}

static int ata_wait_not_busy(void) {
    for (int i = 0; i < 500000; i++) {
        uint8_t st = inb(ATA_PRIMARY_STATUS);
        if (st == 0xFF) return -ENODEV;
        if (!(st & ATA_SR_BSY)) return 0;
    }
    return -EIO;
}

static int ata_wait_drq(void) {
    for (int i = 0; i < 500000; i++) {
        uint8_t st = inb(ATA_PRIMARY_STATUS);
        if (st == 0xFF) return -ENODEV;
        if (st & ATA_SR_BSY) continue;
        if (st & (ATA_SR_ERR | ATA_SR_DF)) return -EIO;
        if (st & ATA_SR_DRQ) return 0;
    }
    return -EIO;
}

bool ata_init(void) {
    uint64_t irq = spin_lock_irqsave(&g_ata_lock);

    outb(ATA_PRIMARY_DRIVE_HEAD, 0xA0);
    ata_io_delay();
    outb(ATA_PRIMARY_SECCOUNT, 0);
    outb(ATA_PRIMARY_LBA_LO, 0);
    outb(ATA_PRIMARY_LBA_MID, 0);
    outb(ATA_PRIMARY_LBA_HI, 0);
    outb(ATA_PRIMARY_CMD, ATA_CMD_IDENTIFY);
    ata_io_delay();

    uint8_t st = inb(ATA_PRIMARY_STATUS);
    if (st == 0x00 || st == 0xFF) {
        g_ata_present = false;
        spin_unlock_irqrestore(&g_ata_lock, irq);
        pr_info("ATA0 Master   : not attached\n");
        return false;
    }

    if (ata_wait_not_busy() < 0 || ata_wait_drq() < 0) {
        g_ata_present = false;
        spin_unlock_irqrestore(&g_ata_lock, irq);
        return false;
    }

    uint16_t id[256];
    for (int i = 0; i < 256; i++) {
        id[i] = inw(ATA_PRIMARY_DATA);
    }

    g_ata_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    g_ata_present = (g_ata_sectors > 0);
    spin_unlock_irqrestore(&g_ata_lock, irq);

    if (g_ata_present) {
        pr_ok("ATA0 Master   : %u sectors (%u KiB) 28-bit LBA disk online\n",
              g_ata_sectors, (g_ata_sectors * ATA_SECTOR_SIZE) / 1024U);
    }
    return g_ata_present;
}

bool ata_is_present(void) {
    return g_ata_present;
}

uint32_t ata_sector_count(void) {
    return g_ata_sectors;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buf) {
    if (!g_ata_present || !buf || count == 0) return -ENODEV;
    if (lba + count > g_ata_sectors) return -EINVAL;

    uint64_t irq = spin_lock_irqsave(&g_ata_lock);
    if (ata_wait_not_busy() < 0) {
        spin_unlock_irqrestore(&g_ata_lock, irq);
        return -EIO;
    }

    outb(ATA_PRIMARY_DRIVE_HEAD, (uint8_t)(0xE0U | ((lba >> 24) & 0x0FU)));
    ata_io_delay();
    outb(ATA_PRIMARY_SECCOUNT, count);
    outb(ATA_PRIMARY_LBA_LO, (uint8_t)(lba & 0xFFU));
    outb(ATA_PRIMARY_LBA_MID, (uint8_t)((lba >> 8) & 0xFFU));
    outb(ATA_PRIMARY_LBA_HI, (uint8_t)((lba >> 16) & 0xFFU));
    outb(ATA_PRIMARY_CMD, ATA_CMD_READ_PIO);

    uint16_t *dst = (uint16_t *)buf;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_drq() < 0) {
            spin_unlock_irqrestore(&g_ata_lock, irq);
            return -EIO;
        }
        for (int w = 0; w < 256; w++) {
            *dst++ = inw(ATA_PRIMARY_DATA);
        }
    }

    spin_unlock_irqrestore(&g_ata_lock, irq);
    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf) {
    if (!g_ata_present || !buf || count == 0) return -ENODEV;
    if (lba + count > g_ata_sectors) return -EINVAL;

    uint64_t irq = spin_lock_irqsave(&g_ata_lock);
    if (ata_wait_not_busy() < 0) {
        spin_unlock_irqrestore(&g_ata_lock, irq);
        return -EIO;
    }

    outb(ATA_PRIMARY_DRIVE_HEAD, (uint8_t)(0xE0U | ((lba >> 24) & 0x0FU)));
    ata_io_delay();
    outb(ATA_PRIMARY_SECCOUNT, count);
    outb(ATA_PRIMARY_LBA_LO, (uint8_t)(lba & 0xFFU));
    outb(ATA_PRIMARY_LBA_MID, (uint8_t)((lba >> 8) & 0xFFU));
    outb(ATA_PRIMARY_LBA_HI, (uint8_t)((lba >> 16) & 0xFFU));
    outb(ATA_PRIMARY_CMD, ATA_CMD_WRITE_PIO);

    const uint16_t *src = (const uint16_t *)buf;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_drq() < 0) {
            spin_unlock_irqrestore(&g_ata_lock, irq);
            return -EIO;
        }
        for (int w = 0; w < 256; w++) {
            outw(ATA_PRIMARY_DATA, *src++);
        }
    }

    outb(ATA_PRIMARY_CMD, ATA_CMD_CACHE_FLUSH);
    (void)ata_wait_not_busy();

    spin_unlock_irqrestore(&g_ata_lock, irq);
    return 0;
}

int ata_flush(void) {
    if (!g_ata_present) return -ENODEV;
    uint64_t irq = spin_lock_irqsave(&g_ata_lock);
    outb(ATA_PRIMARY_DRIVE_HEAD, 0xE0);
    outb(ATA_PRIMARY_CMD, ATA_CMD_CACHE_FLUSH);
    int rc = ata_wait_not_busy();
    spin_unlock_irqrestore(&g_ata_lock, irq);
    return rc;
}
