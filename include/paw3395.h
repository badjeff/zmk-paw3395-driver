/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/drivers/sensor.h>
#include "pixart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Helper macros used to convert sensor values. */
#define PAW3395_SVALUE_TO_CPI(svalue) ((uint32_t)(svalue).val1)

/** @brief Sensor specific attributes of PAW3395. */
enum paw3395_attribute {

	/** Sensor CPI for both X and Y axes. */
	PAW3395_ATTR_CPI,

};

#ifdef __cplusplus
}
#endif
