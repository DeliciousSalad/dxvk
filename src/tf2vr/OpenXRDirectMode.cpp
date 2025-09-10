#include "OpenXRDirectMode.h"
#include "HMDInterface.h"
#include "hud_position_shared.h"
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

// Forward declaration for TF2VR VGUI functions
extern "C" void TF2VR_NotifyVGUIPresentComplete();

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

	// COMPOSITOR FRAME CONTROL: Block TF2 at frame START if compositor is active
	extern bool IsVRCompositorActive();
	if (IsVRCompositorActive()) {
		if (g_vrCompositor) {
			Logger::info("OpenXRDirectMode: 🚫 PrePresent BLOCKING TF2 - waiting for VR frame permission");
			
			// BLOCK HERE: Wait for compositor to allow this frame to proceed
			std::unique_lock<std::mutex> lock(g_vrCompositor->m_tf2FrameSignalMutex);
			g_vrCompositor->m_tf2FrameCondition.wait(lock, [&] {
				return g_vrCompositor->m_tf2CanRenderFrame.load();
			});
			
			// CONSUME PERMISSION: Set to false so only this frame can proceed
			g_vrCompositor->m_tf2CanRenderFrame = false;
			
			Logger::info("OpenXRDirectMode: ✅ PrePresent UNBLOCKED - TF2 frame allowed to proceed");
		} else {
			Logger::info("OpenXRDirectMode: PrePresent skipped - compositor is active");
		}
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
		// LOCK-FREE APPROACH: No synchronization needed in PostPresent
		// Texture copying happens independently without blocking TF2
		Logger::info("OpenXRDirectMode: PostPresent skipped - compositor is active (lock-free mode)");
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
	extern std::timed_mutex* GetPresentSyncMutex();  // Forward declaration
	if (IsVRCompositorActive()) {
		// TEXTURE COPY NOTIFICATION: Right before DXVK present - perfect timing!
		// This mirrors the main rendering setup timing
		TF2VR_NotifyVGUIPresentComplete();
		
		Logger::info("OpenXRDirectMode: 📸 TEXTURE COPY - Right before DXVK present (perfect timing)");
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
	extern std::timed_mutex* GetPresentSyncMutex();  // Forward declaration
	if (IsVRCompositorActive()) {
		// Notify that a VGUI frame may have been completed
		TF2VR_NotifyVGUIFrameComplete();
		
		// NOTIFY COMPLETION: Signal compositor that frame is ready for copy
		if (g_vrCompositor) {
			Logger::info("OpenXRDirectMode: 📝 PostPresent - notifying compositor of frame completion");
			g_vrCompositor->NotifyTF2FrameComplete();
		}
		
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
	// Add timeout safeguard to prevent infinite loops during map transitions
	auto timeoutStart = std::chrono::steady_clock::now();
	const auto maxWaitTime = std::chrono::milliseconds(50);
	int timeoutWarningCount = 0;
	
	while (m_bFrameRunning.load(std::memory_order_acquire)) {
		std::unique_lock<std::mutex> lock(m_mutex);
		if (m_frameCompletionEvent.wait_for(lock, std::chrono::milliseconds(16)) == std::cv_status::timeout) {
			timeoutWarningCount++;
			Logger::warn(str::format("OpenXRDirectMode: Waiting for previous frame completion (", timeoutWarningCount, ")"));
			
			// Check if we've exceeded the maximum wait time
			auto elapsed = std::chrono::steady_clock::now() - timeoutStart;
			if (elapsed > maxWaitTime) {
				Logger::err("OpenXRDirectMode: Frame completion timeout exceeded! Force-clearing frame state to prevent infinite loop.");
				m_bFrameRunning.store(false, std::memory_order_release);
				break;
			}
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
// NOTE: No mutex needed - using single mutex approach to prevent deadlocks

// Dual texture tracking for TF2's double-buffering
struct VGUITextureInfo {
    dxvk::D3D9CommonTexture* texture = nullptr;
    dxvk::D3D9Surface* surface = nullptr;
    std::chrono::high_resolution_clock::time_point lastUsed;
    int useCount = 0;
    void* textureAddress = nullptr;  // Track D3D9 texture object address
};
static VGUITextureInfo g_vguiTextureA;
static VGUITextureInfo g_vguiTextureB;
static VGUITextureInfo* g_mostRecentVGUITexture = nullptr;

// Track render operations to detect multi-pass rendering
static std::atomic<int> g_renderOpsSinceLastCopy{0};

// Global frame tracking with queue to prevent dropped frames
static std::atomic<int> g_frameCompleteCount{0};
static std::atomic<int> g_lastProcessedFrame{0};  // Track which frame we last processed

// Track source texture stability to prevent copying from actively written textures
static VkImage g_lastSourceTexture = VK_NULL_HANDLE;
static int g_sourceTextureStableCount = 0;

// Track texture usage frequency to prefer final outputs over intermediates

// ============================================================================
// HUD Position Sharing System
// ============================================================================


// Global shared HUD position data - accessible from both game and compositor
static SharedHUDPositionData g_sharedHUDPosition = {
    {0.0f, 0.0f, 0.0f},      // viewer_pos
    {0.0f, 0.0f, 0.0f},      // upper_left
    {0.0f, 0.0f, 0.0f},      // upper_right
    {0.0f, 0.0f, 0.0f},      // lower_left
    {0.0f, 0.0f, 0.0f},      // lower_right
    0.0,                     // last_update_time
    false,                   // is_valid
    false,                   // is_custom_bounds
    0,                       // frame_number
    1.0f                     // world_scale
};

// Mutex to protect shared HUD position data
static std::mutex g_hudPositionMutex;

// Function for VRCompositor to access current HUD position data
SharedHUDPositionData GetCurrentHUDPosition() {
    std::lock_guard<std::mutex> lock(g_hudPositionMutex);
    return g_sharedHUDPosition;  // Return a copy to avoid race conditions
}
struct TextureUsageInfo {
    VkImage textureHandle;
    int usageCount;
    std::chrono::steady_clock::time_point lastUsage;
};
static std::vector<TextureUsageInfo> g_textureUsageHistory;

// Track VGUI render completion to avoid capturing partial renders
static std::atomic<int> g_vguiRenderPassCount{0};
static std::chrono::steady_clock::time_point g_lastVGUIRenderTime;
// Removed g_vguiTimingMutex to prevent deadlocks - using lock-free approach

// Track VGUI flush completion - this is when UI rendering is truly complete
static std::atomic<bool> g_vguiFlushCompleted{false};
static std::atomic<int> g_vguiFlushCount{0};

// Async texture readiness signal
static std::atomic<bool> g_textureReady{false};

// ASYNC TEXTURE COPY - Called from VR compositor thread only when signaled
void CheckAndCopyTrackedVGUITexture() {
    // Check if TF2 has signaled a new texture is ready
    if (!g_textureReady.load()) {
        return; // No new texture ready
    }
    
    // Clear signal and proceed with copy
    g_textureReady.store(false);
    
    static int checkCount = 0;
    checkCount++;
    
    dxvk::Logger::info(str::format("VR Compositor: 🔄 ASYNC COPY #", checkCount, " - signaled by TF2"));
    
    // NO LOCKS NEEDED - purely async approach
    
    // Use the most recently tracked texture (simple approach)
    VGUITextureInfo* textureToUse = g_mostRecentVGUITexture;
    
    // DYNAMIC MODE: Use Slot B during loading, Slot A otherwise  
    static const int FORCE_SLOT_MODE = 9; // NEW: Simple state-based slot selection
    
    if (FORCE_SLOT_MODE == 1 && g_vguiTextureA.texture != nullptr) {
        // Force use Slot A only
        textureToUse = &g_vguiTextureA;
    } else if (FORCE_SLOT_MODE == 2 && g_vguiTextureB.texture != nullptr) {
        // Force use Slot B only
        textureToUse = &g_vguiTextureB;
    } else if (FORCE_SLOT_MODE == 8) {
        // SMART MODE: Detect loading state and choose appropriate slot
        // During loading: use Slot B (has loading screen content)
        // During menu/game: use Slot A (has complete UI)
        
        // Simple heuristic: Use Slot B if it's been updated more recently than Slot A
        // During loading, Slot B gets the loading screen content while Slot A stays stale
        // During normal UI, Slot A gets updated with complete UI
        
        auto now = std::chrono::high_resolution_clock::now();
        
        // Calculate how recently each slot was used
        auto slotAAge = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_vguiTextureA.lastUsed).count();
        auto slotBAge = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_vguiTextureB.lastUsed).count();
        
        // If Slot B is much more recent than Slot A, we're probably loading
        bool probablyLoading = (g_vguiTextureB.texture != nullptr) && 
                              (g_vguiTextureA.texture != nullptr) &&
                              (slotBAge < slotAAge - 100); // Slot B updated more than 100ms more recently
        
        if (probablyLoading) {
            textureToUse = &g_vguiTextureB;  // Loading screen is on Slot B
            static int logCount = 0;
            if (++logCount % 120 == 1) {
                dxvk::Logger::info(str::format("TF2VR: 🔄 SMART MODE - Using Slot B (loading, B age: ", slotBAge, "ms, A age: ", slotAAge, "ms)"));
            }
        } else if (g_vguiTextureA.texture != nullptr) {
            textureToUse = &g_vguiTextureA;  // Complete UI is on Slot A
            static int logCount = 0;
            if (++logCount % 120 == 1) {
                dxvk::Logger::info(str::format("TF2VR: 🔄 SMART MODE - Using Slot A (menu/game, A age: ", slotAAge, "ms, B age: ", slotBAge, "ms)"));
            }
        } else if (g_vguiTextureB.texture != nullptr) {
            textureToUse = &g_vguiTextureB;  // Fallback to Slot B
        }
    } else if (FORCE_SLOT_MODE == 9) {
        // REALTIME MODE: Check loading state directly without relying on delayed state updates
        // Use the same logic as DetermineSourceState() but execute it here in real-time
        
        // Get the stored state first (might be delayed)
        SourceEngineState storedState = g_vrCompositor ? g_vrCompositor->GetCurrentState() : SOURCE_STATE_GAMEPLAY;
        
        // Also check loading directly (same as what the client checks)
        // Note: We can't call engine functions from DXVK, but we can use texture usage patterns
        // If Slot B is more active than Slot A, we're probably loading
        auto now = std::chrono::high_resolution_clock::now();
        auto slotAAge = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_vguiTextureA.lastUsed).count();
        auto slotBAge = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_vguiTextureB.lastUsed).count();
        
        // Prefer stored state if available, fall back to heuristic
        bool probablyLoading = (storedState == SOURCE_STATE_LOADING) || 
                              (storedState == SOURCE_STATE_GAMEPLAY && slotBAge < slotAAge - 50);
        
        // Minimal logging for state decisions
        static int debugCount = 0;
        if (++debugCount % 300 == 1) {  // Much less frequent logging
            dxvk::Logger::info(str::format("TF2VR: State=", storedState, " Loading=", probablyLoading ? "YES" : "NO"));
        }
        
        if (probablyLoading && g_vguiTextureB.texture != nullptr) {
            textureToUse = &g_vguiTextureB;  // Loading screen is on Slot B
        } else if (g_vguiTextureA.texture != nullptr) {
            textureToUse = &g_vguiTextureA;  // Complete UI is on Slot A
        } else if (g_vguiTextureB.texture != nullptr) {
            textureToUse = &g_vguiTextureB;  // Fallback to Slot B
        }
    } else if (FORCE_SLOT_MODE == 4 && g_vguiTextureA.texture != nullptr) {
        // Slot A on flush only - use the existing flush hook without additional timing
        // This relies on D3D9 Flush() being called when UI rendering is complete
        textureToUse = &g_vguiTextureA;
    } else if (FORCE_SLOT_MODE == 5 && g_vguiTextureA.texture != nullptr) {
        // Slot A on paint complete - triggered right after VGui_Paint() finishes
        // This is the PERFECT timing - UI is painted but before any cleanup
        textureToUse = &g_vguiTextureA;
    } else if (FORCE_SLOT_MODE == 6 && g_vguiTextureA.texture != nullptr) {
        // Present-triggered - called from TF2VR_NotifyVGUIPresentComplete()
        // RenderDoc shows this is PERFECT - after all 5 passes, final complete texture!
        textureToUse = &g_vguiTextureA;
    } else if (FORCE_SLOT_MODE == 7) {
        // STABLE TEXTURE MODE: Pick one texture object and stick with it
        // This eliminates flickering caused by TF2's texture switching
        static VGUITextureInfo* stableTexture = nullptr;
        static bool stableTextureChosen = false;
        
        if (!stableTextureChosen) {
            // Choose the first texture we see with good characteristics
            if (g_vguiTextureA.texture != nullptr && g_vguiTextureA.useCount > 10) {
                stableTexture = &g_vguiTextureA;
                stableTextureChosen = true;
                dxvk::Logger::info("VR Compositor: 🔒 STABLE MODE - locked to Slot A texture for consistent rendering");
            } else if (g_vguiTextureB.texture != nullptr && g_vguiTextureB.useCount > 10) {
                stableTexture = &g_vguiTextureB;
                stableTextureChosen = true;
                dxvk::Logger::info("VR Compositor: 🔒 STABLE MODE - locked to Slot B texture for consistent rendering");
            }
        }
        
        if (stableTextureChosen && stableTexture && stableTexture->texture != nullptr) {
            textureToUse = stableTexture;
        }
    } else if (FORCE_SLOT_MODE == 0) {
        // Auto mode - use timestamp comparison
        if (g_vguiTextureA.texture != nullptr && g_vguiTextureB.texture != nullptr) {
            if (g_vguiTextureA.lastUsed > g_vguiTextureB.lastUsed) {
                textureToUse = &g_vguiTextureA;
            } else {
                textureToUse = &g_vguiTextureB;
            }
        } else if (g_vguiTextureA.texture != nullptr) {
            textureToUse = &g_vguiTextureA;
        } else if (g_vguiTextureB.texture != nullptr) {
            textureToUse = &g_vguiTextureB;
        }
    }
    
    // Fallback if forced slot is empty
    if (!textureToUse) {
        if (g_vguiTextureA.texture != nullptr) {
            textureToUse = &g_vguiTextureA;
        } else if (g_vguiTextureB.texture != nullptr) {
            textureToUse = &g_vguiTextureB;
        } else {
            textureToUse = g_mostRecentVGUITexture;
        }
    }
    
    // Fallback to regular tracking if dual tracking hasn't been set up yet
    if (!textureToUse && g_trackedVGUITexture && g_trackedVGUISurface) {
        // Create a temporary texture info for backwards compatibility
        static VGUITextureInfo fallbackInfo;
        fallbackInfo.texture = g_trackedVGUITexture;
        fallbackInfo.surface = g_trackedVGUISurface;
        fallbackInfo.textureAddress = (void*)g_trackedVGUITexture;
        textureToUse = &fallbackInfo;
    }
    
    if (textureToUse && textureToUse->texture && textureToUse->surface) {
        auto desc = textureToUse->texture->Desc();
        auto dxvkImage = textureToUse->texture->GetImage();
        
        if (dxvkImage != nullptr) {
            VkImage vkImage = dxvkImage->handle();
            
            // IMMEDIATE RESPONSE: TF2 knows what texture it wants us to use
            // Trust TF2's texture management and copy whatever it gives us
            g_lastSourceTexture = vkImage;
            g_sourceTextureStableCount++;
            
            // Log which texture we're copying from (A or B) and why
            const char* slotName = (textureToUse == &g_vguiTextureA) ? "A" : 
                                   (textureToUse == &g_vguiTextureB) ? "B" : "FALLBACK";
            const char* reason = (FORCE_SLOT_MODE == 1) ? "FORCE_A" : 
                                 (FORCE_SLOT_MODE == 2) ? "FORCE_B" : 
                                 (FORCE_SLOT_MODE == 4) ? "FLUSH_A" :
                                 (FORCE_SLOT_MODE == 5) ? "PAINT_A" :
                                 (FORCE_SLOT_MODE == 6) ? "PRESENT_A" :
                                 (FORCE_SLOT_MODE == 7) ? "STABLE" :
                                 (FORCE_SLOT_MODE == 0) ? "AUTO" : "FALLBACK";
            
            // Count render operations to detect if we're capturing mid-render
            int renderOps = g_renderOpsSinceLastCopy.exchange(0);
            
            dxvk::Logger::info(str::format("VR Compositor: 🚀 IMMEDIATE COPY - VkImage: ", (void*)vkImage, " (call #", g_sourceTextureStableCount, ") from D3D9 texture: ", (void*)textureToUse->texture, " [slot ", slotName, " - ", reason, "] after ", renderOps, " render ops"));
            
            // CRITICAL: Check if this texture is likely to be a blank/empty texture
            // TF2 might be alternating between actual VGUI content and blank textures
            
            // Copy immediately for maximum responsiveness
            // The frame-complete signal should be sufficient synchronization
            
            // This should now be safe because TF2 has finished its frame + safety delay + texture is stable
            bool copySuccess = g_vrCompositor->CopyAndStoreMenuTexture(vkImage, desc->Width, desc->Height);
            
            if (copySuccess) {
                dxvk::Logger::info("VR Compositor: ✅ Copy successful - texture ready for rendering");
                // NOTE: No need to call SubmitFrame - the copy operation makes the texture available for rendering
                // The compositor's render loop will automatically use the copied texture
            } else {
                dxvk::Logger::err("VR Compositor: ❌ Copy failed");
            }
        }
    } else {
        dxvk::Logger::warn("VR Compositor: Frame-complete signal but no tracked VGUI texture");
    }
}

// TF2VR: VGUI Flush Completion Handler - Called when D3D9 flush completes
extern "C" void TF2VR_NotifyVGUIFlushComplete() {
    if (!g_vrCompositor || !g_vrCompositor->IsCompositorActive()) {
        return;
    }
    
    // Signal that VGUI rendering is complete and safe to copy
    int flushNum = g_vguiFlushCount.fetch_add(1) + 1;
    g_vguiFlushCompleted.store(true);
    
    if (flushNum % 60 == 1) {  // Log occasionally
        dxvk::Logger::info(str::format("TF2VR: 🏁 VGUI FLUSH COMPLETE #", flushNum, " - UI rendering finished"));
    }
}

// TF2VR: VGUI Paint Completion Handler - Call this RIGHT AFTER VGui_Paint()
extern "C" void __declspec(dllexport) TF2VR_NotifyVGUIPaintComplete() {
    static int callCount = 0;
    callCount++;
    
    if (!g_vrCompositor) {
        if (callCount % 60 == 1) {
            dxvk::Logger::warn("TF2VR: 🚫 PAINT COMPLETE REJECTED - g_vrCompositor is null");
        }
        return;
    }
    
    if (!g_vrCompositor->IsCompositorActive()) {
        if (callCount % 60 == 1) {
            dxvk::Logger::warn("TF2VR: 🚫 PAINT COMPLETE REJECTED - compositor not active");
        }
        return;
    }
    
    // This is called immediately after render->VGui_Paint() completes
    // This is the PERFECT time to copy Slot A - UI painting is done!
    static std::atomic<bool> g_vguiPaintCompleted{false};
    g_vguiPaintCompleted.store(true);
    
    static int paintCompleteCount = 0;
    if (++paintCompleteCount % 60 == 1) {  // Log occasionally
        dxvk::Logger::info(str::format("TF2VR: 🎨 VGUI PAINT COMPLETE #", paintCompleteCount, " - UI painting finished, safe to copy Slot A"));
    }
    
    // TF2VR: Also signal that texture is ready for copy (same as TF2VR_NotifyVGUIPresentComplete)
    g_textureReady.store(true);
    dxvk::Logger::info("TF2VR: 🚀 TEXTURE READY - VGUI paint complete, signaling for copy");
}

// TF2VR: VGUI Present Completion Handler - Call this RIGHT AFTER vkQueuePresentKHR!
extern "C" void TF2VR_NotifyVGUIPresentComplete() {
    if (!g_vrCompositor || !g_vrCompositor->IsCompositorActive()) {
        return;
    }
    
    // Measure timing for synchronization analysis
    static auto lastPresentTime = std::chrono::high_resolution_clock::now();
    auto currentTime = std::chrono::high_resolution_clock::now();
    auto deltaMs = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - lastPresentTime).count() / 1000.0f;
    lastPresentTime = currentTime;
    
    static int presentCompleteCount = 0;
    presentCompleteCount++;
    
    // Log present completion
    dxvk::Logger::info(str::format("TF2VR: 🖼️ PRESENT COMPLETE #", presentCompleteCount, " - All 5 passes done, final texture ready! (", deltaMs, "ms since last)"));
    
    // COMPLETELY ASYNC: Just signal texture is ready, copy happens on compositor thread
    // No blocking operations in TF2's present path to prevent deadlocks
    g_textureReady.store(true);
    
    dxvk::Logger::info("TF2VR: 🚀 ASYNC SIGNAL - texture ready for compositor copy");
}

// Global access to the present synchronization mutex (using timed_mutex for timeout support)
std::timed_mutex* GetPresentSyncMutex() {
    static std::timed_mutex presentSyncMutex;
    return &presentSyncMutex;
}

// Compositor-controlled texture sync - mirrors OpenXR BeginFrame pattern
// Removed complex synchronization system - using simple texture copying only
extern "C" void __declspec(dllexport) TF2VR_CompositorBeginTextureSync() {
    // No-op - synchronization removed to fix VK_ERROR_DEVICE_LOST
    static int syncRequestCount = 0;
    syncRequestCount++;
    dxvk::Logger::info(str::format("TF2VR: 🎬 COMPOSITOR SYNC DISABLED #", syncRequestCount, " - no frame locking"));
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
    if (g_vrCompositor) {
        g_vrCompositor->SetSourceState(static_cast<SourceEngineState>(state));
    }
}

bool __declspec(dllexport) dxvkIsCompositorActive() {
    return g_vrCompositor ? g_vrCompositor->IsCompositorActive() : false;
}

void __declspec(dllexport) dxvkSubmitMenuFrame(void* textureHandle, int width, int height) {
    // CRITICAL: Disable direct submission - we now use frame-complete driven copying instead
    static int callCount = 0;
    callCount++;
    
    if (callCount % 60 == 1) {  // Log occasionally to show it's being ignored
        dxvk::Logger::info(str::format("VR Compositor: dxvkSubmitMenuFrame #", callCount, " IGNORED (frame-complete system active) - handle: ", (void*)textureHandle, " size: ", width, "x", height));
    }
    
    // DON'T submit - let the frame-complete system handle it instead
    return;
}

// TF2VR: VGUI Render Target Tracking Implementation
extern "C" void TF2VR_TrackVGUIRenderTarget(dxvk::D3D9Surface* surface, dxvk::D3D9CommonTexture* texture) {
    if (!g_vrCompositor || !g_vrCompositor->IsCompositorActive()) {
        return; // Only track when compositor is active
    }
    
    // NO LOCK NEEDED - tracking is now lock-free to prevent deadlock
    
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
            
            // CRITICAL: Only update tracking if this is likely a different/better texture
            // Don't constantly switch between similar textures (which might include blanks)
            bool shouldUpdateTracking = false;
            
            if (g_trackedVGUITexture == nullptr) {
                // No current texture - accept this one
                shouldUpdateTracking = true;
                dxvk::Logger::info("TF2VR: 🎯 FIRST VGUI texture - accepting");
            } else {
                // We already have a tracked texture - be more selective
                auto currentDesc = g_trackedVGUITexture->Desc();
                if (currentDesc->Width != desc->Width || currentDesc->Height != desc->Height) {
                    // Different size - probably a legitimate change
                    shouldUpdateTracking = true;
                    dxvk::Logger::info(str::format("TF2VR: 📐 SIZE CHANGE - updating from ", currentDesc->Width, "x", currentDesc->Height, " to ", desc->Width, "x", desc->Height));
                } else {
                    // Same size - but let's be more responsive to texture changes
                    // TF2 might create new _rt_vgui textures when UI content changes significantly
                    
                    // Check if this is a different texture object (different memory address)
                    if (texture != g_trackedVGUITexture) {
                        // This is a completely different texture object - TF2 wants us to use it
                        shouldUpdateTracking = true;
                        dxvk::Logger::info(str::format("TF2VR: 🔄 NEW TEXTURE OBJECT - switching from ", (void*)g_trackedVGUITexture, " to ", (void*)texture));
                    } else {
                        // Same texture object being set again - keep using it
                        shouldUpdateTracking = false;
                        dxvk::Logger::info(str::format("TF2VR: ♻️ SAME TEXTURE OBJECT - keeping current (", desc->Width, "x", desc->Height, ")"));
                    }
                }
            }
            
            if (shouldUpdateTracking) {
                // Store the DXVK surface and texture for later use
                g_trackedVGUISurface = surface;
                g_trackedVGUITexture = texture;
                
                // Update dual texture tracking - track which of the two textures TF2 is using
                auto now = std::chrono::high_resolution_clock::now();
                
                // Determine which slot to use based on texture address
                VGUITextureInfo* targetSlot = nullptr;
                if (g_vguiTextureA.textureAddress == nullptr || g_vguiTextureA.textureAddress == (void*)texture) {
                    targetSlot = &g_vguiTextureA;
                } else if (g_vguiTextureB.textureAddress == nullptr || g_vguiTextureB.textureAddress == (void*)texture) {
                    targetSlot = &g_vguiTextureB;
                } else {
                    // Both slots occupied with different textures - use least recently used
                    targetSlot = (g_vguiTextureA.lastUsed < g_vguiTextureB.lastUsed) ? &g_vguiTextureA : &g_vguiTextureB;
                }
                
                // Update the target slot
                targetSlot->texture = texture;
                targetSlot->surface = surface;
                targetSlot->textureAddress = (void*)texture;
                targetSlot->lastUsed = now;
                targetSlot->useCount++;
                
                // Mark this as the most recently used texture
                g_mostRecentVGUITexture = targetSlot;
                
                // Debug: Log slot assignments
                const char* slotName = (targetSlot == &g_vguiTextureA) ? "A" : "B";
                dxvk::Logger::info(str::format("TF2VR: 📍 ASSIGNED texture ", (void*)texture, " to SLOT ", slotName, " (useCount: ", targetSlot->useCount, ")"));
                dxvk::Logger::info(str::format("TF2VR: 🔍 SLOTS - A: ", (void*)g_vguiTextureA.textureAddress, " (", g_vguiTextureA.useCount, " uses) | B: ", (void*)g_vguiTextureB.textureAddress, " (", g_vguiTextureB.useCount, " uses)"));
            }
            
            // CRITICAL: Track VGUI render activity to detect completion (lock-free)
            {
                // No lock needed - using atomic operations only
                g_vguiRenderPassCount.fetch_add(1);
                g_lastVGUIRenderTime = std::chrono::steady_clock::now();
                
                // Track render operations for multi-pass detection
                g_renderOpsSinceLastCopy.fetch_add(1);
            }
            
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

// TF2VR: HUD Position Communication - Update HUD quad position from game
extern "C" void __declspec(dllexport) TF2VR_UpdateHUDPosition(
    float viewer_x, float viewer_y, float viewer_z,
    float ul_x, float ul_y, float ul_z,
    float ur_x, float ur_y, float ur_z,
    float ll_x, float ll_y, float ll_z,
    float lr_x, float lr_y, float lr_z,
    bool is_custom_bounds, int frame_number, float world_scale) {
    
    std::lock_guard<std::mutex> lock(g_hudPositionMutex);
    
    // Update the shared HUD position data
    g_sharedHUDPosition.viewer_pos[0] = viewer_x;
    g_sharedHUDPosition.viewer_pos[1] = viewer_y;
    g_sharedHUDPosition.viewer_pos[2] = viewer_z;
    
    g_sharedHUDPosition.upper_left[0] = ul_x;
    g_sharedHUDPosition.upper_left[1] = ul_y;
    g_sharedHUDPosition.upper_left[2] = ul_z;
    
    g_sharedHUDPosition.upper_right[0] = ur_x;
    g_sharedHUDPosition.upper_right[1] = ur_y;
    g_sharedHUDPosition.upper_right[2] = ur_z;
    
    g_sharedHUDPosition.lower_left[0] = ll_x;
    g_sharedHUDPosition.lower_left[1] = ll_y;
    g_sharedHUDPosition.lower_left[2] = ll_z;
    
    g_sharedHUDPosition.lower_right[0] = lr_x;
    g_sharedHUDPosition.lower_right[1] = lr_y;
    g_sharedHUDPosition.lower_right[2] = lr_z;
    
    g_sharedHUDPosition.is_custom_bounds = is_custom_bounds;
    g_sharedHUDPosition.frame_number = frame_number;
    g_sharedHUDPosition.world_scale = world_scale;
    g_sharedHUDPosition.last_update_time = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    g_sharedHUDPosition.is_valid = true;
    
    static int updateCount = 0;
    updateCount++;
    
    // Log occasionally to track updates
    // if (updateCount % 60 == 1) {
        dxvk::Logger::info(str::format("TF2VR: 📐 HUD POSITION UPDATE #", updateCount, 
            " - Viewer: (", viewer_x, ", ", viewer_y, ", ", viewer_z, ")",
            " - UL: (", ul_x, ", ", ul_y, ", ", ul_z, ")",
            " - Custom: ", is_custom_bounds ? "true" : "false"));
    // }
}

} // extern "C"

// Internal C++ function - no C linkage needed
void TF2VR_NotifyVGUIFrameComplete() {
    static int totalCallCount = 0;
    totalCallCount++;
    
    // CRITICAL DEBUG: Always log the first few calls to ensure this function is being called
    if (totalCallCount <= 5 || totalCallCount % 100 == 1) {
        dxvk::Logger::info(str::format("TF2VR: 🔔 NotifyVGUIFrameComplete CALLED #", totalCallCount));
    }
    
    if (!g_vrCompositor) {
        if (totalCallCount <= 5) {
            dxvk::Logger::warn("TF2VR: NotifyVGUIFrameComplete - g_vrCompositor is null");
        }
        return;
    }
    
    if (!g_vrCompositor->IsCompositorActive()) {
        if (totalCallCount <= 5) {
            dxvk::Logger::warn("TF2VR: NotifyVGUIFrameComplete - compositor not active");
        }
        return;
    }
    
    // Increment frame counter and signal that a fresh frame is ready
    int frameNum = g_frameCompleteCount.fetch_add(1) + 1;
    
    // CRITICAL TIMING DEBUG: Track how often this is called vs actual copies
    static auto lastCallTime = std::chrono::high_resolution_clock::now();
    auto currentTime = std::chrono::high_resolution_clock::now();
    auto timeSinceLastCall = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - lastCallTime).count();
    lastCallTime = currentTime;
    
    // Quick check without lock - if we have a tracked texture, signal the compositor
    if (g_trackedVGUITexture && g_trackedVGUISurface) {
        // With queue-based system, we just increment frame count - no dropped frames!
        
        // Log timing and signal state  
        if (frameNum % 30 == 1) {  // More frequent logging for debugging
            int lastProcessed = g_lastProcessedFrame.load();
            int pendingFrames = frameNum - lastProcessed;
            dxvk::Logger::info(str::format("TF2VR: 📝 FRAME COMPLETE #", frameNum, " (", timeSinceLastCall, "μs gap) - pending frames: ", pendingFrames));
        }
    } else {
        // CRITICAL DEBUG: Log why frame complete is not signaling
        if (frameNum <= 5 || frameNum % 100 == 1) {
            dxvk::Logger::warn(str::format("TF2VR: Frame complete #", frameNum, " but tracked texture state: VGUI=", (void*)g_trackedVGUITexture, " Surface=", (void*)g_trackedVGUISurface));
        }
    }
}



