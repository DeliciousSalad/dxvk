#ifndef VRCOMPOSITOR_H_INCLUDED
#define VRCOMPOSITOR_H_INCLUDED

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <chrono>
#include <memory>

// Must define Win32 platform before including Vulkan headers
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include "vulkan/vulkan.h"
#include "openxr/openxr.h"
#define XR_USE_GRAPHICS_API_VULKAN
#include "openxr/openxr_platform.h"

#include "VRControllerModel.h"

// Forward declarations
class OpenXRDirectMode;

// State tracking for Source Engine (must match hmdWrapper.h)
enum SourceEngineState {
    SOURCE_STATE_GAMEPLAY = 0,     // Normal game - Source handles VR
    SOURCE_STATE_MENU = 1,         // Main menu - compositor takes over
    SOURCE_STATE_LOADING = 2,      // Loading screen - compositor takes over
    SOURCE_STATE_TRANSITION = 3    // Brief transitions
};

namespace dxvk {

//
// VRCompositor: Independent VR rendering compositor for VGUI overlays
//
// This class provides a separate Vulkan device and rendering pipeline specifically
// for rendering VGUI menu textures as floating 3D quads in VR space. It operates
// independently from the main Source engine rendering pipeline to avoid conflicts
// and provide smooth VR menu experiences during loading screens and when the
// normal rendering pipeline is not active or performing poorly.
//
class VRCompositor {
private:
    std::atomic<SourceEngineState> m_currentState{SOURCE_STATE_GAMEPLAY};
    std::atomic<bool> m_compositorActive{false};
    std::thread m_compositorThread;
    std::mutex m_frameMutex;
    std::atomic<bool> m_shouldStop{false};
    
    // OpenXR session access (shared with main DXVK)
    ::OpenXRDirectMode* m_pOpenXRManager = nullptr;
    
    // Cached values to avoid calling OpenXRDirectMode methods (avoid circular dependency)
    VkPhysicalDevice m_cachedPhysicalDevice = VK_NULL_HANDLE;
    VkInstance m_cachedInstance = VK_NULL_HANDLE;
    XrSession m_cachedSession = XR_NULL_HANDLE;
    XrSpace m_cachedReferenceSpace = XR_NULL_HANDLE;
    uint32_t m_cachedRenderWidth = 1440;
    uint32_t m_cachedRenderHeight = 1600;
    
    // Compositor's own OpenXR resources
    XrSwapchain m_compositorSwapchains[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::vector<XrSwapchainImageVulkan2KHR> m_compositorSwapchainImages[2];
    bool m_compositorSwapchainsCreated = false;
    
    // Compositor's own Vulkan resources (completely independent device)
    VkDevice m_compositorDevice = VK_NULL_HANDLE;
    VkQueue m_compositorQueue = VK_NULL_HANDLE;
    uint32_t m_compositorQueueFamily = 0;
    VkCommandPool m_compositorCommandPool = VK_NULL_HANDLE;
    
    // Stored menu texture (our own copy to avoid race conditions)
    VkImage m_storedMenuTexture = VK_NULL_HANDLE;
    VkDeviceMemory m_storedMenuTextureMemory = VK_NULL_HANDLE;
    uint32_t m_storedTextureWidth = 0;
    uint32_t m_storedTextureHeight = 0;
    
    // Deferred copy request structure
    struct PendingCopyRequest {
        VkImage sourceTexture = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        bool hasPendingCopy = false;
    } m_pendingCopyRequest;
    
    // 3D Rendering Structures
    struct Matrix4x4 {
        float m[16];  // 4x4 matrix in column-major order
    };
    
    struct QuadVertex {
        float position[3];  // X, Y, Z
        float texCoord[2];  // U, V
        float color[3];     // R, G, B  
    };
    
    struct MenuQuad3D {
        float width = 2.0f;     // Width in meters
        float height = 1.125f;  // Height in meters (16:9 default)
        float distance = 2.5f;  // Distance from viewer in meters
        float x = 0.0f;         // X offset from center
        float y = 0.0f;         // Y offset from center
    };
    
    // 3D rendering data
    QuadVertex m_quadVertices[4];
    Matrix4x4 m_mvpMatrix;
    bool m_has3DQuad = false;
    
    VkCommandBuffer m_compositorCommandBuffer = VK_NULL_HANDLE;
    VkFence m_compositorFence = VK_NULL_HANDLE;
    bool m_compositorVulkanResourcesCreated = false;
    
    // 3D Graphics Pipeline Resources
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_graphicsPipeline = VK_NULL_HANDLE;
    
    // Texture descriptor resources
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkSampler m_textureSampler = VK_NULL_HANDLE;
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer m_uniformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_uniformBufferMemory = VK_NULL_HANDLE;
    VkFramebuffer m_currentFramebuffer = VK_NULL_HANDLE;
    VkImageView m_currentImageView = VK_NULL_HANDLE;
    bool m_graphicsPipelineCreated = false;
    
    // Shader modules
    VkShaderModule m_vertShaderModule = VK_NULL_HANDLE;
    VkShaderModule m_fragShaderModule = VK_NULL_HANDLE;
    
    // Safe texture copying - our own texture copy
    VkImage m_copiedMenuTexture = VK_NULL_HANDLE;
    VkDeviceMemory m_copiedMenuTextureMemory = VK_NULL_HANDLE;
    VkImageView m_copiedMenuTextureView = VK_NULL_HANDLE;
    int m_copiedTextureWidth = 0;
    int m_copiedTextureHeight = 0;
    bool m_menuTextureCopied = false;
    
    // Default magenta fallback texture
    VkImage m_fallbackTexture = VK_NULL_HANDLE;
    VkDeviceMemory m_fallbackTextureMemory = VK_NULL_HANDLE;
    VkImageView m_fallbackTextureView = VK_NULL_HANDLE;
    
    // Rounded corner mask texture
    VkImage m_maskTexture = VK_NULL_HANDLE;
    VkDeviceMemory m_maskTextureMemory = VK_NULL_HANDLE;
    VkImageView m_maskTextureView = VK_NULL_HANDLE;
    VkSampler m_maskTextureSampler = VK_NULL_HANDLE;
    bool m_maskTextureLoaded = false;
    
    // =========================================================================
    // Controller Model Rendering
    // =========================================================================
    
    // Controller model manager
    std::unique_ptr<VRControllerModelManager> m_controllerModelManager;
    bool m_controllerModelsLoaded = false;
    bool m_controllerModelsInitialized = false;
    
    // Depth buffer resources (per-eye)
    VkImage m_depthImages[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory m_depthMemory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView m_depthImageViews[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFormat m_depthFormat = VK_FORMAT_D32_SFLOAT;
    
    // Controller model render pass (with depth attachment)
    VkRenderPass m_controllerRenderPass = VK_NULL_HANDLE;
    
    // Controller model graphics pipeline
    VkPipeline m_controllerPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_controllerPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_controllerDescriptorSetLayout = VK_NULL_HANDLE;
    
    // Controller model shaders
    VkShaderModule m_controllerVertShader = VK_NULL_HANDLE;
    VkShaderModule m_controllerFragShader = VK_NULL_HANDLE;
    
    // Framebuffers for controller rendering (per-eye, includes depth)
    VkFramebuffer m_controllerFramebuffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    
    bool m_controllerPipelineCreated = false;
    
    // Frame submission data
    struct FrameData {
        void* textureHandle = nullptr;
        int width = 0;
        int height = 0;
        bool hasNewFrame = false;
        bool isVGUITexture = false;  // True if this is a VGUI texture to render
        bool isDXVKTexture = false;  // True if textureHandle is a DXVK VkImage, false if Source ITexture
    };
    FrameData m_latestFrame;

public:
    VRCompositor() = default;
    ~VRCompositor();
    
    // Initialization and lifecycle
    void SetOpenXRManager(::OpenXRDirectMode* manager);
    ::OpenXRDirectMode* GetOpenXRManager() const { return m_pOpenXRManager; }
    void CacheOpenXRData(VkPhysicalDevice physicalDevice, VkInstance instance, XrSession session, XrSpace referenceSpace, uint32_t width, uint32_t height);
    bool Initialize();
    void Shutdown();
    
    // Frame submission API
    void SubmitFrame(void* textureHandle, int width, int height, bool isVGUITexture = false, bool isDXVKTexture = false);
    bool RenderFrame(const XrFrameState& frameState, std::vector<XrCompositionLayerBaseHeader*>& layers);
    
    // State management
    void SetSourceEngineState(SourceEngineState state);
    SourceEngineState GetCurrentState() const { return m_currentState.load(); }
    bool IsActive() const { return m_compositorActive.load(); }
    bool IsCompositorActive() const;
    void SetSourceState(SourceEngineState state);
    void SubmitDXVKTexture(void* textureHandle, int width, int height);
    
    // SAFE TEXTURE COPYING - Public method for external calls
    bool CopyAndStoreMenuTexture(VkImage sourceTexture, int width, int height);
    
    // TF2 FRAME SYNCHRONIZATION - Block TF2 at frame end for texture copy
    std::mutex m_tf2FrameSignalMutex;
    std::condition_variable m_tf2FrameCondition;
    std::atomic<bool> m_tf2CanRenderFrame{false};
    std::atomic<bool> m_tf2FrameReady{false};
    void NotifyTF2FrameComplete();
    
    // Get the current predicted display time for input synchronization
    XrTime GetCurrentPredictedDisplayTime() const;
    
    // Controller model rendering
    bool InitializeControllerModels();
    void RenderControllerModels(int eye, VkCommandBuffer cmdBuffer, XrTime displayTime);
    bool AreControllerModelsLoaded() const { return m_controllerModelsLoaded; }

private:
    // Frame state for input synchronization
    mutable std::mutex m_frameStateMutex;
    XrFrameState m_currentFrameState;
    // Vulkan resource management
    bool CreateIndependentVulkanDevice();
    bool CreateCompositorSwapchains();
            bool CreateVulkanPipeline();
        bool CreateVertexBuffer();
        bool CreateSimple3DQuad();
        bool CreateWorkingVulkanPipeline();
        void Render3DQuad(int eye, VkImage targetImage, XrTime displayTime);
        void RenderSimpleQuad(int eye, VkImage targetImage);
    void CleanupVulkanResources();
    
    // Graphics pipeline setup
    bool EnsureGraphicsPipelineCreated();
    bool CreateRenderPass();
    bool CreateShaderModules();
    bool CreatePipelineLayout();
    bool CreateDescriptorResources();
    bool CreateTextureSampler();
    bool CreateSimpleFallbackTexture();
    bool CreateGraphicsPipeline();
    bool LoadMaskTexture(const char* pngPath);
    bool CreateMaskTextureFromPNG(const unsigned char* pngData, int width, int height);
    

    
    // 3D MVP calculation
    bool CalculateMVPMatrixForEye(int eye, XrTime displayTime, float* mvpMatrix);
    void CreateViewMatrix(const XrPosef& pose, float* viewMatrix);
    void CreateProperViewMatrix(const XrPosef& pose, float* viewMatrix);
    void CreateProperProjectionMatrix(const XrFovf& fov, float* projMatrix);
    void MultiplyMatrix4x4(const float* a, const float* b, float* result);
    
    // Vulkan helper functions
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    
    // Texture copying and management
    bool EnsureCopiedTextureCreated(int width, int height);
    bool PerformImmediateCopy(VkImage sourceTexture, int width, int height);
    void CompositorThreadFunc();
    
    // Basic rendering methods
    bool RenderEye(int eye, const XrFrameState& frameState);
    bool CreateOpenXRLayer(const XrFrameState& frameState, std::vector<XrCompositionLayerBaseHeader*>& layers);
    
    // Texture copying helpers
    bool CreateStoredTexture(uint32_t width, uint32_t height);
    bool CopyTextureImmediate(VkImage sourceTexture, VkImage destTexture, uint32_t width, uint32_t height);
    
    // 3D rendering setup
    void SetupQuadVertices();
    void SetupMVPMatrix();
    
    // Texture management
    void UpdateDescriptorSetWithTexture(VkImageView textureView);
    void UpdateDescriptorSetWithTextures(VkImageView mainTextureView, VkImageView maskTextureView);
    
    // HUD Position Management
    bool UpdateQuadFromGameHUD();
    void CheckAndUpdateHUDPosition();               // Check for updated HUD position data and update vertices
    bool InitializeQuadWithPlayspaceCoordinates();  // Initialize HUD quad using base playspace coordinates
    bool UpdateVertexBuffer();  // Update GPU vertex buffer with current m_quadVertices
    
    // Compositor thread management
    void StartCompositor();
    void StopCompositor();
    bool RunIndependentFrame();
    
    // Controller model rendering pipeline setup
    bool CreateDepthResources();
    bool CreateControllerRenderPass();
    bool CreateControllerShaderModules();
    bool CreateControllerPipelineLayout();
    bool CreateControllerGraphicsPipeline();
    bool CreateControllerFramebuffers();
    bool EnsureControllerPipelineCreated();
    void CleanupControllerResources();
    
    // Controller model helper methods
    void RenderSingleControllerModel(VkCommandBuffer cmdBuffer, const ControllerModel* model, 
                                      const float* viewProjMatrix, XrTime displayTime);
    void ComputeControllerMVP(const ControllerModel* model, int eye, XrTime displayTime, float* mvpOut);
};

} // namespace dxvk

#endif // VRCOMPOSITOR_H_INCLUDED
