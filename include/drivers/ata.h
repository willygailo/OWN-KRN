#ifndef OWNKRN_DRIVERS_ATA_H
#define OWNKRN_DRIVERS_ATA_H

#include <kernel/types.h>

#define ATA_SECTOR_SIZE 512U

bool     ata_init(void);
bool     ata_is_present(void);
uint32_t ata_sector_count(void);
int      ata_read_sectors(uint32_t lba, uint8_t count, void *buf);
int      ata_write_sectors(uint32_t lba, uint8_t count, const void *buf);
int      ata_flush(void);

#endif /* OWNKRN_DRIVERS_ATA_H */
