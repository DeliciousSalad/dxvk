#ifndef OPENXRDIRECTMODE_H_INCLUDED
#define OPENXRDIRECTMODE_H_INCLUDED

#pragma warning (disable : 4005)

#include "HMDInterface.h"
#include "VkSubmitThreadCallback.h"

#include <vector>
#include <string>
#include <memory>

#include <vulkan/vulkan.h>

#include "openxr/openxr.h"
#define XR_USE_GRAPHICS_API_VULKAN
#include "openxr/openxr_platform.h"

// Structure to hold swapchain image information
struct SwapchainImageData {
	XrSwapchainImageVulkanKHR image;
};

// Structure for shared texture information
struct SharedTextureData {
	VkImage sourceImage;
	VkImageLayout currentLayout;
	uint32_t width;
	uint32_t height;
	VkFormat format;
};

struct ButtonState {
	bool m_touched;
	float m_value;
};

/**
* OpenXR Direct Mode render class.
*/
class OpenXRDirectMode : public HMDInterface,
	public VkSubmitThreadCallback
{
public:
	OpenXRDirectMode();
	virtual ~OpenXRDirectMode();

	virtual bool Init(XrInstance instance, XrSystemId systemId, XrSession session, XrSpace space);

	virtual VkSubmitThreadCallback* GetVkSubmitThreadCallback();

	// VkSubmitThreadCallback
	virtual void PrePresentCallBack();
	virtual void PostPresentCallback();

	//HMDInterface
	virtual void PrePresent();
	virtual void PostPresent();

	virtual void GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight);
	virtual bool BeginFrame();
	virtual bool EndFrame();

	virtual void GetPredictedDisplayTime(XrTime& time);

	virtual void StoreSharedTexture(int index, VulkanTextureData* vulkanData);

private:
	uint32_t m_nRenderWidth;
	uint32_t m_nRenderHeight;

	XrInstance m_instance;
	XrSession m_session;
	XrSpace m_space;
	XrSwapchain m_swapchain;
	XrSystemId m_systemId;
	XrViewState m_viewState;

	// Frame status tracking
	bool m_bFrameStarted = false;

	// Synchronization
	std::atomic<bool> m_bPosesStale = { true };
	std::mutex m_mutex;
	std::condition_variable m_cv;

	uint32_t m_frameCounter = 0;

	// Vulkan handles
	VkInstance m_vkInstance = VK_NULL_HANDLE;
	VkPhysicalDevice m_vkPhysicalDevice = VK_NULL_HANDLE;
	VkDevice m_vkDevice = VK_NULL_HANDLE;
	VkQueue m_vkQueue = VK_NULL_HANDLE;
	uint32_t m_vkQueueFamilyIndex = 0;
	VkCommandPool m_currentCommandPool = VK_NULL_HANDLE;

	// Storage for shared textures from the game
	std::vector<SharedTextureData> m_sharedTextures;

	// Frame timing data
	XrFrameState m_frameState;
	
	// Eye views
	std::vector<XrView> m_views;
	std::vector<XrCompositionLayerProjectionView> m_projectionViews;

	// Swapchains for each eye
	struct SwapchainInfo {
		XrSwapchain handle;
		std::vector<SwapchainImageData> images;
		uint32_t width;
		uint32_t height;
		int64_t format;
	};
	std::vector<SwapchainInfo> m_eyeSwapchains;

	// Format conversion flags
	bool m_needManualGammaCorrection = false;
	bool m_forceLinearFormats = true;  // Try to force linear formats when true

	// Create OpenXR swapchains for eye rendering
	bool CreateEyeSwapchains();

	// Copy shared textures to OpenXR swapchains
	bool CopyToSwapchains();

	// Helper methods for command buffer management
	VkCommandBuffer beginSingleTimeCommands();
	void endSingleTimeCommands(VkCommandBuffer commandBuffer);
	
	// Helper method to verify swapchain contents
	void VerifySwapchainContents();

	// Helper method to get views
	void GetViews(XrView*& views, uint32_t& viewCount);

	bool WaitPoses();
};

#endif //OPENXRDIRECTMODE_H_INCLUDED