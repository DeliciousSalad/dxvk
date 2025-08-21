#include "OpenXRDirectMode.h"
#include "HMDInterface.h"
#include <cstring>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <thread>
#include <future>

#include "../util/log/log.h"
#include "../d3d9/d3d9_common_texture.h"
#include "../util/util_singleton.h"
#include "../d3d9/d3d9_device.h"
#include "../util/util_string.h"
#include "VkSubmitThreadCallback.h"

// Forward declare global VRCompositor instance - must be early for constructor access
namespace dxvk { class VRCompositor; }
static ::dxvk::VRCompositor* g_vrCompositor = nullptr;

#include <vector>
#include <string>
#include <memory>
#include <algorithm> // For std::min

#include <vulkan/vulkan.h>

using namespace dxvk;

extern VkSubmitThreadCallback *g_pVkSubmitThreadCallback;

#define BUTTON_DEADZONE  0.05f;

// Ensure correct swapchain image type
#ifndef XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR
#define XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR XrStructureType(1000027001)
#endif

using PFN_xrGetVulkanInstanceExtensionsKHR = XrResult (XRAPI_PTR *)(XrInstance instance, XrSystemId systemId, uint32_t bufferCapacityInput, uint32_t* bufferCountOutput, char* buffer);
using PFN_xrGetVulkanDeviceExtensionsKHR = XrResult (XRAPI_PTR *)(XrInstance instance, XrSystemId systemId, uint32_t bufferCapacityInput, uint32_t* bufferCountOutput, char* buffer);
using PFN_xrGetVulkanGraphicsDeviceKHR = XrResult (XRAPI_PTR *)(XrInstance instance, XrSystemId systemId, VkInstance vkInstance, VkPhysicalDevice* vkPhysicalDevice);
using PFN_xrGetVulkanGraphicsRequirementsKHR = XrResult (XRAPI_PTR *)(XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsVulkanKHR* graphicsRequirements);
using PFN_xrCreateVulkanInstanceKHR = XrResult (XRAPI_PTR *)(XrInstance instance, const XrVulkanInstanceCreateInfoKHR* createInfo, VkInstance* vulkanInstance, VkResult* vulkanResult);
using PFN_xrCreateVulkanDeviceKHR = XrResult (XRAPI_PTR *)(XrInstance instance, const XrVulkanDeviceCreateInfoKHR* createInfo, VkDevice* vulkanDevice, VkResult* vulkanResult);

using Clock = std::chrono::high_resolution_clock;
using TimePoint = std::chrono::high_resolution_clock::time_point;
using Duration = std::chrono::duration<double, std::milli>;

void Matrix_SetIdentity(float matrix[4][4])
{
	memset(matrix, 0, 16 * sizeof(float));
	matrix[0][0] = matrix[1][1] = matrix[2][2] = matrix[3][3] = 1.0f;
}

inline void Swap(float& a, float& b)
{
	float tmp = a;
	a = b;
	b = tmp;
}

void MatrixTranspose(float src[4][4], float dst[4][4])
{
	if (src == dst)
	{
		Swap(dst[0][1], dst[1][0]);
		Swap(dst[0][2], dst[2][0]);
		Swap(dst[0][3], dst[3][0]);
		Swap(dst[1][2], dst[2][1]);
		Swap(dst[1][3], dst[3][1]);
		Swap(dst[2][3], dst[3][2]);
	}
	else
	{
		dst[0][0] = src[0][0]; dst[0][1] = src[1][0]; dst[0][2] = src[2][0]; dst[0][3] = src[3][0];
		dst[1][0] = src[0][1]; dst[1][1] = src[1][1]; dst[1][2] = src[2][1]; dst[1][3] = src[3][1];
		dst[2][0] = src[0][2]; dst[2][1] = src[1][2]; dst[2][2] = src[2][2]; dst[2][3] = src[3][2];
		dst[3][0] = src[0][3]; dst[3][1] = src[1][3]; dst[3][2] = src[2][3]; dst[3][3] = src[3][3];
	}
}

OpenXRDirectMode::OpenXRDirectMode()
	: m_nRenderWidth(1440)
	, m_nRenderHeight(1600)
	, m_instance(XR_NULL_HANDLE)
	, m_session(XR_NULL_HANDLE)
	, m_referenceSpace(nullptr)
	, m_headSpace(nullptr)
	, m_swapchain(nullptr)
	, m_frameCounter(0)
	, m_bFrameStarted(false)
	, m_needManualGammaCorrection(false)
	, m_bPosesStale(true)
	, m_currentTimingIndex(0)
	, m_queryPool(VK_NULL_HANDLE)
	, m_vkDevice(VK_NULL_HANDLE)
	, m_vkInstance(VK_NULL_HANDLE)
	, m_vkPhysicalDevice(VK_NULL_HANDLE)
	, m_vkQueue(VK_NULL_HANDLE)
{
	Logger::info("OpenXRDirectMode: Constructor called");
	
	// Initialize VR Compositor
	m_vrCompositor = std::make_unique<dxvk::VRCompositor>();
	m_vrCompositor->SetOpenXRManager(this);
	
	// Set global pointer for legacy functions
	g_vrCompositor = m_vrCompositor.get();
	
	// Initialize frameState with default values
	m_frameState = { XR_TYPE_FRAME_STATE };
	m_frameState.predictedDisplayTime = 0;
	m_frameState.predictedDisplayPeriod = 0;
	m_frameState.shouldRender = XR_FALSE;

	// Pre-allocate timing storage
	m_frameTimings.resize(TIMING_HISTORY_SIZE);
}

OpenXRDirectMode::~OpenXRDirectMode()
{
	Logger::info("OpenXRDirectMode: Destructor called");
	
	// Clean up eye swapchains
	for (auto& swapchain : m_eyeSwapchains) {
		if (swapchain.handle != XR_NULL_HANDLE) {
			xrDestroySwapchain(swapchain.handle);
			swapchain.handle = XR_NULL_HANDLE;
		}
	}
	m_eyeSwapchains.clear();
	
	// Clean up command buffers and pool
	if (m_persistentCommandPool != VK_NULL_HANDLE) {
		if (!m_commandBuffers.empty()) {
			// Wait for any pending operations
			vkDeviceWaitIdle(m_vkDevice);
			
			// Free command buffers
			vkFreeCommandBuffers(m_vkDevice, m_persistentCommandPool, m_commandBuffers.size(), m_commandBuffers.data());
		}
		vkDestroyCommandPool(m_vkDevice, m_persistentCommandPool, nullptr);
		m_persistentCommandPool = VK_NULL_HANDLE;
	}
	m_commandBuffers.clear();

	// Clean up synchronization primitives
	for (VkFence fence : m_commandBufferFences) {
		if (fence != VK_NULL_HANDLE) {
			vkDestroyFence(m_vkDevice, fence, nullptr);
		}
	}
	m_commandBufferFences.clear();

	for (VkSemaphore semaphore : m_frameSyncSemaphores) {
		if (semaphore != VK_NULL_HANDLE) {
			vkDestroySemaphore(m_vkDevice, semaphore, nullptr);
		}
	}
	m_frameSyncSemaphores.clear();
	
	// Clean up query pool
	if (m_queryPool != VK_NULL_HANDLE) {
		vkDestroyQueryPool(m_vkDevice, m_queryPool, nullptr);
		m_queryPool = VK_NULL_HANDLE;
	}
	
	// Reset global VkSubmitThreadCallback if it's pointing to this instance
	if (g_pVkSubmitThreadCallback == this) {
		g_pVkSubmitThreadCallback = nullptr;
	}
	
	m_instance = XR_NULL_HANDLE;
	m_session = XR_NULL_HANDLE;
	m_referenceSpace = nullptr;
	m_headSpace = nullptr;
	m_swapchain = nullptr;
	m_systemId = XR_NULL_SYSTEM_ID;
}

bool OpenXRDirectMode::Init(XrInstance instance, XrSystemId systemId, XrSession session, XrSpace referenceSpace, XrSpace headSpace) 
{
	m_instance = instance;
	m_session = session;
	m_referenceSpace = referenceSpace;
	m_headSpace = headSpace;
	m_systemId = systemId;

	// Create query pool for GPU timing
	VkQueryPoolCreateInfo queryPoolInfo = {};
	queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
	queryPoolInfo.queryCount = TIMING_HISTORY_SIZE * 4; // 4 timestamps per frame
	
	if (m_vkDevice != VK_NULL_HANDLE) {
		VkResult result = vkCreateQueryPool(m_vkDevice, &queryPoolInfo, nullptr, &m_queryPool);
		if (result != VK_SUCCESS) {
			Logger::err("OpenXRDirectMode: Failed to create query pool for GPU timing");
		}
	}

	// Enumerate available swapchain formats
	uint32_t formatCount = 0;
	XrResult result = xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr);
	if (XR_FAILED(result)) {
		Logger::err("OpenXRDirectMode: Failed to get swapchain format count");
		return false;
	}

	std::vector<int64_t> formats(formatCount);
	result = xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data());
	if (XR_FAILED(result)) {
		Logger::err("OpenXRDirectMode: Failed to enumerate swapchain formats");
		return false;
	}

	// Print out format information for debugging
	Logger::info("OpenXRDirectMode: Available swapchain formats:");
	for (uint32_t i = 0; i < formatCount; i++) {
		const char* formatName = "UNKNOWN";
		if (formats[i] == VK_FORMAT_B8G8R8A8_SRGB) formatName = "B8G8R8A8_SRGB";
		else if (formats[i] == VK_FORMAT_R8G8B8A8_SRGB) formatName = "R8G8B8A8_SRGB";
		else if (formats[i] == VK_FORMAT_R8G8B8A8_UNORM) formatName = "R8G8B8A8_UNORM";
		else if (formats[i] == VK_FORMAT_B8G8R8A8_UNORM) formatName = "B8G8R8A8_UNORM";
		Logger::info(str::format("  - Format: ", formats[i], " (", formatName, ")"));
	}

	// Get source format from the shared texture
	int64_t selectedFormat = formats[0]; // Default to first format
	int64_t sourceFormat = VK_FORMAT_B8G8R8A8_SRGB; // Default format
	
	if (!m_sharedTextures.empty()) {
		sourceFormat = static_cast<int64_t>(m_sharedTextures[0].format);
		
		// Known situation: TF2 gives B8G8R8A8_UNORM, OpenXR provides B8G8R8A8_SRGB
		// Prioritize selection in this order:
		// 1. Exact match (ideal but unlikely)
		// 2. Same channel order (BGRA), with UNORM format (if available)
		// 3. Same channel order (BGRA), with SRGB format
		
		// First try exact match
		bool exactMatchFound = false;
		for (int64_t format : formats) {
			if (format == sourceFormat) {
				selectedFormat = format;
				Logger::info("OpenXRDirectMode: Found exact matching format!");
				m_needManualGammaCorrection = false;
				exactMatchFound = true;
				break;
			}
		}
		
		// If no exact match, try to find BGRA_UNORM
		if (!exactMatchFound && sourceFormat == VK_FORMAT_B8G8R8A8_UNORM) {
			// Try to force the exact UNORM format even if not reported
			if (m_forceLinearFormats) {
				Logger::info("OpenXRDirectMode: Forcing BGRA_UNORM format even if not reported as available");
				selectedFormat = VK_FORMAT_B8G8R8A8_UNORM;
				m_needManualGammaCorrection = false;
			} else {
				// Otherwise look for BGRA_SRGB
				bool foundBGRA_SRGB = false;
				for (int64_t format : formats) {
					if (format == VK_FORMAT_B8G8R8A8_SRGB) {
						selectedFormat = format;
						Logger::info("OpenXRDirectMode: Using BGRA_SRGB format (will need gamma correction)");
						m_needManualGammaCorrection = true;
						foundBGRA_SRGB = true;
						break;
					}
				}
				
				// If no BGRA format at all, fall back to first available format
				if (!foundBGRA_SRGB) {
					selectedFormat = formats[0];
					Logger::info(str::format("OpenXRDirectMode: No compatible BGRA format found, using format ", formats[0]));
					m_needManualGammaCorrection = true;
				}
			}
		}
	}
	
	Logger::info(str::format("OpenXRDirectMode: Using swapchain format: ", selectedFormat,
		", Source format: ", sourceFormat, 
		", Need gamma correction: ", m_needManualGammaCorrection ? "Yes" : "No"));

	// Setup view configuration
	uint32_t viewConfigTypeCount = 0;
	result = xrEnumerateViewConfigurations(m_instance, m_systemId, 0, &viewConfigTypeCount, nullptr);
	if (XR_FAILED(result)) 
	{
		Logger::err("OpenXRDirectMode: Failed to get view configuration count");
		return false;
	}

	std::vector<XrViewConfigurationType> viewConfigTypes(viewConfigTypeCount);
	result = xrEnumerateViewConfigurations(m_instance, m_systemId, viewConfigTypeCount, &viewConfigTypeCount, viewConfigTypes.data());
	if (XR_FAILED(result)) 
	{
		Logger::err("OpenXRDirectMode: Failed to enumerate view configurations");
		return false;
	}

	// Find stereo view configuration
	XrViewConfigurationType stereoViewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	bool foundStereoView = false;
	for (XrViewConfigurationType viewConfigType : viewConfigTypes) 
	{
		if (viewConfigType == stereoViewConfigType) {
			foundStereoView = true;
			break;
		}
	}

	if (!foundStereoView) 
	{
		Logger::err("OpenXRDirectMode: Stereo view configuration not supported");
		return false;
	}

	// Get the view configuration properties
	uint32_t viewCount = 0;
	result = xrEnumerateViewConfigurationViews(m_instance, m_systemId, stereoViewConfigType, 0, &viewCount, nullptr);
	if (XR_FAILED(result) || viewCount != 2) 
	{
		Logger::err("OpenXRDirectMode: Failed to get view configuration views count");
		return false;
	}

	std::vector<XrViewConfigurationView> configViews(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	for (uint32_t i = 0; i < viewCount; i++)
	{
		configViews[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
		configViews[i].next = nullptr;
	}
	result = xrEnumerateViewConfigurationViews(m_instance, m_systemId, stereoViewConfigType, viewCount, &viewCount, configViews.data());
	if (XR_FAILED(result)) 
	{
		Logger::err("OpenXRDirectMode: Failed to enumerate view configuration views with error code: " + str::format(result));
		return false;
	}

	// Use the recommended size for rendering
	m_nRenderWidth = configViews[0].recommendedImageRectWidth;
	m_nRenderHeight = configViews[0].recommendedImageRectHeight;

	// Set global VkSubmitThreadCallback
	g_pVkSubmitThreadCallback = this;

	// Initialize views for use on first frame
	m_views.resize(2, {XR_TYPE_VIEW});
	m_projectionViews.resize(2, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
	Logger::info("OpenXRDirectMode: Created default views for frame submission");

	// VRCompositor initialization will be deferred until Vulkan device is available
	// This happens in StoreSharedTexture() when the first texture is processed
	Logger::info("OpenXRDirectMode: VRCompositor initialization deferred until Vulkan device is available");

	// All is good!
	Logger::info(str::format("OpenXRDirectMode: Initialized with render target size: ", m_nRenderWidth, "x", m_nRenderHeight));
	return true;
}

void OpenXRDirectMode::TryInitializeVRCompositor() {
	if (!m_vrCompositor) {
		Logger::warn("OpenXRDirectMode: TryInitializeVRCompositor called but no VRCompositor instance");
		return;
	}
	
	// Check if already initialized
	static bool vrCompositorInitialized = false;
	if (vrCompositorInitialized) {
		Logger::info("OpenXRDirectMode: VRCompositor already initialized, skipping");
		return;
	}
	
	Logger::info("OpenXRDirectMode: Attempting to initialize VRCompositor with Vulkan device info");
	
	// Cache OpenXR data with the now-available Vulkan device info
	m_vrCompositor->CacheOpenXRData(m_vkPhysicalDevice, m_vkInstance, m_session, m_referenceSpace, m_nRenderWidth, m_nRenderHeight);
	
	// Initialize VRCompositor now that both OpenXR and Vulkan are ready
	if (!m_vrCompositor->Initialize()) {
		Logger::err("OpenXRDirectMode: Failed to initialize VRCompositor (deferred)");
	} else {
		Logger::info("OpenXRDirectMode: ✅ VRCompositor initialized successfully (deferred)");
		vrCompositorInitialized = true;
	}
}

VkSubmitThreadCallback* OpenXRDirectMode::GetVkSubmitThreadCallback() 
{
	return this;
}

void OpenXRDirectMode::PrePresent(dxvk::D3D9DeviceEx *device) 
{
	// Called before d3d9 swapchain present
	if (!m_session) {
		return;
	}

	// Don't interfere with compositor's frame management
	extern bool IsVRCompositorActive();
	if (IsVRCompositorActive()) {
		Logger::info("OpenXRDirectMode: PrePresent skipped - compositor is active");
		return;
	}

	m_lastUsedDevice = device;
	m_bSubmitCalled = true;
	Logger::info("OpenXRDirectMode: Pre-present phase");
}

void OpenXRDirectMode::PostPresent() 
{
	// Called after d3d9 swapchain present
	if (!m_session) {
		return;
	}

	// Don't interfere with compositor's frame management
	extern bool IsVRCompositorActive();
	if (IsVRCompositorActive()) {
		Logger::info("OpenXRDirectMode: PostPresent skipped - compositor is active");
		return;
	}

	// Note: EndFrame will be called explicitly after the frame is rendered
	// Just log that we're in the post-present phase for debugging
	Logger::info("OpenXRDirectMode: Post-present phase");
}

void OpenXRDirectMode::PrePresentCallBack() 
{
	// Called before the DXVK presenter calls presentImage
	if (!m_session) {
		return;
	}

	// Don't interfere with compositor's frame management
	extern bool IsVRCompositorActive();
	if (IsVRCompositorActive()) {
		Logger::info("OpenXRDirectMode: PrePresentCallback skipped - compositor is active");
		return;
	}

	Logger::info("OpenXRDirectMode: PrePresentCallback called!");

	auto& timing = m_frameTimings[m_currentTimingIndex];
	timing.presentStart = Clock::now();
	
	if(m_bSubmitCalled) {
		EndFrame();
		m_bSubmitCalled = false;
	}
		
}

void OpenXRDirectMode::PostPresentCallback() 
{
	if (!m_session) {
		return;
	}

	// Don't interfere with compositor's frame management
	extern bool IsVRCompositorActive();
	extern void TF2VR_NotifyVGUIFrameComplete();
	if (IsVRCompositorActive()) {
		// Notify that a VGUI frame may have been completed
		TF2VR_NotifyVGUIFrameComplete();
		Logger::info("OpenXRDirectMode: PostPresentCallback skipped - compositor is active");
		return;
	}

	// Signal frame completion using atomic operation and notify condition variable
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_bFrameRunning.store(false, std::memory_order_release);
		m_frameCompletionEvent.notify_one();
	}

	auto& timing = m_frameTimings[m_currentTimingIndex];
	timing.presentEnd = Clock::now();
}

void OpenXRDirectMode::GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight) 
{
	if (pnWidth) *pnWidth = m_nRenderWidth;
	if (pnHeight) *pnHeight = m_nRenderHeight;
}

bool OpenXRDirectMode::BeginFrame()
{	
	Logger::info("OpenXRDirectMode: BeginFrame called");
	// BeginFrameTiming();
	
	if (!m_session) {
		Logger::err("OpenXRDirectMode: Cannot begin frame - no active session");
		return false;
	}

	// Don't interfere with compositor's frame management
	extern bool IsVRCompositorActive();
	if (IsVRCompositorActive()) {
		Logger::info("OpenXRDirectMode: BeginFrame skipped - compositor is active");
		return false;
	}

	if (m_lastUsedDevice) {
    	// flush and synchronize with submission queue
    	m_lastUsedDevice->Flush();
    	m_lastUsedDevice->SynchronizeCsThread(dxvk::DxvkCsThread::SynchronizeAll);
  	}

	Logger::info("OpenXRDirectMode: BeginFrame called! Blocking until ready...");

	// Use atomic operation with mutex+condition variable for frame synchronization
	while (m_bFrameRunning.load(std::memory_order_acquire)) {
		std::unique_lock<std::mutex> lock(m_mutex);
		if (m_frameCompletionEvent.wait_for(lock, std::chrono::milliseconds(16)) == std::cv_status::timeout) {
			Logger::warn("OpenXRDirectMode: Waiting for previous frame completion");
		}
	}

	auto& timing = m_frameTimings[m_currentTimingIndex];
	timing.waitPosesStart = Clock::now();
	WaitPoses();
	timing.waitPosesEnd = Clock::now();
	
	return true;
}

bool OpenXRDirectMode::EndFrame() 
{
	auto& timing = m_frameTimings[m_currentTimingIndex];
	timing.submitStart = Clock::now();
	
	if (!m_session) {
		Logger::err("OpenXRDirectMode: Cannot end frame - no active session");
		return false;
	}

	// Don't interfere with compositor's frame management
	extern bool IsVRCompositorActive();
	if (IsVRCompositorActive()) {
		Logger::info("OpenXRDirectMode: EndFrame skipped - compositor is active");
		return false;
	}

	Logger::info("OpenXRDirectMode: EndFrame called!");

	// Check if we've actually started a frame
	if (!m_bFrameStarted) {
		Logger::warn(str::format("OpenXRDirectMode: EndFrame called without BeginFrame - frame: ", m_frameCounter));
		return false;
	}

	if(m_eyeSwapchains.empty() && !m_sharedTextures.empty()) 
	{
		// If we have shared textures but no swapchains yet, create them
		Logger::info("OpenXRDirectMode: Creating eye swapchains from shared textures");
		if (!CreateEyeSwapchains()) {
			Logger::err("OpenXRDirectMode: Failed to create eye swapchains");
			m_bFrameStarted = false; // Reset the flag even on failure
			return false;
		}
	}

	if (!m_eyeSwapchains.empty()) 
	{
		if (!CopyToSwapchains()) {
			Logger::err("OpenXRDirectMode: Failed to copy to swapchains");
			m_bFrameStarted = false; // Reset the flag even on failure
			return false;
		}
	}

	// Get the current view poses
	XrViewLocateInfo viewLocateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
	viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	viewLocateInfo.displayTime = m_frameState.predictedDisplayTime;
	viewLocateInfo.space = m_referenceSpace;

	uint32_t viewCount = (uint32_t)m_views.size();
	//XrResult result = xrLocateViews(m_session, &viewLocateInfo, &m_viewState, viewCount, &viewCount, m_views.data());
	XrResult result;

	if (XR_FAILED(result)) 
	{
		Logger::err(str::format("OpenXRDirectMode: xrLocateViews failed with error code: ", (int)result));
		m_bFrameStarted = false;
		return false;
	}
	
	// Only use the views if they're valid, otherwise use default values
	bool validViews = (m_viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
					 (m_viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
	
	if (validViews) 
	{
		// Build the projection views
		for (uint32_t i = 0; i < m_views.size() && i < m_eyeSwapchains.size(); i++) {
			const SwapchainInfo& swapchain = m_eyeSwapchains[i];
			XrCompositionLayerProjectionView& projectionView = m_projectionViews[i];
			
			projectionView.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projectionView.pose = m_views[i].pose;
			projectionView.fov = m_views[i].fov;
			projectionView.subImage.swapchain = swapchain.handle;
			projectionView.subImage.imageRect.offset = {0, 0};
			projectionView.subImage.imageRect.extent = {(int32_t)swapchain.width, (int32_t)swapchain.height};
		}
	} else {
		Logger::warn("OpenXRDirectMode: Views not valid, using default projection");
	}
	
	// Submit layers for composition
	const XrCompositionLayerBaseHeader* layers[1] = {nullptr};
	uint32_t layerCount = 0;

	// Create a scope for the layer that extends past xrEndFrame
	{
		// Only submit the layer if we have valid swapchains and views
		if (!m_projectionViews.empty() && !m_eyeSwapchains.empty()) {
			XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
			layer.space = m_referenceSpace;
			// Calculate view count as the minimum of projection views and swapchain count
			uint32_t viewCount = static_cast<uint32_t>(m_projectionViews.size());
			if (m_eyeSwapchains.size() < m_projectionViews.size()) {
				viewCount = static_cast<uint32_t>(m_eyeSwapchains.size());
			}
			layer.viewCount = viewCount;
			layer.views = m_projectionViews.data();
			layers[0] = (XrCompositionLayerBaseHeader*)&layer;
			layerCount = 1;
			
			Logger::info(str::format("OpenXRDirectMode: Submitting ", layer.viewCount, " views for composition"));
		
			// End the frame
			XrFrameEndInfo frameEndInfo = {XR_TYPE_FRAME_END_INFO};
			frameEndInfo.displayTime = m_frameState.predictedDisplayTime;
			frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			frameEndInfo.layerCount = layerCount;
			frameEndInfo.layers = layers;

			result = xrEndFrame(m_session, &frameEndInfo);
			if (!XR_SUCCEEDED(result))
			{
				Logger::err(str::format("OpenXRDirectMode: xrEndFrame failed for frame ", m_frameCounter, " with error code: ", (int)result));
			} else {
				Logger::info(str::format("OpenXRDirectMode: Frame ", m_frameCounter, " ended successfully"));
			}
		} else {
			Logger::warn("OpenXRDirectMode: No views/swapchains ready for submission");
			
			// End the frame with no layers
			XrFrameEndInfo frameEndInfo = {XR_TYPE_FRAME_END_INFO};
			frameEndInfo.displayTime = m_frameState.predictedDisplayTime;
			frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			frameEndInfo.layerCount = 0;
			frameEndInfo.layers = nullptr;

			result = xrEndFrame(m_session, &frameEndInfo);
			if (!XR_SUCCEEDED(result))
			{
				Logger::err(str::format("OpenXRDirectMode: xrEndFrame failed for frame ", m_frameCounter, " with error code: ", (int)result));
			}
		}
	}
	
	m_bFrameStarted = false;
	Logger::info("OpenXRDirectMode: EndFrame completed");
	timing.submitEnd = Clock::now();
	// EndFrameTiming();
	return XR_SUCCEEDED(result);
}

void OpenXRDirectMode::StoreSharedTexture(int index, VulkanTextureData* vulkanData)
{
	if (!vulkanData || index < 0) {
		return;
	}

	// Store Vulkan device and queue information if not already set
	bool vulkanDeviceJustSet = false;
	
	if (m_vkDevice == VK_NULL_HANDLE && vulkanData->device != VK_NULL_HANDLE) {
		m_vkDevice = vulkanData->device;
		Logger::info("OpenXRDirectMode: Stored Vulkan device from shared texture");
		vulkanDeviceJustSet = true;
	}

	if (m_vkInstance == VK_NULL_HANDLE && vulkanData->instance != VK_NULL_HANDLE) {
		m_vkInstance = vulkanData->instance;
		Logger::info("OpenXRDirectMode: Stored Vulkan instance from shared texture");
		vulkanDeviceJustSet = true;
	}

	if (m_vkPhysicalDevice == VK_NULL_HANDLE && vulkanData->physicalDevice != VK_NULL_HANDLE) {
		m_vkPhysicalDevice = vulkanData->physicalDevice;
		Logger::info("OpenXRDirectMode: Stored Vulkan physical device from shared texture");
		vulkanDeviceJustSet = true;
	}
	
	// Initialize VRCompositor now that we have Vulkan device info
	if (vulkanDeviceJustSet && m_vrCompositor && m_session != XR_NULL_HANDLE) {
		TryInitializeVRCompositor();
	}

	if (m_vkQueue == VK_NULL_HANDLE && vulkanData->queue != VK_NULL_HANDLE) {
		m_vkQueue = vulkanData->queue;
		m_vkQueueFamilyIndex = vulkanData->queueFamilyIndex;
		Logger::info(str::format("OpenXRDirectMode: Stored Vulkan queue (family index: ", m_vkQueueFamilyIndex, ")"));
	}

	// Store this texture for use in OpenXR rendering
	// Check if this texture is large enough to be an eye texture
	if (vulkanData->width >= m_nRenderWidth && vulkanData->height >= m_nRenderHeight) {
		// This could be an eye texture for VR rendering
		SharedTextureData sharedTexture;
		sharedTexture.sourceImage = vulkanData->image;
		sharedTexture.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // Assume shader read layout initially
		sharedTexture.width = vulkanData->width;
		sharedTexture.height = vulkanData->height;
		sharedTexture.format = vulkanData->format;

		// Store by index - typically 0 for left eye, 1 for right eye
		if (m_sharedTextures.size() <= index) {
			m_sharedTextures.resize(index + 1);
		}
		m_sharedTextures[index] = sharedTexture;
		
		Logger::info(str::format("OpenXRDirectMode: Stored shared texture ", index, 
			" (", vulkanData->width, "x", vulkanData->height, 
			", format: ", (uint32_t)vulkanData->format, ")", ", image: ", (uint64_t)vulkanData->image));
	}
}

bool OpenXRDirectMode::CreateEyeSwapchains()
{
	// Get information about the OpenXR environment
	uint32_t viewCount = 0;
	XrResult result = xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
	if (XR_FAILED(result)) {
		Logger::err(str::format("OpenXRDirectMode: Failed to get view configuration views count, error: ", (int)result));
		return false;
	}

	// Initialize the view configuration views with proper type
	std::vector<XrViewConfigurationView> configViews(viewCount);
	for (uint32_t i = 0; i < viewCount; i++) {
		configViews[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
		configViews[i].next = nullptr;
	}
	
	result = xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, configViews.data());
	if (XR_FAILED(result)) {
		Logger::err(str::format("OpenXRDirectMode: Failed to enumerate view configuration views, error: ", (int)result));
		return false;
	}
	
	// Log the retrieved view configurations
	Logger::info(str::format("OpenXRDirectMode: Enumerated ", viewCount, " view configurations"));
	for (uint32_t i = 0; i < viewCount; i++) {
		Logger::info(str::format("  View ", i, ": ", configViews[i].recommendedImageRectWidth, "x", configViews[i].recommendedImageRectHeight));
	}

	// Determine swapchain size based on shared textures
	uint32_t swapchainWidth = configViews[0].recommendedImageRectWidth;
	uint32_t swapchainHeight = configViews[0].recommendedImageRectHeight;
		
	// If we have a shared texture, use its dimensions
	// For side-by-side rendering, each eye gets half the width
	if (!m_sharedTextures.empty()) {
		if (m_sharedTextures.size() == 1 && viewCount == 2) {
		// Single texture for both eyes - each eye gets half the width
			swapchainWidth = m_sharedTextures[0].width / 2;
			swapchainHeight = m_sharedTextures[0].height;
			Logger::info(str::format("OpenXRDirectMode: Using shared texture dimensions for swapchains: ", 
				swapchainWidth, "x", swapchainHeight, " (half width of source texture)"));
		} else if (m_sharedTextures.size() >= viewCount) {
			// Separate texture for each eye
			swapchainWidth = m_sharedTextures[0].width;
			swapchainHeight = m_sharedTextures[0].height;
			Logger::info(str::format("OpenXRDirectMode: Using shared texture dimensions for swapchains: ", 
				swapchainWidth, "x", swapchainHeight));
		}
	} else {
		Logger::info(str::format("OpenXRDirectMode: Using recommended swapchain dimensions: ", 
			swapchainWidth, "x", swapchainHeight));
	}

	// We know both source and swapchain use B8G8R8A8_SRGB (format 50)
	int64_t targetFormat = VK_FORMAT_B8G8R8A8_SRGB; // Format 50
	
	// For completeness, enumerate formats
	uint32_t formatCount = 0;
	result = xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr);
	if (XR_FAILED(result)) {
		Logger::err("OpenXRDirectMode: Failed to get swapchain format count");
		return false;
	}
	
	std::vector<int64_t> formats(formatCount);
	result = xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data());
	if (XR_FAILED(result)) {
		Logger::err("OpenXRDirectMode: Failed to enumerate swapchain formats");
		return false;
	}
	
	// Log available formats for debugging but use our known format
	Logger::info("OpenXRDirectMode: Available swapchain formats:");
	bool formatSupported = false;
	for (uint32_t i = 0; i < formatCount; i++) {
		const char* formatName = "UNKNOWN";
		if (formats[i] == VK_FORMAT_R8G8B8A8_SRGB) formatName = "R8G8B8A8_SRGB";
		else if (formats[i] == VK_FORMAT_B8G8R8A8_SRGB) formatName = "B8G8R8A8_SRGB";
		else if (formats[i] == VK_FORMAT_R8G8B8A8_UNORM) formatName = "R8G8B8A8_UNORM";
		else if (formats[i] == VK_FORMAT_B8G8R8A8_UNORM) formatName = "B8G8R8A8_UNORM";
		
		Logger::info(str::format("  - Format: ", formats[i], " (", formatName, ")"));
		
		// Check if our target format is supported
		if (formats[i] == targetFormat) {
			formatSupported = true;
		}
	}
	
	if (!formatSupported) {
		Logger::warn("OpenXRDirectMode: B8G8R8A8_SRGB format not found in enumerated formats, but we'll try using it anyway");
	}
	
	// No gamma correction needed since both source and destination use sRGB format
	m_needManualGammaCorrection = false;
	
	Logger::info(str::format("OpenXRDirectMode: Using format: ", targetFormat, " (B8G8R8A8_SRGB) for both source and swapchain"));

	// Create a swapchain for each view
	m_eyeSwapchains.resize(viewCount);
	
	// Initialize the view structures for each eye
	m_views.resize(viewCount);
	for (uint32_t i = 0; i < viewCount; i++) {
		m_views[i].type = XR_TYPE_VIEW;
		m_views[i].next = nullptr;
	}
	
	// Initialize the projection view structures for each eye
	m_projectionViews.resize(viewCount);
	for (uint32_t i = 0; i < viewCount; i++) {
		m_projectionViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		m_projectionViews[i].next = nullptr;
	}

	for (uint32_t i = 0; i < viewCount; i++) {
		SwapchainInfo& swapchainInfo = m_eyeSwapchains[i];
		swapchainInfo.width = swapchainWidth;
		swapchainInfo.height = swapchainHeight;
		swapchainInfo.format = targetFormat;

		XrSwapchainCreateInfo createInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
		createInfo.createFlags = 0;
		createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		createInfo.format = targetFormat;
		createInfo.sampleCount = 1;
		createInfo.width = swapchainInfo.width;
		createInfo.height = swapchainInfo.height;
		createInfo.faceCount = 1;
		createInfo.arraySize = 1;
		createInfo.mipCount = 1;

		result = xrCreateSwapchain(m_session, &createInfo, &swapchainInfo.handle);
		if (XR_FAILED(result)) {
			Logger::err(str::format("OpenXRDirectMode: Failed to create swapchain for view ", i, " - error: ", (int)result));
			return false;
		}

		// Get swapchain images
		uint32_t imageCount = 0;
		result = xrEnumerateSwapchainImages(swapchainInfo.handle, 0, &imageCount, nullptr);
		if (XR_FAILED(result)) {
			Logger::err(str::format("OpenXRDirectMode: Failed to get swapchain image count for view ", i, ", error: ", (int)result));
			return false;
		}

		// Properly initialize each swapchain image structure with the correct type
		std::vector<XrSwapchainImageVulkan2KHR> swapchainImages(imageCount);
		for (uint32_t j = 0; j < imageCount; j++) {
			swapchainImages[j].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
			swapchainImages[j].next = nullptr;
		}
		
		result = xrEnumerateSwapchainImages(swapchainInfo.handle, imageCount, &imageCount, 
			reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchainImages.data()));
		if (XR_FAILED(result)) {
			Logger::err(str::format("OpenXRDirectMode: Failed to enumerate swapchain images for view ", i, ", error: ", (int)result));
			return false;
		}

		swapchainInfo.images.resize(imageCount);
		for (uint32_t j = 0; j < imageCount; j++) {
			swapchainInfo.images[j].image = swapchainImages[j];
		}

		Logger::info(str::format("OpenXRDirectMode: Created swapchain for view ", i, " with ", imageCount, " images"));
	}

	return true;
}

bool OpenXRDirectMode::CopyToSwapchains()
{
	// Only try to use timing if we have a valid session and query pool
	bool useGPUTiming = false;
	//bool useGPUTiming = (m_session != XR_NULL_HANDLE && m_queryPool != VK_NULL_HANDLE);
	
	if (useGPUTiming) {
		auto& timing = m_frameTimings[m_currentTimingIndex];
		timing.copyStart = Clock::now();
	}
	
	if (m_sharedTextures.empty() || m_eyeSwapchains.empty()) {
		Logger::err("OpenXRDirectMode: No textures to copy");
		return false;
	}

	// Create a single command buffer for all copy operations
	VkCommandBuffer cmdBuffer = beginSingleTimeCommands();
	if (cmdBuffer == VK_NULL_HANDLE) {
		Logger::err("OpenXRDirectMode: Failed to begin command buffer for copy");
		return false;
	}

	// Batch all image barriers for better performance
	std::vector<VkImageMemoryBarrier> preBarriers;
	std::vector<VkImageMemoryBarrier> postBarriers;
	std::vector<VkImageCopy> copyRegions;
	std::vector<std::pair<XrSwapchain, uint32_t>> swapchainReleaseQueue;

	preBarriers.reserve(m_eyeSwapchains.size());
	postBarriers.reserve(m_eyeSwapchains.size());
	copyRegions.reserve(m_eyeSwapchains.size());
	swapchainReleaseQueue.reserve(m_eyeSwapchains.size());

	// First, acquire all swapchain images
	for (uint32_t eyeIndex = 0; eyeIndex < m_eyeSwapchains.size(); eyeIndex++) {
		SwapchainInfo& swapchainInfo = m_eyeSwapchains[eyeIndex];
		
		uint32_t swapchainImageIndex;
		XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		XrResult result = xrAcquireSwapchainImage(swapchainInfo.handle, &acquireInfo, &swapchainImageIndex);
		if (XR_FAILED(result)) {
			Logger::err(str::format("OpenXRDirectMode: Failed to acquire swapchain image for eye ", eyeIndex));
			continue;
		}

		// Wait for the image to be ready
		XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		waitInfo.timeout = 8333333; // ~8.3ms timeout (1 frame at 120fps)
		result = xrWaitSwapchainImage(swapchainInfo.handle, &waitInfo);
		if (XR_FAILED(result)) {
			Logger::err(str::format("OpenXRDirectMode: Failed to wait for swapchain image for eye ", eyeIndex));
			continue;
		}

		VkImage dstImage = m_eyeSwapchains[eyeIndex].images[swapchainImageIndex].image.image;
		uint32_t sharedTextureIndex = (m_sharedTextures.size() == 1) ? 0 : eyeIndex;
		
		if (sharedTextureIndex >= m_sharedTextures.size()) {
			Logger::err(str::format("OpenXRDirectMode: No shared texture available for eye ", eyeIndex));
			continue;
		}
		else{
			Logger::info(str::format("OpenXRDirectMode: Swapchain image index: ", swapchainImageIndex));
		}
		
		SharedTextureData& srcData = m_sharedTextures[sharedTextureIndex];
		
		// Set up pre-copy barrier
		VkImageMemoryBarrier preBarrier = {};
		preBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		preBarrier.srcAccessMask = 0;
		preBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		preBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		preBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		preBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		preBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		preBarrier.image = dstImage;
		preBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		preBarrier.subresourceRange.baseMipLevel = 0;
		preBarrier.subresourceRange.levelCount = 1;
		preBarrier.subresourceRange.baseArrayLayer = 0;
		preBarrier.subresourceRange.layerCount = 1;
		
		// Set up post-copy barrier
		VkImageMemoryBarrier postBarrier = preBarrier;
		postBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		postBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		postBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		postBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		// Set up copy region
		VkImageCopy copyRegion = {};
		copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.srcSubresource.mipLevel = 0;
		copyRegion.srcSubresource.baseArrayLayer = 0;
		copyRegion.srcSubresource.layerCount = 1;
		copyRegion.dstSubresource = copyRegion.srcSubresource;
		
		if (m_sharedTextures.size() == 1 && srcData.width >= swapchainInfo.width * 2) {
			copyRegion.srcOffset = {static_cast<int32_t>(eyeIndex * swapchainInfo.width), 0, 0};
			copyRegion.extent = {swapchainInfo.width, swapchainInfo.height, 1};
		} else {
			copyRegion.srcOffset = {0, 0, 0};
			copyRegion.extent = {srcData.width, srcData.height, 1};
		}
		copyRegion.dstOffset = {0, 0, 0};

		preBarriers.push_back(preBarrier);
		postBarriers.push_back(postBarrier);
		copyRegions.push_back(copyRegion);
		swapchainReleaseQueue.push_back({swapchainInfo.handle, swapchainImageIndex});
	}

	// Batch all pre-copy barriers
	if (!preBarriers.empty()) {
		vkCmdPipelineBarrier(cmdBuffer,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0,
			0, nullptr,
			0, nullptr,
			preBarriers.size(), preBarriers.data());
	}

	// Perform all copies
	for (size_t i = 0; i < copyRegions.size(); i++) {
		uint32_t sharedTextureIndex = (m_sharedTextures.size() == 1) ? 0 : i;
		SharedTextureData& srcData = m_sharedTextures[sharedTextureIndex];
		
		vkCmdCopyImage(cmdBuffer,
			srcData.sourceImage, srcData.currentLayout,
			m_eyeSwapchains[i].images[swapchainReleaseQueue[i].second].image.image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &copyRegions[i]);
	}

	// Batch all post-copy barriers
	if (!postBarriers.empty()) {
		vkCmdPipelineBarrier(cmdBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			0,
			0, nullptr,
			0, nullptr,
			postBarriers.size(), postBarriers.data());
	}

	if (useGPUTiming) {
		vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, m_currentTimingIndex * 4);
	}
	
	// Submit all copy operations at once
	endSingleTimeCommands(cmdBuffer);

	// Release all swapchain images
	for (const auto& release : swapchainReleaseQueue) {
		XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		XrResult result = xrReleaseSwapchainImage(release.first, &releaseInfo);
		if (XR_FAILED(result)) {
			Logger::err("OpenXRDirectMode: Failed to release swapchain image");
		}
	}
	
	if (useGPUTiming) {
		vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, m_currentTimingIndex * 4 + 1);
		
		auto& timing = m_frameTimings[m_currentTimingIndex];
		timing.copyEnd = Clock::now();
	}

	return true;
}

void OpenXRDirectMode::GetPredictedDisplayTime(XrTime& time)
{
	time = m_frameState.predictedDisplayTime;
}

// Helper methods for command buffer management
VkCommandBuffer OpenXRDirectMode::beginSingleTimeCommands() {
    // Create persistent command pool if it doesn't exist
    if (m_persistentCommandPool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_vkQueueFamilyIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        VkResult vkResult = vkCreateCommandPool(m_vkDevice, &poolInfo, nullptr, &m_persistentCommandPool);
        if (vkResult != VK_SUCCESS) {
            Logger::err(str::format("OpenXRDirectMode: Failed to create persistent command pool, error: ", vkResult));
            return VK_NULL_HANDLE;
        }

        // Allocate command buffers for triple buffering
        const uint32_t numBuffers = 3;
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_persistentCommandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = numBuffers;

        m_commandBuffers.resize(numBuffers);
        vkResult = vkAllocateCommandBuffers(m_vkDevice, &allocInfo, m_commandBuffers.data());
        if (vkResult != VK_SUCCESS) {
            Logger::err(str::format("OpenXRDirectMode: Failed to allocate command buffers, error: ", vkResult));
            vkDestroyCommandPool(m_vkDevice, m_persistentCommandPool, nullptr);
            m_persistentCommandPool = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }

        // Create fences and semaphores for each command buffer
        m_commandBufferFences.resize(numBuffers);
        m_frameSyncSemaphores.resize(numBuffers);
        
        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // Start signaled
        
        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        for (uint32_t i = 0; i < numBuffers; i++) {
            // Create fence
            vkResult = vkCreateFence(m_vkDevice, &fenceInfo, nullptr, &m_commandBufferFences[i]);
            if (vkResult != VK_SUCCESS) {
                Logger::err(str::format("OpenXRDirectMode: Failed to create fence ", i));
                // Clean up previously created resources
                for (uint32_t j = 0; j < i; j++) {
                    vkDestroyFence(m_vkDevice, m_commandBufferFences[j], nullptr);
                    vkDestroySemaphore(m_vkDevice, m_frameSyncSemaphores[j], nullptr);
                }
                vkFreeCommandBuffers(m_vkDevice, m_persistentCommandPool, numBuffers, m_commandBuffers.data());
                vkDestroyCommandPool(m_vkDevice, m_persistentCommandPool, nullptr);
                m_persistentCommandPool = VK_NULL_HANDLE;
                return VK_NULL_HANDLE;
            }
            
            // Create semaphore
            vkResult = vkCreateSemaphore(m_vkDevice, &semaphoreInfo, nullptr, &m_frameSyncSemaphores[i]);
            if (vkResult != VK_SUCCESS) {
                Logger::err(str::format("OpenXRDirectMode: Failed to create semaphore ", i));
                // Clean up
                vkDestroyFence(m_vkDevice, m_commandBufferFences[i], nullptr);
                for (uint32_t j = 0; j < i; j++) {
                    vkDestroyFence(m_vkDevice, m_commandBufferFences[j], nullptr);
                    vkDestroySemaphore(m_vkDevice, m_frameSyncSemaphores[j], nullptr);
                }
                vkFreeCommandBuffers(m_vkDevice, m_persistentCommandPool, numBuffers, m_commandBuffers.data());
                vkDestroyCommandPool(m_vkDevice, m_persistentCommandPool, nullptr);
                m_persistentCommandPool = VK_NULL_HANDLE;
                return VK_NULL_HANDLE;
            }
        }
    }

    // Get the next command buffer and reset it
    VkCommandBuffer commandBuffer = m_commandBuffers[m_currentCommandBufferIndex];
    vkResetCommandBuffer(commandBuffer, 0);

    // Begin command buffer
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkResult vkResult = vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (vkResult != VK_SUCCESS) {
        Logger::err(str::format("OpenXRDirectMode: Failed to begin command buffer, error: ", vkResult));
        return VK_NULL_HANDLE;
    }

    return commandBuffer;
}

void OpenXRDirectMode::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
    if (commandBuffer == VK_NULL_HANDLE) {
        Logger::err("OpenXRDirectMode: Invalid command buffer in endSingleTimeCommands");
        return;
    }

    // End the command buffer
    VkResult vkResult = vkEndCommandBuffer(commandBuffer);
    if (vkResult != VK_SUCCESS) {
        Logger::err(str::format("OpenXRDirectMode: Failed to end command buffer, error: ", vkResult));
        return;
    }

    // Get the fence for this command buffer
    VkFence fence = m_commandBufferFences[m_currentCommandBufferIndex];

    // Wait for previous submission to complete with a shorter timeout
    // Only wait if we're running out of command buffers
    if ((m_currentCommandBufferIndex + 1) % m_commandBuffers.size() == m_lastCompletedBufferIndex) {
        vkWaitForFences(m_vkDevice, 1, &fence, VK_TRUE, 8333333); // ~8.3ms timeout for 120Hz
        m_lastCompletedBufferIndex = m_currentCommandBufferIndex;
    }
    vkResetFences(m_vkDevice, 1, &fence);

    // Set up pipeline stage flags for better pipelining
    VkPipelineStageFlags waitStages[] = { 
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT 
    };

    // Use semaphores for GPU-side synchronization
    VkSemaphore waitSemaphore = m_frameSyncSemaphores[m_currentCommandBufferIndex];
    VkSemaphore signalSemaphore = m_frameSyncSemaphores[(m_currentCommandBufferIndex + 1) % m_commandBuffers.size()];

    // Submit the command buffer
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    // Wait on the previous frame's semaphore if it exists
    if (m_frameCounter > 0) {
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &waitSemaphore;
        submitInfo.pWaitDstStageMask = waitStages;
    }
    
    // Signal the next frame's semaphore
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &signalSemaphore;

    vkResult = vkQueueSubmit(m_vkQueue, 1, &submitInfo, fence);
    if (vkResult != VK_SUCCESS) {
        Logger::err(str::format("OpenXRDirectMode: Failed to submit queue, error: ", vkResult));
        return;
    }

    // Wait for the command buffer to complete before proceeding
    // This ensures the copy operation is finished before we release the swapchain image
    vkResult = vkWaitForFences(m_vkDevice, 1, &fence, VK_TRUE, UINT64_MAX);
    if (vkResult != VK_SUCCESS) {
        Logger::err(str::format("OpenXRDirectMode: Failed to wait for fence, error: ", vkResult));
        return;
    }

    // Now it's safe to release the swapchain image
    // The GPU has finished all operations in this command buffer
}

void OpenXRDirectMode::GetViews(XrView*& views, XrSpaceLocation& headLocation, uint32_t& viewCount)
{
	Logger::info("OpenXRDirectMode: GetViews called");
	m_bPosesStale = true;

	views = m_views.data();
	headLocation = m_headLocation;
	viewCount = m_views.size();
}

void OpenXRDirectMode::GetViewsPublic(XrView* views, XrSpaceLocation& headLocation, uint32_t& viewCount)
{
	XrView* viewsPtr = views;
	GetViews(viewsPtr, headLocation, viewCount);
	// Copy the data since GetViews expects a reference to pointer
	if (views && viewsPtr && viewCount <= 2) {
		for (uint32_t i = 0; i < viewCount; ++i) {
			views[i] = viewsPtr[i];
		}
	}
}

void OpenXRDirectMode::ResetFrameState()
{
	Logger::info("OpenXRDirectMode: Resetting frame state for compositor takeover");
	
	// Reset frame tracking flags
	m_bFrameStarted = false;
	m_bSubmitCalled = false;
	
	// Reset atomic frame running state
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_bFrameRunning.store(false, std::memory_order_release);
		m_frameCompletionEvent.notify_all();
	}
}

bool OpenXRDirectMode::WaitPoses()
{
	Logger::info("OpenXRDirectMode: WaitPoses called - checking compositor state");
	
	// If VRCompositor is active, skip main pipeline processing
	// VRCompositor runs its own independent frame loop on a separate thread
	extern bool IsVRCompositorActive();
	bool compositorActive = IsVRCompositorActive();
	Logger::info(str::format("OpenXRDirectMode: IsVRCompositorActive() returned: ", compositorActive));
	
	if (compositorActive) {
		Logger::info("OpenXRDirectMode: WaitPoses - VRCompositor active, skipping main pipeline (compositor runs independently)");
		return true; // Return true but don't do any OpenXR calls - compositor handles everything
	}
	
	// VRCompositor is inactive - main pipeline should handle VR
	Logger::info("OpenXRDirectMode: WaitPoses - VRCompositor inactive, main pipeline resuming");

	// Check if we already have a frame in progress
	if (m_bFrameStarted) {
		Logger::warn(str::format("OpenXRDirectMode: WaitPoses called when a frame is already in progress (frame ", m_frameCounter, ")"));
		// Return true without starting a new frame - don't end the current one here
		return true;
	}

	Logger::info("OpenXRDirectMode: WaitPoses called");

	// Prepare the next frame and wait for the predicted display time
	XrFrameWaitInfo frameWaitInfo = { XR_TYPE_FRAME_WAIT_INFO };
	
	// Initialize frame state with the correct type and next pointer
	m_frameState = {};
	m_frameState.type = XR_TYPE_FRAME_STATE;
	m_frameState.next = nullptr;
	
	XrResult result = xrWaitFrame(m_session, &frameWaitInfo, &m_frameState);
	if (XR_FAILED(result)) {
		Logger::err(str::format("OpenXRDirectMode: xrWaitFrame failed with error code: ", (int)result));
		return false;
	}

	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_bFrameRunning.store(true, std::memory_order_release);
	}

	XrFrameBeginInfo frameBeginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
	result = xrBeginFrame(m_session, &frameBeginInfo);
	if (XR_FAILED(result)) {
		Logger::err(str::format("OpenXRDirectMode: xrBeginFrame failed with error code: ", (int)result));
		return false;
	}

	// Get the current view poses
	XrViewLocateInfo viewLocateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
	viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	viewLocateInfo.displayTime = m_frameState.predictedDisplayTime;
	viewLocateInfo.space = m_referenceSpace;

	// Properly initialize the view state structure
	m_viewState  = {};
	m_viewState.type = XR_TYPE_VIEW_STATE;
	m_viewState.next = nullptr;

	uint32_t viewCount = (uint32_t)m_views.size();
	result = xrLocateViews(m_session, &viewLocateInfo, &m_viewState, viewCount, &viewCount, m_views.data());
	if (XR_FAILED(result)) 
	{
		Logger::err(str::format("OpenXRDirectMode: xrLocateViews failed with error code: ", (int)result));
		m_bFrameStarted = false;
		return false;
	}

	XrSpaceLocation headLocation{XR_TYPE_SPACE_LOCATION};
	result = xrLocateSpace(m_headSpace, m_referenceSpace, m_frameState.predictedDisplayTime, &headLocation);
	if (XR_FAILED(result))
	{
		Logger::err(str::format("OpenXRDirectMode: xrLocateSpace failed with error code: ", (int)result));
		return false;
	}
	m_headLocation = headLocation;

	// Increment the frame counter after successfully starting a frame
	m_frameCounter++;
	Logger::info(str::format("OpenXRDirectMode: Frame ", m_frameCounter, " started successfully"));
	m_bFrameStarted = true;
	return true;
}

void OpenXRDirectMode::SetRenderTextureSize(uint32_t width, uint32_t height, int msaa)
{
	m_nRenderWidth = width;
	m_nRenderHeight = height;
	m_nMSAA = std::max(1, std::min(16, msaa));

  	if (m_nMSAA == 1)
    	m_nMSAA = 0;
}

void OpenXRDirectMode::OnRenderTargetChanged(dxvk::Rc<dxvk::DxvkDevice> device, dxvk::D3D9Surface* rt)
{
	D3DSURFACE_DESC desc;
	rt->GetDesc(&desc);

	VulkanTextureData vulkanData;

	if (desc.Width == m_nRenderWidth && desc.Height >= m_nRenderHeight) {
		vulkanData.height = desc.Height;
		vulkanData.width = desc.Width;
		// VkPhysicalDevice
		vulkanData.physicalDevice = device->adapter()->handle();
		// VkDevice
		vulkanData.device = device->handle();
		// VkImage
		vulkanData.image = rt->GetCommonTexture()->GetImage()->handle();
		// VkInstance
		vulkanData.instance = device->instance()->vki()->instance();
		// VkQueue
		vulkanData.queue = device->queues().graphics.queueHandle;
		vulkanData.queueFamilyIndex = device->queues().graphics.queueFamily;
		vulkanData.format = VK_FORMAT_B8G8R8A8_SRGB;
		vulkanData.sampleCount = VkSampleCountFlagBits(m_nMSAA);

		if (m_nMSAA > 1) {
			// submitting multi-sampled textures to OpenVR seems to cause driver crashes with AMD under certain circumstances
			// so submit the resolved texture, instead. it should be resolved at the point it's submitted.
			// vulkanData.image = rt->GetCommonTexture()->GetResolveImage()->handle();
			// vulkanData.sampleCount = VkSampleCountFlagBits::VK_SAMPLE_COUNT_1_BIT;
		}

		StoreSharedTexture(0, &vulkanData);
	}
}

void OpenXRDirectMode::BeginFrameTiming() {
    // Safety check - don't proceed if we don't have a valid session
    if (!m_session || m_frameTimings.empty()) {
        return;
    }
    
    m_currentTimingIndex = (m_currentTimingIndex + 1) % TIMING_HISTORY_SIZE;
    m_frameTimings[m_currentTimingIndex] = FrameTimings();
    m_frameTimings[m_currentTimingIndex].frameStart = Clock::now();
}

void OpenXRDirectMode::EndFrameTiming() {
    auto& timing = m_frameTimings[m_currentTimingIndex];
    timing.frameEnd = Clock::now();
    
    // Log timings every 60 frames
    if (m_frameCounter % 60 == 0) {
        LogTimings();
    }
}

void OpenXRDirectMode::LogTimings() {
    // Safety check - don't proceed if we don't have a valid session
    if (!m_session || m_frameTimings.empty()) {
        return;
    }

    std::vector<double> frameTimes;
    std::vector<double> waitPosesTimes;
    std::vector<double> copyTimes;
    std::vector<double> submitTimes;
    std::vector<double> presentTimes;
    std::vector<double> gpuCopyTimes;
    std::vector<double> gpuSubmitTimes;
    
    frameTimes.reserve(TIMING_HISTORY_SIZE);
    waitPosesTimes.reserve(TIMING_HISTORY_SIZE);
    copyTimes.reserve(TIMING_HISTORY_SIZE);
    submitTimes.reserve(TIMING_HISTORY_SIZE);
    presentTimes.reserve(TIMING_HISTORY_SIZE);
    gpuCopyTimes.reserve(TIMING_HISTORY_SIZE);
    gpuSubmitTimes.reserve(TIMING_HISTORY_SIZE);
    
    for (const auto& timing : m_frameTimings) {
        if (timing.frameStart != TimePoint() && timing.frameEnd != TimePoint()) {
            frameTimes.push_back(Duration(timing.frameEnd - timing.frameStart).count());
            
            if (timing.waitPosesStart != TimePoint() && timing.waitPosesEnd != TimePoint()) {
                waitPosesTimes.push_back(Duration(timing.waitPosesEnd - timing.waitPosesStart).count());
            }
            
            if (timing.copyStart != TimePoint() && timing.copyEnd != TimePoint()) {
                copyTimes.push_back(Duration(timing.copyEnd - timing.copyStart).count());
            }
            
            if (timing.submitStart != TimePoint() && timing.submitEnd != TimePoint()) {
                submitTimes.push_back(Duration(timing.submitEnd - timing.submitStart).count());
            }
            
            if (timing.presentStart != TimePoint() && timing.presentEnd != TimePoint()) {
                presentTimes.push_back(Duration(timing.presentEnd - timing.presentStart).count());
            }
        }
    }

    // Only try to get GPU timings if we have a valid query pool
    if (m_queryPool != VK_NULL_HANDLE) {
        uint64_t timestamps[TIMING_HISTORY_SIZE * 4];
        VkResult result = vkGetQueryPoolResults(m_vkDevice, m_queryPool, 0, 
            TIMING_HISTORY_SIZE * 4, sizeof(timestamps), timestamps, 
            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            
        if (result == VK_SUCCESS) {
            for (size_t i = 0; i < TIMING_HISTORY_SIZE; i++) {
                uint64_t copyStart = timestamps[i * 4];
                uint64_t copyEnd = timestamps[i * 4 + 1];
                uint64_t submitStart = timestamps[i * 4 + 2];
                uint64_t submitEnd = timestamps[i * 4 + 3];
                
                if (copyEnd > copyStart) {
                    gpuCopyTimes.push_back(static_cast<double>(copyEnd - copyStart) * 1e-6);
                }
                if (submitEnd > submitStart) {
                    gpuSubmitTimes.push_back(static_cast<double>(submitEnd - submitStart) * 1e-6);
                }
            }
        }
    }
    
    Logger::info(str::format("Frame Timing Statistics (ms):"));
    Logger::info(str::format("  Total Frame:     Avg: ", GetAverageTime(frameTimes)));
    Logger::info(str::format("  Wait Poses:      Avg: ", GetAverageTime(waitPosesTimes)));
    Logger::info(str::format("  Copy (CPU):      Avg: ", GetAverageTime(copyTimes)));
    Logger::info(str::format("  Submit (CPU):    Avg: ", GetAverageTime(submitTimes)));
    Logger::info(str::format("  Present:         Avg: ", GetAverageTime(presentTimes)));
    
    if (!gpuCopyTimes.empty()) {
        Logger::info(str::format("  Copy (GPU):      Avg: ", GetAverageTime(gpuCopyTimes)));
    }
    if (!gpuSubmitTimes.empty()) {
        Logger::info(str::format("  Submit (GPU):    Avg: ", GetAverageTime(gpuSubmitTimes)));
    }
}

double OpenXRDirectMode::GetAverageTime(const std::vector<double>& times) const {
    if (times.empty()) return 0.0;
    
    double sum = 0.0;
    for (const double& time : times) {
        sum += time;
    }
    return sum / times.size();
}

// ============================================================================
// VR Compositor State Management
// ============================================================================

// Forward declarations for functions used by VRCompositor
void CheckAndCopyTrackedVGUITexture();



// Global compositor instance - NOW USING OpenXRDirectMode's m_vrCompositor
// static VRCompositor g_vrCompositor;
// g_vrCompositor is declared earlier in the file

// TF2VR: VGUI Render Target Tracking (global scope for access from VRCompositor)
dxvk::D3D9Surface* g_trackedVGUISurface = nullptr;
dxvk::D3D9CommonTexture* g_trackedVGUITexture = nullptr;
std::mutex g_vguiTrackingMutex;

// SAFE VGUI TEXTURE POLLING - Called from compositor thread
void CheckAndCopyTrackedVGUITexture() {
    static int checkCount = 0;
    checkCount++;
    
    // DEBUG: Log every call to see if this function is being called
    if (checkCount % 120 == 1) {  // Log every ~2 seconds
        dxvk::Logger::info(str::format("VR Compositor: 🔍 CheckAndCopyTrackedVGUITexture called #", checkCount));
    }
    
    // Try to acquire the lock, but don't block if we can't get it
    std::unique_lock<std::mutex> lock(g_vguiTrackingMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        if (checkCount % 120 == 1) {  // Log lock failures occasionally
            dxvk::Logger::info("VR Compositor: 🔒 Lock busy, skipping copy");
        }
        return; // Source is busy, try next frame
    }
    
    // Check if we have a tracked texture to copy
    if (g_trackedVGUITexture && g_trackedVGUISurface) {
        auto desc = g_trackedVGUITexture->Desc();
        auto dxvkImage = g_trackedVGUITexture->GetImage();
        
        if (dxvkImage != nullptr) {
            VkImage vkImage = dxvkImage->handle();
            
            if (checkCount % 60 == 1) {  // Log every ~1 second
                dxvk::Logger::info(str::format("VR Compositor: 🔄 SAFE POLLING COPY #", checkCount, " - VkImage: ", (void*)vkImage, " size: ", desc->Width, "x", desc->Height));
            }
            
            // This is safe because we're in the compositor thread, not interrupting Source
            g_vrCompositor->CopyAndStoreMenuTexture(vkImage, desc->Width, desc->Height);
            
            // CRITICAL: Submit a frame so the compositor knows to render the copied texture
            g_vrCompositor->SubmitDXVKTexture(reinterpret_cast<void*>(vkImage), desc->Width, desc->Height);
        }
    }
}

// Initialize compositor with OpenXR manager
void InitVRCompositor(OpenXRDirectMode* manager) {
    dxvk::Logger::info("VR Compositor: Initializing with OpenXR manager");
    if (g_vrCompositor) {
        g_vrCompositor->SetOpenXRManager(manager);
    }
}

// Check if compositor is active (used by normal VR hooks)
bool IsVRCompositorActive() {
    bool isActive = g_vrCompositor ? g_vrCompositor->IsCompositorActive() : false;
    static bool lastState = false;
    if (isActive != lastState) {
        dxvk::Logger::info(str::format("IsVRCompositorActive: State changed to ", isActive ? "ACTIVE" : "INACTIVE"));
        lastState = isActive;
    }
    return isActive;
}

// ============================================================================
// Export Functions
// ============================================================================

extern "C" {

void __declspec(dllexport) dxvkSetSourceState(int state) {
    dxvk::Logger::info(str::format("dxvkSetSourceState called with state: ", state, " g_vrCompositor: ", (void*)g_vrCompositor));
    if (g_vrCompositor) {
        g_vrCompositor->SetSourceState(static_cast<SourceEngineState>(state));
    } else {
        dxvk::Logger::warn("dxvkSetSourceState: g_vrCompositor is null!");
    }
}

bool __declspec(dllexport) dxvkIsCompositorActive() {
    return g_vrCompositor ? g_vrCompositor->IsCompositorActive() : false;
}

void __declspec(dllexport) dxvkSubmitMenuFrame(void* textureHandle, int width, int height) {
    dxvk::Logger::info(str::format("VR Compositor: dxvkSubmitMenuFrame called - handle: ", (void*)textureHandle, " size: ", width, "x", height));
    if (g_vrCompositor) {
        g_vrCompositor->SubmitFrame(textureHandle, width, height, true, true); // isVGUITexture=true, isDXVKTexture=true
    } else {
        dxvk::Logger::warn("VR Compositor: dxvkSubmitMenuFrame called but g_vrCompositor is null");
    }
}

// TF2VR: VGUI Render Target Tracking Implementation
extern "C" void TF2VR_TrackVGUIRenderTarget(dxvk::D3D9Surface* surface, dxvk::D3D9CommonTexture* texture) {
    if (!g_vrCompositor || !g_vrCompositor->IsCompositorActive()) {
        return; // Only track when compositor is active
    }
    
    std::lock_guard<std::mutex> lock(g_vguiTrackingMutex);
    
    auto desc = texture->Desc();
    
    // Be more selective - only track textures that look like main UI render targets
    // Log all candidates but be selective about what we actually track
    static int candidateCount = 0;
    dxvk::Logger::info(str::format("TF2VR: CANDIDATE #", ++candidateCount, " render target - size: ", desc->Width, "x", desc->Height, 
                                   " format: ", desc->Format, " usage: ", desc->Usage, " aspect: ", (float)desc->Width/desc->Height));
    
    // Only track if this looks like a primary UI texture:
    // - Reasonable UI size (not too small, not too huge)  
    // - Standard aspect ratio for menus
    // - Appropriate format for UI rendering
    if (desc->Width >= 800 && desc->Width <= 2560 &&
        desc->Height >= 600 && desc->Height <= 1440) {
        
        float aspectRatio = (float)desc->Width / desc->Height;
        if (aspectRatio >= 1.3f && aspectRatio <= 1.8f) {  // Common 4:3 to 16:9 range
            
            // Store the DXVK surface and texture for later use
            g_trackedVGUISurface = surface;
            g_trackedVGUITexture = texture;
            
            // DEFERRED COPY: Just mark that we need a copy, don't do it immediately
            // Immediate copying during SetRenderTarget is too dangerous - Source is actively rendering
            static int setRenderTargetDetectCount = 0;
            setRenderTargetDetectCount++;
            if (setRenderTargetDetectCount % 60 == 1) {  // Log every ~1 second
                dxvk::Logger::info(str::format("TF2VR: 🎯 DETECTED VGUI RENDER TARGET #", setRenderTargetDetectCount, " - size: ", desc->Width, "x", desc->Height, " (will copy later)"));
            }
            
            dxvk::Logger::info(str::format("TF2VR: *** TRACKING *** VGUI render target - size: ", desc->Width, "x", desc->Height, 
                                           " format: ", desc->Format, " usage: ", desc->Usage));
        } else {
            dxvk::Logger::info(str::format("TF2VR: Rejected (aspect ratio ", aspectRatio, " outside 1.3-1.8 range)"));
        }
    } else {
        dxvk::Logger::info(str::format("TF2VR: Rejected (size outside 800x600 to 2560x1440 range)"));
    }
}

} // extern "C"

// Internal C++ function - no C linkage needed
void TF2VR_NotifyVGUIFrameComplete() {
    if (!g_vrCompositor || !g_vrCompositor->IsCompositorActive()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(g_vguiTrackingMutex);
    
    // If we have a tracked VGUI texture, submit it to the compositor
    if (g_trackedVGUITexture && g_trackedVGUISurface) {
        auto desc = g_trackedVGUITexture->Desc();
        
        // Get the underlying Vulkan image from the D3D9CommonTexture
        auto dxvkImage = g_trackedVGUITexture->GetImage();
        if (dxvkImage != nullptr) {
            // DISABLED: Let safe polling handle copying instead of immediate copying here
            VkImage vkImage = dxvkImage->handle();
            
            static int copyCount = 0;
            copyCount++;
            if (copyCount % 30 == 1) {  // Log every ~0.5 seconds at 60fps
                dxvk::Logger::info(str::format("TF2VR: 📝 MENU UPDATE DETECTED #", copyCount, " - VkImage: ", (void*)vkImage, " size: ", desc->Width, "x", desc->Height, " (safe polling will copy)"));
            }
            
            // NOTE: CopyAndStoreMenuTexture will be called by CheckAndCopyTrackedVGUITexture() instead
        } else {
            dxvk::Logger::warn("TF2VR: Failed to get DXVK image from tracked texture");
        }
    } else {
        dxvk::Logger::warn("TF2VR: NotifyVGUIFrameComplete called but no tracked VGUI texture");
    }
}



