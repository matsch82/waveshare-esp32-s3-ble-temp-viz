#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Board battery voltage on GPIO4 (ADC1 channel 3) through a 200k/200k
 * divider. On USB power it reads the charger voltage instead.
 */

/**
 * Measure the battery voltage (averaged, calibrated) and release the ADC.
 * @return millivolts, or -1 on error
 */
int battery_read_mv(void);

#ifdef __cplusplus
}
#endif
