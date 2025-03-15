#pragma once

extern "C" bool __declspec(dllexport) dxvkInitOpenXR(XrInstance instance, XrSystemId systemId, XrSession session, XrSpace space);
extern "C" void __declspec(dllexport) dxvkShutdownOpenXR();
extern "C" bool __declspec(dllexport) dxvkSetRenderTextureSize(uint32_t width, uint32_t height);
extern "C" bool __declspec(dllexport) dxvkBeginFrame();
extern "C" bool __declspec(dllexport) dxvkEndFrame();
extern "C" void __declspec(dllexport) dxvkGetPredictedDisplayTime(XrTime& time);
extern "C" void __declspec(dllexport) dxvkGetViews(XrView*& views, uint32_t& viewCount);
