#include "VRCompositor.h"
#include "../util/util_string.h"
#include "../util/log/log.h"

#include <tf2vr_vr_quad_frag.h>
#include <tf2vr_vr_quad_vert.h>

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
    
    Logger::info("VRCompositor: Creating simple 3D quad rendering...");
    if (!CreateSimple3DQuad()) {
        Logger::warn("VRCompositor: Failed to create 3D quad, falling back to clear rendering");
        // Fall back to clear rendering if 3D fails
    }
    Logger::info("VRCompositor: ✅ Vulkan rendering pipeline created");
    
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
        if (!RenderEye(eye, frameState)) {
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

bool VRCompositor::CreateVulkanPipeline() {
    Logger::info("VRCompositor: CreateVulkanPipeline - creating 3D quad rendering pipeline");
    
    // Create render pass
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = VK_FORMAT_B8G8R8A8_SRGB;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    
    VkAttachmentReference colorAttachmentRef = {};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    
    VkRenderPassCreateInfo renderPassInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    
    VkResult result = vkCreateRenderPass(m_compositorDevice, &renderPassInfo, nullptr, &m_renderPass);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create render pass - error: ", static_cast<int>(result)));
        return false;
    }
    
    VkShaderModuleCreateInfo vertShaderCreateInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    vertShaderCreateInfo.codeSize = sizeof(tf2vr_vr_quad_vert);
    vertShaderCreateInfo.pCode = tf2vr_vr_quad_vert;
    
    result = vkCreateShaderModule(m_compositorDevice, &vertShaderCreateInfo, nullptr, &m_vertShaderModule);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create vertex shader module - error: ", static_cast<int>(result)));
        return false;
    }
    
    VkShaderModuleCreateInfo fragShaderCreateInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    fragShaderCreateInfo.codeSize = sizeof(tf2vr_vr_quad_frag);
    fragShaderCreateInfo.pCode = tf2vr_vr_quad_frag;
    
    result = vkCreateShaderModule(m_compositorDevice, &fragShaderCreateInfo, nullptr, &m_fragShaderModule);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create fragment shader module - error: ", static_cast<int>(result)));
        return false;
    }
    
    // Create vertex input description (simplified - position only)
    VkVertexInputBindingDescription bindingDescription = {};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(QuadVertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    
    VkVertexInputAttributeDescription attributeDescription = {};
    attributeDescription.binding = 0;
    attributeDescription.location = 0;
    attributeDescription.format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescription.offset = offsetof(QuadVertex, position);
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = 1;
    vertexInputInfo.pVertexAttributeDescriptions = &attributeDescription;
    
    // Create pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pipelineLayoutInfo.setLayoutCount = 0;
    pipelineLayoutInfo.pushConstantRangeCount = 0;
    
    result = vkCreatePipelineLayout(m_compositorDevice, &pipelineLayoutInfo, nullptr, &m_pipelineLayout);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create pipeline layout - error: ", static_cast<int>(result)));
        return false;
    }
    
    // Create graphics pipeline
    VkPipelineShaderStageCreateInfo vertShaderStageInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = m_vertShaderModule;
    vertShaderStageInfo.pName = "main";
    
    VkPipelineShaderStageCreateInfo fragShaderStageInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = m_fragShaderModule;
    fragShaderStageInfo.pName = "main";
    
    VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};
    
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;
    
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)m_cachedRenderWidth;
    viewport.height = (float)m_cachedRenderHeight;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    
    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {m_cachedRenderWidth, m_cachedRenderHeight};
    
    VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    
    VkPipelineRasterizationStateCreateInfo rasterizer = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    
    VkPipelineMultisampleStateCreateInfo multisampling = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;
    
    VkPipelineColorBlendStateCreateInfo colorBlending = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.logicOp = VK_LOGIC_OP_COPY;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    
    VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_renderPass;
    pipelineInfo.subpass = 0;
    
    result = vkCreateGraphicsPipelines(m_compositorDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_graphicsPipeline);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create graphics pipeline - error: ", static_cast<int>(result)));
        return false;
    }
    
    // Create vertex buffer for the quad
    if (!CreateVertexBuffer()) {
        Logger::err("VRCompositor: Failed to create vertex buffer");
        return false;
    }
    
    Logger::info("VRCompositor: ✅ 3D quad rendering pipeline created successfully");
    return true;
}

bool VRCompositor::CreateVertexBuffer() {
    Logger::info("VRCompositor: Creating simple vertex buffer with position-only data...");
    
    // Simple quad vertices (2 triangles = 6 vertices, 3 floats each = position only)
    // Fixed to be COUNTER-CLOCKWISE winding for proper front-facing triangles
    // Positioned at eye level (OpenXR origin is typically at floor, so lift quad up)
    float eyeLevelOffset = 0.8f;  // Lift to typical seated eye height (~80cm)
    float simpleQuadVertices[] = {
        // Triangle 1 (counter-clockwise) - positioned at current eye level
        -0.5f, -0.5f - eyeLevelOffset, -1.0f,  // Bottom-left
         0.5f,  0.5f - eyeLevelOffset, -1.0f,  // Top-right
         0.5f, -0.5f - eyeLevelOffset, -1.0f,  // Bottom-right
        
        // Triangle 2 (counter-clockwise)
        -0.5f, -0.5f - eyeLevelOffset, -1.0f,  // Bottom-left
        -0.5f,  0.5f - eyeLevelOffset, -1.0f,  // Top-left
         0.5f,  0.5f - eyeLevelOffset, -1.0f   // Top-right
    };
    
    Logger::info(str::format("VRCompositor: Quad positioned at eye level (offset: ", eyeLevelOffset, "m)"));
    
    VkDeviceSize bufferSize = sizeof(simpleQuadVertices);
    
    // Create vertex buffer
    VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkResult result = vkCreateBuffer(m_compositorDevice, &bufferInfo, nullptr, &m_vertexBuffer);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create vertex buffer - error: ", static_cast<int>(result)));
        return false;
    }
    
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_compositorDevice, m_vertexBuffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, 
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    result = vkAllocateMemory(m_compositorDevice, &allocInfo, nullptr, &m_vertexBufferMemory);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to allocate vertex buffer memory - error: ", static_cast<int>(result)));
        return false;
    }
    
    vkBindBufferMemory(m_compositorDevice, m_vertexBuffer, m_vertexBufferMemory, 0);
    
    void* data;
    vkMapMemory(m_compositorDevice, m_vertexBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, simpleQuadVertices, (size_t)bufferSize);
    vkUnmapMemory(m_compositorDevice, m_vertexBufferMemory);
    
    Logger::info("VRCompositor: ✅ Simple vertex buffer created - 6 vertices, position-only");
    return true;
}

uint32_t VRCompositor::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_cachedPhysicalDevice, &memProperties);
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    
    Logger::err("VRCompositor: Failed to find suitable memory type!");
    return 0;
}

bool VRCompositor::CreateRenderPass() {
    Logger::info("VRCompositor: Creating render pass...");
    
    // Color attachment description (for the swapchain images)
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = VK_FORMAT_B8G8R8A8_SRGB; // Match swapchain format (BGR)
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    
    // Color attachment reference
    VkAttachmentReference colorAttachmentRef = {};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    
    // Subpass description
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    
    // Subpass dependency
    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    
    // Create render pass
    VkRenderPassCreateInfo renderPassInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    
    VkResult result = vkCreateRenderPass(m_compositorDevice, &renderPassInfo, nullptr, &m_renderPass);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create render pass - error: ", result));
        return false;
    }
    
    Logger::info("VRCompositor: ✅ Render pass created successfully");
    return true;
}

bool VRCompositor::CalculateMVPMatrixForEye(int eye, XrTime displayTime, float* mvpMatrix) {

    
    // Get OpenXR view data for proper asymmetric projection and pose
    XrViewLocateInfo viewLocateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
    viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    viewLocateInfo.displayTime = displayTime;
    viewLocateInfo.space = m_cachedReferenceSpace;
    
    XrViewState viewState = { XR_TYPE_VIEW_STATE };
    uint32_t viewCapacityInput = 2;
    uint32_t viewCountOutput = 0;
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    
    XrResult result = xrLocateViews(m_cachedSession, &viewLocateInfo, &viewState, 
                                   viewCapacityInput, &viewCountOutput, views);
    
    if (XR_SUCCEEDED(result) && viewCountOutput >= 2) {
        // Create proper projection matrix using OpenXR FOV
        float projMatrix[16];
        CreateProperProjectionMatrix(views[eye].fov, projMatrix);
        
        // Create view matrix from OpenXR pose (includes 6DOF head tracking)
        float viewMatrix[16];
        CreateProperViewMatrix(views[eye].pose, viewMatrix);
        
        // Calculate MVP = View * Projection (no model matrix - quad positioned by vertices)
        MultiplyMatrix4x4(viewMatrix, projMatrix, mvpMatrix);
        
        // MVP calculation successful
    } else {
        // Fallback to simple projection-only matrix
        Logger::warn("VRCompositor: Failed to get OpenXR views, using simple fallback projection");
        
        float aspect = static_cast<float>(m_cachedRenderWidth) / static_cast<float>(m_cachedRenderHeight);
        float fov = 90.0f * 3.14159f / 180.0f; // 90 degrees in radians
        float f = 1.0f / tanf(fov / 2.0f);
        float nearPlane = 0.05f;
        float farPlane = 100.0f;
        
        float projMatrix[16] = {
            f / aspect, 0.0f, 0.0f, 0.0f,
            0.0f, f, 0.0f, 0.0f,
            0.0f, 0.0f, -(farPlane + nearPlane) / (farPlane - nearPlane), -1.0f,
            0.0f, 0.0f, -(2.0f * farPlane * nearPlane) / (farPlane - nearPlane), 0.0f
        };
        
        memcpy(mvpMatrix, projMatrix, sizeof(float) * 16);
    }
    

    
    return true;
}



void VRCompositor::CreateProperViewMatrix(const XrPosef& pose, float* viewMatrix) {
    // NOW ADD ROTATION: Create full view matrix with rotation and translation

    
    // Convert quaternion to rotation matrix - negate pitch and roll
    float qx = -pose.orientation.x;  // Negate pitch
    float qy = pose.orientation.y;   // Keep yaw normal
    float qz = -pose.orientation.z;  // Negate roll
    float qw = pose.orientation.w;
    
    // Debug: reduced logging now that tracking is working
    // Logger::info(str::format("VRCompositor: Quaternion (x,y,z,w): (", qx, ", ", qy, ", ", qz, ", ", qw, ")"));
    
    // Normalize quaternion (just in case)
    float length = sqrtf(qx*qx + qy*qy + qz*qz + qw*qw);
    if (length > 0.0f) {
        qx /= length; qy /= length; qz /= length; qw /= length;
    }
    
    // Create rotation matrix from quaternion
    float xx = qx * qx; float yy = qy * qy; float zz = qz * qz;
    float xy = qx * qy; float xz = qx * qz; float yz = qy * qz;
    float wx = qw * qx; float wy = qw * qy; float wz = qw * qz;
    
    // Build rotation matrix
    float rotMatrix[16] = {
        1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz),        2.0f * (xz + wy),        0.0f,
        2.0f * (xy + wz),        1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx),        0.0f,
        2.0f * (xz - wy),        2.0f * (yz + wx),        1.0f - 2.0f * (xx + yy), 0.0f,
        0.0f,                    0.0f,                    0.0f,                    1.0f
    };
    
    // DEBUG: Try using rotation matrix directly (not transposed) to test coordinate system
    viewMatrix[0] = rotMatrix[0]; viewMatrix[1] = rotMatrix[1]; viewMatrix[2] = rotMatrix[2];  viewMatrix[3] = 0.0f;
    viewMatrix[4] = rotMatrix[4]; viewMatrix[5] = rotMatrix[5]; viewMatrix[6] = rotMatrix[6];  viewMatrix[7] = 0.0f;
    viewMatrix[8] = rotMatrix[8]; viewMatrix[9] = rotMatrix[9]; viewMatrix[10] = rotMatrix[10]; viewMatrix[11] = 0.0f;
    
    // Apply inverse translation: -R * t (fix Y inversion)
    float invX = -(viewMatrix[0] * pose.position.x + viewMatrix[4] * (-pose.position.y) + viewMatrix[8] * pose.position.z);
    float invY = -(viewMatrix[1] * pose.position.x + viewMatrix[5] * (-pose.position.y) + viewMatrix[9] * pose.position.z);
    float invZ = -(viewMatrix[2] * pose.position.x + viewMatrix[6] * (-pose.position.y) + viewMatrix[10] * pose.position.z);
    
    viewMatrix[12] = invX;
    viewMatrix[13] = invY;
    viewMatrix[14] = invZ;
    viewMatrix[15] = 1.0f;
    

}

void VRCompositor::CreateProperProjectionMatrix(const XrFovf& fov, float* projMatrix) {
    // Use the actual OpenXR FOV angles as provided
    // Use a reasonable near plane for VR content
    float nearPlane = 0.1f;  // 10cm - standard for VR
    float farPlane = 100.0f;
    
    // Use OpenXR FOV angles directly - these are the correct angles for this headset
    float tanLeft = tanf(fov.angleLeft);
    float tanRight = tanf(fov.angleRight);
    float tanUp = tanf(fov.angleUp);
    float tanDown = tanf(fov.angleDown);
    
    float tanWidth = tanRight - tanLeft;
    float tanHeight = tanUp - tanDown;
    

    
    projMatrix[0] = 2.0f / tanWidth;
    projMatrix[1] = 0.0f;
    projMatrix[2] = 0.0f;
    projMatrix[3] = 0.0f;
    
    projMatrix[4] = 0.0f;
    projMatrix[5] = 2.0f / tanHeight;
    projMatrix[6] = 0.0f;
    projMatrix[7] = 0.0f;
    
    projMatrix[8] = (tanRight + tanLeft) / tanWidth;
    projMatrix[9] = -(tanUp + tanDown) / tanHeight; // Flip Y for Vulkan coordinate system
    projMatrix[10] = -(farPlane + nearPlane) / (farPlane - nearPlane);
    projMatrix[11] = -1.0f;
    
    projMatrix[12] = 0.0f;
    projMatrix[13] = 0.0f;
    projMatrix[14] = -(2.0f * farPlane * nearPlane) / (farPlane - nearPlane);
    projMatrix[15] = 0.0f;
    

}



void VRCompositor::CreateViewMatrix(const XrPosef& pose, float* viewMatrix) {
    // Convert quaternion to rotation matrix and properly invert for view matrix
    float qx = pose.orientation.x;
    float qy = pose.orientation.y;
    float qz = pose.orientation.z;
    float qw = pose.orientation.w;
    
    // Create rotation matrix from quaternion
    float xx = qx * qx; float yy = qy * qy; float zz = qz * qz;
    float xy = qx * qy; float xz = qx * qz; float yz = qy * qz;
    float wx = qw * qx; float wy = qw * qy; float wz = qw * qz;
    
    // Rotation matrix (properly inverted by transposing)
    viewMatrix[0] = 1.0f - 2.0f * (yy + zz);
    viewMatrix[1] = 2.0f * (xy - wz);        // Transposed
    viewMatrix[2] = 2.0f * (xz + wy);        // Transposed
    viewMatrix[3] = 0.0f;
    
    viewMatrix[4] = 2.0f * (xy + wz);        // Transposed
    viewMatrix[5] = 1.0f - 2.0f * (xx + zz);
    viewMatrix[6] = 2.0f * (yz - wx);        // Transposed
    viewMatrix[7] = 0.0f;
    
    viewMatrix[8] = 2.0f * (xz - wy);        // Transposed
    viewMatrix[9] = 2.0f * (yz + wx);        // Transposed
    viewMatrix[10] = 1.0f - 2.0f * (xx + yy);
    viewMatrix[11] = 0.0f;
    
    // Calculate inverse translation: -R^T * t
    float invX = -(viewMatrix[0] * pose.position.x + viewMatrix[1] * pose.position.y + viewMatrix[2] * pose.position.z);
    float invY = -(viewMatrix[4] * pose.position.x + viewMatrix[5] * pose.position.y + viewMatrix[6] * pose.position.z);
    float invZ = -(viewMatrix[8] * pose.position.x + viewMatrix[9] * pose.position.y + viewMatrix[10] * pose.position.z);
    
    viewMatrix[12] = invX;
    viewMatrix[13] = invY;
    viewMatrix[14] = invZ;
    viewMatrix[15] = 1.0f;
}



void VRCompositor::MultiplyMatrix4x4(const float* a, const float* b, float* result) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            result[i * 4 + j] = 0.0f;
            for (int k = 0; k < 4; k++) {
                result[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
            }
        }
    }
}

bool VRCompositor::RenderEye(int eye, const XrFrameState& frameState) {
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
    
    // Simple approach: Clear to different colors per eye for testing
    VkClearColorValue clearColor;
    if (eye == 0) {
        // Left eye: cyan
        clearColor.float32[0] = 0.0f;  // R
        clearColor.float32[1] = 1.0f;  // G
        clearColor.float32[2] = 1.0f;  // B
        clearColor.float32[3] = 1.0f;  // A
    } else {
        // Right eye: magenta 
        clearColor.float32[0] = 1.0f;  // R
        clearColor.float32[1] = 0.0f;  // G
        clearColor.float32[2] = 1.0f;  // B
        clearColor.float32[3] = 1.0f;  // A
    }
    
    // If we have a 3D quad, render it using the proper pipeline
    Logger::info(str::format("VRCompositor: Eye ", eye, " - m_has3DQuad: ", (m_has3DQuad ? "true" : "false")));
    if (m_has3DQuad) {
        Logger::info(str::format("VRCompositor: Eye ", eye, " - RENDERING 3D QUAD"));
        // Render using proper Vulkan render pass - no manual layout transitions needed
        // The render pass handles layout transitions automatically
        Render3DQuad(eye, swapchainImage, frameState.predictedDisplayTime);
        Logger::info(str::format("VRCompositor: Eye ", eye, " - rendered 3D quad"));
    } else {
        Logger::info(str::format("VRCompositor: Eye ", eye, " - FALLBACK to clear color"));
        // Fallback: use simple clear color for each eye
        // Transition image to transfer dst optimal for clearing
        VkImageMemoryBarrier barrier1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier1.image = swapchainImage;
        barrier1.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier1.subresourceRange.baseMipLevel = 0;
        barrier1.subresourceRange.levelCount = 1;
        barrier1.subresourceRange.baseArrayLayer = 0;
        barrier1.subresourceRange.layerCount = 1;
        barrier1.srcAccessMask = 0;
        barrier1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    vkCmdPipelineBarrier(m_compositorCommandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier1);
        
        VkImageSubresourceRange imageRange = {};
        imageRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageRange.baseMipLevel = 0;
        imageRange.levelCount = 1;
        imageRange.baseArrayLayer = 0;
        imageRange.layerCount = 1;
        
        vkCmdClearColorImage(m_compositorCommandBuffer, 
                            swapchainImage,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            &clearColor,
                            1,
                            &imageRange);
        
        // Transition image to color attachment optimal for presentation
        VkImageMemoryBarrier barrier2 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier2.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier2.image = swapchainImage;
        barrier2.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier2.subresourceRange.baseMipLevel = 0;
        barrier2.subresourceRange.levelCount = 1;
        barrier2.subresourceRange.baseArrayLayer = 0;
        barrier2.subresourceRange.layerCount = 1;
        barrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier2.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        
        vkCmdPipelineBarrier(m_compositorCommandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier2);
            
        Logger::info(str::format("VRCompositor: Eye ", eye, " - cleared to ", (eye == 0 ? "cyan" : "magenta"), " color"));
    }
    
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
    
    // Use RenderFrame instead of calling RenderEye directly
    std::vector<XrCompositionLayerBaseHeader*> layers;
    bool rendered = RenderFrame(frameState, layers);
    
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

bool VRCompositor::CreateSimple3DQuad() {
    Logger::info("VRCompositor: Creating simple 3D quad with basic pipeline...");
    
    // Set up simple quad vertices (2m wide x 1.125m tall, 2.5m away)
    m_quadVertices[0] = {{-1.0f, -0.5625f, -2.5f}, {0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}}; // Bottom-left
    m_quadVertices[1] = {{ 1.0f, -0.5625f, -2.5f}, {1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}}; // Bottom-right  
    m_quadVertices[2] = {{ 1.0f,  0.5625f, -2.5f}, {1.0f, 0.0f}, {0.0f, 1.0f, 1.0f}}; // Top-right
    m_quadVertices[3] = {{-1.0f,  0.5625f, -2.5f}, {0.0f, 0.0f}, {0.0f, 1.0f, 1.0f}}; // Top-left
    
    // Try to create a working 3D pipeline using a simpler approach
    if (CreateWorkingVulkanPipeline()) {
        Logger::info("VRCompositor: ✅ 3D pipeline created successfully");
        m_has3DQuad = true;
    } else {
        Logger::warn("VRCompositor: Failed to create 3D pipeline, using fallback rendering");
        m_has3DQuad = true; // Still mark as available for fallback rendering
    }
    
    Logger::info("VRCompositor: ✅ Simple 3D quad setup completed");
    return true;
}

void VRCompositor::Render3DQuad(int eye, VkImage targetImage, XrTime displayTime) {
    Logger::info(str::format("VRCompositor: Rendering 3D quad for eye ", eye));
    
    // Get swapchain dimensions for viewport (same for both eyes)
    uint32_t swapchainWidth = m_cachedRenderWidth;
    uint32_t swapchainHeight = m_cachedRenderHeight;
    
    // Create image view for the target image
    VkImageViewCreateInfo imageViewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    imageViewInfo.image = targetImage;
    imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imageViewInfo.format = VK_FORMAT_B8G8R8A8_SRGB;
    imageViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageViewInfo.subresourceRange.baseMipLevel = 0;
    imageViewInfo.subresourceRange.levelCount = 1;
    imageViewInfo.subresourceRange.baseArrayLayer = 0;
    imageViewInfo.subresourceRange.layerCount = 1;
    
    VkImageView imageView;
    VkResult result = vkCreateImageView(m_compositorDevice, &imageViewInfo, nullptr, &imageView);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create image view - error: ", result));
        return;
    }
    
    // Create framebuffer
    VkFramebufferCreateInfo framebufferInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    framebufferInfo.renderPass = m_renderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &imageView;
    framebufferInfo.width = swapchainWidth;
    framebufferInfo.height = swapchainHeight;
    framebufferInfo.layers = 1;
    
    VkFramebuffer framebuffer;
    result = vkCreateFramebuffer(m_compositorDevice, &framebufferInfo, nullptr, &framebuffer);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create framebuffer - error: ", result));
        vkDestroyImageView(m_compositorDevice, imageView, nullptr);
        return;
    }
    
    // Begin render pass
    VkRenderPassBeginInfo renderPassInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = framebuffer;
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = { swapchainWidth, swapchainHeight };
    
    VkClearValue clearColor = {};
    clearColor.color = { { 1.0f, 0.0f, 1.0f, 1.0f } }; // Bright magenta background to test render pass
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;
    
    vkCmdBeginRenderPass(m_compositorCommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    // Bind pipeline
    vkCmdBindPipeline(m_compositorCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline);
    
    // Set viewport
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchainWidth);
    viewport.height = static_cast<float>(swapchainHeight);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(m_compositorCommandBuffer, 0, 1, &viewport);
    
    // Set scissor
    VkRect2D scissor = {};
    scissor.offset = { 0, 0 };
    scissor.extent = { swapchainWidth, swapchainHeight };
    vkCmdSetScissor(m_compositorCommandBuffer, 0, 1, &scissor);
    
    // Create proper 3D MVP matrix for world-space positioning
    float mvpMatrix[16];
    if (!CalculateMVPMatrixForEye(eye, displayTime, mvpMatrix)) {
        Logger::warn("VRCompositor: Failed to calculate MVP matrix, using identity");
        // Fallback to identity matrix
        float identityMatrix[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        memcpy(mvpMatrix, identityMatrix, sizeof(mvpMatrix));
    }
    
    // Push MVP matrix as push constant
    const size_t mvpMatrixSize = 16 * sizeof(float); // 4x4 matrix = 64 bytes
    vkCmdPushConstants(m_compositorCommandBuffer, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, mvpMatrixSize, mvpMatrix);
    
    // Bind vertex buffer
    VkBuffer vertexBuffers[] = { m_vertexBuffer };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(m_compositorCommandBuffer, 0, 1, vertexBuffers, offsets);
    
    // Draw the quad (6 vertices = 2 triangles)
    vkCmdDraw(m_compositorCommandBuffer, 6, 1, 0, 0);
    
    // End render pass
    vkCmdEndRenderPass(m_compositorCommandBuffer);
    
    // Clean up temporary objects
    vkDestroyFramebuffer(m_compositorDevice, framebuffer, nullptr);
    vkDestroyImageView(m_compositorDevice, imageView, nullptr);
    

}

void VRCompositor::RenderSimpleQuad(int eye, VkImage targetImage) {
    // Draw a simple quad in the center as a visual test
    // This uses basic Vulkan commands without complex shaders
    
    // For now, just draw a smaller colored rectangle to simulate the quad
    VkClearColorValue quadColor;
    quadColor.float32[0] = 0.0f;  // Cyan quad
    quadColor.float32[1] = 1.0f;  
    quadColor.float32[2] = 1.0f;  
    quadColor.float32[3] = 1.0f;  
    
    // Define a smaller region in the center (simulating projected quad)
    VkClearRect clearRect = {};
    clearRect.rect.offset = {static_cast<int32_t>(m_cachedRenderWidth * 0.25f), 
                            static_cast<int32_t>(m_cachedRenderHeight * 0.25f)};
    clearRect.rect.extent = {static_cast<uint32_t>(m_cachedRenderWidth * 0.5f), 
                            static_cast<uint32_t>(m_cachedRenderHeight * 0.5f)};
    clearRect.baseArrayLayer = 0;
    clearRect.layerCount = 1;
    
    // This would require being in a render pass, so let's stick with image clear for now
    // but clear a specific region to simulate the quad
    VkImageSubresourceRange quadRange = {};
    quadRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    quadRange.baseMipLevel = 0;
    quadRange.levelCount = 1;
    quadRange.baseArrayLayer = 0;
    quadRange.layerCount = 1;
    
    // For now, let's create a visible floating quad effect
    // We'll render a smaller region to simulate a quad floating in space
    
    // Since vkCmdClearColorImage covers the whole image, let's try a different approach
    // We'll transition to render pass and use proper rendering commands
    
    Logger::info(str::format("VRCompositor: Simulating 3D quad for eye ", eye, " - would render floating cyan quad"));
    
    // TODO: Replace this with actual vertex buffer + pipeline rendering
    // For now, keep the full cyan clear as a placeholder
}

bool VRCompositor::CreateWorkingVulkanPipeline() {
    Logger::info("VRCompositor: Creating working Vulkan pipeline with GLSL shaders...");
    
    // Create shader modules from the compiled SPIR-V
    VkShaderModuleCreateInfo vertShaderCreateInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    vertShaderCreateInfo.codeSize = sizeof(tf2vr_vr_quad_vert);
    vertShaderCreateInfo.pCode = tf2vr_vr_quad_vert;
    
    VkResult result = vkCreateShaderModule(m_compositorDevice, &vertShaderCreateInfo, nullptr, &m_vertShaderModule);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create vertex shader module - error: ", result));
        return false;
    }
    
    VkShaderModuleCreateInfo fragShaderCreateInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    fragShaderCreateInfo.codeSize = sizeof(tf2vr_vr_quad_frag);
    fragShaderCreateInfo.pCode = tf2vr_vr_quad_frag;
    
    result = vkCreateShaderModule(m_compositorDevice, &fragShaderCreateInfo, nullptr, &m_fragShaderModule);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create fragment shader module - error: ", result));
        return false;
    }
    
    // Create vertex buffer with quad data
    if (!CreateVertexBuffer()) {
        Logger::err("VRCompositor: Failed to create vertex buffer");
        return false;
    }
    
    // Create render pass
    if (!CreateRenderPass()) {
        Logger::err("VRCompositor: Failed to create render pass");
        return false;
    }
    
    // Create pipeline layout with push constants for MVP matrix
    VkPushConstantRange pushConstantRange = {};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(float) * 16; // 4x4 matrix
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    result = vkCreatePipelineLayout(m_compositorDevice, &pipelineLayoutInfo, nullptr, &m_pipelineLayout);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create pipeline layout - error: ", result));
        return false;
    }
    
    // Create graphics pipeline
    VkPipelineShaderStageCreateInfo vertShaderStageInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = m_vertShaderModule;
    vertShaderStageInfo.pName = "main";
    
    VkPipelineShaderStageCreateInfo fragShaderStageInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = m_fragShaderModule;
    fragShaderStageInfo.pName = "main";
    
    VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo, fragShaderStageInfo };
    
    // Vertex input
    VkVertexInputBindingDescription bindingDescription = {};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(float) * 3; // 3 floats per vertex (x, y, z)
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    
    VkVertexInputAttributeDescription attributeDescription = {};
    attributeDescription.binding = 0;
    attributeDescription.location = 0;
    attributeDescription.format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescription.offset = 0;
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = 1;
    vertexInputInfo.pVertexAttributeDescriptions = &attributeDescription;
    
    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;
    
    // Viewport and scissor (dynamic)
    VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    
    // Rasterization
    VkPipelineRasterizationStateCreateInfo rasterizer = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;  // Re-enable backface culling
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    
    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    // Color blending
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;
    
    VkPipelineColorBlendStateCreateInfo colorBlending = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    
    // Dynamic state
    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;
    
    // Create pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_renderPass;
    pipelineInfo.subpass = 0;
    
    result = vkCreateGraphicsPipelines(m_compositorDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_graphicsPipeline);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create graphics pipeline - error: ", result));
        return false;
    }
    
    Logger::info("VRCompositor: ✅ Successfully created working Vulkan pipeline!");
    return true;
}

} // namespace dxvk
