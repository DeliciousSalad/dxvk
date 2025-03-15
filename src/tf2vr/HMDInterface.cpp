#include "HMDInterface.h"

#include "OpenXRDirectMode.h"

#include "hmdWrapper.h"
#include "../util/log/log.h"

HMDInterface *g_pHMDInterface = NULL;

HMDInterface* HMDInterface::Get()
{
	g_pHMDInterface = new OpenXRDirectMode();

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
	return g_pHMDInterface->Init(instance, systemId, session, referenceSpace, headSpace);
}


extern "C" bool __declspec(dllexport) dxvkBeginFrame()
{
	return g_pHMDInterface->BeginFrame();
}

extern "C" bool __declspec(dllexport) dxvkEndFrame()
{
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

