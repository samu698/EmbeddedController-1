/* Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#define DT_DRV_COMPAT cros_lis2dw12_emul

#include <device.h>
#include <drivers/i2c.h>
#include <drivers/i2c_emul.h>
#include <emul.h>
#include <errno.h>
#include <sys/__assert.h>

#include "driver/accel_lis2dw12.h"
#include "emul/emul_common_i2c.h"
#include "emul/emul_lis2dw12.h"
#include "i2c.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(lis2dw12_emul, CONFIG_LIS2DW12_EMUL_LOG_LEVEL);

struct lis2dw12_emul_data {
	/** Common I2C data */
	struct i2c_common_emul_data common;
};

struct lis2dw12_emul_cfg {
	/** Common I2C config */
	struct i2c_common_emul_cfg common;
};

static struct i2c_emul_api lis2dw12_emul_api_i2c = {
	.transfer = i2c_common_emul_transfer,
};

static int emul_lis2dw12_init(const struct emul *emul,
			      const struct device *parent)
{
	const struct lis2dw12_emul_cfg *lis2dw12_cfg = emul->cfg;
	const struct i2c_common_emul_cfg *cfg = &(lis2dw12_cfg->common);
	struct lis2dw12_emul_data *data = emul->data;

	data->common.emul.api = &lis2dw12_emul_api_i2c;
	data->common.emul.addr = cfg->addr;
	data->common.emul.parent = emul;
	data->common.i2c = parent;
	data->common.cfg = cfg;
	i2c_common_emul_init(&data->common);

	return i2c_emul_register(parent, emul->dev_label, &data->common.emul);
}

#define INIT_LIS2DW12(n)                                                  \
	static struct lis2dw12_emul_data lis2dw12_emul_data_##n = {       \
		.common = {                                               \
		},                                                        \
	};                                                                \
	static const struct lis2dw12_emul_cfg lis2dw12_emul_cfg_##n = {   \
		.common = {                                               \
			.i2c_label = DT_INST_BUS_LABEL(n),                \
			.dev_label = DT_INST_LABEL(n),                    \
			.addr = DT_INST_REG_ADDR(n),                      \
		},                                                        \
	};                                                                \
	EMUL_DEFINE(emul_lis2dw12_init, DT_DRV_INST(n),                   \
		    &lis2dw12_emul_cfg_##n, &lis2dw12_emul_data_##n)

DT_INST_FOREACH_STATUS_OKAY(INIT_LIS2DW12)
