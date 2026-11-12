#pragma once
// Headless, scriptable input for the F3 trainer tests; no graphics or runtime dependencies.
enum {
    KEY_UP = 1, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER, KEY_KP_ENTER, KEY_SPACE,
    KEY_ESCAPE, KEY_TAB, KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT, KEY_LEFT_CONTROL,
    KEY_RIGHT_CONTROL, KEY_F2, KEY_F3
};
inline int g_stubPressedKey = 0; // the key "pressed" in the current host frame (0 = none)
inline double g_stubTime = 0.0;
inline bool IsKeyDown(int) { return false; }
inline bool IsKeyPressed(int key) { return key == g_stubPressedKey; }
inline double GetTime() { return g_stubTime; }
inline float GetMouseWheelMove() { return 0.0f; }
