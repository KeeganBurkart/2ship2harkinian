#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"
#include "MouseCamera.h"

static float sMouseDeltaX = 0.0f;
static float sMouseDeltaY = 0.0f;
static bool sMouseActive = false;

extern "C" float MouseCamera_GetDeltaX() {
    return sMouseDeltaX;
}

extern "C" float MouseCamera_GetDeltaY() {
    return sMouseDeltaY;
}

extern "C" int MouseCamera_IsActive() {
    return sMouseActive;
}

void RegisterMouseCamera() {
    COND_HOOK(OnGameStateMainStart, true, []() {
        static bool sInitialCaptureApplied = false;
        std::shared_ptr<Ship::Window> window = Ship::Context::GetRawInstance()->GetWindow();

        // Auto-capture is otherwise only applied when the menu is toggled, so apply it once on the first frame
        if (!sInitialCaptureApplied) {
            window->GetMouseStateManager()->UpdateMouseCapture();
            sInitialCaptureApplied = true;
        }

        // Always read the delta so movement made while inactive doesn't arrive as one big jump later
        Ship::Coords delta = window->GetMouseDelta();
        sMouseActive = CVarGetInteger("gSettings.EnableMouse", 0) && window->IsMouseCaptured();
        sMouseDeltaX = sMouseActive ? delta.x : 0.0f;
        sMouseDeltaY = sMouseActive ? delta.y : 0.0f;
    });
}

static RegisterShipInitFunc initFunc(RegisterMouseCamera);
