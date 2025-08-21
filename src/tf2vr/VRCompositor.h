#ifndef VRCOMPOSITOR_H_INCLUDED
#define VRCOMPOSITOR_H_INCLUDED

#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <chrono>

#include "vulkan/vulkan.h"
#include "openxr/openxr.h"
#define XR_USE_GRAPHICS_API_VULKAN
#include "openxr/openxr_platform.h"

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
    OpenXRDirectMode* m_pOpenXRManager = nullptr;
    
    // Cached values to avoid calling OpenXRDirectMode methods (avoid circular dependency)
    VkPhysicalDevice m_cachedPhysicalDevice = VK_NULL_HANDLE;
    VkInstance m_cachedInstance = VK_NULL_HANDLE;
    XrSession m_cachedSession = XR_NULL_HANDLE;
    XrSpace m_cachedReferenceSpace = XR_NULL_HANDLE;
    uint32_t m_cachedRenderWidth = 1440;
    uint32_t m_cachedRenderHeight = 1600;
    
    // Compositor's own OpenXR resources
    XrSwapchain m_compositorSwapchains[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::vector<XrSwapchainImageVulkanKHR> m_compositorSwapchainImages[2];
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
    void SetOpenXRManager(OpenXRDirectMode* manager);
    OpenXRDirectMode* GetOpenXRManager() const { return m_pOpenXRManager; }
    void CacheOpenXRData(VkPhysicalDevice physicalDevice, VkInstance instance, XrSession session, XrSpace referenceSpace, uint32_t width, uint32_t height);
    bool Initialize();
    void Shutdown();
    
    // Frame submission API
    void SubmitFrame(void* textureHandle, int width, int height, bool isVGUITexture = false, bool isDXVKTexture = false);
    bool RenderFrame(const XrFrameState& frameState, std::vector<XrCompositionLayerBaseHeader*>& layers);
    
    // State management
    void SetSourceEngineState(SourceEngineState state);
    bool IsActive() const { return m_compositorActive.load(); }
    bool IsCompositorActive() const;
    void SetSourceState(SourceEngineState state);
    void SubmitDXVKTexture(void* textureHandle, int width, int height);
    
    // SAFE TEXTURE COPYING - Public method for external calls
    bool CopyAndStoreMenuTexture(VkImage sourceTexture, int width, int height);

private:
    // Vulkan resource management
    bool CreateIndependentVulkanDevice();
    bool CreateCompositorSwapchains();
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
    
    // Rendering operations
    bool BeginTrue3DRenderPass(VkImage targetImage);
    void EndTrue3DRenderPass();
    bool SubmitTrue3DCommands();
    bool RenderTrue3DQuad(VkImage targetImage, const QuadVertex vertices[4], const Matrix4x4& mvp);
    
    // Texture and vertex operations
    bool CreateVertexBuffer(const QuadVertex vertices[4]);
    bool UpdateDescriptorSet(VkImage textureImage, VkImageView textureImageView);
    bool TransitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    
    // Helper functions
    bool EnsureFramebufferCreated(VkImage targetImage);
    void SetupQuadVertices(QuadVertex vertices[4], const MenuQuad3D& quad3D);
    Matrix4x4 CalculateMVPMatrix(const XrView& view, const MenuQuad3D& quad3D);
    
    // Matrix operations
    Matrix4x4 CreateTranslationMatrix(float x, float y, float z);
    Matrix4x4 CreateViewMatrixFromPose(const XrPosef& pose);
    Matrix4x4 CreateProjectionMatrixFromFOV(const XrFovf& fov);
    Matrix4x4 MultiplyMatrices(const Matrix4x4& a, const Matrix4x4& b);
    
    // Rendering helpers
    bool Render3DTexturedQuad(int eye, uint32_t imageIndex, const FrameData& frame, const XrView& view, const MenuQuad3D& quad3D);
    bool RenderVGUITextureToEyes(const XrFrameState& frameState, std::vector<XrCompositionLayerBaseHeader*>& layers, const FrameData& frame);
    
    // Texture copying and management
    bool EnsureCopiedTextureCreated(int width, int height);
    bool PerformImmediateCopy(VkImage sourceTexture, int width, int height);
    void CheckAndCopyTrackedVGUITexture();
    void CompositorThreadFunc();
    
    // Basic rendering methods
    bool RenderEye(int eye);
    bool CreateOpenXRLayer(const XrFrameState& frameState, std::vector<XrCompositionLayerBaseHeader*>& layers);
    
    // Texture copying helpers
    bool CreateStoredTexture(uint32_t width, uint32_t height);
    bool CopyTextureImmediate(VkImage sourceTexture, VkImage destTexture, uint32_t width, uint32_t height);
    
    // 3D rendering setup
    void SetupQuadVertices();
    void SetupMVPMatrix();
    
    // Compositor thread management
    void StartCompositor();
    void StopCompositor();
    bool RunIndependentFrame();
};

} // namespace dxvk

#endif // VRCOMPOSITOR_H_INCLUDED
