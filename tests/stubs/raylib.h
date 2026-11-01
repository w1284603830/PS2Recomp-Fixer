#pragma once
// Headless input for the cheat parser tests; no graphics or runtime dependencies.
enum {
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER, KEY_KP_ENTER, KEY_SPACE,
    KEY_ESCAPE, KEY_TAB, KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT, KEY_LEFT_CONTROL,
    KEY_RIGHT_CONTROL, KEY_F2
};
inline bool IsKeyDown(int) { return false; }
inline bool IsKeyPressed(int) { return false; }
inline double GetTime() { return 0.0; }
inline float GetMouseWheelMove() { return 0.0f; }
