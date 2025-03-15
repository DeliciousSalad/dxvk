#include "OpenXRDirectMode.h"
#include "HMDInterface.h"
#include <cstring>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <thread>
#include <future>

#include "../util/log/log.h"
#include "../util/util_singleton.h"
#include "../d3d9/d3d9_device.h"
#include "../util/util_string.h"
#include "VkSubmitThreadCallback.h"

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
	, m_space(nullptr)
	, m_swapchain(nullptr)
	, m_frameCounter(0)
	, m_bFrameStarted(false)
	, m_needManualGammaCorrection(false)
	, m_bPosesStale(true)
{
	Logger::info("OpenXRDirectMode: Constructor called");
	
	// Initialize frameState with default values
	m_frameState = { XR_TYPE_FRAME_STATE };
	m_frameState.predictedDisplayTime = 0;
	m_frameState.predictedDisplayPeriod = 0;
	m_frameState.shouldRender = XR_FALSE;
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
	
	// Reset global VkSubmitThreadCallback if it's pointing to this instance
	if (g_pVkSubmitThreadCallback == this) {
		g_pVkSubmitThreadCallback = nullptr;
	}
	
	// Note: We don't destroy the instance, session, or space
	// since those were created externally and passed to us
	m_instance = XR_NULL_HANDLE;
	m_session = XR_NULL_HANDLE;
	m_space = nullptr;
	m_swapchain = nullptr;
	m_systemId = XR_NULL_SYSTEM_ID;
}

bool OpenXRDirectMode::Init(XrInstance instance, XrSystemId systemId, XrSession session, XrSpace space) 
{
	m_instance = instance;
	m_session = session;
	m_space = space;
	m_systemId = systemId;

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
		if (formats[i] == VK_FORMAT_R8G8B8A8_SRGB) formatName = "R8G8B8A8_SRGB";
		else if (formats[i] == VK_FORMAT_B8G8R8A8_SRGB) formatName = "B8G8R8A8_SRGB";
		else if (formats[i] == VK_FORMAT_R8G8B8A8_UNORM) formatName = "R8G8B8A8_UNORM";
		else if (formats[i] == VK_FORMAT_B8G8R8A8_UNORM) formatName = "B8G8R8A8_UNORM";
		Logger::info(str::format("  - Format: ", formats[i], " (", formatName, ")"));
	}

	// Get source format from the shared texture
	int64_t selectedFormat = formats[0]; // Default to first format
	int64_t sourceFormat = VK_FORMAT_B8G8R8A8_UNORM; // Default format
	
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

	// All is good!
	Logger::info(str::format("OpenXRDirectMode: Initialized with render target size: ", m_nRenderWidth, "x", m_nRenderHeight));
	return true;
}

VkSubmitThreadCallback* OpenXRDirectMode::GetVkSubmitThreadCallback() 
{
	return this;
}

void OpenXRDirectMode::PrePresentCallBack() 
{
	// Called before the DXVK presenter calls presentImage
	if (!m_session) {
		return;
	}
	Logger::info("OpenXRDirectMode: PrePresentCallback called!");
	EndFrame();
}

void OpenXRDirectMode::PostPresentCallback() 
{
	// Called after the DXVK presenter calls presentImage
	if (!m_session) {
		return;
	}

	if(m_bPosesStale){
		Logger::info("OpenXRDirectMode: PostPresentCallback called!");
		WaitPoses();
		m_bPosesStale = false;

		m_cv.notify_one();
	}

	// This is where we'd finalize any OpenXR rendering after the main display presentation
}

void OpenXRDirectMode::PrePresent() 
{
	// Called before d3d9 swapchain present
	if (!m_session) {
		return;
	}

	{
		m_bPosesStale = true;
	}

	Logger::info("OpenXRDirectMode: Pre-present phase");
}

void OpenXRDirectMode::PostPresent() 
{
	// Called after d3d9 swapchain present
	if (!m_session) {
		return;
	}

	// Note: EndFrame will be called explicitly after the frame is rendered
	// Just log that we're in the post-present phase for debugging
	Logger::info("OpenXRDirectMode: Post-present phase");
}

void OpenXRDirectMode::GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight) 
{
	if (pnWidth) *pnWidth = m_nRenderWidth;
	if (pnHeight) *pnHeight = m_nRenderHeight;
}

bool OpenXRDirectMode::BeginFrame()
{	
	if (!m_session) {
		Logger::err("OpenXRDirectMode: Cannot begin frame - no active session");
		return false;
	}

	Logger::info("OpenXRDirectMode: BeginFrame called! Blocking until poses are ready");
	std::unique_lock<std::mutex> lock(m_mutex);
	m_cv.wait(lock, [this]() { return !m_bPosesStale; });
	Logger::info("OpenXRDirectMode: Poses are ready!, continuing BeginFrame");

	return true;
}

bool OpenXRDirectMode::EndFrame() 
{
	if (!m_session) {
		Logger::err("OpenXRDirectMode: Cannot end frame - no active session");
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

	// Make sure we have valid views before attempting to end the frame
	if (m_views.empty()) 
	{
		// If views aren't initialized yet, create them
		m_views.resize(2, {XR_TYPE_VIEW});
		m_projectionViews.resize(2, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
		Logger::info("OpenXRDirectMode: Created default views for frame submission");
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

	XrResult result;

	// Create a scope for the layer that extends past xrEndFrame
	{
		// Only submit the layer if we have valid swapchains and views
		if (!m_projectionViews.empty() && !m_eyeSwapchains.empty()) {
			XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
			layer.space = m_space;
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
	return XR_SUCCEEDED(result);
}

void OpenXRDirectMode::StoreSharedTexture(int index, VulkanTextureData* vulkanData)
{
	if (!vulkanData || index < 0) {
		return;
	}

	// Store Vulkan device and queue information if not already set
	if (m_vkDevice == VK_NULL_HANDLE && vulkanData->device != VK_NULL_HANDLE) {
		m_vkDevice = vulkanData->device;
		Logger::info("OpenXRDirectMode: Stored Vulkan device from shared texture");
	}

	if (m_vkInstance == VK_NULL_HANDLE && vulkanData->instance != VK_NULL_HANDLE) {
		m_vkInstance = vulkanData->instance;
		Logger::info("OpenXRDirectMode: Stored Vulkan instance from shared texture");
	}

	if (m_vkPhysicalDevice == VK_NULL_HANDLE && vulkanData->physicalDevice != VK_NULL_HANDLE) {
		m_vkPhysicalDevice = vulkanData->physicalDevice;
		Logger::info("OpenXRDirectMode: Stored Vulkan physical device from shared texture");
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
			", format: ", (uint32_t)vulkanData->format, ")"));
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
		std::vector<XrSwapchainImageVulkanKHR> swapchainImages(imageCount);
		for (uint32_t j = 0; j < imageCount; j++) {
			swapchainImages[j].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
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
	if (m_sharedTextures.empty() || m_eyeSwapchains.empty()) {
		Logger::err("OpenXRDirectMode: No textures to copy");
		return false;
	}

	// This function should be called after BeginFrame
	if (!m_bFrameStarted) {
		Logger::err(str::format("OpenXRDirectMode: CopyToSwapchains called without BeginFrame - frame: ", m_frameCounter));
		return false;
	}

	XrResult result;
	
	// Process each eye swapchain
	for (uint32_t eyeIndex = 0; eyeIndex < m_eyeSwapchains.size(); eyeIndex++) {
		SwapchainInfo& swapchainInfo = m_eyeSwapchains[eyeIndex];
		
		// Get the index of the swapchain image to acquire
		uint32_t swapchainImageIndex;
		XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		result = xrAcquireSwapchainImage(swapchainInfo.handle, &acquireInfo, &swapchainImageIndex);
		if (XR_FAILED(result)) {
			Logger::err(str::format("OpenXRDirectMode: Failed to acquire swapchain image for eye ", eyeIndex));
			return false;
		}

		// Wait for the image to be ready
		XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		waitInfo.timeout = 1000000000; // 1 second timeout
		result = xrWaitSwapchainImage(swapchainInfo.handle, &waitInfo);
		if (XR_FAILED(result)) {
			Logger::err(str::format("OpenXRDirectMode: Failed to wait for swapchain image for eye ", eyeIndex));
			return false;
		}

		// Get the acquired Vulkan image
		VkImage dstImage = m_eyeSwapchains[eyeIndex].images[swapchainImageIndex].image.image;
		
		// Determine which shared texture to use
		uint32_t sharedTextureIndex = (m_sharedTextures.size() == 1) ? 0 : eyeIndex;
		if (sharedTextureIndex >= m_sharedTextures.size()) {
			Logger::err(str::format("OpenXRDirectMode: No shared texture available for eye ", eyeIndex));
			return false;
		}
		
		SharedTextureData& srcData = m_sharedTextures[sharedTextureIndex];
		VkImage srcImage = srcData.sourceImage;
		
		// DIAGNOSTIC: Force both eyes to use left eye portion for testing
		// This will help determine if the issue is with TF2 rendering or OpenXR pipeline
		bool forceLeftEyeContent = false; // Set to true to test if right eye rendering pipeline works
		VkOffset3D srcOffset = {0, 0, 0};
		VkExtent3D srcExtent = {srcData.width, srcData.height, 1};
		
		// Set up region for copying
		VkImageSubresourceRange srcSubresourceRange = {};
		srcSubresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		srcSubresourceRange.baseMipLevel = 0;
		srcSubresourceRange.levelCount = 1;
		srcSubresourceRange.baseArrayLayer = 0;
		srcSubresourceRange.layerCount = 1;
		
		if (m_sharedTextures.size() == 1 && srcData.width >= swapchainInfo.width * 2) {
			// It's a single texture containing both eye images side by side
			Logger::info(str::format("OpenXRDirectMode: For eye ", eyeIndex, 
				" splitting single texture - offset: ", forceLeftEyeContent ? 0 : (eyeIndex * swapchainInfo.width), 
				", extent: ", swapchainInfo.width, "x", swapchainInfo.height,
				" (original width: ", srcData.width, ")"));
			
			// Extract just this eye's portion of the texture
			srcOffset.x = forceLeftEyeContent ? 0 : (eyeIndex * swapchainInfo.width);
			srcExtent.width = swapchainInfo.width;
		} else {
			Logger::info(str::format("OpenXRDirectMode: For eye ", eyeIndex, 
				" using full texture - extent: ", srcData.width, "x", srcData.height));
		}
		
		// Get source layout - DO NOT transition the source image's layout
		VkImageLayout prevSrcLayout = srcData.currentLayout;
		
		// Transition destination image to TRANSFER_DST layout
		VkImageMemoryBarrier dstBarrier = {};
		dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		dstBarrier.srcAccessMask = 0;
		dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		dstBarrier.image = dstImage;
		dstBarrier.subresourceRange = srcSubresourceRange;
		
		// Begin command buffer
		VkCommandBuffer cmdBuffer = beginSingleTimeCommands();
		if (cmdBuffer == VK_NULL_HANDLE) {
			Logger::err("OpenXRDirectMode: Failed to begin command buffer for copy");
			return false;
		}
		
		// Perform the layout transitions
		vkCmdPipelineBarrier(cmdBuffer,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			0, nullptr, 0, nullptr, 1, &dstBarrier);
		
		// Set up the copy region
		VkImageCopy copyRegion = {};
		copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.srcSubresource.mipLevel = 0;
		copyRegion.srcSubresource.baseArrayLayer = 0;
		copyRegion.srcSubresource.layerCount = 1;
		copyRegion.srcOffset = srcOffset;
		
		copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.dstSubresource.mipLevel = 0;
		copyRegion.dstSubresource.baseArrayLayer = 0;
		copyRegion.dstSubresource.layerCount = 1;
		copyRegion.dstOffset = {0, 0, 0};
		
		copyRegion.extent = srcExtent;
		
		// Use direct copy since both formats match (both B8G8R8A8_SRGB)
		vkCmdCopyImage(cmdBuffer,
			srcImage, prevSrcLayout,
			dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &copyRegion);
		
		// Transition destination image back to COLOR_ATTACHMENT layout
		dstBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		dstBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		dstBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		dstBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		
		vkCmdPipelineBarrier(cmdBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
			0, nullptr, 0, nullptr, 1, &dstBarrier);
		
		// End command buffer and submit
		endSingleTimeCommands(cmdBuffer);
		
		// Release the swapchain image
		XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		result = xrReleaseSwapchainImage(swapchainInfo.handle, &releaseInfo);
		if (XR_FAILED(result)) {
			Logger::err(str::format("OpenXRDirectMode: Failed to release swapchain image for eye ", eyeIndex));
			return false;
		}
		
		// Setup projection view for this eye
		m_projectionViews[eyeIndex].pose = m_views[eyeIndex].pose;
		m_projectionViews[eyeIndex].fov = m_views[eyeIndex].fov;
		m_projectionViews[eyeIndex].subImage.swapchain = swapchainInfo.handle;
		m_projectionViews[eyeIndex].subImage.imageRect.offset = {0, 0};
		m_projectionViews[eyeIndex].subImage.imageRect.extent = {
			static_cast<int32_t>(swapchainInfo.width),
			static_cast<int32_t>(swapchainInfo.height)
		};
	}
	
	return true;
}

void OpenXRDirectMode::GetPredictedDisplayTime(XrTime& time)
{
	time = m_frameState.predictedDisplayTime;
}

// Helper methods for command buffer management
VkCommandBuffer OpenXRDirectMode::beginSingleTimeCommands() {
	// Create a command pool for the copy operations
	VkCommandPool commandPool = VK_NULL_HANDLE;
	VkCommandPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.queueFamilyIndex = m_vkQueueFamilyIndex;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

	VkResult vkResult = vkCreateCommandPool(m_vkDevice, &poolInfo, nullptr, &commandPool);
	if (vkResult != VK_SUCCESS) {
		Logger::err(str::format("OpenXRDirectMode: Failed to create command pool, error: ", vkResult));
		return VK_NULL_HANDLE;
	}

	// Store command pool for later cleanup
	m_currentCommandPool = commandPool;

	// Create a command buffer
	VkCommandBufferAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	vkResult = vkAllocateCommandBuffers(m_vkDevice, &allocInfo, &commandBuffer);
	if (vkResult != VK_SUCCESS) {
		Logger::err(str::format("OpenXRDirectMode: Failed to allocate command buffer, error: ", vkResult));
		vkDestroyCommandPool(m_vkDevice, commandPool, nullptr);
		m_currentCommandPool = VK_NULL_HANDLE;
		return VK_NULL_HANDLE;
	}

	// Begin the command buffer
	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	vkResult = vkBeginCommandBuffer(commandBuffer, &beginInfo);
	if (vkResult != VK_SUCCESS) {
		Logger::err(str::format("OpenXRDirectMode: Failed to begin command buffer, error: ", vkResult));
		vkFreeCommandBuffers(m_vkDevice, commandPool, 1, &commandBuffer);
		vkDestroyCommandPool(m_vkDevice, commandPool, nullptr);
		m_currentCommandPool = VK_NULL_HANDLE;
		return VK_NULL_HANDLE;
	}

	return commandBuffer;
}

void OpenXRDirectMode::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
	if (commandBuffer == VK_NULL_HANDLE || m_currentCommandPool == VK_NULL_HANDLE) {
		Logger::err("OpenXRDirectMode: Invalid command buffer or command pool in endSingleTimeCommands");
		return;
	}

	// End the command buffer
	VkResult vkResult = vkEndCommandBuffer(commandBuffer);
	if (vkResult != VK_SUCCESS) {
		Logger::err(str::format("OpenXRDirectMode: Failed to end command buffer, error: ", vkResult));
		vkFreeCommandBuffers(m_vkDevice, m_currentCommandPool, 1, &commandBuffer);
		vkDestroyCommandPool(m_vkDevice, m_currentCommandPool, nullptr);
		m_currentCommandPool = VK_NULL_HANDLE;
		return;
	}

	// Submit the command buffer
	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;

	vkResult = vkQueueSubmit(m_vkQueue, 1, &submitInfo, VK_NULL_HANDLE);
	if (vkResult != VK_SUCCESS) {
		Logger::err(str::format("OpenXRDirectMode: Failed to submit queue, error: ", vkResult));
	} else {
		// Wait for the queue to be idle
		vkQueueWaitIdle(m_vkQueue);
	}

	// Free the command buffer and destroy the command pool
	vkFreeCommandBuffers(m_vkDevice, m_currentCommandPool, 1, &commandBuffer);
	vkDestroyCommandPool(m_vkDevice, m_currentCommandPool, nullptr);
	m_currentCommandPool = VK_NULL_HANDLE;
}

void OpenXRDirectMode::GetViews(XrView*& views, uint32_t& viewCount)
{
	Logger::info("OpenXRDirectMode: GetViews called");
	std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this]() { return !m_bPosesStale; });  // Wait for update

	m_bPosesStale = true;
	views = m_views.data();
	viewCount = m_views.size();
}

bool OpenXRDirectMode::WaitPoses()
{
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

	// Begin the frame and get information
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
	viewLocateInfo.space = m_space;
	
	// Properly initialize the view state structure
	m_viewState  = {};
	m_viewState.type = XR_TYPE_VIEW_STATE;
	m_viewState.next = nullptr;
	
	if (XR_FAILED(result)) 
	{
		Logger::err(str::format("OpenXRDirectMode: xrLocateViews failed with error code: ", (int)result));
		m_bFrameStarted = false;
		return false;
	}

	uint32_t viewCount = (uint32_t)m_views.size();
	result = xrLocateViews(m_session, &viewLocateInfo, &m_viewState, viewCount, &viewCount, m_views.data());

	// Increment the frame counter after successfully starting a frame
	m_frameCounter++;
	Logger::info(str::format("OpenXRDirectMode: Frame ", m_frameCounter, " started successfully"));
	m_bFrameStarted = true;
	return true;
}