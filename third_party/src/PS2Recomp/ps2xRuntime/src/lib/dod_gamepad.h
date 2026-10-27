#pragma once
#include <cstdint>

// SDL is owned exclusively by the host/window thread. The IOP only publishes
// motor requests; it never waits for USB/Bluetooth I/O.
void dodConfigureGamepad(bool rumbleEnabled);
void dodRequestPadRumble(int port, uint8_t large, uint8_t small);
bool dodPollGamepad(uint8_t *data32, bool focused);
bool dodGamepadBackendReady();
void dodShutdownGamepad();
