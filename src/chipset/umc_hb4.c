/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		Implementation of the UMC HB4 "Super Energy Star Green" PCI Chipset.
 *
 *		Note: This chipset has no datasheet, everything were done via
 *		reverse engineering the BIOS of various machines using it.
 * 
 *		Note 2: Additional information were also used from all
 *		around the web.
 *
 * Authors:	Tiseno100,
 *		Miran Grca, <mgrca8@gmail.com>
 *
 *		Copyright 2021 Tiseno100.
 *		Copyright 2021 Miran Grca.
 */

/*
   UMC HB4 Configuration Registers

   Sources & Notes:
   Cache registers were found at Vogons: https://www.vogons.org/viewtopic.php?f=46&t=68829&start=20
   Basic Reverse engineering effort was done personally by me

   Warning: Register documentation may be inaccurate!

   UMC 8881x:

   Register 50:
   Bit 7: Enable L2 Cache
   Bit 6: Cache Policy (0: Write Thru / 1: Write Back)

   Bit 5-4 Cache Speed
       0 0 Read 3-2-2-2 Write 3T
       0 1 Read 3-1-1-1 Write 3T
       1 0 Read 2-2-2-2 Write 2T
       1 1 Read 2-1-1-1 Write 2T

   Bit 3 Cache Banks (0: 1 Bank / 1: 2 Banks)

   Bit 2-1-0 Cache Size
       0 0 0 0KB
       0 0 1 64KB
       x-x-x Multiplications of 2(64*2 for 0 1 0) till 2MB

   Register 51:
   Bit 7-6 DRAM Read Speed
       5-4 DRAM Write Speed
       0 0 1 Waits
       0 1 1 Waits
       1 0 1 Wait
       1 1 0 Waits

   Bit 3 Resource Lock Enable
   Bit 2 Graphics Adapter (0: VL Bus / 1: PCI Bus)
   Bit 1 L1 WB Policy (0: WT / 1: WB)
   Bit 0 L2 Cache Tag Lenght (0: 7 Bits / 1: 8 Bits)

   Register 52:
   Bit 7: Host-to-PCI Post Write (0: 1 Wait State / 1: 0 Wait States)

   Register 54:
   Bit 7: DC000-DFFFF Read Enable
   Bit 6: D8000-DBFFF Read Enable
   Bit 5: D4000-D7FFF Read Enable
   Bit 4: D0000-D3FFF Read Enable
   Bit 3: CC000-CFFFF Read Enable
   Bit 2: C8000-CBFFF Read Enable
   Bit 1: C0000-C7FFF Read Enable
   Bit 0: Enable C0000-DFFFF Shadow Segment Bits

   Register 55:
   Bit 7: E0000-FFFF Read Enable
   Bit 6: Shadow Write Status (1: Write Protect/0: Write)

   Register 56h & 57h: DRAM Bank 0 Configuration
   Register 58h & 59h: DRAM Bank 1 Configuration
  
   Register 60:
   Bit 5: If set and SMRAM is enabled, data cycles go to PCI and code cycles go to DRAM
   Bit 0: SMRAM Local Access Enable - if set, SMRAM is also enabled outside SMM
	  SMRAM appears to always be enabled in SMM, and always set to A0000-BFFFF.
*/

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include <86box/timer.h>
#include <86box/io.h>
#include <86box/device.h>

#include <86box/mem.h>
#include <86box/pci.h>
#include <86box/port_92.h>
#include <86box/smram.h>

#include <86box/chipset.h>


#ifdef ENABLE_HB4_LOG
int hb4_do_log = ENABLE_HB4_LOG;


static void
hb4_log(const char *fmt, ...)
{
    va_list ap;

    if (hb4_do_log) {
	va_start(ap, fmt);
	pclog_ex(fmt, ap);
	va_end(ap);
    }
}
#else
#define hb4_log(fmt, ...)
#endif


typedef struct hb4_t
{
    uint8_t	pci_conf[128];	/* PCI Registers */
    smram_t	*smram;		/* SMRAM Handler */
} hb4_t;


void
hb4_shadow(hb4_t *dev)
{
    mem_set_mem_state_both(0xe0000, 0x20000, ((dev->pci_conf[0x55] & 0x80) ? MEM_READ_INTERNAL : MEM_READ_EXTANY) |
					     ((dev->pci_conf[0x55] & 0x40) ? MEM_WRITE_EXTANY : MEM_WRITE_INTERNAL));

    if (dev->pci_conf[0x54] & 1) {
	if (dev->pci_conf[0x54] & 2)
		mem_set_mem_state_both(0xc0000, 0x8000, MEM_READ_INTERNAL | ((dev->pci_conf[0x55] & 0x40) ? MEM_WRITE_EXTANY :MEM_WRITE_INTERNAL));
	else
		mem_set_mem_state_both(0xc0000, 0x8000, MEM_READ_EXTANY | MEM_WRITE_EXTANY);

	for (int i = 0; i < 5; i++) {
		if ((dev->pci_conf[0x54] >> i) & 4)
			mem_set_mem_state_both(0xc8000 + (i << 14), 0x8000, MEM_READ_INTERNAL | ((dev->pci_conf[0x55] & 0x40) ? MEM_WRITE_EXTANY :MEM_WRITE_INTERNAL));
		else
			mem_set_mem_state_both(0xc8000 + (i << 14), 0x8000, MEM_READ_EXTANY | MEM_WRITE_EXTANY);
	}
    }

    flushmmucache_nopc();
}


static void
hb4_smram(hb4_t *dev)
{
    smram_disable_all();

    /* Bit 0, if set, enables SMRAM access outside SMM. SMRAM appears to be always enabled
       in SMM, and is always set to A0000-BFFFF. */
    smram_enable(dev->smram, 0x000a0000, 0x000a0000, 0x20000, dev->pci_conf[0x60] & 0x01, 1);

    /* Bit 5 seems to set data to go to PCI and code to DRAM. The Samsung SPC7700P-LW uses
       this. */
    if (dev->pci_conf[0x60] & 0x20) {
	if (dev->pci_conf[0x60] & 0x01)
		mem_set_mem_state_smram_ex(0, 0x000a0000, 0x20000, 0x02);
	mem_set_mem_state_smram_ex(1, 0x000a0000, 0x20000, 0x02);
    }
}


static void
hb4_write(int func, int addr, uint8_t val, void *priv)
{
    hb4_t *dev = (hb4_t *)priv;
    hb4_log("UM8881: dev->regs[%02x] = %02x POST: %02x \n", addr, val, inb(0x80));

    switch (addr) {
	case 0x04: case 0x05:
		dev->pci_conf[addr] = val;
		break;

	case 0x07:
		dev->pci_conf[addr] &= ~(val & 0xf9);
		break;

	case 0x0c: case 0x0d:
		dev->pci_conf[addr] = val;
		break;

	case 0x50:
		dev->pci_conf[addr] = ((val & 0xf8) | 4); /* Hardcode Cache Size to 512KB */
		cpu_cache_ext_enabled = !!(val & 0x80);	  /* Fixes freezing issues on the HOT-433A*/
		cpu_update_waitstates();
		break;

	case 0x51: case 0x52:
	case 0x53:
		dev->pci_conf[addr] = val;
		break;

	case 0x54: case 0x55:
		dev->pci_conf[addr] = val;
		hb4_shadow(dev);
		break;

	case 0x56 ... 0x5f:
		dev->pci_conf[addr] = val;
		break;

	case 0x60:
		dev->pci_conf[addr] = val;
		hb4_smram(dev);
		break;

	case 0x61:
		dev->pci_conf[addr] = val;
		break;
    }
}


static uint8_t
hb4_read(int func, int addr, void *priv)
{
    hb4_t *dev = (hb4_t *)priv;

    return dev->pci_conf[addr];
}


static void
hb4_reset(void *priv)
{
    hb4_t *dev = (hb4_t *)priv;

    dev->pci_conf[0] = 0x60; /* UMC */
    dev->pci_conf[1] = 0x10;

    dev->pci_conf[2] = 0x81; /* 8881x */
    dev->pci_conf[3] = 0x88;

    dev->pci_conf[7] = 2;

    dev->pci_conf[8] = 4;

    dev->pci_conf[0x09] = 0x00;
    dev->pci_conf[0x0a] = 0x00;
    dev->pci_conf[0x0b] = 0x06;

    dev->pci_conf[0x51] = 1;
    dev->pci_conf[0x52] = 1;
    dev->pci_conf[0x5a] = 4;
    dev->pci_conf[0x5c] = 0xc0;
    dev->pci_conf[0x5d] = 0x20;
    dev->pci_conf[0x5f] = 0xff;

    hb4_shadow(dev);
    hb4_write(0, 0x60, 0x20, dev);
}


static void
hb4_close(void *priv)
{
    hb4_t *dev = (hb4_t *)priv;

    free(dev);
}


static void *
hb4_init(const device_t *info)
{
    hb4_t *dev = (hb4_t *)malloc(sizeof(hb4_t));
    memset(dev, 0, sizeof(hb4_t));

    pci_add_card(PCI_ADD_NORTHBRIDGE, hb4_read, hb4_write, dev);	/* Device 10: UMC 8881x */

    /* Port 92 */
    device_add(&port_92_pci_device);

    /* SMRAM */
    dev->smram = smram_add();

    hb4_reset(dev);

    return dev;
}


const device_t umc_hb4_device = {
    "UMC HB4(8881F)",
    DEVICE_PCI,
    0x886a,
    hb4_init, hb4_close, hb4_reset,
    { NULL }, NULL, NULL,
    NULL
};
