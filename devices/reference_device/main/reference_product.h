/*
 * reference_product — Golden Peripheral product logic (docs §53..#57).
 *
 * Hardware: 1 LED (GPIO8) + 1 button (GPIO9).
 * Commands: set_led, get_state.
 * Events: button_pressed, state_changed.
 */
#ifndef REFERENCE_PRODUCT_H
#define REFERENCE_PRODUCT_H

#include "device_app.h"

#ifdef __cplusplus
extern "C" {
#endif

const device_app_profile_t *reference_product_profile(void);

#ifdef __cplusplus
}
#endif

#endif /* REFERENCE_PRODUCT_H */
