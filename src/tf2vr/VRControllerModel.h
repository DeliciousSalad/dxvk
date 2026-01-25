#ifndef VRCONTROLLERMODEL_H_INCLUDED
#define VRCONTROLLERMODEL_H_INCLUDED

#include <vector>
#include <string>
#include <cstdint>
#include <array>

// Must define Win32 platform before including Vulkan headers
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include <vulkan/vulkan.h>
#include "openxr/openxr.h"

// Forward declaration - OpenXRDirectMode is in global namespace
class OpenXRDirectMode;

namespace dxvk {

// 3D vertex format for controller models
struct Vertex3D {
    float position[3];  // XYZ position
    float normal[3];    // Normal vector for lighting
    float texCoord[2];  // UV texture coordinates
    
    static VkVertexInputBindingDescription GetBindingDescription() {
        VkVertexInputBindingDescription bindingDesc = {};
        bindingDesc.binding = 0;
        bindingDesc.stride = sizeof(Vertex3D);
        bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDesc;
    }
    
    static std::array<VkVertexInputAttributeDescription, 3> GetAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 3> attrs = {};
        
        // Position
        attrs[0].binding = 0;
        attrs[0].location = 0;
        attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[0].offset = offsetof(Vertex3D, position);
        
        // Normal
        attrs[1].binding = 0;
        attrs[1].location = 1;
        attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset = offsetof(Vertex3D, normal);
        
        // TexCoord
        attrs[2].binding = 0;
        attrs[2].location = 2;
        attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
        attrs[2].offset = offsetof(Vertex3D, texCoord);
        
        return attrs;
    }
};

// A single mesh primitive within a controller model
struct ControllerMesh {
    std::vector<Vertex3D> vertices;
    std::vector<uint32_t> indices;
    
    // Vulkan resources
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    
    uint32_t indexCount = 0;
    int materialIndex = -1;  // Index into materials array, -1 if none
};

// Material properties extracted from glTF
struct ControllerMaterial {
    // Base color texture (Vulkan resources)
    VkImage texture = VK_NULL_HANDLE;
    VkImageView textureView = VK_NULL_HANDLE;
    VkDeviceMemory textureMemory = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    
    // Temporary storage for decoded texture pixels
    std::vector<uint8_t> texturePixels;
    int textureWidth = 0;
    int textureHeight = 0;
    
    // Base color factor (RGBA)
    float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    
    // Metallic-roughness
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    
    // Emissive
    float emissiveFactor[3] = {0.0f, 0.0f, 0.0f};
    
    bool hasTexture = false;
};

// A node in the controller model hierarchy (for animation)
struct ControllerNode {
    std::string name;           // Node name (matches glTF node names for animation)
    int meshIndex = -1;         // Index into meshes array, -1 if no mesh
    
    // Local transform components
    float translation[3] = {0.0f, 0.0f, 0.0f};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // Quaternion (x, y, z, w)
    float scale[3] = {1.0f, 1.0f, 1.0f};
    
    // Computed local transform matrix (4x4 column-major)
    float localMatrix[16];
    
    // Combined world transform (includes parent transforms)
    float worldMatrix[16];
    
    // Hierarchy
    int parentIndex = -1;       // Index of parent node, -1 if root
    std::vector<int> children;  // Indices of child nodes
    
    // Animation state (updated per-frame from OpenXR)
    bool isAnimatable = false;  // True if this node can be animated
    bool isVisible = true;      // Visibility state from OpenXR
    
    void ComputeLocalMatrix();
    void UpdateWorldMatrix(const float* parentWorld);
};

// Complete controller model data
struct ControllerModel {
    // OpenXR handles
    XrRenderModelIdEXT renderModelId = 0;
    XrRenderModelEXT renderModel = XR_NULL_HANDLE;
    XrRenderModelAssetEXT assetHandle = XR_NULL_HANDLE;
    XrSpace modelSpace = XR_NULL_HANDLE;
    
    // Model properties
    XrUuidEXT cacheId = {};
    uint32_t animatableNodeCount = 0;
    
    // Parsed glTF data
    std::vector<ControllerMesh> meshes;
    std::vector<ControllerMaterial> materials;
    std::vector<ControllerNode> nodes;
    std::vector<int> rootNodes;  // Indices of root nodes (no parent)
    
    // Animation state buffer (sized to animatableNodeCount)
    std::vector<XrRenderModelNodeStateEXT> nodeStates;
    
    // Tracking state
    bool isLoaded = false;
    bool isVisible = false;
    XrPosef currentPose = {{0, 0, 0, 1}, {0, 0, 0}};  // Identity quaternion
    
    // Descriptor set for this model's textures
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
};

// Push constants for controller model rendering
struct ControllerModelPushConstants {
    float mvpMatrix[16];     // Model-View-Projection matrix
    float modelMatrix[16];   // Model matrix (for normal transform)
    float baseColor[4];      // Base color factor (RGBA)
    float emissive[4];       // Emissive factor (RGB + hasTexture flag in W)
};

// Manager class for loading and managing controller models
class VRControllerModelManager {
public:
    VRControllerModelManager() = default;
    ~VRControllerModelManager();
    
    // Initialize with OpenXR manager reference
    bool Initialize(::OpenXRDirectMode* openxrManager, VkDevice device, VkPhysicalDevice physDevice);
    void Shutdown();
    
    // Load controller models from OpenXR
    bool LoadControllerModels(XrSession session);
    
    // Update animation state for all models
    void UpdateAnimationState(XrTime displayTime);
    
    // Get models for rendering
    const ControllerModel* GetLeftController() const { return m_leftController.isLoaded ? &m_leftController : nullptr; }
    const ControllerModel* GetRightController() const { return m_rightController.isLoaded ? &m_rightController : nullptr; }
    ControllerModel* GetLeftController() { return m_leftController.isLoaded ? &m_leftController : nullptr; }
    ControllerModel* GetRightController() { return m_rightController.isLoaded ? &m_rightController : nullptr; }
    
private:
    ::OpenXRDirectMode* m_openxrManager = nullptr;
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physDevice = VK_NULL_HANDLE;
    
    ControllerModel m_leftController;
    ControllerModel m_rightController;
    
    // Parse GLB data into a ControllerModel
    bool ParseGLTFModel(const uint8_t* data, size_t size, ControllerModel& outModel);
    
    // Create Vulkan resources for a loaded model
    bool CreateModelVulkanResources(ControllerModel& model);
    
    // Cleanup a single model
    void CleanupModel(ControllerModel& model);
    
    // Helper to find memory type
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    
    // Create a buffer with memory
    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, 
                      VkMemoryPropertyFlags properties,
                      VkBuffer& buffer, VkDeviceMemory& memory);
    
    // Create an image from texture data
    bool CreateTextureImage(const uint8_t* pixels, int width, int height,
                           VkImage& image, VkDeviceMemory& memory, VkImageView& view);
};

// Utility functions
void IdentityMatrix(float* m);
void TransposeMatrix(const float* src, float* dst);
void MultiplyMatrices(const float* a, const float* b, float* result);
void TranslationMatrix(float x, float y, float z, float* m);
void ScaleMatrix(float x, float y, float z, float* m);
void QuaternionToMatrix(float x, float y, float z, float w, float* m);

} // namespace dxvk

#endif // VRCONTROLLERMODEL_H_INCLUDED
