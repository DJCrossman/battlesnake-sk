#pragma once

// The three ways Snake Pad talks to the game. Each runs until the player holds Back.

/** WiFi link: a WiFi Devboard on the GPIO header holds the connection to the game server, and
 * the Flipper sends it the settings from the SD card and the button bytes. Needs a hotspot. */
void wifi_link_run(void);

/** Bluetooth serial link: sends button bytes and shows the board the pad page writes back.
 * Needs Web Bluetooth (Chrome on Android). */
void board_link_run(void);

/** Bluetooth keyboard link: buttons become arrow (or WASD) key taps. Works with any phone. */
void keyboard_link_run(void);
