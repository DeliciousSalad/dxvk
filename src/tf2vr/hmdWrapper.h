#pragma once

#include "openxr/openxr.h"
#include <mutex>

// Note: SourceEngineState enum moved to VRCompositor.h to avoid circular dependencies

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
void CheckAndCopyTrackedVGUITexture();  // Internal texture copying function
std::timed_mutex* GetPresentSyncMutex();  // Synchronization mutex for atomic texture copying

// Simple single-mutex blocking approach

// VGUI Rendering Completion Hooks (for perfect texture capture timing)
extern "C" void __declspec(dllexport) TF2VR_NotifyVGUIPaintComplete();  // Called right after VGui_Paint() completes
extern "C" void __declspec(dllexport) TF2VR_NotifyVGUIPresentComplete();  // Called right after vkQueuePresentKHR() completes

// Compositor-Controlled Synchronization (mirrors OpenXR BeginFrame pattern)
extern "C" void __declspec(dllexport) TF2VR_CompositorBeginTextureSync();  // Called by VR compositor to request texture sync