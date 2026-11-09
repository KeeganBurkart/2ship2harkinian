#ifndef MOUSE_CAMERA_H
#define MOUSE_CAMERA_H

#ifdef __cplusplus
extern "C" {
#endif

// Mouse movement captured for the current game frame, in pixels. Zero unless the mouse camera is enabled and the
// mouse is captured (menu closed, F2 toggles capture).
float MouseCamera_GetDeltaX(void);
float MouseCamera_GetDeltaY(void);
int MouseCamera_IsActive(void);

#ifdef __cplusplus
}
#endif

#endif // MOUSE_CAMERA_H
