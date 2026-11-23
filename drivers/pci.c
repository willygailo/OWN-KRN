#include <arch/io.h>
#include <drivers/pci.h>
#include <kernel/printk.h>
#include <lib/printf.h>
#include <lib/string.h>

#define MAX_PCI_DEVS 32

struct pci_dev {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  rev;
    uint8_t  irq;
    uint32_t bar0;
};

static struct pci_dev g_pci_devs[MAX_PCI_DEVS];
static size_t         g_pci_count = 0;

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = 0x80000000U |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    (offset & 0xFCU);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static const char *pci_class_name(uint8_t cls, uint8_t sub) {
    if (cls == 0x01 && sub == 0x01) return "IDE interface";
    if (cls == 0x01 && sub == 0x06) return "SATA controller";
    if (cls == 0x01)                return "Mass storage controller";
    if (cls == 0x02 && sub == 0x00) return "Ethernet controller";
    if (cls == 0x03 && sub == 0x00) return "VGA compatible controller";
    if (cls == 0x04)                return "Multimedia audio controller";
    if (cls == 0x06 && sub == 0x00) return "Host bridge";
    if (cls == 0x06 && sub == 0x01) return "ISA bridge";
    if (cls == 0x06 && sub == 0x80) return "Bridge";
    if (cls == 0x0C && sub == 0x03) return "USB controller";
    return "System peripheral";
}

static const char *pci_device_desc(uint16_t vendor, uint16_t device) {
    if (vendor == 0x8086 && device == 0x1237) return "Intel 440FX - 82441FX PMC [Natoma]";
    if (vendor == 0x8086 && device == 0x7000) return "Intel 82371SB PIIX3 ISA";
    if (vendor == 0x8086 && device == 0x7010) return "Intel 82371SB PIIX3 IDE";
    if (vendor == 0x8086 && device == 0x7020) return "Intel 82371SB PIIX3 USB UHCI";
    if (vendor == 0x8086 && device == 0x7113) return "Intel 82371AB/EB/MB PIIX4 ACPI";
    if (vendor == 0x8086 && device == 0x100E) return "Intel 82540EM Gigabit Ethernet";
    if (vendor == 0x1234 && device == 0x1111) return "QEMU Bochs VBE Graphics Adapter";
    if (vendor == 0x1AF4)                     return "Red Hat / VirtIO Device";
    if (vendor == 0x8086)                     return "Intel Corporation Device";
    return "PCI Device";
}

void pci_init(void) {
    g_pci_count = 0;

    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t id0 = pci_cfg_read32(0, slot, 0, 0x00);
        if ((id0 & 0xFFFFU) == 0xFFFFU) continue;

        uint32_t hdr0 = pci_cfg_read32(0, slot, 0, 0x0C);
        uint8_t max_func = ((hdr0 >> 16) & 0x80U) ? 8 : 1;

        for (uint8_t func = 0; func < max_func; func++) {
            uint32_t id = pci_cfg_read32(0, slot, func, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFFU);
            uint16_t device = (uint16_t)((id >> 16) & 0xFFFFU);
            if (vendor == 0xFFFFU) continue;
            if (g_pci_count >= MAX_PCI_DEVS) break;

            uint32_t cl = pci_cfg_read32(0, slot, func, 0x08);
            uint32_t b0 = pci_cfg_read32(0, slot, func, 0x10);
            uint32_t il = pci_cfg_read32(0, slot, func, 0x3C);

            struct pci_dev *d = &g_pci_devs[g_pci_count++];
            d->bus        = 0;
            d->slot       = slot;
            d->func       = func;
            d->vendor_id  = vendor;
            d->device_id  = device;
            d->rev        = (uint8_t)(cl & 0xFFU);
            d->prog_if    = (uint8_t)((cl >> 8) & 0xFFU);
            d->subclass   = (uint8_t)((cl >> 16) & 0xFFU);
            d->class_code = (uint8_t)((cl >> 24) & 0xFFU);
            d->bar0       = b0 & 0xFFFFFFF0U;
            d->irq        = (uint8_t)(il & 0xFFU);
        }
    }

    pr_ok("PCI Bus 0000:00 enumerated (%zu devices discovered)\n", g_pci_count);
}

size_t pci_format_proc(char *buf, size_t cap) {
    size_t pos = 0;
    for (size_t i = 0; i < g_pci_count && pos + 128 < cap; i++) {
        const struct pci_dev *d = &g_pci_devs[i];
        pos += (size_t)snprintf(buf + pos, cap - pos,
            "%02x:%02x.%x %s: %s [%04x:%04x] (rev %02x",
            d->bus, d->slot, d->func,
            pci_class_name(d->class_code, d->subclass),
            pci_device_desc(d->vendor_id, d->device_id),
            d->vendor_id, d->device_id, d->rev);
        if (d->bar0 != 0) {
            pos += (size_t)snprintf(buf + pos, cap - pos, ", bar0=0x%08x", d->bar0);
        }
        pos += (size_t)snprintf(buf + pos, cap - pos, ")\n");
    }
    return pos;
}
