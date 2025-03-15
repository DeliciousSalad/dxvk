
#ifndef HMDINTERFACE_H_INCLUDED
#define HMDINTERFACE_H_INCLUDED

#include "VkSubmitThreadCallback.h"
#include "stCommon.h"
#include "../d3d9/d3d9_device.h"
#include "openxr/openxr.h"

struct VulkanTextureData 
{
    VkInstance instance;
    VkDevice device;
    VkImage image;
    VkFormat format;
    VkSampleCountFlagBits sampleCount;
	uint32_t width;
	uint32_t height;
	VkPhysicalDevice physicalDevice;
	VkQueue queue;
	uint32_t queueFamilyIndex;
};

//Generic HMD interface in case we do want to add other APIs other than SteamVR
class HMDInterface
{
public:
	static HMDInterface* Get();

	virtual ~HMDInterface() {}

	virtual bool Init(XrInstance instance, XrSystemId systemId, XrSession session, XrSpace referenceSpace, XrSpace headSpace) = 0;

	virtual VkSubmitThreadCallback* GetVkSubmitThreadCallback() = 0;

	virtual void GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight) = 0;
	virtual bool BeginFrame() = 0;
	virtual bool EndFrame() = 0;

	virtual void PrePresent(dxvk::D3D9DeviceEx *device) = 0;
	virtual void PostPresent() = 0;
	virtual void GetPredictedDisplayTime(XrTime& time) = 0;
	virtual void GetViews(XrView*& views, XrSpaceLocation& headLocation, uint32_t& viewCount) = 0;

	virtual void StoreSharedTexture(int index, VulkanTextureData* vulkanData) = 0;

	virtual void SetRenderTextureSize(uint32_t width, uint32_t height, int msaa) = 0;
	virtual void OnRenderTargetChanged(dxvk::Rc<dxvk::DxvkDevice> device, dxvk::D3D9Surface* rt) = 0;
};

#endif