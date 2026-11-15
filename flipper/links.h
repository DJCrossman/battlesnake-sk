#pragma once

// The two ways Snake Pad talks to the phone. Each runs until the player holds Back.

/** Bluetooth serial link: sends button bytes and shows the board the pad page writes back.
 * Needs Web Bluetooth (Chrome on Android). */
void board_link_run(void);

/** Bluetooth keyboard link: buttons become arrow (or WASD) key taps. Works with any phone. */
void keyboard_link_run(void);
