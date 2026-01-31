#include "VRLaserPointer.h"
#include "VRControllerModel.h"  // For ControllerModel struct
#include "OpenXRDirectMode.h"
#include "../util/log/log.h"

#include <cstring>
#include <cmath>

namespace dxvk {

// Push constants structure (must match controller shader)
struct LaserPushConstants {
    float mvpMatrix[16];     // Model-View-Projection matrix
    float modelMatrix[16];   // Model matrix (for normal transform)
    float baseColor[4];      // Base color factor (RGBA)
    float emissive[4];       // Emissive factor (RGB + hasTexture flag in W)
};

VRLaserPointer::VRLaserPointer() {
}

VRLaserPointer::~VRLaserPointer() {
    Shutdown();
}

bool VRLaserPointer::Initialize(OpenXRDirectMode* openxrManager, VkDevice device, VkPhysicalDevice physicalDevice) {
    m_openxrManager = openxrManager;
    m_device = device;
    m_physicalDevice = physicalDevice;
    
    Logger::info("VRLaserPointer: Initialized");
    return true;
}

void VRLaserPointer::Shutdown() {
    if (m_device == VK_NULL_HANDLE) return;
    
    vkDeviceWaitIdle(m_device);
    
    if (m_vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
        m_vertexBuffer = VK_NULL_HANDLE;
    }
    if (m_vertexMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_vertexMemory, nullptr);
        m_vertexMemory = VK_NULL_HANDLE;
    }
    if (m_indexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_indexBuffer, nullptr);
        m_indexBuffer = VK_NULL_HANDLE;
    }
    if (m_indexMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_indexMemory, nullptr);
        m_indexMemory = VK_NULL_HANDLE;
    }
    
    m_resourcesCreated = false;
    Logger::info("VRLaserPointer: Shutdown complete");
}

bool VRLaserPointer::CreateResources(VkDescriptorSetLayout descriptorSetLayout,
                                     VkDescriptorPool descriptorPool,
                                     VkSampler sampler) {
    if (m_resourcesCreated) return true;
    
    if (!CreateVertexBuffer()) {
        Logger::err("VRLaserPointer: Failed to create vertex buffer");
        return false;
    }
    
    if (!CreateIndexBuffer()) {
        Logger::err("VRLaserPointer: Failed to create index buffer");
        return false;
    }
    
    m_resourcesCreated = true;
    Logger::info("VRLaserPointer: Resources created successfully");
    return true;
}

uint32_t VRLaserPointer::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && 
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool VRLaserPointer::CreateVertexBuffer() {
    // Create a box mesh for the laser
    // 8 vertices with position (3), normal (3), uv (2) = 8 floats per vertex
    VkDeviceSize bufferSize = VERTEX_COUNT * VERTEX_STRIDE;
    
    VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_vertexBuffer) != VK_SUCCESS) {
        return false;
    }
    
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, m_vertexBuffer, &memReqs);
    
    VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (allocInfo.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(m_device, &allocInfo, nullptr, &m_vertexMemory) != VK_SUCCESS) {
        return false;
    }
    
    vkBindBufferMemory(m_device, m_vertexBuffer, m_vertexMemory, 0);
    
    // Initialize with identity pose (will be updated per-frame)
    XrPosef identityPose = {{0, 0, 0, 1}, {0, 0, 0}};
    float vertices[VERTEX_COUNT * 8];
    UpdateLaserGeometry(identityPose, vertices);
    
    void* data;
    vkMapMemory(m_device, m_vertexMemory, 0, bufferSize, 0, &data);
    memcpy(data, vertices, bufferSize);
    vkUnmapMemory(m_device, m_vertexMemory);
    
    return true;
}

bool VRLaserPointer::CreateIndexBuffer() {
    // Box indices: 6 faces * 2 triangles * 3 vertices = 36 indices
    uint32_t indices[INDEX_COUNT] = {
        // Back face
        0, 1, 2, 0, 2, 3,
        // Front face
        4, 6, 5, 4, 7, 6,
        // Bottom face
        0, 4, 5, 0, 5, 1,
        // Top face
        3, 2, 6, 3, 6, 7,
        // Left face
        0, 3, 7, 0, 7, 4,
        // Right face
        1, 5, 6, 1, 6, 2
    };
    
    VkDeviceSize bufferSize = sizeof(indices);
    
    VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_indexBuffer) != VK_SUCCESS) {
        return false;
    }
    
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, m_indexBuffer, &memReqs);
    
    VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (allocInfo.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(m_device, &allocInfo, nullptr, &m_indexMemory) != VK_SUCCESS) {
        return false;
    }
    
    vkBindBufferMemory(m_device, m_indexBuffer, m_indexMemory, 0);
    
    void* data;
    vkMapMemory(m_device, m_indexMemory, 0, bufferSize, 0, &data);
    memcpy(data, indices, bufferSize);
    vkUnmapMemory(m_device, m_indexMemory);
    
    return true;
}

void VRLaserPointer::UpdateLaserGeometry(const XrPosef& pose, float* vertices) {
    // Build rotation matrix from quaternion
    float qx = pose.orientation.x;
    float qy = pose.orientation.y;
    float qz = pose.orientation.z;
    float qw = pose.orientation.w;
    
    float xx = qx * qx, yy = qy * qy, zz = qz * qz;
    float xy = qx * qy, xz = qx * qz, yz = qy * qz;
    float wx = qw * qx, wy = qw * qy, wz = qw * qz;
    
    // Rotation matrix columns
    float r00 = 1.0f - 2.0f * (yy + zz);
    float r10 = 2.0f * (xy + wz);
    float r20 = 2.0f * (xz - wy);
    
    float r01 = 2.0f * (xy - wz);
    float r11 = 1.0f - 2.0f * (xx + zz);
    float r21 = 2.0f * (yz + wx);
    
    float r02 = 2.0f * (xz + wy);
    float r12 = 2.0f * (yz - wx);
    float r22 = 1.0f - 2.0f * (xx + yy);
    
    // Z axis is backward in OpenXR, so -Z is forward
    // Get local coordinate axes
    float forwardX = -r02, forwardY = -r12, forwardZ = -r22;  // -Z column
    float rightX = r00, rightY = r10, rightZ = r20;            // X column
    float upX = r01, upY = r11, upZ = r21;                     // Y column
    
    // Laser start at controller position, offset forward to approximate aim point
    float startX = pose.position.x + forwardX * m_originOffset;
    float startY = pose.position.y + forwardY * m_originOffset;
    float startZ = pose.position.z + forwardZ * m_originOffset;
    
    // Use actual length (intersection length if set, otherwise default)
    float actualLength = GetActualLength();
    
    // Laser end extends forward
    float endX = startX + forwardX * actualLength;
    float endY = startY + forwardY * actualLength;
    float endZ = startZ + forwardZ * actualLength;
    
    float hw = m_width * 0.5f;  // half width
    
    // Define 8 vertices of the box in OpenXR coordinates
    // Start end (back of laser)
    float v0[3] = {startX - upX*hw - rightX*hw, startY - upY*hw - rightY*hw, startZ - upZ*hw - rightZ*hw};
    float v1[3] = {startX - upX*hw + rightX*hw, startY - upY*hw + rightY*hw, startZ - upZ*hw + rightZ*hw};
    float v2[3] = {startX + upX*hw + rightX*hw, startY + upY*hw + rightY*hw, startZ + upZ*hw + rightZ*hw};
    float v3[3] = {startX + upX*hw - rightX*hw, startY + upY*hw - rightY*hw, startZ + upZ*hw - rightZ*hw};
    // End end (front of laser)
    float v4[3] = {endX - upX*hw - rightX*hw, endY - upY*hw - rightY*hw, endZ - upZ*hw - rightZ*hw};
    float v5[3] = {endX - upX*hw + rightX*hw, endY - upY*hw + rightY*hw, endZ - upZ*hw + rightZ*hw};
    float v6[3] = {endX + upX*hw + rightX*hw, endY + upY*hw + rightY*hw, endZ + upZ*hw + rightZ*hw};
    float v7[3] = {endX + upX*hw - rightX*hw, endY + upY*hw - rightY*hw, endZ + upZ*hw - rightZ*hw};
    
    float* allVerts[8] = {v0, v1, v2, v3, v4, v5, v6, v7};
    
    // Fill vertex buffer: pos(3), normal(3), uv(2)
    for (int i = 0; i < 8; i++) {
        int offset = i * 8;
        // Position
        vertices[offset + 0] = allVerts[i][0];
        vertices[offset + 1] = allVerts[i][1];
        vertices[offset + 2] = allVerts[i][2];
        // Normal (pointing outward, simplified - just use forward)
        vertices[offset + 3] = forwardX;
        vertices[offset + 4] = forwardY;
        vertices[offset + 5] = forwardZ;
        // UV
        vertices[offset + 6] = 0.0f;
        vertices[offset + 7] = 0.0f;
    }
}

void VRLaserPointer::Render(VkCommandBuffer cmdBuffer,
                           VkPipeline pipeline,
                           VkPipelineLayout pipelineLayout,
                           const ControllerModel* controllerModel,
                           const float* viewProjMatrix,
                           uint32_t viewportWidth,
                           uint32_t viewportHeight,
                           bool isLeftHand) {
    if (!m_enabled || !m_resourcesCreated || !controllerModel || !controllerModel->isLoaded) {
        return;
    }
    
    // Skip if controller is not visible/tracked
    if (!controllerModel->isVisible) {
        return;
    }
    
    // Update vertex buffer with current controller pose
    const XrPosef& pose = controllerModel->currentPose;
    
    float vertices[VERTEX_COUNT * 8];
    UpdateLaserGeometry(pose, vertices);
    
    // Update vertex buffer
    void* data;
    VkDeviceSize bufferSize = VERTEX_COUNT * VERTEX_STRIDE;
    vkMapMemory(m_device, m_vertexMemory, 0, bufferSize, 0, &data);
    memcpy(data, vertices, bufferSize);
    vkUnmapMemory(m_device, m_vertexMemory);
    
    // Bind pipeline (same as controllers)
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    
    // Set viewport and scissor (dynamic state - must be set after binding pipeline)
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(viewportWidth);
    viewport.height = static_cast<float>(viewportHeight);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);
    
    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {viewportWidth, viewportHeight};
    vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);
    
    // Set up push constants
    // For the laser, we use identity model matrix since vertices are already in world space
    LaserPushConstants pc = {};
    
    // Identity model matrix
    memset(pc.modelMatrix, 0, sizeof(pc.modelMatrix));
    pc.modelMatrix[0] = 1.0f;
    pc.modelMatrix[5] = 1.0f;
    pc.modelMatrix[10] = 1.0f;
    pc.modelMatrix[15] = 1.0f;
    
    // Invert Y for Vulkan coordinate system (like controllers do)
    pc.modelMatrix[5] = -1.0f;
    
    memcpy(pc.mvpMatrix, viewProjMatrix, sizeof(float) * 16);
    
    // Color - apply sRGB to linear conversion for correct display
    // The swapchain expects linear colors, but our input is sRGB
    auto srgbToLinear = [](float c) {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    
    pc.baseColor[0] = srgbToLinear(m_color[0]);
    pc.baseColor[1] = srgbToLinear(m_color[1]);
    pc.baseColor[2] = srgbToLinear(m_color[2]);
    pc.baseColor[3] = m_color[3];
    
    // No emissive - just base color
    pc.emissive[0] = 0.0f;
    pc.emissive[1] = 0.0f;
    pc.emissive[2] = 0.0f;
    pc.emissive[3] = 0.0f;  // No texture
    
    // Push constants
    vkCmdPushConstants(cmdBuffer, pipelineLayout,
                      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                      0, sizeof(LaserPushConstants), &pc);
    
    // Bind vertex and index buffers
    VkBuffer vertexBuffers[] = {m_vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmdBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmdBuffer, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    
    // Draw
    vkCmdDrawIndexed(cmdBuffer, INDEX_COUNT, 1, 0, 0, 0);
}

void VRLaserPointer::RenderWithPose(VkCommandBuffer cmdBuffer,
                                   VkPipeline pipeline,
                                   VkPipelineLayout pipelineLayout,
                                   const XrPosef& aimPose,
                                   const float* viewProjMatrix,
                                   uint32_t viewportWidth,
                                   uint32_t viewportHeight) {
    if (!m_enabled || !m_resourcesCreated) {
        return;
    }
    
    // Update vertex buffer with aim pose (no origin offset needed - aim pose is already at correct position)
    float vertices[VERTEX_COUNT * 8];
    
    // For aim pose, we don't apply origin offset since the aim pose IS the origin
    float savedOffset = m_originOffset;
    m_originOffset = 0.0f;  // Temporarily disable offset for aim pose
    UpdateLaserGeometry(aimPose, vertices);
    m_originOffset = savedOffset;
    
    // Update vertex buffer
    void* data;
    VkDeviceSize bufferSize = VERTEX_COUNT * VERTEX_STRIDE;
    vkMapMemory(m_device, m_vertexMemory, 0, bufferSize, 0, &data);
    memcpy(data, vertices, bufferSize);
    vkUnmapMemory(m_device, m_vertexMemory);
    
    // Bind pipeline
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    
    // Set viewport and scissor
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(viewportWidth);
    viewport.height = static_cast<float>(viewportHeight);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);
    
    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {viewportWidth, viewportHeight};
    vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);
    
    // Set up push constants
    LaserPushConstants pc = {};
    
    // Identity model matrix with Y-flip for Vulkan
    memset(pc.modelMatrix, 0, sizeof(pc.modelMatrix));
    pc.modelMatrix[0] = 1.0f;
    pc.modelMatrix[5] = -1.0f;  // Y-flip
    pc.modelMatrix[10] = 1.0f;
    pc.modelMatrix[15] = 1.0f;
    
    memcpy(pc.mvpMatrix, viewProjMatrix, sizeof(float) * 16);
    
    // Color - apply sRGB to linear conversion for correct display
    auto srgbToLinear = [](float c) {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    
    pc.baseColor[0] = srgbToLinear(m_color[0]);
    pc.baseColor[1] = srgbToLinear(m_color[1]);
    pc.baseColor[2] = srgbToLinear(m_color[2]);
    pc.baseColor[3] = m_color[3];
    
    // No emissive - just base color to match in-game
    pc.emissive[0] = 0.0f;
    pc.emissive[1] = 0.0f;
    pc.emissive[2] = 0.0f;
    pc.emissive[3] = 0.0f;
    
    vkCmdPushConstants(cmdBuffer, pipelineLayout,
                      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                      0, sizeof(LaserPushConstants), &pc);
    
    // Bind buffers and draw
    VkBuffer vertexBuffers[] = {m_vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmdBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmdBuffer, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    
    vkCmdDrawIndexed(cmdBuffer, INDEX_COUNT, 1, 0, 0, 0);
}

void VRLaserPointer::SetColor(float r, float g, float b, float a) {
    m_color[0] = r;
    m_color[1] = g;
    m_color[2] = b;
    m_color[3] = a;
}

void VRLaserPointer::SetLength(float lengthMeters) {
    m_length = lengthMeters;
}

void VRLaserPointer::SetWidth(float widthMeters) {
    m_width = widthMeters;
}

void VRLaserPointer::SetIntersectionLength(float lengthMeters) {
    m_intersectionLength = lengthMeters;
    m_intersectionLengthTime = std::chrono::steady_clock::now();
}

void VRLaserPointer::ClearIntersectionLength() {
    m_intersectionLength = -1.0f;  // Invalid value signals to use default
}

float VRLaserPointer::GetActualLength() const {
    // Check if we have a fresh intersection length
    if (m_intersectionLength > 0.0f) {
        auto now = std::chrono::steady_clock::now();
        auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_intersectionLengthTime).count();
        if (ageMs < INTERSECTION_STALE_MS) {
            return m_intersectionLength;
        }
    }
    // Fall back to default length
    return m_length;
}

float VRLaserPointer::CalculatePlaneIntersection(const XrPosef& pose,
                                                  const float* corner0, const float* corner1,
                                                  const float* corner2, const float* corner3) const {
    // Build rotation matrix from quaternion
    float qx = pose.orientation.x;
    float qy = pose.orientation.y;
    float qz = pose.orientation.z;
    float qw = pose.orientation.w;
    
    float xx = qx * qx, yy = qy * qy, zz = qz * qz;
    float xz = qx * qz, yz = qy * qz;
    float wy = qw * qy, wx = qw * qx;
    
    // Z column of rotation matrix (forward direction is -Z in OpenXR)
    float r02 = 2.0f * (xz + wy);
    float r12 = 2.0f * (yz - wx);
    float r22 = 1.0f - 2.0f * (xx + yy);
    
    // Ray origin - the quad corners have Y flipped in the game's coordinate conversion
    // so we need to flip the aim pose Y to match
    float rayOriginX = pose.position.x;
    float rayOriginY = -pose.position.y;  // Flip Y to match quad coordinate system
    float rayOriginZ = pose.position.z;
    
    // Ray direction - also flip the Y component
    float rayDirX = -r02;  // -Z is forward
    float rayDirY = -(-r12);  // Flip Y component (double negative = positive)
    float rayDirZ = -r22;
    
    // Quad layout: corner0=LL, corner1=LR, corner2=UR, corner3=UL
    // Calculate plane normal from two edges
    float edge1X = corner1[0] - corner0[0];  // LL -> LR (right)
    float edge1Y = corner1[1] - corner0[1];
    float edge1Z = corner1[2] - corner0[2];
    
    float edge2X = corner3[0] - corner0[0];  // LL -> UL (up)
    float edge2Y = corner3[1] - corner0[1];
    float edge2Z = corner3[2] - corner0[2];
    
    // Cross product for normal (edge1 x edge2 = right x up = forward/out)
    float normalX = edge1Y * edge2Z - edge1Z * edge2Y;
    float normalY = edge1Z * edge2X - edge1X * edge2Z;
    float normalZ = edge1X * edge2Y - edge1Y * edge2X;
    
    // Normalize
    float normalLen = std::sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ);
    if (normalLen < 0.0001f) return -1.0f;
    normalX /= normalLen;
    normalY /= normalLen;
    normalZ /= normalLen;
    
    // Ray-plane intersection: t = ((planePoint - rayOrigin) . normal) / (rayDir . normal)
    float denom = rayDirX * normalX + rayDirY * normalY + rayDirZ * normalZ;
    if (std::abs(denom) < 0.0001f) return -1.0f;  // Ray parallel to plane
    
    float dx = corner0[0] - rayOriginX;
    float dy = corner0[1] - rayOriginY;
    float dz = corner0[2] - rayOriginZ;
    
    float t = (dx * normalX + dy * normalY + dz * normalZ) / denom;
    
    // Must be in front of ray origin and within reasonable range
    if (t < 0.001f || t > 50.0f) return -1.0f;  // Too close, behind origin, or too far
    
    // Calculate hit point
    float hitX = rayOriginX + rayDirX * t;
    float hitY = rayOriginY + rayDirY * t;
    float hitZ = rayOriginZ + rayDirZ * t;
    
    // Project hit point onto quad's 2D coordinate system
    // Use edge1 (right) and edge2 (up) as basis vectors
    float edge1Len = std::sqrt(edge1X * edge1X + edge1Y * edge1Y + edge1Z * edge1Z);
    float edge2Len = std::sqrt(edge2X * edge2X + edge2Y * edge2Y + edge2Z * edge2Z);
    
    if (edge1Len < 0.0001f || edge2Len < 0.0001f) return -1.0f;
    
    // Normalize edges
    float right[3] = {edge1X / edge1Len, edge1Y / edge1Len, edge1Z / edge1Len};
    float up[3] = {edge2X / edge2Len, edge2Y / edge2Len, edge2Z / edge2Len};
    
    // Vector from corner0 to hit point
    float toHitX = hitX - corner0[0];
    float toHitY = hitY - corner0[1];
    float toHitZ = hitZ - corner0[2];
    
    // Project onto local coordinates
    float u = (toHitX * right[0] + toHitY * right[1] + toHitZ * right[2]) / edge1Len;
    float v = (toHitX * up[0] + toHitY * up[1] + toHitZ * up[2]) / edge2Len;
    
    // Check if within quad bounds (0 to 1 range) with small margin for edge cases
    const float margin = 0.05f;  // 5% margin
    if (u >= -margin && u <= 1.0f + margin && v >= -margin && v <= 1.0f + margin) {
        return t;
    }
    
    return -1.0f;  // Hit plane but outside quad bounds
}

} // namespace dxvk
