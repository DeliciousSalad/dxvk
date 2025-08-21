#pragma once

#include "openxr/openxr.h"

// Source Engine State for VR Compositor
enum SourceEngineState {
    SOURCE_STATE_GAMEPLAY = 0,     // Normal game - Source handles VR
    SOURCE_STATE_MENU = 1,         // Main menu - compositor takes over
    SOURCE_STATE_LOADING = 2,      // Loading screen - compositor takes over
    SOURCE_STATE_TRANSITION = 3    // Brief transitions
};

extern "C" bool __declspec(dllexport) dxvkInitOpenXR(XrInstance instance, XrSystemId systemId, XrSession session, XrSpace referenceSpace, XrSpace headSpace);
extern "C" void __declspec(dllexport) dxvkShutdownOpenXR();
extern "C" void __declspec(dllexport) dxvkSetRenderTextureSize(uint32_t width, uint32_t height, int msaa);
extern "C" bool __declspec(dllexport) dxvkBeginFrame();
extern "C" bool __declspec(dllexport) dxvkEndFrame();
extern "C" void __declspec(dllexport) dxvkGetPredictedDisplayTime(XrTime& time);
extern "C" void __declspec(dllexport) dxvkGetViews(XrView*& views, XrSpaceLocation& headLocation, uint32_t& viewCount);

// New VR Compositor State Management
extern "C" void __declspec(dllexport) dxvkSetSourceState(int state);
extern "C" bool __declspec(dllexport) dxvkIsCompositorActive();
extern "C" void __declspec(dllexport) dxvkSubmitMenuFrame(void* textureHandle, int width, int height);

// VR Compositor Internal Functions (not exported, but declared for linking)
void InitVRCompositor(class OpenXRDirectMode* manager);
bool IsVRCompositorActive();  // Internal version (calls the implementation directly)
