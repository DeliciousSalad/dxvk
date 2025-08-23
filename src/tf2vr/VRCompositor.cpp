#include "VRCompositor.h"
#include "../util/util_string.h"
#include "../util/log/log.h"

// Forward declare what we need from OpenXRDirectMode
class OpenXRDirectMode;

namespace dxvk {
    
VRCompositor::~VRCompositor() { 
    Shutdown(); 
    CleanupVulkanResources();
}

void VRCompositor::CleanupVulkanResources() {
    if (m_compositorDevice == VK_NULL_HANDLE) {
        return;
    }
    
    Logger::info("VRCompositor: Cleaning up Vulkan resources");
    
    // Wait for device to be idle
    vkDeviceWaitIdle(m_compositorDevice);
    
    // Cleanup OpenXR swapchains
    for (int eye = 0; eye < 2; eye++) {
        if (m_compositorSwapchains[eye] != XR_NULL_HANDLE) {
            xrDestroySwapchain(m_compositorSwapchains[eye]);
            m_compositorSwapchains[eye] = XR_NULL_HANDLE;
        }
    }
    
    // Cleanup shader modules
    if (m_vertShaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_compositorDevice, m_vertShaderModule, nullptr);
        m_vertShaderModule = VK_NULL_HANDLE;
    }
    if (m_fragShaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_compositorDevice, m_fragShaderModule, nullptr);
        m_fragShaderModule = VK_NULL_HANDLE;
    }
    
    // Cleanup graphics pipeline
    if (m_graphicsPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_compositorDevice, m_graphicsPipeline, nullptr);
        m_graphicsPipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_compositorDevice, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_compositorDevice, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
    
    // Cleanup descriptor resources
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_compositorDevice, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
        m_descriptorSet = VK_NULL_HANDLE; // Destroyed with pool
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_compositorDevice, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_textureSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_compositorDevice, m_textureSampler, nullptr);
        m_textureSampler = VK_NULL_HANDLE;
    }
    
    // Cleanup buffers
    if (m_vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_compositorDevice, m_vertexBuffer, nullptr);
        m_vertexBuffer = VK_NULL_HANDLE;
    }
    if (m_vertexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_compositorDevice, m_vertexBufferMemory, nullptr);
        m_vertexBufferMemory = VK_NULL_HANDLE;
    }
    if (m_uniformBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_compositorDevice, m_uniformBuffer, nullptr);
        m_uniformBuffer = VK_NULL_HANDLE;
    }
    if (m_uniformBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_compositorDevice, m_uniformBufferMemory, nullptr);
        m_uniformBufferMemory = VK_NULL_HANDLE;
    }
    
    // Cleanup framebuffers and image views
    if (m_currentFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_compositorDevice, m_currentFramebuffer, nullptr);
        m_currentFramebuffer = VK_NULL_HANDLE;
    }
    if (m_currentImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_compositorDevice, m_currentImageView, nullptr);
        m_currentImageView = VK_NULL_HANDLE;
    }
    
    // Cleanup stored textures
    if (m_storedMenuTexture != VK_NULL_HANDLE) {
        vkDestroyImage(m_compositorDevice, m_storedMenuTexture, nullptr);
        m_storedMenuTexture = VK_NULL_HANDLE;
    }
    if (m_storedMenuTextureMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_compositorDevice, m_storedMenuTextureMemory, nullptr);
        m_storedMenuTextureMemory = VK_NULL_HANDLE;
    }
    
    // Cleanup copied textures
    if (m_copiedMenuTexture != VK_NULL_HANDLE) {
        vkDestroyImage(m_compositorDevice, m_copiedMenuTexture, nullptr);
        m_copiedMenuTexture = VK_NULL_HANDLE;
    }
    if (m_copiedMenuTextureMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_compositorDevice, m_copiedMenuTextureMemory, nullptr);
        m_copiedMenuTextureMemory = VK_NULL_HANDLE;
    }
    if (m_copiedMenuTextureView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_compositorDevice, m_copiedMenuTextureView, nullptr);
        m_copiedMenuTextureView = VK_NULL_HANDLE;
    }
    
    // Cleanup fallback texture
    if (m_fallbackTexture != VK_NULL_HANDLE) {
        vkDestroyImage(m_compositorDevice, m_fallbackTexture, nullptr);
        m_fallbackTexture = VK_NULL_HANDLE;
    }
    if (m_fallbackTextureMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_compositorDevice, m_fallbackTextureMemory, nullptr);
        m_fallbackTextureMemory = VK_NULL_HANDLE;
    }
    if (m_fallbackTextureView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_compositorDevice, m_fallbackTextureView, nullptr);
        m_fallbackTextureView = VK_NULL_HANDLE;
    }
    
    // Cleanup command resources
    if (m_compositorFence != VK_NULL_HANDLE) {
        vkDestroyFence(m_compositorDevice, m_compositorFence, nullptr);
        m_compositorFence = VK_NULL_HANDLE;
    }
    if (m_compositorCommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_compositorDevice, m_compositorCommandPool, nullptr);
        m_compositorCommandPool = VK_NULL_HANDLE;
        m_compositorCommandBuffer = VK_NULL_HANDLE; // Destroyed with pool
    }
    
    // Finally destroy the device
    if (m_compositorDevice != VK_NULL_HANDLE) {
        vkDestroyDevice(m_compositorDevice, nullptr);
        m_compositorDevice = VK_NULL_HANDLE;
        m_compositorQueue = VK_NULL_HANDLE; // Queue is destroyed with device
    }
    
    Logger::info("VRCompositor: ✅ Vulkan resources cleaned up");
}

void VRCompositor::SetOpenXRManager(OpenXRDirectMode* manager) {
    m_pOpenXRManager = manager;
}

void VRCompositor::CacheOpenXRData(VkPhysicalDevice physicalDevice, VkInstance instance, XrSession session, XrSpace referenceSpace, uint32_t width, uint32_t height) {
    Logger::info(str::format("VRCompositor: CacheOpenXRData called - physicalDevice: ", (void*)physicalDevice, 
                            " instance: ", (void*)instance, " session: ", (void*)session, 
                            " referenceSpace: ", (void*)referenceSpace, " resolution: ", width, "x", height));
    
    m_cachedPhysicalDevice = physicalDevice;
    m_cachedInstance = instance;
    m_cachedSession = session;
    m_cachedReferenceSpace = referenceSpace;
    m_cachedRenderWidth = width;
    m_cachedRenderHeight = height;
    
    if (physicalDevice == VK_NULL_HANDLE) {
        Logger::warn("VRCompositor: ⚠️ Cached physical device is NULL!");
    }
    if (instance == VK_NULL_HANDLE) {
        Logger::warn("VRCompositor: ⚠️ Cached instance is NULL!");
    }
    if (session == XR_NULL_HANDLE) {
        Logger::warn("VRCompositor: ⚠️ Cached session is NULL!");
    }
    
    Logger::info("VRCompositor: ✅ OpenXR data cached successfully");
}

bool VRCompositor::Initialize() {
    Logger::info("VRCompositor: Initializing independent VR compositor");
    
    // Initialize 3D quad vertices and matrices
    SetupQuadVertices();
    SetupMVPMatrix();
    
    if (!m_pOpenXRManager) {
        Logger::err("VRCompositor: No OpenXR manager - cannot initialize");
        return false;
    }
    
    Logger::info("VRCompositor: Creating independent Vulkan device...");
    if (!CreateIndependentVulkanDevice()) {
        Logger::err("VRCompositor: Failed to create independent Vulkan device");
        return false;
    }
    Logger::info("VRCompositor: ✅ Independent Vulkan device created");
    
    Logger::info("VRCompositor: Creating compositor swapchains...");
    if (!CreateCompositorSwapchains()) {
        Logger::err("VRCompositor: Failed to create compositor swapchains");
        return false;
    }
    Logger::info("VRCompositor: ✅ Compositor swapchains created");
    
    Logger::info(str::format("VRCompositor: Swapchains created flag: ", m_compositorSwapchainsCreated));
    Logger::info("VRCompositor: ✅ VR Compositor initialized successfully - ready for activation");
    return true;
}

void VRCompositor::Shutdown() {
    m_shouldStop = true;
    
    if (m_compositorThread.joinable()) {
        m_compositorThread.join();
    }
    
    m_compositorActive = false;
    Logger::info("VRCompositor: Compositor shut down");
}

void VRCompositor::SubmitFrame(void* textureHandle, int width, int height, bool isVGUITexture, bool isDXVKTexture) {
    Logger::info(str::format("VRCompositor: SubmitFrame called - handle: ", textureHandle, " size: ", width, "x", height, 
                            " isVGUI: ", isVGUITexture, " isDXVK: ", isDXVKTexture));
    
    std::lock_guard<std::mutex> lock(m_frameMutex);
    
    m_latestFrame.textureHandle = textureHandle;
    m_latestFrame.width = width;
    m_latestFrame.height = height;
    m_latestFrame.hasNewFrame = true;
    m_latestFrame.isVGUITexture = isVGUITexture;
    m_latestFrame.isDXVKTexture = isDXVKTexture;
    
    // If this is a VGUI texture from DXVK, defer the copy to avoid conflicts
    if (isVGUITexture && isDXVKTexture && textureHandle) {
        VkImage vkImage = reinterpret_cast<VkImage>(textureHandle);
        Logger::info(str::format("VRCompositor: 🔄 Scheduling deferred copy for VkImage: ", (void*)vkImage));
        
        // Store the copy request for later processing by the compositor thread
        m_pendingCopyRequest.sourceTexture = vkImage;
        m_pendingCopyRequest.width = width;
        m_pendingCopyRequest.height = height;
        m_pendingCopyRequest.hasPendingCopy = true;
        
        Logger::info("VRCompositor: ✅ Copy request scheduled for compositor thread");
    }
    
    Logger::info("VRCompositor: ✅ Frame submitted and stored");
}

void VRCompositor::SetSourceEngineState(SourceEngineState state) {
    const char* stateNames[] = {"GAMEPLAY", "MENU", "LOADING", "TRANSITION"};
    Logger::info(str::format("VRCompositor: 🎮 State change: ", stateNames[state], " (", static_cast<int>(state), ")"));
    
    m_currentState = state;
    
    // Activate compositor during loading and menu states
    bool shouldActivate = (state == SOURCE_STATE_MENU || state == SOURCE_STATE_LOADING);
    
    if (shouldActivate && !m_compositorActive.load()) {
        Logger::info("VRCompositor: 🚀 Should activate compositor");
        StartCompositor();
    } else if (!shouldActivate && m_compositorActive.load()) {
        Logger::info("VRCompositor: 🛑 Should deactivate compositor - returning control to main pipeline");
        StopCompositor();
    }
}

bool VRCompositor::RenderFrame(const XrFrameState& frameState, std::vector<XrCompositionLayerBaseHeader*>& layers) {
    if (!m_compositorSwapchainsCreated || !frameState.shouldRender) {
        return false;
    }
    
    // Wait for fence to ensure previous frame is complete
    VkResult result = vkWaitForFences(m_compositorDevice, 1, &m_compositorFence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        Logger::warn(str::format("VRCompositor: Failed to wait for fence - error: ", static_cast<int>(result)));
        return false;
    }
    
    result = vkResetFences(m_compositorDevice, 1, &m_compositorFence);
    if (result != VK_SUCCESS) {
        Logger::warn(str::format("VRCompositor: Failed to reset fence - error: ", static_cast<int>(result)));
        return false;
    }
    
    // Begin command buffer
    result = vkResetCommandBuffer(m_compositorCommandBuffer, 0);
    if (result != VK_SUCCESS) {
        Logger::warn("VRCompositor: Failed to reset command buffer");
        return false;
    }
    
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    result = vkBeginCommandBuffer(m_compositorCommandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        Logger::warn("VRCompositor: Failed to begin command buffer");
        return false;
    }
    
    // Render to both eyes
    for (int eye = 0; eye < 2; eye++) {
        if (!RenderEye(eye)) {
            Logger::warn(str::format("VRCompositor: Failed to render eye ", eye));
            continue;
        }
    }
    
    // End command buffer
    result = vkEndCommandBuffer(m_compositorCommandBuffer);
    if (result != VK_SUCCESS) {
        Logger::warn("VRCompositor: Failed to end command buffer");
        return false;
    }
    
    // Submit commands
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_compositorCommandBuffer;
    
    result = vkQueueSubmit(m_compositorQueue, 1, &submitInfo, m_compositorFence);
    if (result != VK_SUCCESS) {
        Logger::warn(str::format("VRCompositor: Failed to submit commands - error: ", static_cast<int>(result)));
        return false;
    }
    
    // Create OpenXR layer
    if (!CreateOpenXRLayer(frameState, layers)) {
        Logger::warn("VRCompositor: Failed to create OpenXR layer");
        return false;
    }
    
    Logger::info("VRCompositor: ✅ Frame rendered successfully");
    return true;
}

bool VRCompositor::CopyAndStoreMenuTexture(VkImage sourceTexture, int width, int height) {
    if (!m_compositorDevice || sourceTexture == VK_NULL_HANDLE) {
        Logger::warn("VRCompositor: CopyAndStoreMenuTexture - invalid device or texture");
        return false;
    }
    
    if (!m_cachedPhysicalDevice) {
        Logger::warn("VRCompositor: CopyAndStoreMenuTexture - no cached physical device");
        return false;
    }
    
    Logger::info(str::format("VRCompositor: 📸 Texture submitted - VkImage: ", (void*)sourceTexture, " size: ", width, "x", height));
    Logger::info("VRCompositor: Using 3D colored quad instead of texture copying");
    
    // For now, we'll render our own 3D colored quad instead of copying textures
    // This simplifies the implementation and focuses on getting 3D rendering working
    return true;
}

void VRCompositor::SubmitDXVKTexture(void* textureHandle, int width, int height) {
    SubmitFrame(textureHandle, width, height, true, true);
}

bool VRCompositor::IsCompositorActive() const {
    return m_compositorActive.load();
}

void VRCompositor::SetSourceState(SourceEngineState state) {
    SetSourceEngineState(state);
}

bool VRCompositor::CreateIndependentVulkanDevice() {
    if (!m_pOpenXRManager) {
        Logger::err("VRCompositor: No OpenXR manager available");
        return false;
    }
    
    // Use cached Vulkan objects
    VkPhysicalDevice physicalDevice = m_cachedPhysicalDevice;
    VkInstance instance = m_cachedInstance;
    
    if (physicalDevice == VK_NULL_HANDLE || instance == VK_NULL_HANDLE) {
        Logger::err("VRCompositor: No valid cached Vulkan physical device or instance - call CacheOpenXRData first");
        return false;
    }
    
    // Find graphics queue family
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());
    
    m_compositorQueueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            m_compositorQueueFamily = i;
            break;
        }
    }
    
    if (m_compositorQueueFamily == UINT32_MAX) {
        Logger::err("VRCompositor: No graphics queue family found");
        return false;
    }
    
    // Create independent logical device
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueCreateInfo.queueFamilyIndex = m_compositorQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;
    
    VkDeviceCreateInfo deviceCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    
    VkResult result = vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &m_compositorDevice);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create device - error: ", static_cast<int>(result)));
        return false;
    }
    
    // Get queue from our device
    vkGetDeviceQueue(m_compositorDevice, m_compositorQueueFamily, 0, &m_compositorQueue);
    
    // Create command pool
    VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_compositorQueueFamily;
    
    result = vkCreateCommandPool(m_compositorDevice, &poolInfo, nullptr, &m_compositorCommandPool);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create command pool - error: ", static_cast<int>(result)));
        return false;
    }
    
    // Allocate command buffer
    VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.commandPool = m_compositorCommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    
    result = vkAllocateCommandBuffers(m_compositorDevice, &allocInfo, &m_compositorCommandBuffer);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to allocate command buffer - error: ", static_cast<int>(result)));
        return false;
    }
    
    // Create fence for synchronization
    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    
    result = vkCreateFence(m_compositorDevice, &fenceInfo, nullptr, &m_compositorFence);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create fence - error: ", static_cast<int>(result)));
        return false;
    }
    
    Logger::info("VRCompositor: ✅ Independent Vulkan device created successfully");
    return true;
}

bool VRCompositor::CreateCompositorSwapchains() {
    Logger::info("VRCompositor: CreateCompositorSwapchains called");
    
    if (!m_pOpenXRManager) {
        Logger::err("VRCompositor: Cannot create swapchains - no OpenXR manager");
        return false;
    }
    
    if (m_compositorDevice == VK_NULL_HANDLE) {
        Logger::err("VRCompositor: Cannot create swapchains - no Vulkan device");
        return false;
    }
    
    XrSession session = m_cachedSession;
    if (session == XR_NULL_HANDLE) {
        Logger::err("VRCompositor: No valid cached OpenXR session - call CacheOpenXRData first");
        return false;
    }
    
    Logger::info(str::format("VRCompositor: Using cached session: ", (void*)session, 
                            " resolution: ", m_cachedRenderWidth, "x", m_cachedRenderHeight));
    
    // Create swapchains for both eyes
    XrSwapchainCreateInfo swapchainInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainInfo.format = VK_FORMAT_B8G8R8A8_SRGB; // Match main swapchain format
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = m_cachedRenderWidth;
    swapchainInfo.height = m_cachedRenderHeight;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;
    
    for (int eye = 0; eye < 2; eye++) {
        XrResult result = xrCreateSwapchain(session, &swapchainInfo, &m_compositorSwapchains[eye]);
        if (XR_FAILED(result)) {
            Logger::err(str::format("VRCompositor: Failed to create swapchain for eye ", eye, " - error: ", static_cast<int>(result)));
            return false;
        }
        
        // Enumerate swapchain images
        uint32_t imageCount = 0;
        result = xrEnumerateSwapchainImages(m_compositorSwapchains[eye], 0, &imageCount, nullptr);
        if (XR_FAILED(result)) {
            Logger::err(str::format("VRCompositor: Failed to get swapchain image count for eye ", eye));
            return false;
        }
        
        m_compositorSwapchainImages[eye].resize(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
        result = xrEnumerateSwapchainImages(m_compositorSwapchains[eye], imageCount, &imageCount,
                                           reinterpret_cast<XrSwapchainImageBaseHeader*>(m_compositorSwapchainImages[eye].data()));
        if (XR_FAILED(result)) {
            Logger::err(str::format("VRCompositor: Failed to enumerate swapchain images for eye ", eye));
            return false;
        }
        
        Logger::info(str::format("VRCompositor: Eye ", eye, " swapchain created with ", imageCount, " images"));
    }
    
    m_compositorSwapchainsCreated = true;
    Logger::info("VRCompositor: ✅ Compositor swapchains created successfully");
    return true;
}

bool VRCompositor::RenderEye(int eye) {
    if (eye < 0 || eye >= 2 || m_compositorSwapchains[eye] == XR_NULL_HANDLE) {
        return false;
    }
    
    // Acquire swapchain image
    XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    uint32_t imageIndex;
    XrResult result = xrAcquireSwapchainImage(m_compositorSwapchains[eye], &acquireInfo, &imageIndex);
    if (XR_FAILED(result)) {
        Logger::warn(str::format("VRCompositor: Failed to acquire swapchain image for eye ", eye, " - error: ", static_cast<int>(result)));
        return false;
    }
    
    Logger::info(str::format("VRCompositor: Eye ", eye, " acquired swapchain image index ", imageIndex));
    
    // Wait for image to be available
    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(m_compositorSwapchains[eye], &waitInfo);
    if (XR_FAILED(result)) {
        Logger::warn(str::format("VRCompositor: Failed to wait for swapchain image for eye ", eye));
        return false;
    }
    
    // Get the Vulkan image
    VkImage swapchainImage = m_compositorSwapchainImages[eye][imageIndex].image;
    
    // Transition image to color attachment optimal
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = swapchainImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    
    vkCmdPipelineBarrier(m_compositorCommandBuffer,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    // Render a bright cyan quad (for now, just clear to cyan - 3D pipeline will be implemented later)
    Logger::info(str::format("VRCompositor: Eye ", eye, " rendering bright cyan quad"));
    
    // For now, clear to bright cyan to show our 3D quad concept works
    // TODO: Implement proper 3D Vulkan pipeline with vertex buffers, MVP matrix, shaders, etc.
    VkClearColorValue clearColor = { { 0.0f, 1.0f, 1.0f, 1.0f } }; // Bright cyan (our target color)
    VkImageSubresourceRange clearRange = {};
    clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clearRange.baseMipLevel = 0;
    clearRange.levelCount = 1;
    clearRange.baseArrayLayer = 0;
    clearRange.layerCount = 1;
    
    vkCmdClearColorImage(m_compositorCommandBuffer, swapchainImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &clearColor, 1, &clearRange);
    Logger::info(str::format("VRCompositor: Eye ", eye, " - cleared to bright cyan (3D quad placeholder)"));
    
    // Release the swapchain image
    Logger::info(str::format("VRCompositor: Eye ", eye, " released swapchain image index ", imageIndex));
    XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    result = xrReleaseSwapchainImage(m_compositorSwapchains[eye], &releaseInfo);
    if (XR_FAILED(result)) {
        Logger::warn(str::format("VRCompositor: Failed to release swapchain image for eye ", eye));
        return false;
    }
    
    return true;
}

bool VRCompositor::CreateOpenXRLayer(const XrFrameState& frameState, std::vector<XrCompositionLayerBaseHeader*>& layers) {
    if (!m_compositorSwapchainsCreated) {
        Logger::warn("VRCompositor: Cannot create layer - swapchains not created");
        return false;
    }
    
    // Get current view poses from OpenXR to fix XR_ERROR_POSE_INVALID
    XrViewLocateInfo viewLocateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
    viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    viewLocateInfo.displayTime = frameState.predictedDisplayTime;
    viewLocateInfo.space = m_cachedReferenceSpace;
    
    XrViewState viewState = { XR_TYPE_VIEW_STATE };
    std::vector<XrView> views(2, { XR_TYPE_VIEW });
    uint32_t viewCount = 2;
    
    XrResult result = xrLocateViews(m_cachedSession, &viewLocateInfo, &viewState, viewCount, &viewCount, views.data());
    if (XR_FAILED(result)) {
        Logger::warn(str::format("VRCompositor: xrLocateViews failed - error: ", static_cast<int>(result)));
        return false;
    }
    
    // Check if views are valid to prevent XR_ERROR_POSE_INVALID
    if (!(viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ||
        !(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        Logger::warn("VRCompositor: Views not valid - skipping layer creation");
        return false;
    }
    
    // Create a simple projection layer for the compositor
    static XrCompositionLayerProjection projectionLayer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    static XrCompositionLayerProjectionView projectionViews[2];
    
    projectionLayer.space = m_cachedReferenceSpace;
    projectionLayer.viewCount = 2;
    projectionLayer.views = projectionViews;
    
    // Set up projection views for both eyes with VALID poses
    for (int eye = 0; eye < 2; eye++) {
        projectionViews[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        projectionViews[eye].pose = views[eye].pose;  // Use VALID poses from xrLocateViews
        projectionViews[eye].fov = views[eye].fov;    // Use VALID FOV from xrLocateViews
        projectionViews[eye].subImage.swapchain = m_compositorSwapchains[eye];
        projectionViews[eye].subImage.imageRect.offset = { 0, 0 };
        projectionViews[eye].subImage.imageRect.extent.width = static_cast<int32_t>(m_cachedRenderWidth);
        projectionViews[eye].subImage.imageRect.extent.height = static_cast<int32_t>(m_cachedRenderHeight);
        projectionViews[eye].subImage.imageArrayIndex = 0;
    }
    
    // Add the layer to the layers vector
    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projectionLayer));
    
    Logger::info(str::format("VRCompositor: ✅ Created OpenXR projection layer with valid poses - viewCount: ", projectionLayer.viewCount));
    return true;
}

void VRCompositor::StartCompositor() {
    if (m_compositorActive.load()) {
        Logger::info("VRCompositor: Already active, ignoring start request");
        return;
    }
    
    Logger::info("VRCompositor: 🚀 Starting compositor thread");
    
    m_shouldStop = false;
    m_compositorActive = true;
    
    // Start the compositor thread
    if (!m_compositorThread.joinable()) {
        m_compositorThread = std::thread(&VRCompositor::CompositorThreadFunc, this);
    }
    
    Logger::info("VRCompositor: ✅ Compositor started and active");
}

void VRCompositor::StopCompositor() {
    Logger::info("VRCompositor: 🛑 Stopping compositor");
    m_shouldStop = true;
    m_compositorActive = false;
    
    if (m_compositorThread.joinable()) {
        m_compositorThread.join();
    }
}

void VRCompositor::CompositorThreadFunc() {
    Logger::info("VRCompositor: 🧵 Compositor thread started");
    
    while (!m_shouldStop.load()) {
        if (!RunIndependentFrame()) {
            Logger::warn("VRCompositor: Independent frame failed, sleeping");
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
    
    Logger::info("VRCompositor: 🧵 Compositor thread ending");
}

void VRCompositor::SetupQuadVertices() {
    // Create a quad positioned 2.5 meters in front of the user
    // Size: 2m wide x 1.125m tall (16:9 aspect ratio)
    float halfWidth = 1.0f;   // 2m wide
    float halfHeight = 0.5625f; // 1.125m tall
    float distance = -2.5f;   // 2.5m in front (negative Z)
    
    // Bottom-left
    m_quadVertices[0].position[0] = -halfWidth;
    m_quadVertices[0].position[1] = -halfHeight;
    m_quadVertices[0].position[2] = distance;
    m_quadVertices[0].texCoord[0] = 0.0f;
    m_quadVertices[0].texCoord[1] = 1.0f; // Flip Y for Vulkan
    m_quadVertices[0].color[0] = 1.0f; // Bright cyan
    m_quadVertices[0].color[1] = 0.0f;
    m_quadVertices[0].color[2] = 1.0f;
    
    // Bottom-right
    m_quadVertices[1].position[0] = halfWidth;
    m_quadVertices[1].position[1] = -halfHeight;
    m_quadVertices[1].position[2] = distance;
    m_quadVertices[1].texCoord[0] = 1.0f;
    m_quadVertices[1].texCoord[1] = 1.0f;
    m_quadVertices[1].color[0] = 1.0f;
    m_quadVertices[1].color[1] = 0.0f;
    m_quadVertices[1].color[2] = 1.0f;
    
    // Top-right
    m_quadVertices[2].position[0] = halfWidth;
    m_quadVertices[2].position[1] = halfHeight;
    m_quadVertices[2].position[2] = distance;
    m_quadVertices[2].texCoord[0] = 1.0f;
    m_quadVertices[2].texCoord[1] = 0.0f;
    m_quadVertices[2].color[0] = 1.0f;
    m_quadVertices[2].color[1] = 0.0f;
    m_quadVertices[2].color[2] = 1.0f;
    
    // Top-left
    m_quadVertices[3].position[0] = -halfWidth;
    m_quadVertices[3].position[1] = halfHeight;
    m_quadVertices[3].position[2] = distance;
    m_quadVertices[3].texCoord[0] = 0.0f;
    m_quadVertices[3].texCoord[1] = 0.0f;
    m_quadVertices[3].color[0] = 1.0f;
    m_quadVertices[3].color[1] = 0.0f;
    m_quadVertices[3].color[2] = 1.0f;
    
    Logger::info("VRCompositor: ✅ Quad vertices set up - cyan 2x1.125m quad at 2.5m distance");
}

void VRCompositor::SetupMVPMatrix() {
    // Create a simple identity matrix for now
    // Later we'll use proper view and projection matrices from OpenXR
    for (int i = 0; i < 16; i++) {
        m_mvpMatrix.m[i] = 0.0f;
    }
    
    // Set diagonal to 1 (identity matrix)
    m_mvpMatrix.m[0] = 1.0f;  // [0,0]
    m_mvpMatrix.m[5] = 1.0f;  // [1,1]
    m_mvpMatrix.m[10] = 1.0f; // [2,2]
    m_mvpMatrix.m[15] = 1.0f; // [3,3]
    
    Logger::info("VRCompositor: ✅ MVP matrix set up (identity for now)");
}

bool VRCompositor::RunIndependentFrame() {
    static int frameCount = 0;
    frameCount++;
    
    if (frameCount == 1) {
        Logger::info("VRCompositor: 🧵 First RunIndependentFrame called - thread is running!");
    }
    
    if (!m_pOpenXRManager) {
        Logger::warn("VRCompositor: RunIndependentFrame failed - no OpenXR manager");
        return false;
    }
    
    if (!m_compositorSwapchainsCreated) {
        Logger::warn("VRCompositor: RunIndependentFrame failed - swapchains not created");
        return false;
    }
    
    if (m_cachedSession == XR_NULL_HANDLE) {
        Logger::warn("VRCompositor: RunIndependentFrame failed - invalid cached session");
        return false;
    }
    
    // Wait for the next frame
    XrFrameWaitInfo frameWaitInfo = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState frameState = { XR_TYPE_FRAME_STATE };
    
    XrResult result = xrWaitFrame(m_cachedSession, &frameWaitInfo, &frameState);
    if (XR_FAILED(result)) {
        Logger::warn(str::format("VRCompositor: Independent xrWaitFrame failed - error: ", static_cast<int>(result)));
        return false;
    }
    
    // Begin the frame
    XrFrameBeginInfo frameBeginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
    result = xrBeginFrame(m_cachedSession, &frameBeginInfo);
    if (XR_FAILED(result)) {
        Logger::warn(str::format("VRCompositor: Independent xrBeginFrame failed - error: ", static_cast<int>(result)));
        return false;
    }
    
    // Render both eyes
    bool eye0Rendered = RenderEye(0);
    bool eye1Rendered = RenderEye(1);
    bool rendered = eye0Rendered && eye1Rendered;
    
    // Create OpenXR layers
    std::vector<XrCompositionLayerBaseHeader*> layers;
    if (rendered) {
        CreateOpenXRLayer(frameState, layers);
    }
    
    // End the frame
    XrFrameEndInfo frameEndInfo = { XR_TYPE_FRAME_END_INFO };
    frameEndInfo.displayTime = frameState.predictedDisplayTime;
    frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    frameEndInfo.layerCount = static_cast<uint32_t>(layers.size());
    frameEndInfo.layers = layers.empty() ? nullptr : layers.data();
    
    result = xrEndFrame(m_cachedSession, &frameEndInfo);
    if (XR_FAILED(result)) {
        Logger::warn(str::format("VRCompositor: Independent xrEndFrame failed - error: ", static_cast<int>(result)));
        return false;
    }
    
    return true;
}

} // namespace dxvk
