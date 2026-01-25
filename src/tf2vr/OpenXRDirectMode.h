#ifndef OPENXRDIRECTMODE_H_INCLUDED
#define OPENXRDIRECTMODE_H_INCLUDED

#pragma warning (disable : 4005)

#include "HMDInterface.h"
#include "VkSubmitThreadCallback.h"
#include "VRCompositor.h"

#include <vector>
#include <string>
#include <memory>

#include <vulkan/vulkan.h>

#include "openxr/openxr.h"
#define XR_USE_GRAPHICS_API_VULKAN
#include "openxr/openxr_platform.h"

#include <atomic>
#include <condition_variable>
#include <chrono>



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

// Add after the existing structs
struct FrameTimings {
	std::chrono::high_resolution_clock::time_point frameStart;
	std::chrono::high_resolution_clock::time_point frameEnd;
	std::chrono::high_resolution_clock::time_point waitPosesStart;
	std::chrono::high_resolution_clock::time_point waitPosesEnd;
	std::chrono::high_resolution_clock::time_point copyStart;
	std::chrono::high_resolution_clock::time_point copyEnd;
	std::chrono::high_resolution_clock::time_point submitStart;
	std::chrono::high_resolution_clock::time_point submitEnd;
	std::chrono::high_resolution_clock::time_point presentStart;
	std::chrono::high_resolution_clock::time_point presentEnd;
	
	// GPU timing queries
	uint64_t gpuCopyStart;
	uint64_t gpuCopyEnd;
	uint64_t gpuSubmitStart;
	uint64_t gpuSubmitEnd;
};

/**
* OpenXR Direct Mode render class.
*/
class OpenXRDirectMode : public HMDInterface,
	public VkSubmitThreadCallback
{
public:
	// Swapchain structure for public access
	struct SwapchainInfo {
		XrSwapchain handle;
		std::vector<SwapchainImageData> images;
		uint32_t width;
		uint32_t height;
		int64_t format;
	};

	OpenXRDirectMode();
	virtual ~OpenXRDirectMode() override;

	virtual bool Init(XrInstance instance, XrSystemId systemId, XrSession session, XrSpace referenceSpace, XrSpace headSpace) override;
	void TryInitializeVRCompositor();

	virtual VkSubmitThreadCallback* GetVkSubmitThreadCallback() override;

	// VkSubmitThreadCallback
	virtual void PrePresentCallBack() override;
	virtual void PostPresentCallback() override;

	//HMDInterface
	virtual void PrePresent(dxvk::D3D9DeviceEx *device) override;
	virtual void PostPresent() override;

	virtual void GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight) override;
	virtual bool BeginFrame() override;
	virtual bool EndFrame() override;

	virtual void GetPredictedDisplayTime(XrTime& time) override;

	virtual void StoreSharedTexture(int index, VulkanTextureData* vulkanData) override;

	virtual void OnRenderTargetChanged(dxvk::Rc<dxvk::DxvkDevice> device, dxvk::D3D9Surface* rt) override;

	// Public accessor methods for VRCompositor
	XrSession GetSession() const { return m_session; }
	XrSpace GetReferenceSpace() const { return m_referenceSpace; }
	VkDevice GetVulkanDevice() const { return m_vkDevice; }
	VkQueue GetVulkanQueue() const { return m_vkQueue; }
	uint32_t GetRenderWidth() const { return m_nRenderWidth; }
	uint32_t GetRenderHeight() const { return m_nRenderHeight; }
	const std::vector<SwapchainInfo>& GetEyeSwapchains() const { return m_eyeSwapchains; }
	VkCommandBuffer GetCommandBuffer() const { 
		return m_commandBuffers.empty() ? VK_NULL_HANDLE : m_commandBuffers[m_currentCommandBufferIndex]; 
	}
	uint32_t GetGraphicsQueueFamily() const { return m_vkQueueFamilyIndex; }
	VkPhysicalDevice GetVulkanPhysicalDevice() const { return m_vkPhysicalDevice; }
	VkInstance GetVulkanInstance() const { return m_vkInstance; }
        void GetViewsPublic(XrView* views, XrSpaceLocation& headLocation, uint32_t& viewCount);
        void ResetFrameState(); // Reset frame tracking for compositor takeover

private:
	uint32_t m_nRenderWidth;
	uint32_t m_nRenderHeight;
	int m_nMSAA;

	XrInstance m_instance;
	XrSession m_session;
	XrSpace m_referenceSpace;
	XrSpace m_headSpace;
	XrSwapchain m_swapchain;
	XrSystemId m_systemId;
	XrViewState m_viewState;
	XrSpaceLocation m_headLocation;

	dxvk::D3D9DeviceEx *m_lastUsedDevice = nullptr;

	// Frame status tracking
	bool m_bFrameStarted = false;
	std::atomic<bool> m_bFrameRunning{false};
	bool m_bSubmitCalled = false;

	// Synchronization
	std::atomic<bool> m_bPosesStale = { true };
	std::mutex m_mutex;
	std::condition_variable m_cv;
	std::condition_variable m_frameCompletionEvent;

	uint32_t m_frameCounter = 0;

	// Vulkan handles
	VkInstance m_vkInstance = VK_NULL_HANDLE;
	VkPhysicalDevice m_vkPhysicalDevice = VK_NULL_HANDLE;
	VkDevice m_vkDevice = VK_NULL_HANDLE;
	VkQueue m_vkQueue = VK_NULL_HANDLE;
	uint32_t m_vkQueueFamilyIndex = 0;
	VkCommandPool m_currentCommandPool = VK_NULL_HANDLE;

	std::chrono::high_resolution_clock::time_point m_tStartTime;
	std::chrono::high_resolution_clock::time_point m_tEndTime;

	// Storage for shared textures from the game
	std::vector<SharedTextureData> m_sharedTextures;

	// Frame timing data
	XrFrameState m_frameState;

	// Eye views
	std::vector<XrView> m_views;
	std::vector<XrCompositionLayerProjectionView> m_projectionViews;

	// Swapchains for each eye
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
	void GetViews(XrView*& views, XrSpaceLocation& headLocation, uint32_t& viewCount);

	bool WaitPoses();

	virtual void SetRenderTextureSize(uint32_t width, uint32_t height, int msaa);
	virtual int DetermineMSAA(uint32_t width, uint32_t height);

	// Command buffer management
	VkCommandPool m_persistentCommandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> m_commandBuffers;
	std::vector<VkFence> m_commandBufferFences;
	std::vector<VkSemaphore> m_frameSyncSemaphores;
	uint32_t m_currentCommandBufferIndex = 0;
	uint32_t m_lastCompletedBufferIndex = 0;

	// Performance measurement
	static const size_t TIMING_HISTORY_SIZE = 60;  // Store 1 second of timing at 60fps
	std::vector<FrameTimings> m_frameTimings;
	VkQueryPool m_queryPool;
	size_t m_currentTimingIndex;
	
	// Helper methods for timing
	void BeginFrameTiming();
	void EndFrameTiming();
	void LogTimings();
	double GetAverageTime(const std::vector<double>& times) const;

	float m_displayRefreshRate;
	
	// VR Compositor for independent menu rendering
	std::unique_ptr<dxvk::VRCompositor> m_vrCompositor;

	// XR_EXT_render_model function pointers
	PFN_xrCreateRenderModelEXT m_pfnCreateRenderModelEXT = nullptr;
	PFN_xrDestroyRenderModelEXT m_pfnDestroyRenderModelEXT = nullptr;
	PFN_xrGetRenderModelPropertiesEXT m_pfnGetRenderModelPropertiesEXT = nullptr;
	PFN_xrCreateRenderModelSpaceEXT m_pfnCreateRenderModelSpaceEXT = nullptr;
	PFN_xrCreateRenderModelAssetEXT m_pfnCreateRenderModelAssetEXT = nullptr;
	PFN_xrDestroyRenderModelAssetEXT m_pfnDestroyRenderModelAssetEXT = nullptr;
	PFN_xrGetRenderModelAssetDataEXT m_pfnGetRenderModelAssetDataEXT = nullptr;
	PFN_xrGetRenderModelAssetPropertiesEXT m_pfnGetRenderModelAssetPropertiesEXT = nullptr;
	PFN_xrGetRenderModelStateEXT m_pfnGetRenderModelStateEXT = nullptr;

	// XR_EXT_interaction_render_model function pointers
	PFN_xrEnumerateInteractionRenderModelIdsEXT m_pfnEnumerateInteractionRenderModelIdsEXT = nullptr;
	PFN_xrEnumerateRenderModelSubactionPathsEXT m_pfnEnumerateRenderModelSubactionPathsEXT = nullptr;
	PFN_xrGetRenderModelPoseTopLevelUserPathEXT m_pfnGetRenderModelPoseTopLevelUserPathEXT = nullptr;

	// Flag indicating if render model extensions are available
	bool m_renderModelExtensionsLoaded = false;

public:
	// Public accessors for render model functions (used by VRCompositor)
	bool HasRenderModelSupport() const { return m_renderModelExtensionsLoaded; }
	PFN_xrEnumerateInteractionRenderModelIdsEXT GetEnumerateInteractionRenderModelIdsEXT() const { return m_pfnEnumerateInteractionRenderModelIdsEXT; }
	PFN_xrCreateRenderModelEXT GetCreateRenderModelEXT() const { return m_pfnCreateRenderModelEXT; }
	PFN_xrDestroyRenderModelEXT GetDestroyRenderModelEXT() const { return m_pfnDestroyRenderModelEXT; }
	PFN_xrGetRenderModelPropertiesEXT GetRenderModelPropertiesEXT() const { return m_pfnGetRenderModelPropertiesEXT; }
	PFN_xrCreateRenderModelSpaceEXT GetCreateRenderModelSpaceEXT() const { return m_pfnCreateRenderModelSpaceEXT; }
	PFN_xrCreateRenderModelAssetEXT GetCreateRenderModelAssetEXT() const { return m_pfnCreateRenderModelAssetEXT; }
	PFN_xrDestroyRenderModelAssetEXT GetDestroyRenderModelAssetEXT() const { return m_pfnDestroyRenderModelAssetEXT; }
	PFN_xrGetRenderModelAssetDataEXT GetRenderModelAssetDataEXT() const { return m_pfnGetRenderModelAssetDataEXT; }
	PFN_xrGetRenderModelAssetPropertiesEXT GetRenderModelAssetPropertiesEXT() const { return m_pfnGetRenderModelAssetPropertiesEXT; }
	PFN_xrGetRenderModelStateEXT GetRenderModelStateEXT() const { return m_pfnGetRenderModelStateEXT; }
	PFN_xrGetRenderModelPoseTopLevelUserPathEXT GetRenderModelPoseTopLevelUserPathEXT() const { return m_pfnGetRenderModelPoseTopLevelUserPathEXT; }
	PFN_xrEnumerateRenderModelSubactionPathsEXT GetEnumerateRenderModelSubactionPathsEXT() const { return m_pfnEnumerateRenderModelSubactionPathsEXT; }
	XrInstance GetXrInstance() const { return m_instance; }
};

#endif //OPENXRDIRECTMODE_H_INCLUDED