#pragma once
enum { KEY_NULL, FLAG_VSYNC_HINT };
inline bool IsAudioDeviceReady() { return false; }
inline void SetMasterVolume(float) {}
inline void SetExitKey(int) {}
inline void SetWindowSize(int, int) {}
inline void ToggleBorderlessWindowed() {}
inline void ClearWindowState(unsigned) {}
inline void SetTargetFPS(int) {}
inline int GetScreenWidth() { return 640; }
inline int GetScreenHeight() { return 448; }
inline int GetCurrentMonitor() { return 0; }
inline int GetMonitorWidth(int) { return 1920; }
inline int GetMonitorHeight(int) { return 1080; }
