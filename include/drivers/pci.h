#ifndef OWNKRN_DRIVERS_PCI_H
#define OWNKRN_DRIVERS_PCI_H

#include <kernel/types.h>

void   pci_init(void);
size_t pci_format_proc(char *buf, size_t cap);

#endif /* OWNKRN_DRIVERS_PCI_H */
