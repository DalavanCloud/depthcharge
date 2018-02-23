/*
 * Copyright 2017 Google Inc.
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but without any warranty; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <libpayload.h>

#include "base/init_funcs.h"
#include "base/list.h"
#include "config.h"
#include "drivers/bus/i2c/designware.h"
#include "drivers/bus/i2c/i2c.h"
#include "drivers/ec/cros/lpc.h"
#include "drivers/flash/flash.h"
#include "drivers/flash/memmapped.h"
#include "drivers/gpio/kern.h"
#include "drivers/gpio/sysinfo.h"
#include "drivers/power/fch.h"
#include "drivers/soc/stoneyridge.h"
#include "drivers/sound/gpio_i2s.h"
#include "drivers/sound/max98357a.h"
#include "drivers/sound/route.h"
#include "drivers/storage/ahci.h"
#include "drivers/storage/blockdev.h"
#include "drivers/storage/sdhci.h"
#include "drivers/tpm/cr50_i2c.h"
#include "drivers/tpm/tpm.h"
#include "drivers/bus/usb/usb.h"
#include "vboot/util/flag.h"

#define EMMC_SD_CLOCK_MIN	400000
#define EMMC_CLOCK_MAX		200000000
#define SD_CLOCK_MAX		52000000

#define FLASH_SIZE		0x1000000
#define FLASH_START		( 0xffffffff - FLASH_SIZE + 1 )

#define DA7219_I2C_ADDR		0x1a

#define DA7219_DAI_CLK_MODE	0x2b
#define   DAI_CLK_EN		0x80

static int cr50_irq_status(void)
{
	return stoneyridge_get_gpe(22);
}

static void audio_setup(void)
{
	/* Setup da7219 on I2C0 */
	DesignwareI2c *i2c0 = new_designware_i2c(AP_I2C0_ADDR, 400000,
						 AP_I2C_CLK_MHZ);

	/* Clear DAI_CLK_MODE.dai_clk_en to set BCLK/WCLK pins as inputs */
	int ret = i2c_clear_bits(&i2c0->ops, DA7219_I2C_ADDR,
				 DA7219_DAI_CLK_MODE, DAI_CLK_EN);
	/*
	 * If we cannot clear DAI_CLK_EN, we may be fighting with it for
	 * control of BCLK/WCLK, so skip audio initialization.
	 */
	if (ret < 0) {
		printf("Failed to clear dai_clk_en (%d), skipping bit-bang i2s config\n",
		       ret);
		return;
	}

	KernGpio *i2s_bclk = new_kern_fch_gpio_output(140, 0);
	KernGpio *i2s_lrclk = new_kern_fch_gpio_output(144, 0);
	KernGpio *i2s2_data = new_kern_fch_gpio_output(143, 0);
	GpioI2s *i2s = new_gpio_i2s(
			&i2s_bclk->ops,		/* I2S Bit Clock GPIO */
			&i2s_lrclk->ops,	/* I2S Frame Sync GPIO */
			&i2s2_data->ops,	/* I2S Data GPIO */
			24000,			/* Sample rate, measured */
			2,			/* Channels */
			0x1FFF);		/* Volume */
	SoundRoute *sound_route = new_sound_route(&i2s->ops);

	KernGpio *spk_pa_en = new_kern_fch_gpio_output(119, 0);
	max98357aCodec *speaker_amp = new_max98357a_codec(&spk_pa_en->ops);

	list_insert_after(&speaker_amp->component.list_node,
			  &sound_route->components);

	sound_set_ops(&sound_route->ops);
}

static int board_setup(void)
{
	sysinfo_install_flags(NULL);
	CrosEcLpcBus *cros_ec_lpc_bus =
		new_cros_ec_lpc_bus(CROS_EC_LPC_BUS_GENERIC);
	CrosEc *cros_ec = new_cros_ec(&cros_ec_lpc_bus->ops, 0, NULL);
	register_vboot_ec(&cros_ec->vboot, 0);

	flash_set_ops(&new_mem_mapped_flash(FLASH_START, FLASH_SIZE)->ops);

	audio_setup();

	SdhciHost *emmc;
	if (IS_ENABLED(CONFIG_FORCE_BH720_SDHCI)) {
		emmc = new_pci_sdhci_host(PCI_DEV(1, 0, 0),
				SDHCI_PLATFORM_CLEAR_TRANSFER_BEFORE_CMD,
				EMMC_SD_CLOCK_MIN, EMMC_CLOCK_MAX);
	} else {
		emmc = new_pci_sdhci_host(PCI_DEV(0, 0x14, 7),
				SDHCI_PLATFORM_NO_EMMC_HS200 |
				SDHCI_PLATFORM_CLEAR_TRANSFER_BEFORE_CMD,
				EMMC_SD_CLOCK_MIN, EMMC_CLOCK_MAX);
	}
	list_insert_after(&emmc->mmc_ctrlr.ctrlr.list_node,
			&fixed_block_dev_controllers);

	/* Setup h1 on I2C1 */
	DesignwareI2c *i2c_h1 = new_designware_i2c(
		AP_I2C1_ADDR, 400000, AP_I2C_CLK_MHZ);
	tpm_set_ops(&new_cr50_i2c(&i2c_h1->ops, 0x50,
				  &cr50_irq_status)->base.ops);

	uintptr_t usb_base;
	UsbHostController *usb_ehci;
	UsbHostController *usb_xchi;
	usb_base = (uintptr_t)pci_read_config32(PCI_DEV(0, 18, 0),
			PCI_BASE_ADDRESS_0) & PCI_BASE_ADDRESS_MEM_MASK;
	if (usb_base){
		usb_ehci = new_usb_hc(EHCI, usb_base);
		list_insert_after(&usb_ehci->list_node, &usb_host_controllers);
	}

	usb_base = (uintptr_t)pci_read_config32(PCI_DEV(0, 16, 0),
			PCI_BASE_ADDRESS_0) & PCI_BASE_ADDRESS_MEM_MASK;
	if (usb_base){
		usb_xchi = new_usb_hc(XHCI, usb_base);
		list_insert_after(&usb_xchi->list_node, &usb_host_controllers);
	}

	power_set_ops(&kern_power_ops);

	return 0;
}

INIT_FUNC(board_setup);