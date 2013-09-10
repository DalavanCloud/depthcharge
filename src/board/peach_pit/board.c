/*
 * Copyright 2013 Google Inc.
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include "base/init_funcs.h"
#include "drivers/bus/i2c/exynos5_usi.h"
#include "drivers/bus/i2s/exynos5.h"
#include "drivers/bus/spi/exynos5.h"
#include "drivers/ec/cros/spi.h"
#include "drivers/flash/spi.h"
#include "drivers/gpio/exynos5420.h"
#include "drivers/power/exynos.h"
#include "drivers/sound/i2s.h"
#include "drivers/sound/route.h"
#include "drivers/sound/sound.h"
#include "drivers/storage/blockdev.h"
#include "drivers/storage/dw_mmc.h"
#include "drivers/tpm/tis_i2c.h"
#include "vboot/util/flag.h"

static int board_setup(void)
{
	Exynos5420Gpio *ec_in_rw = new_exynos5420_gpio_input(GPIO_X, 2, 3);
	if (!ec_in_rw)
		return 1;
	if (flag_install(FLAG_ECINRW, &ec_in_rw->ops))
		return 1;

	Exynos5UsiI2c *i2c9 =
		new_exynos5_usi_i2c((void *)(uintptr_t)0x12e10000, 400000);
	if (!i2c9)
		return 1;

	tis_set_i2c_bus(&i2c9->ops);

	Exynos5Spi *spi1 = new_exynos5_spi((void *)(uintptr_t)0x12d30000);
	Exynos5Spi *spi2 = new_exynos5_spi((void *)(uintptr_t)0x12d40000);
	if (!spi1 || !spi2)
		return 1;

	CrosEcSpiBus *cros_ec_spi_bus = new_cros_ec_spi_bus(&spi2->ops);
	if (!cros_ec_spi_bus)
		return 1;
	cros_ec_set_bus(&cros_ec_spi_bus->ops);

	SpiFlash *flash = new_spi_flash(&spi1->ops, 0x400000);
	if (!flash || flash_set_ops(&flash->ops))
		return 1;

	Exynos5I2s *i2s0 = new_exynos5_i2s_multi(
		(void *)(uintptr_t)0x03830000, 16, 2, 256);
	I2sSource *i2s_source = new_i2s_source(&i2s0->ops, 48000, 2, 16000);
	if (!i2s_source)
		return 1;
	SoundRoute *sound_route = new_sound_route(&i2s_source->ops);
	if (!sound_route)
		return 1;
	if (sound_set_ops(&sound_route->ops))
		return 1;

	DwmciHost *emmc = new_dwmci_host((void *)(uintptr_t)0x12200000,
					 100000000, 8, 0,
					 DWMCI_SET_SAMPLE_CLK(1) |
					 DWMCI_SET_DRV_CLK(3) |
					 DWMCI_SET_DIV_RATIO(3));
	DwmciHost *sd_card = new_dwmci_host((void *)(uintptr_t)0x12220000,
					    100000000, 4, 1,
					    DWMCI_SET_SAMPLE_CLK(1) |
					    DWMCI_SET_DRV_CLK(2) |
					    DWMCI_SET_DIV_RATIO(3));
	if (!emmc || !sd_card)
		return 1;
	list_insert_after(&emmc->mmc.ctrlr.list_node,
			  &fixed_block_dev_controllers);
	list_insert_after(&sd_card->mmc.ctrlr.list_node,
			  &removable_block_dev_controllers);

	if (power_set_ops(&exynos_power_ops))
		return 1;

	return 0;
}

INIT_FUNC(board_setup);
