#include "filesystem/blockdev.h"

#include <stddef.h>

#include "debug/log.h"
#include "platform/io.h"

#define ATA_PRIMARY_IO_BASE 0x1F0U
#define ATA_PRIMARY_CTRL    0x3F6U
#define ATA_SECONDARY_IO_BASE 0x170U
#define ATA_SECONDARY_CTRL    0x376U

#define ATA_REG_DATA     0U
#define ATA_REG_SECCOUNT 2U
#define ATA_REG_LBA0     3U
#define ATA_REG_LBA1     4U
#define ATA_REG_LBA2     5U
#define ATA_REG_HDDEVSEL 6U
#define ATA_REG_COMMAND  7U
#define ATA_REG_STATUS   7U

#define ATA_CMD_READ_SECTORS  0x20U
#define ATA_CMD_WRITE_SECTORS 0x30U
#define ATA_CMD_IDENTIFY      0xECU

#define ATA_STATUS_ERR  0x01U
#define ATA_STATUS_DRQ  0x08U
#define ATA_STATUS_DF   0x20U
#define ATA_STATUS_BSY  0x80U

#define ATA_WAIT_LIMIT 1000000U
#define ATA_LBA28_LIMIT 0x10000000U

typedef struct {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t slave;
    uint32_t sector_count;
} AtaDisk;

static const AtaDisk probe_targets[] = {
    {ATA_PRIMARY_IO_BASE, ATA_PRIMARY_CTRL, 0, 0},
    {ATA_PRIMARY_IO_BASE, ATA_PRIMARY_CTRL, 1, 0},
    {ATA_SECONDARY_IO_BASE, ATA_SECONDARY_CTRL, 0, 0},
    {ATA_SECONDARY_IO_BASE, ATA_SECONDARY_CTRL, 1, 0},
};

static AtaDisk disks[BLOCKDEV_MAX_DRIVES];
static uint32_t drive_count;
static uint32_t active_drive;
static uint32_t volume_start_sector;
static uint32_t volume_sector_count;
static int disk_ready;

static uint8_t ata_status(const AtaDisk* disk) {
    return inb((uint16_t)(disk->io_base + ATA_REG_STATUS));
}

static uint8_t ata_alt_status(const AtaDisk* disk) {
    return inb(disk->ctrl_base);
}

static void ata_delay_400ns(const AtaDisk* disk) {
    (void)ata_alt_status(disk);
    (void)ata_alt_status(disk);
    (void)ata_alt_status(disk);
    (void)ata_alt_status(disk);
}

static int ata_wait_ready(const AtaDisk* disk) {
    for (uint32_t guard = 0; guard < ATA_WAIT_LIMIT; guard++) {
        uint8_t status = ata_alt_status(disk);
        if (status == 0U || status == 0xFFU) {
            return -1;
        }
        if ((status & ATA_STATUS_BSY) == 0U) {
            return 0;
        }
    }

    return -1;
}

static int ata_wait_drq(const AtaDisk* disk) {
    for (uint32_t guard = 0; guard < ATA_WAIT_LIMIT; guard++) {
        uint8_t status = ata_status(disk);
        if (status == 0U || status == 0xFFU) {
            return -1;
        }
        if ((status & ATA_STATUS_BSY) != 0U) {
            continue;
        }
        if ((status & (ATA_STATUS_ERR | ATA_STATUS_DF)) != 0U) {
            return -1;
        }
        if ((status & ATA_STATUS_DRQ) != 0U) {
            return 0;
        }
    }

    return -1;
}

static int ata_identify(AtaDisk* disk) {
    uint16_t words[256];
    uint16_t io = disk->io_base;
    uint8_t status;

    outb((uint16_t)(io + ATA_REG_HDDEVSEL), (uint8_t)(0xA0U | (disk->slave << 4)));
    ata_delay_400ns(disk);
    outb((uint16_t)(io + ATA_REG_SECCOUNT), 0U);
    outb((uint16_t)(io + ATA_REG_LBA0), 0U);
    outb((uint16_t)(io + ATA_REG_LBA1), 0U);
    outb((uint16_t)(io + ATA_REG_LBA2), 0U);
    outb((uint16_t)(io + ATA_REG_COMMAND), ATA_CMD_IDENTIFY);
    status = ata_status(disk);

    if (status == 0U || status == 0xFFU || ata_wait_drq(disk) != 0) {
        return -1;
    }

    for (uint32_t index = 0; index < 256U; index++) {
        words[index] = inw((uint16_t)(io + ATA_REG_DATA));
    }

    /* Bit 15 identifies ATAPI devices, which this disk driver does not handle. */
    if ((words[0] & 0x8000U) != 0U || (words[49] & (1U << 9)) == 0U) {
        return -1;
    }

    disk->sector_count = (uint32_t)words[60] | ((uint32_t)words[61] << 16);
    if (disk->sector_count == 0U) {
        return -1;
    }
    if (disk->sector_count > ATA_LBA28_LIMIT) {
        disk->sector_count = ATA_LBA28_LIMIT;
    }

    return 0;
}

static int ata_select_lba(const AtaDisk* disk, uint32_t lba) {
    uint16_t io = disk->io_base;
    uint8_t selector = (uint8_t)(0xE0U | (disk->slave << 4) | ((lba >> 24) & 0x0FU));

    outb((uint16_t)(io + ATA_REG_HDDEVSEL), selector);
    ata_delay_400ns(disk);
    if (ata_wait_ready(disk) != 0) {
        return -1;
    }

    outb((uint16_t)(io + ATA_REG_SECCOUNT), 1U);
    outb((uint16_t)(io + ATA_REG_LBA0), (uint8_t)(lba & 0xFFU));
    outb((uint16_t)(io + ATA_REG_LBA1), (uint8_t)((lba >> 8) & 0xFFU));
    outb((uint16_t)(io + ATA_REG_LBA2), (uint8_t)((lba >> 16) & 0xFFU));
    return 0;
}

static int ata_read_sector_raw(const AtaDisk* disk, uint32_t sector, void* buffer) {
    uint16_t* destination = (uint16_t*)buffer;

    if (sector >= disk->sector_count || ata_select_lba(disk, sector) != 0) {
        return -1;
    }

    outb((uint16_t)(disk->io_base + ATA_REG_COMMAND), ATA_CMD_READ_SECTORS);
    if (ata_wait_drq(disk) != 0) {
        return -1;
    }

    for (uint32_t index = 0; index < BLOCKDEV_SECTOR_SIZE / 2U; index++) {
        destination[index] = inw((uint16_t)(disk->io_base + ATA_REG_DATA));
    }

    return 0;
}

static int ata_write_sector_raw(const AtaDisk* disk, uint32_t sector, const void* buffer) {
    const uint16_t* source = (const uint16_t*)buffer;

    if (sector >= disk->sector_count || ata_select_lba(disk, sector) != 0) {
        return -1;
    }

    outb((uint16_t)(disk->io_base + ATA_REG_COMMAND), ATA_CMD_WRITE_SECTORS);
    if (ata_wait_drq(disk) != 0) {
        return -1;
    }

    for (uint32_t index = 0; index < BLOCKDEV_SECTOR_SIZE / 2U; index++) {
        outw((uint16_t)(disk->io_base + ATA_REG_DATA), source[index]);
    }

    outb((uint16_t)(disk->io_base + ATA_REG_COMMAND), 0xE7U);
    return ata_wait_ready(disk);
}

int blockdev_init_disk(void) {
    drive_count = 0;
    active_drive = 0;
    disk_ready = 0;

    for (uint32_t target = 0; target < BLOCKDEV_MAX_DRIVES; target++) {
        AtaDisk candidate = probe_targets[target];
        if (ata_identify(&candidate) == 0) {
            disks[drive_count++] = candidate;
        }
    }

    if (drive_count == 0U) {
        ERROR_LOG("no ATA hard drives detected");
        return -1;
    }

    disk_ready = 1;
    volume_start_sector = 0;
    volume_sector_count = disks[active_drive].sector_count;
    DEBUG_LOG("ATA hard drives detected");
    return 0;
}

uint32_t blockdev_drive_count(void) {
    return drive_count;
}

uint32_t blockdev_current_drive(void) {
    return active_drive;
}

int blockdev_get_drive_info(uint32_t drive, blockdev_drive_info_t* out_info) {
    if (out_info == NULL || drive >= drive_count) {
        return -1;
    }

    out_info->sector_count = disks[drive].sector_count;
    return 0;
}

int blockdev_select_drive(uint32_t drive) {
    if (!disk_ready || drive >= drive_count) {
        return -1;
    }

    active_drive = drive;
    volume_start_sector = 0;
    volume_sector_count = disks[active_drive].sector_count;
    return 0;
}

int blockdev_read_sector(uint32_t sector, void* buffer) {
    if (!disk_ready || buffer == NULL || sector >= volume_sector_count) {
        return -1;
    }

    return ata_read_sector_raw(&disks[active_drive], volume_start_sector + sector, buffer);
}

int blockdev_write_sector(uint32_t sector, const void* buffer) {
    if (!disk_ready || buffer == NULL || sector >= volume_sector_count) {
        return -1;
    }

    return ata_write_sector_raw(&disks[active_drive], volume_start_sector + sector, buffer);
}

uint32_t blockdev_sector_count(void) {
    return volume_sector_count;
}

uint32_t blockdev_device_sector_count(void) {
    return disk_ready ? disks[active_drive].sector_count : 0U;
}

int blockdev_set_partition(uint32_t start_sector, uint32_t sector_count) {
    uint32_t drive_sectors = blockdev_device_sector_count();
    if (sector_count == 0U || start_sector >= drive_sectors ||
        sector_count > drive_sectors - start_sector) {
        return -1;
    }

    volume_start_sector = start_sector;
    volume_sector_count = sector_count;
    return 0;
}