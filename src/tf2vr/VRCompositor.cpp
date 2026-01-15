#include "VRCompositor.h"
#include "hmdWrapper.h"
#include "hud_position_shared.h"
#include "../util/util_string.h"
#include "../util/log/log.h"

#include <tf2vr_vr_quad_frag.h>
#include <tf2vr_vr_quad_vert.h>
#include <chrono>
#include <fstream>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Forward declare what we need from OpenXRDirectMode
class OpenXRDirectMode;

// Forward declare VGUI texture checking function
extern void CheckAndCopyTrackedVGUITexture();

// Forward declare compositor synchronization function
extern "C" void TF2VR_CompositorBeginTextureSync();
extern std::timed_mutex* GetPresentSyncMutex();

// Frame tracking variables from OpenXRDirectMode.cpp (global scope)
extern std::atomic<uint64_t> g_tf2CompletedFrameId;
extern std::atomic<uint64_t> g_lastCopiedFrameId;

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
    
    // Cleanup mask texture
    if (m_maskTextureView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_compositorDevice, m_maskTextureView, nullptr);
        m_maskTextureView = VK_NULL_HANDLE;
    }
    if (m_maskTexture != VK_NULL_HANDLE) {
        vkDestroyImage(m_compositorDevice, m_maskTexture, nullptr);
        m_maskTexture = VK_NULL_HANDLE;
    }
    if (m_maskTextureMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_compositorDevice, m_maskTextureMemory, nullptr);
        m_maskTextureMemory = VK_NULL_HANDLE;
    }
    if (m_maskTextureSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_compositorDevice, m_maskTextureSampler, nullptr);
        m_maskTextureSampler = VK_NULL_HANDLE;
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
    m_currentState = state;
    
    // Initialize synchronization - TF2 should wait for VR frame completion
    m_tf2CanRenderFrame = false;
    
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
    
    // Reset the fence for this frame's GPU work tracking
    // The fence should be signaled from the previous frame's wait
    VkResult result = vkResetFences(m_compositorDevice, 1, &m_compositorFence);
    if (result != VK_SUCCESS) {
        Logger::warn(str::format("VRCompositor: Failed to reset fence - error: ", static_cast<int>(result)));
        // Try waiting for the fence if it's still unsignaled
        vkWaitForFences(m_compositorDevice, 1, &m_compositorFence, VK_TRUE, 10000000);
        result = vkResetFences(m_compositorDevice, 1, &m_compositorFence);
        if (result != VK_SUCCESS) {
            Logger::err("VRCompositor: Failed to reset fence even after wait");
            return false;
        }
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
    
    // CRITICAL: Wait for GPU work to complete BEFORE releasing swapchain images
    // Some OpenXR runtimes (non-SteamVR) may not handle async GPU work properly
    // and expect the images to be fully rendered when released.
    // This adds ~1-2ms latency but ensures consistent behavior across runtimes.
    result = vkWaitForFences(m_compositorDevice, 1, &m_compositorFence, VK_TRUE, 10000000); // 10ms timeout
    if (result == VK_TIMEOUT) {
        Logger::warn("VRCompositor: GPU work timeout before swapchain release");
    } else if (result != VK_SUCCESS) {
        Logger::warn(str::format("VRCompositor: Fence wait failed: ", static_cast<int>(result)));
    }
    
    // Release swapchain images AFTER GPU work is complete
    for (int eye = 0; eye < 2; eye++) {
        if (m_compositorSwapchains[eye] != XR_NULL_HANDLE) {
            XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            XrResult xrResult = xrReleaseSwapchainImage(m_compositorSwapchains[eye], &releaseInfo);
            if (XR_FAILED(xrResult)) {
                Logger::warn(str::format("VRCompositor: Failed to release swapchain image for eye ", eye, " - error: ", static_cast<int>(xrResult)));
            }
        }
    }
    
    // Create OpenXR layer
    if (!CreateOpenXRLayer(frameState, layers)) {
        Logger::warn("VRCompositor: Failed to create OpenXR layer");
        return false;
    }
    
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
    
    static int copyCallCount = 0;
    copyCallCount++;

    // Check if we need to recreate the copied texture with new dimensions
    if (m_copiedTextureWidth != width || m_copiedTextureHeight != height || m_copiedMenuTexture == VK_NULL_HANDLE) {
        
        // Clean up existing texture if it exists
        if (m_copiedMenuTextureView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_compositorDevice, m_copiedMenuTextureView, nullptr);
            m_copiedMenuTextureView = VK_NULL_HANDLE;
        }
        if (m_copiedMenuTexture != VK_NULL_HANDLE) {
            vkDestroyImage(m_compositorDevice, m_copiedMenuTexture, nullptr);
            m_copiedMenuTexture = VK_NULL_HANDLE;
        }
        if (m_copiedMenuTextureMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_compositorDevice, m_copiedMenuTextureMemory, nullptr);
            m_copiedMenuTextureMemory = VK_NULL_HANDLE;
        }
        
        // Create new copied texture with the source dimensions
        VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_B8G8R8A8_SRGB; // Match swapchain SRGB format for proper color space
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        VkResult result = vkCreateImage(m_compositorDevice, &imageInfo, nullptr, &m_copiedMenuTexture);
        if (result != VK_SUCCESS) {
            Logger::err(str::format("VRCompositor: Failed to create copied menu texture - error: ", result));
            return false;
        }
        
        // Allocate memory for the copied texture
        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(m_compositorDevice, m_copiedMenuTexture, &memRequirements);
        
        VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        result = vkAllocateMemory(m_compositorDevice, &allocInfo, nullptr, &m_copiedMenuTextureMemory);
        if (result != VK_SUCCESS) {
            Logger::err(str::format("VRCompositor: Failed to allocate copied menu texture memory - error: ", result));
            vkDestroyImage(m_compositorDevice, m_copiedMenuTexture, nullptr);
            m_copiedMenuTexture = VK_NULL_HANDLE;
            return false;
        }
        
        vkBindImageMemory(m_compositorDevice, m_copiedMenuTexture, m_copiedMenuTextureMemory, 0);
        
        // Create image view for the copied texture
        VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = m_copiedMenuTexture;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        
        result = vkCreateImageView(m_compositorDevice, &viewInfo, nullptr, &m_copiedMenuTextureView);
        if (result != VK_SUCCESS) {
            Logger::err(str::format("VRCompositor: Failed to create copied menu texture view - error: ", result));
            vkDestroyImage(m_compositorDevice, m_copiedMenuTexture, nullptr);
            vkFreeMemory(m_compositorDevice, m_copiedMenuTextureMemory, nullptr);
            m_copiedMenuTexture = VK_NULL_HANDLE;
            m_copiedMenuTextureMemory = VK_NULL_HANDLE;
            return false;
        }
        
        m_copiedTextureWidth = width;
        m_copiedTextureHeight = height;
        
        Logger::info(str::format("VRCompositor: ✅ Created copied menu texture ", width, "x", height));
    }
    
    // CRITICAL: Add memory barriers and synchronization to ensure source texture is fully written
    // We need to make sure TF2 has finished writing to the source texture before we copy from it
    // Note: We use pipeline barriers in the command buffer instead of vkQueueWaitIdle
    // to avoid blocking the compositor thread unnecessarily.
    
    // Now perform the actual texture copy using a command buffer
    // Create command buffer for the copy operation
    VkCommandBufferAllocateInfo cmdAllocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdAllocInfo.commandPool = m_compositorCommandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    
    VkCommandBuffer cmdBuffer;
    VkResult result = vkAllocateCommandBuffers(m_compositorDevice, &cmdAllocInfo, &cmdBuffer);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to allocate copy command buffer - error: ", result));
        return false;
    }
    
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkBeginCommandBuffer(cmdBuffer, &beginInfo);
    
    // CRITICAL: Ensure source texture is fully written and ready for reading
    // Add a comprehensive barrier that waits for ALL possible write operations to complete
    VkImageMemoryBarrier srcBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL; // More permissive - source could be in various states
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.image = sourceTexture;
    srcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    srcBarrier.subresourceRange.baseMipLevel = 0;
    srcBarrier.subresourceRange.levelCount = 1;
    srcBarrier.subresourceRange.baseArrayLayer = 0;
    srcBarrier.subresourceRange.layerCount = 1;
    // CRITICAL: Wait for ALL possible write operations (color attachment, shader writes, etc.)
    srcBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | 
                              VK_ACCESS_SHADER_WRITE_BIT | 
                              VK_ACCESS_TRANSFER_WRITE_BIT |
                              VK_ACCESS_MEMORY_WRITE_BIT;
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    
    // Transition destination image to transfer dst optimal
    VkImageMemoryBarrier dstBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBarrier.image = m_copiedMenuTexture;
    dstBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dstBarrier.subresourceRange.baseMipLevel = 0;
    dstBarrier.subresourceRange.levelCount = 1;
    dstBarrier.subresourceRange.baseArrayLayer = 0;
    dstBarrier.subresourceRange.layerCount = 1;
    dstBarrier.srcAccessMask = 0;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    VkImageMemoryBarrier barriers[] = { srcBarrier, dstBarrier };
    
    // CRITICAL: Wait for ALL pipeline stages that could be writing to the source texture
    vkCmdPipelineBarrier(cmdBuffer, 
                        VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, nullptr, 0, nullptr, 2, barriers);
    
    // Copy the image
    VkImageCopy copyRegion = {};
    copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.srcSubresource.mipLevel = 0;
    copyRegion.srcSubresource.baseArrayLayer = 0;
    copyRegion.srcSubresource.layerCount = 1;
    copyRegion.srcOffset = {0, 0, 0};
    copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.dstSubresource.mipLevel = 0;
    copyRegion.dstSubresource.baseArrayLayer = 0;
    copyRegion.dstSubresource.layerCount = 1;
    copyRegion.dstOffset = {0, 0, 0};
    copyRegion.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    
    vkCmdCopyImage(cmdBuffer, sourceTexture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_copiedMenuTexture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
    
    // Transition destination image to shader read only optimal for sampling
    VkImageMemoryBarrier finalBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    finalBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    finalBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    finalBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    finalBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    finalBarrier.image = m_copiedMenuTexture;
    finalBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    finalBarrier.subresourceRange.baseMipLevel = 0;
    finalBarrier.subresourceRange.levelCount = 1;
    finalBarrier.subresourceRange.baseArrayLayer = 0;
    finalBarrier.subresourceRange.layerCount = 1;
    finalBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    finalBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmdBuffer, 
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &finalBarrier);
    
    vkEndCommandBuffer(cmdBuffer);
    
    // CRITICAL: Use a fence to ensure atomic completion and prevent race conditions
    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence copyFence;
    result = vkCreateFence(m_compositorDevice, &fenceInfo, nullptr, &copyFence);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create copy fence - error: ", result));
        vkFreeCommandBuffers(m_compositorDevice, m_compositorCommandPool, 1, &cmdBuffer);
        return false;
    }
    
    // Submit the copy command with fence
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;
    
    result = vkQueueSubmit(m_compositorQueue, 1, &submitInfo, copyFence);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to submit copy command - error: ", result));
        vkDestroyFence(m_compositorDevice, copyFence, nullptr);
        vkFreeCommandBuffers(m_compositorDevice, m_compositorCommandPool, 1, &cmdBuffer);
        return false;
    }
    
    // Wait for the fence with a reasonable timeout (100ms) to avoid indefinite blocking
    // If timeout occurs, we'll skip this copy and try again next frame
    constexpr uint64_t COPY_TIMEOUT_NS = 100000000; // 100ms in nanoseconds
    result = vkWaitForFences(m_compositorDevice, 1, &copyFence, VK_TRUE, COPY_TIMEOUT_NS);
    if (result == VK_TIMEOUT) {
        Logger::warn("VRCompositor: Texture copy fence timeout - will retry next frame");
        vkDestroyFence(m_compositorDevice, copyFence, nullptr);
        vkFreeCommandBuffers(m_compositorDevice, m_compositorCommandPool, 1, &cmdBuffer);
        return false;
    } else if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to wait for copy fence - error: ", result));
    }
    
    // Clean up fence
    vkDestroyFence(m_compositorDevice, copyFence, nullptr);
    
    // Clean up command buffer
    vkFreeCommandBuffers(m_compositorDevice, m_compositorCommandPool, 1, &cmdBuffer);
    
    // Mark that we have successfully copied the menu texture
    m_menuTextureCopied = true;
    
    Logger::info("VRCompositor: ✅ VGUI texture copied successfully - will be used for rendering!");
    Logger::info(str::format("VRCompositor: 🔍 Debug - m_menuTextureCopied set to true, m_copiedMenuTextureView: ", (void*)m_copiedMenuTextureView));
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
    rasterizer.cullMode = VK_CULL_MODE_NONE;  // Disable backface culling for debugging
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    
    VkPipelineMultisampleStateCreateInfo multisampling = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    // Standard alpha blending for UI transparency
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    
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
    Logger::info("VRCompositor: Creating vertex buffer with full QuadVertex data...");
    
    // 16:9 aspect ratio quad vertices (2 triangles = 6 vertices, QuadVertex each)
    // Fixed to be COUNTER-CLOCKWISE winding for proper front-facing triangles
    // Positioned at eye level (OpenXR origin is typically at floor, so lift quad up)
    float eyeLevelOffset = 0.8f;  // Lift to typical seated eye height (~80cm)
    float halfWidth = 1.0f;       // 2m total width (16 units)
    float halfHeight = 0.5625f;   // 1.125m total height (9 units) = 16:9 ratio
    
    QuadVertex quadVertices[6] = {
        // Triangle 1 (counter-clockwise) - Flipped texture coordinates for correct orientation
        { {-halfWidth, -halfHeight - eyeLevelOffset, -1.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f} }, // Bottom-left
        { { halfWidth,  halfHeight - eyeLevelOffset, -1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f} }, // Top-right
        { { halfWidth, -halfHeight - eyeLevelOffset, -1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f} }, // Bottom-right
        
        // Triangle 2 (counter-clockwise) - Flipped texture coordinates for correct orientation
        { {-halfWidth, -halfHeight - eyeLevelOffset, -1.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f} }, // Bottom-left
        { {-halfWidth,  halfHeight - eyeLevelOffset, -1.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f} }, // Top-left
        { { halfWidth,  halfHeight - eyeLevelOffset, -1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f} }  // Top-right
    };
    
    Logger::info(str::format("VRCompositor: Quad positioned at eye level (offset: ", eyeLevelOffset, "m) with texture coordinates"));
    
    VkDeviceSize bufferSize = sizeof(quadVertices);
    
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
    memcpy(data, quadVertices, (size_t)bufferSize);
    vkUnmapMemory(m_compositorDevice, m_vertexBufferMemory);
    
    Logger::info("VRCompositor: ✅ Vertex buffer created - 6 vertices with position, texture coords, and color");
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
    
    // Only log swapchain operations every 60 frames to reduce overhead
    static int logCounter = 0;
    bool shouldLog = (logCounter++ % 60 == 0);
    
    if (shouldLog) {
        Logger::info(str::format("VRCompositor: Eye ", eye, " acquired swapchain image index ", imageIndex));
    }
    
    // Wait for image to be available with a reasonable timeout
    // Using 50ms timeout - if we can't get an image in that time, something is wrong
    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = 50000000; // 50ms in nanoseconds
    result = xrWaitSwapchainImage(m_compositorSwapchains[eye], &waitInfo);
    if (result == XR_TIMEOUT_EXPIRED) {
        Logger::warn(str::format("VRCompositor: Swapchain image wait TIMEOUT for eye ", eye, " - runtime may be overloaded"));
        // Release the acquired image since we can't use it
        XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(m_compositorSwapchains[eye], &releaseInfo);
        return false;
    } else if (XR_FAILED(result)) {
        Logger::warn(str::format("VRCompositor: Failed to wait for swapchain image for eye ", eye, " - error: ", static_cast<int>(result)));
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
    if (m_has3DQuad) {
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
    }
    
    // NOTE: Do NOT release swapchain image here!
    // The release must happen AFTER vkQueueSubmit so the GPU work is actually submitted.
    // See RenderFrame() for where release happens.
    
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
    
    // CAPTURE HUD POSITION: Update quad position from last known gameplay HUD location
    // This ensures seamless transition from gameplay to compositor
    if (UpdateQuadFromGameHUD()) {
        Logger::info("VRCompositor: ✅ Positioned compositor HUD at last known gameplay location");
    } else {
        Logger::info("VRCompositor: ⚠️ No valid gameplay HUD position found, using default positioning");
    }
    
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

void VRCompositor::CheckAndUpdateHUDPosition() {
    // Get the current HUD position data to check if it's been updated
    SharedHUDPositionData hudPos = GetCurrentHUDPosition();
    
    // Check if the data is valid and newer than what we last processed
    if (!hudPos.is_valid) {
        return; // No valid data available
    }
    
    // Track the last frame number we processed to detect updates
    static int lastProcessedFrameNumber = -1;
    
    // If this is new data (different frame number), update the vertex buffer
    if (hudPos.frame_number != lastProcessedFrameNumber) {
        if (UpdateQuadFromGameHUD()) {
            lastProcessedFrameNumber = hudPos.frame_number;
            
            // Log occasionally to show updates are working
            static int updateCount = 0;
            updateCount++;
            if (updateCount <= 3 || updateCount % 60 == 1) {
                Logger::info(str::format("VRCompositor: 🔄 Updated HUD position from game data (frame #", 
                    hudPos.frame_number, ", update #", updateCount, ")"));
            }
        }
    }
}

void VRCompositor::CompositorThreadFunc() {
    Logger::info("VRCompositor: 🧵 Compositor thread started");
    
    while (!m_shouldStop.load()) {
        // NOTE: Texture copying now happens inside RunIndependentFrame after xrBeginFrame
        // This ensures we copy the latest texture right before rendering
        
        // Check for updated HUD position data from game and update vertex buffer if needed
        CheckAndUpdateHUDPosition();
        
        if (!RunIndependentFrame()) {
            Logger::warn("VRCompositor: Independent frame failed, minimal sleep");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));  // Minimal delay for responsiveness
        }
    }
    
    Logger::info("VRCompositor: 🧵 Compositor thread ending");
}

bool VRCompositor::UpdateQuadFromGameHUD() {
    // Get the current HUD position from the shared data
    SharedHUDPositionData hudPos = GetCurrentHUDPosition();
    
    // DEBUG: Add more detailed logging to understand what's happening
    static int debugCallCount = 0;
    debugCallCount++;
    
    // Check if the data is valid and not too old (within last 2 seconds)
    auto currentTime = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    
    if (!hudPos.is_valid || (currentTime - hudPos.last_update_time) > 30.0) 
    {  
        return false;
    }
    
    // Convert Source engine coordinates to OpenXR coordinates
    // Note: You may need to apply coordinate system transformations here
    // depending on how your OpenXR coordinate system relates to Source's
    
    // Set quad vertices based on the HUD bounds from the game
    // We'll create a simple quad using the corners provided by the game
    
    // Calculate the center and size from the corners
    float centerX = (hudPos.upper_left[0] + hudPos.upper_right[0] + hudPos.lower_left[0] + hudPos.lower_right[0]) / 4.0f;
    float centerY = (hudPos.upper_left[1] + hudPos.upper_right[1] + hudPos.lower_left[1] + hudPos.lower_right[1]) / 4.0f;
    float centerZ = (hudPos.upper_left[2] + hudPos.upper_right[2] + hudPos.lower_left[2] + hudPos.lower_right[2]) / 4.0f;
    
    // Calculate width and height vectors
    float width = sqrt(pow(hudPos.upper_right[0] - hudPos.upper_left[0], 2) + 
                      pow(hudPos.upper_right[1] - hudPos.upper_left[1], 2) + 
                      pow(hudPos.upper_right[2] - hudPos.upper_left[2], 2));
    float height = sqrt(pow(hudPos.lower_left[0] - hudPos.upper_left[0], 2) + 
                       pow(hudPos.lower_left[1] - hudPos.upper_left[1], 2) + 
                       pow(hudPos.lower_left[2] - hudPos.upper_left[2], 2));
    
    // Use the actual corner positions (already converted to meters and VR coordinate system by game client)
    // NO additional scaling needed - coordinates are already in meters
    float ul_x = hudPos.upper_left[0];
    float ul_y = hudPos.upper_left[1];
    float ul_z = hudPos.upper_left[2];
    
    float ur_x = hudPos.upper_right[0];
    float ur_y = hudPos.upper_right[1];
    float ur_z = hudPos.upper_right[2];
    
    float ll_x = hudPos.lower_left[0];
    float ll_y = hudPos.lower_left[1];
    float ll_z = hudPos.lower_left[2];
    
    float lr_x = hudPos.lower_right[0];
    float lr_y = hudPos.lower_right[1];
    float lr_z = hudPos.lower_right[2];
    
    // Set vertices using the actual corner positions (preserves rotation and orientation)
    // Bottom-left (corresponds to lower-left)
    m_quadVertices[0].position[0] = ll_x;
    m_quadVertices[0].position[1] = ll_y;
    m_quadVertices[0].position[2] = ll_z;
    m_quadVertices[0].texCoord[0] = 0.0f;
    m_quadVertices[0].texCoord[1] = 0.0f;
    m_quadVertices[0].color[0] = 1.0f;
    m_quadVertices[0].color[1] = 1.0f;
    m_quadVertices[0].color[2] = 1.0f;
    
    // Bottom-right (corresponds to lower-right)
    m_quadVertices[1].position[0] = lr_x;
    m_quadVertices[1].position[1] = lr_y;
    m_quadVertices[1].position[2] = lr_z;
    m_quadVertices[1].texCoord[0] = 1.0f;
    m_quadVertices[1].texCoord[1] = 0.0f;
    m_quadVertices[1].color[0] = 1.0f;
    m_quadVertices[1].color[1] = 1.0f;
    m_quadVertices[1].color[2] = 1.0f;
    
    // Top-right (corresponds to upper-right)
    m_quadVertices[2].position[0] = ur_x;
    m_quadVertices[2].position[1] = ur_y;
    m_quadVertices[2].position[2] = ur_z;
    m_quadVertices[2].texCoord[0] = 1.0f;
    m_quadVertices[2].texCoord[1] = 1.0f;
    m_quadVertices[2].color[0] = 1.0f;
    m_quadVertices[2].color[1] = 1.0f;
    m_quadVertices[2].color[2] = 1.0f;
    
    // Top-left (corresponds to upper-left)
    m_quadVertices[3].position[0] = ul_x;
    m_quadVertices[3].position[1] = ul_y;
    m_quadVertices[3].position[2] = ul_z;
    m_quadVertices[3].texCoord[0] = 0.0f;
    m_quadVertices[3].texCoord[1] = 1.0f;
    m_quadVertices[3].color[0] = 1.0f;
    m_quadVertices[3].color[1] = 1.0f;
    m_quadVertices[3].color[2] = 1.0f;
    
    // Update the GPU vertex buffer with the new positions
    if (!UpdateVertexBuffer()) {
        Logger::warn("VRCompositor: Failed to update vertex buffer after setting new quad position");
        return false;
    }
    
    // Log the successful update
    static int updateCount = 0;
    updateCount++;
    // Log first few updates for verification
    if (updateCount <= 2) {
        Logger::info(str::format("VRCompositor: Updated quad from game HUD #", updateCount,
            " - Custom bounds: ", hudPos.is_custom_bounds ? "true" : "false"));
    }
    
    return true;
}

bool VRCompositor::UpdateVertexBuffer() {
    if (m_vertexBuffer == VK_NULL_HANDLE || m_vertexBufferMemory == VK_NULL_HANDLE) {
        Logger::warn("VRCompositor: Cannot update vertex buffer - not initialized");
        return false;
    }
    
    // Convert m_quadVertices (4 vertices) to triangle list (6 vertices) for GPU
    QuadVertex triangleVertices[6] = {
        // Triangle 1 (counter-clockwise)
        m_quadVertices[0],  // Bottom-left
        m_quadVertices[2],  // Top-right  
        m_quadVertices[1],  // Bottom-right
        
        // Triangle 2 (counter-clockwise)
        m_quadVertices[0],  // Bottom-left
        m_quadVertices[3],  // Top-left
        m_quadVertices[2]   // Top-right
    };
    
    VkDeviceSize bufferSize = sizeof(triangleVertices);
    
    // Map memory and update vertex buffer
    void* data;
    VkResult result = vkMapMemory(m_compositorDevice, m_vertexBufferMemory, 0, bufferSize, 0, &data);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to map vertex buffer memory - error: ", static_cast<int>(result)));
        return false;
    }
    
    memcpy(data, triangleVertices, (size_t)bufferSize);
    vkUnmapMemory(m_compositorDevice, m_vertexBufferMemory);
    
    Logger::info("VRCompositor: ✅ Vertex buffer updated with new quad position");
    return true;
}

bool VRCompositor::InitializeQuadWithPlayspaceCoordinates() {
    Logger::info("VRCompositor: Initializing HUD quad with base playspace coordinates...");
    
    // Try to get position from game first (the game should have set up startup coordinates)
    if (UpdateQuadFromGameHUD()) {
        Logger::info("VRCompositor: ✅ HUD quad initialized using game-provided position data");
        return true;
    }
    
    Logger::warn("VRCompositor: No game HUD data available, using manual fallback setup");
    
    // Fallback - manual setup if no game data is available
    // Use the same coordinate system and approach as the in-world positioning,
    // but with default values suitable for menu/startup scenarios
    
    // Position the HUD quad in a comfortable viewing position
    // These coordinates are in OpenXR playspace coordinates (meters)
    // OpenXR typically has Y=0 at floor level, Z negative going forward
    
    // Default HUD positioning: 2.2m in front, at eye level, centered
    float baseDistance = -2.2f;      // 2.2m in front of user (negative Z in OpenXR)
    float eyeLevelHeight = 1.6f;     // 1.6m above floor (typical standing eye height)
    float centerX = 0.0f;            // Centered horizontally
    
    // HUD dimensions: 16:9 aspect ratio, comfortable viewing size
    float hudWidth = 1.8f;           // 1.8m wide (slightly smaller than the 2m fallback)
    float hudHeight = 1.0125f;       // 1.0125m tall (maintains 16:9 ratio)
    float halfWidth = hudWidth * 0.5f;
    float halfHeight = hudHeight * 0.5f;
    
    // Calculate corner positions in playspace coordinates
    // These match the same pattern as UpdateQuadFromGameHUD() uses
    
    // Lower-left corner
    float ll_x = centerX - halfWidth;
    float ll_y = eyeLevelHeight - halfHeight;  
    float ll_z = baseDistance;
    
    // Lower-right corner  
    float lr_x = centerX + halfWidth;
    float lr_y = eyeLevelHeight - halfHeight;
    float lr_z = baseDistance;
    
    // Upper-right corner
    float ur_x = centerX + halfWidth;
    float ur_y = eyeLevelHeight + halfHeight;
    float ur_z = baseDistance;
    
    // Upper-left corner
    float ul_x = centerX - halfWidth;
    float ul_y = eyeLevelHeight + halfHeight;
    float ul_z = baseDistance;
    
    // Set vertices using the same mapping as UpdateQuadFromGameHUD()
    // This ensures consistency between startup and game-to-menu transitions
    
    // Bottom-left (corresponds to lower-left)
    m_quadVertices[0].position[0] = ll_x;
    m_quadVertices[0].position[1] = ll_y;
    m_quadVertices[0].position[2] = ll_z;
    m_quadVertices[0].texCoord[0] = 0.0f;
    m_quadVertices[0].texCoord[1] = 0.0f;
    m_quadVertices[0].color[0] = 1.0f;
    m_quadVertices[0].color[1] = 1.0f;
    m_quadVertices[0].color[2] = 1.0f;
    
    // Bottom-right (corresponds to lower-right)
    m_quadVertices[1].position[0] = lr_x;
    m_quadVertices[1].position[1] = lr_y;
    m_quadVertices[1].position[2] = lr_z;
    m_quadVertices[1].texCoord[0] = 1.0f;
    m_quadVertices[1].texCoord[1] = 0.0f;
    m_quadVertices[1].color[0] = 1.0f;
    m_quadVertices[1].color[1] = 1.0f;
    m_quadVertices[1].color[2] = 1.0f;
    
    // Top-right (corresponds to upper-right)
    m_quadVertices[2].position[0] = ur_x;
    m_quadVertices[2].position[1] = ur_y;
    m_quadVertices[2].position[2] = ur_z;
    m_quadVertices[2].texCoord[0] = 1.0f;
    m_quadVertices[2].texCoord[1] = 1.0f;
    m_quadVertices[2].color[0] = 1.0f;
    m_quadVertices[2].color[1] = 1.0f;
    m_quadVertices[2].color[2] = 1.0f;
    
    // Top-left (corresponds to upper-left)
    m_quadVertices[3].position[0] = ul_x;
    m_quadVertices[3].position[1] = ul_y;
    m_quadVertices[3].position[2] = ul_z;
    m_quadVertices[3].texCoord[0] = 0.0f;
    m_quadVertices[3].texCoord[1] = 1.0f;
    m_quadVertices[3].color[0] = 1.0f;
    m_quadVertices[3].color[1] = 1.0f;
    m_quadVertices[3].color[2] = 1.0f;
    
    // Update the GPU vertex buffer with the new positions
    if (!UpdateVertexBuffer()) {
        Logger::warn("VRCompositor: Failed to update vertex buffer during playspace initialization");
        return false;
    }
    
    Logger::info(str::format("VRCompositor: ✅ HUD quad initialized with playspace coordinates - ",
        hudWidth, "x", hudHeight, "m at (", centerX, ", ", eyeLevelHeight, ", ", baseDistance, ")"));
    
    return true;
}

void VRCompositor::SetupQuadVertices() {
    // Try to get position from game first
    Logger::info("VRCompositor: SetupQuadVertices - attempting to get game HUD data...");
    if (UpdateQuadFromGameHUD()) {
        // Successfully updated from game data, we're done
        Logger::info("VRCompositor: ✅ Using game HUD position data");
        return;
    }
    
    // Initialize using proper playspace coordinates instead of simple fallback
    Logger::info("VRCompositor: No game HUD data available, initializing with playspace coordinates...");
    if (InitializeQuadWithPlayspaceCoordinates()) {
        Logger::info("VRCompositor: ✅ HUD quad initialized using base playspace coordinates");
        return;
    }
    
    // Final fallback to the original simple positioning if playspace init fails
    Logger::warn("VRCompositor: ⚠️ Playspace initialization failed, using basic fallback position");
    
    // Create a quad positioned 2.5 meters in front of the user (original fallback)
    // Size: 2m wide x 1.125m tall (16:9 aspect ratio)
    float halfWidth = 1.0f;   // 2m wide (16 units)
    float halfHeight = 0.5625f; // 1.125m tall (9 units) = 16:9 ratio
    float distance = -2.5f;   // 2.5m in front (negative Z)
    
    // Bottom-left
    m_quadVertices[0].position[0] = -halfWidth;
    m_quadVertices[0].position[1] = -halfHeight;
    m_quadVertices[0].position[2] = distance;
    m_quadVertices[0].texCoord[0] = 0.0f;
    m_quadVertices[0].texCoord[1] = 0.0f;
    m_quadVertices[0].color[0] = 1.0f;
    m_quadVertices[0].color[1] = 1.0f;
    m_quadVertices[0].color[2] = 1.0f;
    
    // Bottom-right
    m_quadVertices[1].position[0] = halfWidth;
    m_quadVertices[1].position[1] = -halfHeight;
    m_quadVertices[1].position[2] = distance;
    m_quadVertices[1].texCoord[0] = 1.0f;
    m_quadVertices[1].texCoord[1] = 0.0f;
    m_quadVertices[1].color[0] = 1.0f;
    m_quadVertices[1].color[1] = 1.0f;
    m_quadVertices[1].color[2] = 1.0f;
    
    // Top-right
    m_quadVertices[2].position[0] = halfWidth;
    m_quadVertices[2].position[1] = halfHeight;
    m_quadVertices[2].position[2] = distance;
    m_quadVertices[2].texCoord[0] = 1.0f;
    m_quadVertices[2].texCoord[1] = 1.0f;
    m_quadVertices[2].color[0] = 1.0f;
    m_quadVertices[2].color[1] = 1.0f;
    m_quadVertices[2].color[2] = 1.0f;
    
    // Top-left
    m_quadVertices[3].position[0] = -halfWidth;
    m_quadVertices[3].position[1] = halfHeight;
    m_quadVertices[3].position[2] = distance;
    m_quadVertices[3].texCoord[0] = 0.0f;
    m_quadVertices[3].texCoord[1] = 1.0f;
    m_quadVertices[3].color[0] = 1.0f;
    m_quadVertices[3].color[1] = 1.0f;
    m_quadVertices[3].color[2] = 1.0f;
    
    // Update the GPU vertex buffer with the default positions
    UpdateVertexBuffer();
    
    Logger::info("VRCompositor: ✅ Quad vertices set up - basic fallback 2x1.125m quad (16:9) at 2.5m distance");
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
    
    // SIMPLE APPROACH: No complex synchronization - let systems run independently
    
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
    
    // NOTE: GPU fence wait now happens in RenderFrame() after vkQueueSubmit
    // This ensures GPU work is complete before swapchain release, which is the
    // correct synchronization point for OpenXR.
    
    // Track frame-to-frame timing (for debugging pacing issues)
    static auto lastFrameTime = std::chrono::high_resolution_clock::now();
    auto frameStartTime = std::chrono::high_resolution_clock::now();
    lastFrameTime = frameStartTime;
    
    // Wait for the next frame
    XrFrameWaitInfo frameWaitInfo = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState frameState = { XR_TYPE_FRAME_STATE };
    
    XrResult result = xrWaitFrame(m_cachedSession, &frameWaitInfo, &frameState);
    if (XR_FAILED(result)) {
        Logger::warn(str::format("VRCompositor: Independent xrWaitFrame failed - error: ", static_cast<int>(result)));
        return false;
    }
    
    auto afterWaitFrame = std::chrono::high_resolution_clock::now();
    
    // CRITICAL: Store frame state for input synchronization
    // This ensures controller poses use the correct predicted display time
    {
        std::lock_guard<std::mutex> lock(m_frameStateMutex);
        m_currentFrameState = frameState;
    }
    
    // No blocking here - TF2 should already be blocked from previous frame
    
    // Begin the frame
    XrFrameBeginInfo frameBeginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
    result = xrBeginFrame(m_cachedSession, &frameBeginInfo);
    if (XR_FAILED(result)) {
        Logger::warn(str::format("VRCompositor: Independent xrBeginFrame failed - error: ", static_cast<int>(result)));
        return false;
    }
    
    auto afterBeginFrame = std::chrono::high_resolution_clock::now();
    
    // Track if TF2 has a new frame ready before copying
    // g_tf2CompletedFrameId and g_lastCopiedFrameId are defined in OpenXRDirectMode.cpp
    uint64_t tf2Frame = ::g_tf2CompletedFrameId.load(std::memory_order_acquire);
    uint64_t lastCopied = ::g_lastCopiedFrameId.load(std::memory_order_relaxed);
    bool hasNewFrame = (tf2Frame > lastCopied);
    
    // Track frame consistency (only log significant gaps)
    static int framesWithoutNew = 0;
    if (hasNewFrame) {
        framesWithoutNew = 0;
    } else {
        framesWithoutNew++;
        // Only log if gap is significant (TF2 noticeably behind VR refresh)
        if (framesWithoutNew == 5) {
            Logger::info(str::format("VRCompositor: TF2 frame gap detected (tf2=", tf2Frame, ")"));
        }
    }
    
    // Copy texture from TF2 BEFORE unblocking - use what TF2 produced in its last frame
    // IMPORTANT: Must copy BEFORE unblocking to prevent TF2 from modifying the texture
    extern void CheckAndCopyTrackedVGUITexture();
    ::CheckAndCopyTrackedVGUITexture();
    
    auto afterTextureCopy = std::chrono::high_resolution_clock::now();
    
    // NOW unblock TF2 - texture has been safely copied
    // TF2 can prepare its next frame while we render with the copied texture
    {
        std::lock_guard<std::mutex> lock(m_tf2FrameSignalMutex);
        m_tf2CanRenderFrame = true;
    }
    m_tf2FrameCondition.notify_all();
    
    // Use RenderFrame instead of calling RenderEye directly
    std::vector<XrCompositionLayerBaseHeader*> layers;
    bool rendered = RenderFrame(frameState, layers);
    
    auto afterRender = std::chrono::high_resolution_clock::now();
    
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
    
    auto afterEndFrame = std::chrono::high_resolution_clock::now();
    
    // Calculate timing deltas in milliseconds
    auto waitToBegin = std::chrono::duration<double, std::milli>(afterBeginFrame - afterWaitFrame).count();
    auto beginToTexCopy = std::chrono::duration<double, std::milli>(afterTextureCopy - afterBeginFrame).count();
    auto texCopyToRender = std::chrono::duration<double, std::milli>(afterRender - afterTextureCopy).count();
    auto renderToEnd = std::chrono::duration<double, std::milli>(afterEndFrame - afterRender).count();
    auto totalFrameTime = std::chrono::duration<double, std::milli>(afterEndFrame - afterWaitFrame).count();
    
    // Log timing every 60 frames or if frame took too long (>12ms = missed 90Hz)
    static int timingLogCounter = 0;
    timingLogCounter++;
    if (timingLogCounter % 60 == 0 || totalFrameTime > 12.0) {
        Logger::info(str::format("VRCompositor: ⏱️ Frame timing (ms): BeginFrame=", 
            waitToBegin, ", TexCopy=", beginToTexCopy, 
            ", Render=", texCopyToRender, ", EndFrame=", renderToEnd, 
            ", TOTAL=", totalFrameTime, (totalFrameTime > 12.0 ? " ⚠️SLOW" : "")));
    }
    
    return true;
}

bool VRCompositor::CreateSimple3DQuad() {
    Logger::info("VRCompositor: Creating simple 3D quad with basic pipeline...");
    
    // Set up 16:9 aspect ratio quad vertices (2m wide x 1.125m tall, 2.5m away)
    float halfWidth = 1.0f;        // 2m total width (16 units)
    float halfHeight = 0.5625f;    // 1.125m total height (9 units) = 16:9 ratio
    m_quadVertices[0] = {{-halfWidth, -halfHeight, -2.5f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}}; // Bottom-left
    m_quadVertices[1] = {{ halfWidth, -halfHeight, -2.5f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}}; // Bottom-right  
    m_quadVertices[2] = {{ halfWidth,  halfHeight, -2.5f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}}; // Top-right
    m_quadVertices[3] = {{-halfWidth,  halfHeight, -2.5f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}}; // Top-left
    
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
    clearColor.color = { { 0.0f, 0.0f, 0.02094f, 1.0f } };
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
    
    // Push MVP matrix and shader parameters as push constants
    struct PushConstants {
        float mvpMatrix[16];     // 64 bytes
        float hasAlphaMask;      // 4 bytes
        float fullOpacity;       // 4 bytes
        float padding1;          // 4 bytes
        float padding2;          // 4 bytes
    } pushConstants;
    
    // Copy MVP matrix
    memcpy(pushConstants.mvpMatrix, mvpMatrix, sizeof(float) * 16);
    
    // Set shader parameters
    pushConstants.hasAlphaMask = m_maskTextureLoaded ? 1.0f : 0.0f;
    pushConstants.fullOpacity = 0.0f; // Use normal blending mode (not full opacity cutout)
    pushConstants.padding1 = 0.0f;
    pushConstants.padding2 = 0.0f;
    
    vkCmdPushConstants(m_compositorCommandBuffer, m_pipelineLayout, 
                      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 
                      0, sizeof(pushConstants), &pushConstants);
    
    // AGGRESSIVE ANTI-FLICKER: Once we get ANY VGUI texture, never go back to fallback
    static VkImageView persistentVGUITexture = VK_NULL_HANDLE;
    static bool everHadVGUI = false;
    static int debugFrameCount = 0;
    debugFrameCount++;
    
    VkImageView textureToUse = m_fallbackTextureView;
    
    // Debug current state every frame for a few seconds to understand the flickering
    if (debugFrameCount < 300) { // Debug for first 5 seconds at 60fps
        if (debugFrameCount % 30 == 1) { // Every 0.5 seconds
            Logger::info(str::format("VRCompositor: DEBUG Eye ", eye, " frame ", debugFrameCount, 
                                    " - m_copiedMenuTextureView: ", (void*)m_copiedMenuTextureView, 
                                    " m_menuTextureCopied: ", m_menuTextureCopied ? "true" : "false",
                                    " persistentVGUITexture: ", (void*)persistentVGUITexture));
        }
    }
    
    // Check if we have a current VGUI texture available
    bool hasCurrentVGUITexture = (m_copiedMenuTextureView != VK_NULL_HANDLE && m_menuTextureCopied);
    
    if (hasCurrentVGUITexture) {
        // Use current VGUI texture and cache it persistently
        textureToUse = m_copiedMenuTextureView;
        persistentVGUITexture = m_copiedMenuTextureView;  // Cache it
        everHadVGUI = true;
        
        static int currentFrameCount = 0;
        if ((++currentFrameCount % 300) == 1) {
            Logger::info(str::format("VRCompositor: Eye ", eye, " - Using CURRENT VGUI texture (frame #", currentFrameCount, ")"));
        }
    } else if (everHadVGUI && persistentVGUITexture != VK_NULL_HANDLE) {
        // CRITICAL: Always use cached VGUI texture once we've had one (prevents flicker)
        textureToUse = persistentVGUITexture;
        
        static int persistentFrameCount = 0;
        if ((++persistentFrameCount % 120) == 1) {
            Logger::info(str::format("VRCompositor: Eye ", eye, " - Using PERSISTENT VGUI texture to prevent flicker (frame #", persistentFrameCount, ")"));
        }
    } else {
        // Don't render the quad until we receive the first texture update
        static int waitingFrameCount = 0;
        if ((++waitingFrameCount % 60) == 1) {
            Logger::info(str::format("VRCompositor: Eye ", eye, " - WAITING for first VGUI texture, skipping quad render (frame #", waitingFrameCount, ")"));
        }
        // Early return - don't render the quad
        return;
    }
    
    // Update descriptor set for this frame (main texture + mask texture)
    UpdateDescriptorSetWithTextures(textureToUse, m_maskTextureLoaded ? m_maskTextureView : m_fallbackTextureView);
    
    // Bind descriptor set for texture sampling
    vkCmdBindDescriptorSets(m_compositorCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, 
                           m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);
    
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
    
    // Create texture sampler
    if (!CreateTextureSampler()) {
        Logger::err("VRCompositor: Failed to create texture sampler");
        return false;
    }
    
    // Create descriptor resources for texture binding
    if (!CreateDescriptorResources()) {
        Logger::err("VRCompositor: Failed to create descriptor resources");
        return false;
    }
    
    // Create fallback texture if not already created
    if (!CreateSimpleFallbackTexture()) {
        Logger::err("VRCompositor: Failed to create fallback texture");
        return false;
    }
    
    // Load the rounded corner mask texture - try multiple possible paths
    const char* maskPaths[] = {
        "tfvr/materials/vgui/rounded_corner_mask.png",
    };
    
    bool maskLoaded = false;
    for (const char* path : maskPaths) {
        if (LoadMaskTexture(path)) {
            maskLoaded = true;
            break;
        }
    }
    
    if (!maskLoaded) {
        Logger::warn("VRCompositor: Failed to load mask texture from any path, creating fallback white mask");
        
        // Create a fallback white mask texture (no masking effect)
        const int fallbackWidth = 256;
        const int fallbackHeight = 256;
        const size_t fallbackDataSize = fallbackWidth * fallbackHeight * 4;
        std::vector<uint8_t> whiteTextureData(fallbackDataSize, 255);
        
        if (CreateMaskTextureFromPNG(whiteTextureData.data(), fallbackWidth, fallbackHeight)) {
            m_maskTextureLoaded = true;
            Logger::info("VRCompositor: ✅ Fallback white mask texture created (rounded corners disabled)");
        } else {
            Logger::err("VRCompositor: Failed to create fallback mask texture");
            m_maskTextureLoaded = false;
        }
    }
    
    // Create pipeline layout with push constants for MVP matrix + shader params
    VkPushConstantRange pushConstantRange = {};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(float) * 20; // 4x4 matrix (64 bytes) + 4 shader params (16 bytes) = 80 bytes
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
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
    
    // Vertex input - position, texture coordinates, and color
    VkVertexInputBindingDescription bindingDescription = {};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(QuadVertex); // Full QuadVertex structure
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    
    VkVertexInputAttributeDescription attributeDescriptions[3] = {};
    
    // Position (location = 0)
    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(QuadVertex, position);
    
    // Texture coordinates (location = 1)
    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(QuadVertex, texCoord);
    
    // Color (location = 2)
    attributeDescriptions[2].binding = 0;
    attributeDescriptions[2].location = 2;
    attributeDescriptions[2].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[2].offset = offsetof(QuadVertex, color);
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = 3;
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions;
    
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
    rasterizer.cullMode = VK_CULL_MODE_NONE;  // Disable backface culling for debugging
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    
    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    // Color blending - Enable alpha blending for proper UI transparency
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    // Standard alpha blending: (srcColor * srcAlpha) + (dstColor * (1 - srcAlpha))
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    
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

bool VRCompositor::CreateTextureSampler() {
    Logger::info("VRCompositor: Creating texture sampler...");
    
    VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    
    VkResult result = vkCreateSampler(m_compositorDevice, &samplerInfo, nullptr, &m_textureSampler);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create texture sampler - error: ", result));
        return false;
    }
    
    Logger::info("VRCompositor: ✅ Texture sampler created successfully");
    return true;
}

bool VRCompositor::CreateDescriptorResources() {
    Logger::info("VRCompositor: Creating descriptor resources...");
    
    // Create descriptor set layout for both textures (main texture + mask texture)
    VkDescriptorSetLayoutBinding bindings[2] = {};
    
    // Binding 0: Main VGUI texture sampler
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[0].pImmutableSamplers = nullptr;
    
    // Binding 1: Alpha mask texture sampler  
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].pImmutableSamplers = nullptr;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    
    VkResult result = vkCreateDescriptorSetLayout(m_compositorDevice, &layoutInfo, nullptr, &m_descriptorSetLayout);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create descriptor set layout - error: ", result));
        return false;
    }
    
    // Create descriptor pool for both texture samplers
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 2; // Now we need 2 samplers
    
    VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;
    
    result = vkCreateDescriptorPool(m_compositorDevice, &poolInfo, nullptr, &m_descriptorPool);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create descriptor pool - error: ", result));
        return false;
    }
    
    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;
    
    result = vkAllocateDescriptorSets(m_compositorDevice, &allocInfo, &m_descriptorSet);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to allocate descriptor set - error: ", result));
        return false;
    }
    
    Logger::info("VRCompositor: ✅ Descriptor resources created successfully");
    return true;
}

bool VRCompositor::CreateSimpleFallbackTexture() {
    Logger::info("VRCompositor: Creating simple fallback texture...");
    
    // Create a simple 2x2 bright green texture as fallback (visible on magenta background)
    const uint32_t texWidth = 2;
    const uint32_t texHeight = 2;
    const uint32_t texChannels = 4; // RGBA
    
    // Bright green texture data (2x2 pixels, RGBA format) - contrasts with magenta background
    uint8_t textureData[] = {
        0, 255, 0, 255,    // Bright green
        0, 255, 0, 255,    // Bright green
        0, 255, 0, 255,    // Bright green
        0, 255, 0, 255     // Bright green
    };
    
    VkDeviceSize imageSize = texWidth * texHeight * texChannels;
    
    // Create staging buffer first to upload texture data
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    
    VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkResult result = vkCreateBuffer(m_compositorDevice, &bufferInfo, nullptr, &stagingBuffer);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create staging buffer - error: ", result));
        return false;
    }
    
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_compositorDevice, stagingBuffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    result = vkAllocateMemory(m_compositorDevice, &allocInfo, nullptr, &stagingBufferMemory);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to allocate staging buffer memory - error: ", result));
        vkDestroyBuffer(m_compositorDevice, stagingBuffer, nullptr);
        return false;
    }
    
    vkBindBufferMemory(m_compositorDevice, stagingBuffer, stagingBufferMemory, 0);
    
    // Upload texture data to staging buffer
    void* data;
    vkMapMemory(m_compositorDevice, stagingBufferMemory, 0, imageSize, 0, &data);
    memcpy(data, textureData, static_cast<size_t>(imageSize));
    vkUnmapMemory(m_compositorDevice, stagingBufferMemory);
    
    // Create image
    VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = texWidth;
    imageInfo.extent.height = texHeight;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    result = vkCreateImage(m_compositorDevice, &imageInfo, nullptr, &m_fallbackTexture);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create fallback texture image - error: ", result));
        vkDestroyBuffer(m_compositorDevice, stagingBuffer, nullptr);
        vkFreeMemory(m_compositorDevice, stagingBufferMemory, nullptr);
        return false;
    }
    
    // Allocate image memory
    vkGetImageMemoryRequirements(m_compositorDevice, m_fallbackTexture, &memRequirements);
    
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    result = vkAllocateMemory(m_compositorDevice, &allocInfo, nullptr, &m_fallbackTextureMemory);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to allocate fallback texture memory - error: ", result));
        vkDestroyImage(m_compositorDevice, m_fallbackTexture, nullptr);
        vkDestroyBuffer(m_compositorDevice, stagingBuffer, nullptr);
        vkFreeMemory(m_compositorDevice, stagingBufferMemory, nullptr);
        return false;
    }
    
    vkBindImageMemory(m_compositorDevice, m_fallbackTexture, m_fallbackTextureMemory, 0);
    
    // Create command buffer for image layout transitions and buffer copy
    VkCommandBufferAllocateInfo cmdAllocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdAllocInfo.commandPool = m_compositorCommandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    
    VkCommandBuffer cmdBuffer;
    result = vkAllocateCommandBuffers(m_compositorDevice, &cmdAllocInfo, &cmdBuffer);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to allocate command buffer - error: ", result));
        vkDestroyImage(m_compositorDevice, m_fallbackTexture, nullptr);
        vkFreeMemory(m_compositorDevice, m_fallbackTextureMemory, nullptr);
        vkDestroyBuffer(m_compositorDevice, stagingBuffer, nullptr);
        vkFreeMemory(m_compositorDevice, stagingBufferMemory, nullptr);
        return false;
    }
    
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkBeginCommandBuffer(cmdBuffer, &beginInfo);
    
    // Transition image layout from undefined to transfer dst optimal
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_fallbackTexture;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    // Copy buffer to image
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {texWidth, texHeight, 1};
    
    vkCmdCopyBufferToImage(cmdBuffer, stagingBuffer, m_fallbackTexture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    
    // Transition image layout from transfer dst optimal to shader read only optimal
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    vkEndCommandBuffer(cmdBuffer);
    
    // Submit command buffer
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;
    
    result = vkQueueSubmit(m_compositorQueue, 1, &submitInfo, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to submit command buffer - error: ", result));
    }
    
    vkQueueWaitIdle(m_compositorQueue);
    
    // Clean up staging resources
    vkFreeCommandBuffers(m_compositorDevice, m_compositorCommandPool, 1, &cmdBuffer);
    vkDestroyBuffer(m_compositorDevice, stagingBuffer, nullptr);
    vkFreeMemory(m_compositorDevice, stagingBufferMemory, nullptr);
    
    // Create image view
    VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = m_fallbackTexture;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    
    result = vkCreateImageView(m_compositorDevice, &viewInfo, nullptr, &m_fallbackTextureView);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create fallback texture view - error: ", result));
        return false;
    }
    
    // Update descriptor set to use the fallback texture initially for both bindings
    UpdateDescriptorSetWithTextures(m_fallbackTextureView, m_fallbackTextureView);
    
    Logger::info("VRCompositor: ✅ Bright green fallback texture created with proper data upload and descriptor set updated");
    return true;
}

void VRCompositor::UpdateDescriptorSetWithTexture(VkImageView textureView) {
    if (textureView == VK_NULL_HANDLE) {
        Logger::warn("VRCompositor: Cannot update descriptor set with null texture view");
        return;
    }
    
    // Update descriptor set to use the specified texture
    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = textureView;
    imageInfo.sampler = m_textureSampler;
    
    VkWriteDescriptorSet descriptorWrite = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    descriptorWrite.dstSet = m_descriptorSet;
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageInfo;
    
    vkUpdateDescriptorSets(m_compositorDevice, 1, &descriptorWrite, 0, nullptr);
    
    Logger::info("VRCompositor: ✅ Descriptor set updated with new texture");
}

void VRCompositor::UpdateDescriptorSetWithTextures(VkImageView mainTextureView, VkImageView maskTextureView) {
    if (mainTextureView == VK_NULL_HANDLE || maskTextureView == VK_NULL_HANDLE) {
        Logger::warn("VRCompositor: Cannot update descriptor set with null texture views");
        return;
    }
    
    // Update descriptor set to use both textures
    VkDescriptorImageInfo imageInfos[2] = {};
    
    // Binding 0: Main VGUI texture
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[0].imageView = mainTextureView;
    imageInfos[0].sampler = m_textureSampler;
    
    // Binding 1: Mask texture
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[1].imageView = maskTextureView;
    imageInfos[1].sampler = m_maskTextureLoaded ? m_maskTextureSampler : m_textureSampler;
    
    VkWriteDescriptorSet descriptorWrites[2] = {};
    
    // Write for main texture (binding 0)
    descriptorWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    descriptorWrites[0].dstSet = m_descriptorSet;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pImageInfo = &imageInfos[0];
    
    // Write for mask texture (binding 1)
    descriptorWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    descriptorWrites[1].dstSet = m_descriptorSet;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &imageInfos[1];
    
    vkUpdateDescriptorSets(m_compositorDevice, 2, descriptorWrites, 0, nullptr);
    
    Logger::info("VRCompositor: ✅ Descriptor set updated with main and mask textures");
}

void VRCompositor::NotifyTF2FrameComplete() {
    // This is called when TF2 finishes a frame and is blocked in PostPresentCallback
    // The VR compositor will unblock TF2 after completing its next frame
    
    Logger::info("VRCompositor: 📥 TF2 frame complete notification - will be unblocked after next VR frame");
    
    // No action needed here - TF2 will be unblocked after the next VR frame completes
}

XrTime VRCompositor::GetCurrentPredictedDisplayTime() const {
    std::lock_guard<std::mutex> lock(m_frameStateMutex);
    return m_currentFrameState.predictedDisplayTime;
}

bool VRCompositor::LoadMaskTexture(const char* pngPath) {
    Logger::info(str::format("VRCompositor: Loading mask texture from: ", pngPath));
    
    // Load PNG using stb_image
    int width, height, channels;
    unsigned char* imageData = stbi_load(pngPath, &width, &height, &channels, STBI_rgb_alpha);
    
    if (!imageData) {
        Logger::info(str::format("VRCompositor: Could not load PNG from: ", pngPath, " (", stbi_failure_reason(), ")"));
        return false; // Don't create fallback here, let the caller handle it
    }
    
    Logger::info(str::format("VRCompositor: PNG loaded successfully - dimensions: ", width, "x", height, " channels: ", channels));
    
    // Create texture from loaded PNG data
    bool success = CreateMaskTextureFromPNG(imageData, width, height);
    
    // Free the loaded image data
    stbi_image_free(imageData);
    
    if (success) {
        m_maskTextureLoaded = true;
        Logger::info("VRCompositor: ✅ Mask texture loaded and created successfully - rounded corners enabled!");
    }
    
    return success;
}

bool VRCompositor::CreateMaskTextureFromPNG(const unsigned char* pngData, int width, int height) {
    Logger::info(str::format("VRCompositor: Creating mask texture from PNG data - size: ", width, "x", height));
    
    VkDeviceSize imageSize = width * height * 4; // RGBA
    
    // Create staging buffer first to upload texture data
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    
    VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkResult result = vkCreateBuffer(m_compositorDevice, &bufferInfo, nullptr, &stagingBuffer);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create staging buffer for mask texture - error: ", result));
        return false;
    }
    
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_compositorDevice, stagingBuffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    result = vkAllocateMemory(m_compositorDevice, &allocInfo, nullptr, &stagingBufferMemory);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to allocate staging buffer memory for mask texture - error: ", result));
        vkDestroyBuffer(m_compositorDevice, stagingBuffer, nullptr);
        return false;
    }
    
    vkBindBufferMemory(m_compositorDevice, stagingBuffer, stagingBufferMemory, 0);
    
    // Upload PNG data to staging buffer
    void* data;
    vkMapMemory(m_compositorDevice, stagingBufferMemory, 0, imageSize, 0, &data);
    memcpy(data, pngData, static_cast<size_t>(imageSize));
    vkUnmapMemory(m_compositorDevice, stagingBufferMemory);
    
    // Create image
    VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    result = vkCreateImage(m_compositorDevice, &imageInfo, nullptr, &m_maskTexture);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create mask texture image - error: ", result));
        vkDestroyBuffer(m_compositorDevice, stagingBuffer, nullptr);
        vkFreeMemory(m_compositorDevice, stagingBufferMemory, nullptr);
        return false;
    }
    
    // Allocate memory for image
    VkMemoryRequirements imgMemRequirements;
    vkGetImageMemoryRequirements(m_compositorDevice, m_maskTexture, &imgMemRequirements);
    
    VkMemoryAllocateInfo imgAllocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imgAllocInfo.allocationSize = imgMemRequirements.size;
    imgAllocInfo.memoryTypeIndex = FindMemoryType(imgMemRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    result = vkAllocateMemory(m_compositorDevice, &imgAllocInfo, nullptr, &m_maskTextureMemory);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to allocate mask texture memory - error: ", result));
        vkDestroyImage(m_compositorDevice, m_maskTexture, nullptr);
        vkDestroyBuffer(m_compositorDevice, stagingBuffer, nullptr);
        vkFreeMemory(m_compositorDevice, stagingBufferMemory, nullptr);
        return false;
    }
    
    vkBindImageMemory(m_compositorDevice, m_maskTexture, m_maskTextureMemory, 0);
    
    // Copy staging buffer to image (need command buffer for this)
    VkCommandBufferAllocateInfo cmdAllocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandPool = m_compositorCommandPool;
    cmdAllocInfo.commandBufferCount = 1;
    
    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(m_compositorDevice, &cmdAllocInfo, &commandBuffer);
    
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    
    // Transition image layout to transfer destination
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_maskTexture;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    // Copy buffer to image
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, m_maskTexture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    
    // Transition image layout to shader read-only
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    vkEndCommandBuffer(commandBuffer);
    
    // Submit command buffer
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    vkQueueSubmit(m_compositorQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_compositorQueue);
    
    // Cleanup staging resources
    vkFreeCommandBuffers(m_compositorDevice, m_compositorCommandPool, 1, &commandBuffer);
    vkDestroyBuffer(m_compositorDevice, stagingBuffer, nullptr);
    vkFreeMemory(m_compositorDevice, stagingBufferMemory, nullptr);
    
    // Create image view
    VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = m_maskTexture;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    
    result = vkCreateImageView(m_compositorDevice, &viewInfo, nullptr, &m_maskTextureView);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create mask texture view - error: ", result));
        return false;
    }
    
    // Create sampler for mask texture
    VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    
    result = vkCreateSampler(m_compositorDevice, &samplerInfo, nullptr, &m_maskTextureSampler);
    if (result != VK_SUCCESS) {
        Logger::err(str::format("VRCompositor: Failed to create mask texture sampler - error: ", result));
        return false;
    }
    
    Logger::info("VRCompositor: ✅ Mask texture created successfully");
    return true;
}

} // namespace dxvk
