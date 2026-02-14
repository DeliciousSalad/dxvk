#include "HMDInterface.h"

#include "OpenXRDirectMode.h"
#include "VRCompositor.h"  // For SourceEngineState enum

#include "hmdWrapper.h"
#include "../util/log/log.h"

HMDInterface *g_pHMDInterface = NULL;

HMDInterface* HMDInterface::Get()
{
	if (g_pHMDInterface == NULL) {
		g_pHMDInterface = new OpenXRDirectMode();
	}
	return g_pHMDInterface;
}

void notimplemented(const char *function)
{
	char buffer[256];
	sprintf_s(buffer, "Function: %s   is currently not implemented!!\n", function);
	OutputDebugString(buffer);
}

extern "C" bool __declspec(dllexport) dxvkInitOpenXR(XrInstance instance, XrSystemId systemId, XrSession session, XrSpace referenceSpace, XrSpace headSpace)
{
	bool result = g_pHMDInterface->Init(instance, systemId, session, referenceSpace, headSpace);
	
	// Initialize VR compositor with the OpenXR manager
	if (result) {
		InitVRCompositor(static_cast<OpenXRDirectMode*>(g_pHMDInterface));
	}
	
	return result;
}

extern "C" void __declspec(dllexport) dxvkSetSessionFocused(bool focused)
{
	// Session focus is now detected directly via xrPollEvent in VRControllerModel
	// This function is kept for backwards compatibility but is no longer needed
	auto* openxr = static_cast<OpenXRDirectMode*>(g_pHMDInterface);
	if (openxr) {
		openxr->SetSessionFocused(focused);
	}
}

extern "C" bool __declspec(dllexport) dxvkIsSessionFocused()
{
	auto* openxr = static_cast<OpenXRDirectMode*>(g_pHMDInterface);
	if (openxr) {
		return openxr->IsSessionFocused();
	}
	return true;  // Default to focused if no manager
}

extern "C" bool __declspec(dllexport) dxvkBeginFrame()
{
	// Don't allow normal VR hooks when compositor is active
	if (IsVRCompositorActive()) {
		return false; // Compositor handles frame timing
	}
	
	return g_pHMDInterface->BeginFrame();
}

extern "C" bool __declspec(dllexport) dxvkEndFrame()
{
	// Don't allow normal VR hooks when compositor is active
	if (IsVRCompositorActive()) {
		return false; // Compositor handles frame timing
	}
	
	return g_pHMDInterface->EndFrame();
}

extern "C" void __declspec(dllexport) dxvkGetRecommendedRenderTargetSize(uint32_t& width, uint32_t& height)
{
	g_pHMDInterface->GetRecommendedRenderTargetSize(&width, &height);
}

extern "C" void __declspec(dllexport) dxvkGetPredictedDisplayTime(XrTime& time)
{
	g_pHMDInterface->GetPredictedDisplayTime(time);
}

extern "C" void __declspec(dllexport) dxvkGetViews(XrView*& views, XrSpaceLocation& headLocation, uint32_t& viewCount)
{
	g_pHMDInterface->GetViews(views, headLocation, viewCount);
}

extern "C" void __declspec(dllexport) dxvkSetRenderTextureSize(uint32_t width, uint32_t height, int msaa)
{
	g_pHMDInterface->SetRenderTextureSize(width, height, msaa);
}

extern "C" void __declspec(dllexport) dxvkSetLaserActiveHand(bool isLeftHand)
{
	auto* openxr = static_cast<OpenXRDirectMode*>(g_pHMDInterface);
	if (openxr) {
		auto* compositor = openxr->GetVRCompositor();
		if (compositor) {
			compositor->SetLaserActiveHand(isLeftHand);
		}
	}
}

extern "C" void __declspec(dllexport) dxvkSetControllerAimPose(bool isLeftHand, const XrPosef* pose)
{
	auto* openxr = static_cast<OpenXRDirectMode*>(g_pHMDInterface);
	if (openxr) {
		auto* compositor = openxr->GetVRCompositor();
		if (compositor && pose) {
			compositor->SetControllerAimPose(isLeftHand, *pose);
		}
	}
}

extern "C" void __declspec(dllexport) dxvkSetLaserColor(float r, float g, float b)
{
	auto* openxr = static_cast<OpenXRDirectMode*>(g_pHMDInterface);
	if (openxr) {
		auto* compositor = openxr->GetVRCompositor();
		if (compositor) {
			compositor->SetLaserColor(r, g, b);
		}
	}
}

extern "C" void __declspec(dllexport) dxvkSetLaserLength(float lengthMeters)
{
	auto* openxr = static_cast<OpenXRDirectMode*>(g_pHMDInterface);
	if (openxr) {
		auto* compositor = openxr->GetVRCompositor();
		if (compositor) {
			compositor->SetLaserLength(lengthMeters);
		}
	}
}

extern "C" void __declspec(dllexport) dxvkSetLaserWidth(float widthMeters)
{
	auto* openxr = static_cast<OpenXRDirectMode*>(g_pHMDInterface);
	if (openxr) {
		auto* compositor = openxr->GetVRCompositor();
		if (compositor) {
			compositor->SetLaserWidth(widthMeters);
		}
	}
}

extern "C" void __declspec(dllexport) dxvkSetAimSpaces(XrSpace leftAimSpace, XrSpace rightAimSpace)
{
	auto* openxr = static_cast<OpenXRDirectMode*>(g_pHMDInterface);
	if (openxr) {
		auto* compositor = openxr->GetVRCompositor();
		if (compositor) {
			compositor->SetAimSpaces(leftAimSpace, rightAimSpace);
		}
	}
}

extern "C" void __declspec(dllexport) dxvkSetLaserIntersectionLength(float lengthMeters)
{
	auto* openxr = static_cast<OpenXRDirectMode*>(g_pHMDInterface);
	if (openxr) {
		auto* compositor = openxr->GetVRCompositor();
		if (compositor) {
			compositor->SetLaserIntersectionLength(lengthMeters);
		}
	}
}