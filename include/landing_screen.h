#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Create the landing and WiFi screens and show the landing screen. */
void landing_screen_init();
/** Refresh the landing screen with the latest service data. */
void landing_screen_tick();

#ifdef __cplusplus
}
#endif
