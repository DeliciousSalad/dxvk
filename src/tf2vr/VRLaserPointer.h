#pragma once

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <array>
#include <chrono>

// Forward declaration - OpenXRDirectMode is in global namespace
class OpenXRDirectMode;

namespace dxvk {

struct ControllerModel;

class VRLaserPointer {
public:
    VRLaserPointer();
    ~VRLaserPointer();
    
    bool Initialize(::OpenXRDirectMode* openxrManager, VkDevice device, VkPhysicalDevice physicalDevice);
    void Shutdown();
    
    // Create GPU resources (call after pipeline is available)
    bool CreateResources(VkDescriptorSetLayout descriptorSetLayout, 
                        VkDescriptorPool descriptorPool,
                        VkSampler sampler);
    
    // Render laser for a controller (using controller model's pose)
    void Render(VkCommandBuffer cmdBuffer, 
               VkPipeline pipeline,
               VkPipelineLayout pipelineLayout,
               const ControllerModel* controllerModel,
               const float* viewProjMatrix,
               uint32_t viewportWidth,
               uint32_t viewportHeight,
               bool isLeftHand);
    
    // Render laser with explicit aim pose (preferred method)
    void RenderWithPose(VkCommandBuffer cmdBuffer,
                       VkPipeline pipeline,
                       VkPipelineLayout pipelineLayout,
                       const XrPosef& aimPose,
                       const float* viewProjMatrix,
                       uint32_t viewportWidth,
                       uint32_t viewportHeight);
    
    // Settings
    void SetColor(float r, float g, float b, float a = 1.0f);
    void SetLength(float lengthMeters);
    void SetWidth(float widthMeters);
    void SetIntersectionLength(float lengthMeters);  // Override length with intersection result
    void ClearIntersectionLength();  // Clear intersection override, use default length
    float GetActualLength() const;  // Returns intersection length if set, otherwise default length
    
    // Calculate intersection with a plane defined by 4 corners (in OpenXR space)
    // Returns intersection distance, or -1 if no intersection
    float CalculatePlaneIntersection(const XrPosef& pose, 
                                     const float* corner0, const float* corner1, 
                                     const float* corner2, const float* corner3) const;
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }
    void SetActiveHand(bool isLeftHand) { m_activeHandIsLeft = isLeftHand; }
    bool IsLeftHandActive() const { return m_activeHandIsLeft; }
    
    // Origin offset (from grip to aim point, in local controller space)
    void SetOriginOffset(float forwardMeters) { m_originOffset = forwardMeters; }
    
private:
    bool CreateVertexBuffer();
    bool CreateIndexBuffer();
    void UpdateLaserGeometry(const XrPosef& pose, float* vertices);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    
    ::OpenXRDirectMode* m_openxrManager = nullptr;
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    
    // GPU resources
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vertexMemory = VK_NULL_HANDLE;
    VkBuffer m_indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_indexMemory = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    
    bool m_resourcesCreated = false;
    bool m_enabled = true;
    bool m_activeHandIsLeft = false;  // Default to right hand
    
    // Laser properties - match in-game defaults
    // Color: R=128, G=183, B=24 → (0.502, 0.718, 0.094)
    std::array<float, 4> m_color = {0.502f, 0.718f, 0.094f, 1.0f};
    float m_length = 2.54f;   // 100 game units ≈ 2.54 meters
    float m_width = 0.003f;   // Width in meters (~3mm, visible)
    float m_originOffset = 0.08f;  // Offset forward from grip to approximate aim point (8cm)
    float m_intersectionLength = -1.0f;  // Override length (-1 = use default)
    std::chrono::steady_clock::time_point m_intersectionLengthTime;
    static constexpr int64_t INTERSECTION_STALE_MS = 100;  // Consider stale after 100ms
    
    // Box geometry: 8 vertices, 36 indices (12 triangles)
    static constexpr int VERTEX_COUNT = 8;
    static constexpr int INDEX_COUNT = 36;
    static constexpr int VERTEX_STRIDE = 8 * sizeof(float);  // pos(3) + normal(3) + uv(2)
};

} // namespace dxvk
