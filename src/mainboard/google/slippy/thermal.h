/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2012 Google Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef THERMAL_H
#define THERMAL_H

/* Config TDP Sensor ID */
#define CTDP_SENSOR_ID			0 /* PECI */

/* Config TDP Nominal */
#define CTDP_NOMINAL_THRESHOLD_OFF	0
#define CTDP_NOMINAL_THRESHOLD_ON	0

/* Config TDP Down */
#define CTDP_DOWN_THRESHOLD_OFF		80
#define CTDP_DOWN_THRESHOLD_ON		90

/* Temperature which OS will shutdown at */
#define CRITICAL_TEMPERATURE		104

/* Temperature which OS will throttle CPU */
#define PASSIVE_TEMPERATURE		100

/* Tj_max value for calculating PECI CPU temperature */
#define MAX_TEMPERATURE			105

#endif
