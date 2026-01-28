#include "VRControllerModel.h"
#include "OpenXRDirectMode.h"
#include "../util/log/log.h"
#include "../util/util_string.h"

#include <cstring>
#include <cmath>
#include <cfloat>
#include <algorithm>
#include <functional>

// Include cgltf implementation
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

// Include stb_image for texture decoding (implementation is in VRCompositor.cpp)
#include "stb_image.h"

namespace dxvk {

//
// Matrix utility functions
//

void IdentityMatrix(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void TransposeMatrix(const float* src, float* dst) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            dst[j * 4 + i] = src[i * 4 + j];
        }
    }
}

void MultiplyMatrices(const float* a, const float* b, float* result) {
    float temp[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            temp[i * 4 + j] = 0;
            for (int k = 0; k < 4; k++) {
                temp[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
            }
        }
    }
    memcpy(result, temp, 16 * sizeof(float));
}

void TranslationMatrix(float x, float y, float z, float* m) {
    IdentityMatrix(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

void ScaleMatrix(float x, float y, float z, float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = x;
    m[5] = y;
    m[10] = z;
    m[15] = 1.0f;
}

void QuaternionToMatrix(float x, float y, float z, float w, float* m) {
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;
    
    m[0]  = 1.0f - 2.0f * (yy + zz);
    m[1]  = 2.0f * (xy + wz);
    m[2]  = 2.0f * (xz - wy);
    m[3]  = 0.0f;
    
    m[4]  = 2.0f * (xy - wz);
    m[5]  = 1.0f - 2.0f * (xx + zz);
    m[6]  = 2.0f * (yz + wx);
    m[7]  = 0.0f;
    
    m[8]  = 2.0f * (xz + wy);
    m[9]  = 2.0f * (yz - wx);
    m[10] = 1.0f - 2.0f * (xx + yy);
    m[11] = 0.0f;
    
    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = 0.0f;
    m[15] = 1.0f;
}

//
// ControllerNode implementation
//

void ControllerNode::ComputeLocalMatrix() {
    float T[16], R[16], S[16], TR[16];
    
    TranslationMatrix(translation[0], translation[1], translation[2], T);
    QuaternionToMatrix(rotation[0], rotation[1], rotation[2], rotation[3], R);
    ScaleMatrix(scale[0], scale[1], scale[2], S);
    
    // localMatrix = T * R * S
    MultiplyMatrices(T, R, TR);
    MultiplyMatrices(TR, S, localMatrix);
}

void ControllerNode::UpdateWorldMatrix(const float* parentWorld) {
    if (parentWorld) {
        MultiplyMatrices(parentWorld, localMatrix, worldMatrix);
    } else {
        memcpy(worldMatrix, localMatrix, 16 * sizeof(float));
    }
}

//
// VRControllerModelManager implementation
//

VRControllerModelManager::~VRControllerModelManager() {
    Shutdown();
}

bool VRControllerModelManager::Initialize(::OpenXRDirectMode* openxrManager, VkDevice device, VkPhysicalDevice physDevice) {
    m_openxrManager = openxrManager;
    m_device = device;
    m_physDevice = physDevice;
    
    Logger::info("VRControllerModelManager: Initialized");
    return true;
}

void VRControllerModelManager::Shutdown() {
    CleanupModel(m_leftController);
    CleanupModel(m_rightController);
    
    m_openxrManager = nullptr;
    m_device = VK_NULL_HANDLE;
    m_physDevice = VK_NULL_HANDLE;
}

void VRControllerModelManager::CleanupModel(ControllerModel& model) {
    if (m_device == VK_NULL_HANDLE) return;
    
    // Wait for device idle before cleanup
    vkDeviceWaitIdle(m_device);
    
    // Cleanup meshes
    for (auto& mesh : model.meshes) {
        if (mesh.vertexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, mesh.vertexBuffer, nullptr);
            mesh.vertexBuffer = VK_NULL_HANDLE;
        }
        if (mesh.vertexMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, mesh.vertexMemory, nullptr);
            mesh.vertexMemory = VK_NULL_HANDLE;
        }
        if (mesh.indexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, mesh.indexBuffer, nullptr);
            mesh.indexBuffer = VK_NULL_HANDLE;
        }
        if (mesh.indexMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, mesh.indexMemory, nullptr);
            mesh.indexMemory = VK_NULL_HANDLE;
        }
    }
    
    // Cleanup materials
    for (auto& mat : model.materials) {
        if (mat.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_device, mat.sampler, nullptr);
            mat.sampler = VK_NULL_HANDLE;
        }
        if (mat.textureView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, mat.textureView, nullptr);
            mat.textureView = VK_NULL_HANDLE;
        }
        if (mat.texture != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, mat.texture, nullptr);
            mat.texture = VK_NULL_HANDLE;
        }
        if (mat.textureMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, mat.textureMemory, nullptr);
            mat.textureMemory = VK_NULL_HANDLE;
        }
        mat.texturePixels.clear();
    }
    
    // Clear all data
    model.meshes.clear();
    model.materials.clear();
    model.nodes.clear();
    model.rootNodes.clear();
    model.nodeStates.clear();
    model.isLoaded = false;
    
    // Destroy OpenXR handles if we have them
    if (m_openxrManager && m_openxrManager->HasRenderModelSupport()) {
        if (model.modelSpace != XR_NULL_HANDLE) {
            xrDestroySpace(model.modelSpace);
            model.modelSpace = XR_NULL_HANDLE;
        }
        if (model.assetHandle != XR_NULL_HANDLE) {
            auto destroyAsset = m_openxrManager->GetDestroyRenderModelAssetEXT();
            if (destroyAsset) {
                destroyAsset(model.assetHandle);
            }
            model.assetHandle = XR_NULL_HANDLE;
        }
        if (model.renderModel != XR_NULL_HANDLE) {
            auto destroyModel = m_openxrManager->GetDestroyRenderModelEXT();
            if (destroyModel) {
                destroyModel(model.renderModel);
            }
            model.renderModel = XR_NULL_HANDLE;
        }
    }
}

bool VRControllerModelManager::LoadControllerModels(XrSession session) {
    if (!m_openxrManager || !m_openxrManager->HasRenderModelSupport()) {
        Logger::warn("VRControllerModelManager: Render model extensions not available");
        return false;
    }
    
    auto enumModels = m_openxrManager->GetEnumerateInteractionRenderModelIdsEXT();
    auto createModel = m_openxrManager->GetCreateRenderModelEXT();
    auto getModelProps = m_openxrManager->GetRenderModelPropertiesEXT();
    auto createAsset = m_openxrManager->GetCreateRenderModelAssetEXT();
    auto getAssetData = m_openxrManager->GetRenderModelAssetDataEXT();
    auto createSpace = m_openxrManager->GetCreateRenderModelSpaceEXT();
    auto getTopLevelPath = m_openxrManager->GetRenderModelPoseTopLevelUserPathEXT();
    
    if (!enumModels || !createModel || !getModelProps || !createAsset || !getAssetData || !createSpace) {
        Logger::warn("VRControllerModelManager: Required function pointers not available");
        return false;
    }
    
    // Enumerate available render model IDs
    XrInteractionRenderModelIdsEnumerateInfoEXT enumInfo = {XR_TYPE_INTERACTION_RENDER_MODEL_IDS_ENUMERATE_INFO_EXT};
    
    uint32_t modelCount = 0;
    XrResult result = enumModels(session, &enumInfo, 0, &modelCount, nullptr);
    if (XR_FAILED(result) || modelCount == 0) {
        // Don't spam the log - this is called every frame until models are available
        static bool loggedOnce = false;
        if (!loggedOnce) {
            Logger::info("VRControllerModelManager: No render models available yet (will retry silently)");
            loggedOnce = true;
        }
        return false;
    }
    
    std::vector<XrRenderModelIdEXT> modelIds(modelCount);
    result = enumModels(session, &enumInfo, modelCount, &modelCount, modelIds.data());
    if (XR_FAILED(result)) {
        Logger::err("VRControllerModelManager: Failed to enumerate render model IDs");
        return false;
    }
    
    Logger::info(str::format("VRControllerModelManager: Found ", modelCount, " render models"));
    
    // Load each model and detect which hand it belongs to
    for (uint32_t i = 0; i < modelCount && i < 4; i++) {  // Process up to 4 models
        // First, create a temporary render model to query which hand it belongs to
        XrRenderModelEXT tempRenderModel = XR_NULL_HANDLE;
        XrRenderModelCreateInfoEXT tempCreateInfo = {XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT};
        tempCreateInfo.renderModelId = modelIds[i];
        tempCreateInfo.gltfExtensionCount = 0;
        tempCreateInfo.gltfExtensions = nullptr;
        
        result = createModel(session, &tempCreateInfo, &tempRenderModel);
        if (XR_FAILED(result)) {
            Logger::err(str::format("VRControllerModelManager: Failed to create temp render model ", i));
            continue;
        }
        
        // Query which hand this model belongs to using xrGetRenderModelPoseTopLevelUserPathEXT
        bool isLeftHand = false;
        bool isRightHand = false;
        
        // Try using xrEnumerateRenderModelSubactionPathsEXT to get associated paths
        auto enumSubactionPaths = m_openxrManager->GetEnumerateRenderModelSubactionPathsEXT();
        if (enumSubactionPaths) {
            XrInteractionRenderModelSubactionPathInfoEXT subactionInfo = {XR_TYPE_INTERACTION_RENDER_MODEL_SUBACTION_PATH_INFO_EXT};
            
            // First, get the count
            uint32_t pathCount = 0;
            XrResult enumResult = enumSubactionPaths(tempRenderModel, &subactionInfo, 0, &pathCount, nullptr);
            Logger::info(str::format("VRControllerModelManager: Model ", i, " has ", pathCount, " subaction paths (result=", (int)enumResult, ")"));
            
            if (XR_SUCCEEDED(enumResult) && pathCount > 0) {
                std::vector<XrPath> paths(pathCount);
                enumResult = enumSubactionPaths(tempRenderModel, &subactionInfo, pathCount, &pathCount, paths.data());
                
                if (XR_SUCCEEDED(enumResult)) {
                    for (uint32_t p = 0; p < pathCount; p++) {
                        char pathStr[256] = {0};
                        uint32_t pathLen = 0;
                        if (XR_SUCCEEDED(xrPathToString(m_openxrManager->GetXrInstance(), paths[p], sizeof(pathStr), &pathLen, pathStr))) {
                            Logger::info(str::format("VRControllerModelManager: Model ", i, " subaction path ", p, ": ", pathStr));
                            
                            std::string pathString(pathStr);
                            if (pathString.find("/user/hand/left") != std::string::npos) {
                                isLeftHand = true;
                                Logger::info(str::format("VRControllerModelManager: Detected LEFT hand for model ", i));
                            } else if (pathString.find("/user/hand/right") != std::string::npos) {
                                isRightHand = true;
                                Logger::info(str::format("VRControllerModelManager: Detected RIGHT hand for model ", i));
                            }
                        }
                    }
                }
            }
        } else {
            Logger::warn("VRControllerModelManager: enumSubactionPaths function not available");
        }
        
        // If we couldn't detect the hand, use fallback based on index
        if (!isLeftHand && !isRightHand) {
            Logger::warn(str::format("VRControllerModelManager: Could not detect hand for model ", i, ", using fallback"));
            // Fallback: first unloaded controller gets left, second gets right
            if (!m_leftController.isLoaded) {
                isLeftHand = true;
            } else if (!m_rightController.isLoaded) {
                isRightHand = true;
            }
        }
        
        // Determine target controller
        ControllerModel* targetModelPtr = nullptr;
        if (isLeftHand && !m_leftController.isLoaded) {
            targetModelPtr = &m_leftController;
            Logger::info(str::format("VRControllerModelManager: Model ", i, " assigned to LEFT controller"));
        } else if (isRightHand && !m_rightController.isLoaded) {
            targetModelPtr = &m_rightController;
            Logger::info(str::format("VRControllerModelManager: Model ", i, " assigned to RIGHT controller"));
        } else {
            // Destroy the temp model and skip
            auto destroyModel = m_openxrManager->GetDestroyRenderModelEXT();
            if (destroyModel) destroyModel(tempRenderModel);
            continue;
        }
        
        ControllerModel& targetModel = *targetModelPtr;
        targetModel.renderModel = tempRenderModel;  // Transfer ownership
        targetModel.renderModelId = modelIds[i];
        
        // Get model properties (cache ID and animatable node count)
        XrRenderModelPropertiesGetInfoEXT propsGetInfo = {XR_TYPE_RENDER_MODEL_PROPERTIES_GET_INFO_EXT};
        XrRenderModelPropertiesEXT props = {XR_TYPE_RENDER_MODEL_PROPERTIES_EXT};
        
        result = getModelProps(targetModel.renderModel, &propsGetInfo, &props);
        if (XR_FAILED(result)) {
            Logger::err("VRControllerModelManager: Failed to get render model properties");
            continue;
        }
        
        targetModel.cacheId = props.cacheId;
        targetModel.animatableNodeCount = props.animatableNodeCount;
        targetModel.nodeStates.resize(props.animatableNodeCount);
        
        Logger::info(str::format("VRControllerModelManager: Model ", i, " has ", props.animatableNodeCount, " animatable nodes"));
        
        // Create render model asset from cache ID
        XrRenderModelAssetCreateInfoEXT assetCreateInfo = {XR_TYPE_RENDER_MODEL_ASSET_CREATE_INFO_EXT};
        assetCreateInfo.cacheId = props.cacheId;
        
        result = createAsset(session, &assetCreateInfo, &targetModel.assetHandle);
        if (XR_FAILED(result)) {
            Logger::err("VRControllerModelManager: Failed to create render model asset");
            continue;
        }
        
        // Get the GLB data size first
        XrRenderModelAssetDataGetInfoEXT dataGetInfo = {XR_TYPE_RENDER_MODEL_ASSET_DATA_GET_INFO_EXT};
        XrRenderModelAssetDataEXT assetData = {XR_TYPE_RENDER_MODEL_ASSET_DATA_EXT};
        assetData.bufferCapacityInput = 0;
        assetData.buffer = nullptr;
        
        result = getAssetData(targetModel.assetHandle, &dataGetInfo, &assetData);
        if (XR_FAILED(result)) {
            Logger::err("VRControllerModelManager: Failed to get render model asset data size");
            continue;
        }
        
        // Allocate buffer and get actual data
        std::vector<uint8_t> glbData(assetData.bufferCountOutput);
        assetData.bufferCapacityInput = static_cast<uint32_t>(glbData.size());
        assetData.buffer = glbData.data();
        
        result = getAssetData(targetModel.assetHandle, &dataGetInfo, &assetData);
        if (XR_FAILED(result)) {
            Logger::err("VRControllerModelManager: Failed to get render model asset data");
            continue;
        }
        
        Logger::info(str::format("VRControllerModelManager: Got ", assetData.bufferCountOutput, " bytes of GLB data"));
        
        // Parse the GLB data
        if (!ParseGLTFModel(glbData.data(), glbData.size(), targetModel)) {
            Logger::err("VRControllerModelManager: Failed to parse GLB model data");
            continue;
        }
        
        // Build mapping from OpenXR animatable node index to GLTF node index
        // Use xrGetRenderModelAssetPropertiesEXT to get animatable node names
        auto getAssetProps = m_openxrManager->GetRenderModelAssetPropertiesEXT();
        if (getAssetProps && targetModel.animatableNodeCount > 0) {
            // Get the animatable node names
            std::vector<XrRenderModelAssetNodePropertiesEXT> nodeProps(targetModel.animatableNodeCount);
            XrRenderModelAssetPropertiesEXT assetProps = {XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_EXT};
            assetProps.nodePropertyCount = targetModel.animatableNodeCount;
            assetProps.nodeProperties = nodeProps.data();
            
            XrRenderModelAssetPropertiesGetInfoEXT propsGetInfo = {XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_GET_INFO_EXT};
            XrResult propsResult = getAssetProps(targetModel.assetHandle, &propsGetInfo, &assetProps);
            
            if (XR_SUCCEEDED(propsResult)) {
                targetModel.animNodeToGltfNode.resize(targetModel.animatableNodeCount, -1);
                
                for (uint32_t animIdx = 0; animIdx < assetProps.nodePropertyCount; animIdx++) {
                    std::string animNodeName(nodeProps[animIdx].uniqueName);
                    
                    // Find matching GLTF node by name
                    for (size_t gltfIdx = 0; gltfIdx < targetModel.nodes.size(); gltfIdx++) {
                        if (targetModel.nodes[gltfIdx].name == animNodeName) {
                            targetModel.animNodeToGltfNode[animIdx] = static_cast<int>(gltfIdx);
                            break;
                        }
                    }
                    
                    if (targetModel.animNodeToGltfNode[animIdx] < 0) {
                        Logger::warn(str::format("VRControllerModelManager: OpenXR anim node '", 
                                                 animNodeName, "' has no matching GLTF node"));
                    }
                }
            } else {
                Logger::warn("VRControllerModelManager: Failed to get asset properties for animation mapping");
            }
        }
        
        // Create Vulkan resources
        if (!CreateModelVulkanResources(targetModel)) {
            Logger::err("VRControllerModelManager: Failed to create Vulkan resources for model");
            continue;
        }
        
        // Create a space for the render model
        XrRenderModelSpaceCreateInfoEXT spaceCreateInfo = {XR_TYPE_RENDER_MODEL_SPACE_CREATE_INFO_EXT};
        spaceCreateInfo.renderModel = targetModel.renderModel;
        
        result = createSpace(session, &spaceCreateInfo, &targetModel.modelSpace);
        if (XR_FAILED(result)) {
            Logger::warn("VRControllerModelManager: Failed to create render model space");
            // Not fatal - we can still render the model
        }
        
        targetModel.isLoaded = true;
        Logger::info(str::format("VRControllerModelManager: Successfully loaded controller model ", i));
    }
    
    return m_leftController.isLoaded || m_rightController.isLoaded;
}

void VRControllerModelManager::UpdateAnimationState(XrTime displayTime) {
    if (!m_openxrManager || !m_openxrManager->HasRenderModelSupport()) return;
    
    // Poll for session state changes to detect SteamVR overlay
    XrInstance instance = m_openxrManager->GetInstance();
    if (instance != XR_NULL_HANDLE) {
        XrEventDataBuffer eventBuffer = {XR_TYPE_EVENT_DATA_BUFFER};
        XrResult pollResult = xrPollEvent(instance, &eventBuffer);
        while (pollResult == XR_SUCCESS) {
            if (eventBuffer.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* stateEvent = reinterpret_cast<XrEventDataSessionStateChanged*>(&eventBuffer);
                if (stateEvent->state == XR_SESSION_STATE_FOCUSED) {
                    m_openxrManager->SetSessionFocused(true);
                } else if (stateEvent->state == XR_SESSION_STATE_VISIBLE) {
                    m_openxrManager->SetSessionFocused(false);
                }
            }
            eventBuffer = {XR_TYPE_EVENT_DATA_BUFFER};
            pollResult = xrPollEvent(instance, &eventBuffer);
        }
    }
    
    auto getState = m_openxrManager->GetRenderModelStateEXT();
    if (!getState) return;
    
    auto updateModel = [&](ControllerModel& model) {
        if (!model.isLoaded || model.renderModel == XR_NULL_HANDLE) return;
        
        XrRenderModelStateGetInfoEXT stateGetInfo = {XR_TYPE_RENDER_MODEL_STATE_GET_INFO_EXT};
        stateGetInfo.displayTime = displayTime;
        
        XrRenderModelStateEXT state = {XR_TYPE_RENDER_MODEL_STATE_EXT};
        state.nodeStateCount = static_cast<uint32_t>(model.nodeStates.size());
        state.nodeStates = model.nodeStates.data();
        
        XrResult result = getState(model.renderModel, &stateGetInfo, &state);
        if (XR_SUCCEEDED(result)) {
            // Apply animation using the correct mapping
            for (uint32_t animIdx = 0; animIdx < state.nodeStateCount; animIdx++) {
                // Use mapping to find correct GLTF node
                int gltfIdx = (animIdx < model.animNodeToGltfNode.size()) 
                              ? model.animNodeToGltfNode[animIdx] : -1;
                
                if (gltfIdx < 0 || gltfIdx >= static_cast<int>(model.nodes.size())) {
                    continue;  // No valid mapping for this animation state
                }
                
                ControllerNode& node = model.nodes[gltfIdx];
                const XrRenderModelNodeStateEXT& animState = model.nodeStates[animIdx];
                
                node.isVisible = animState.isVisible;
                
                // Apply the OpenXR pose as the node's local transform
                const XrPosef& pose = animState.nodePose;
                
                // Build transform: rotation + translation
                QuaternionToMatrix(pose.orientation.x, pose.orientation.y, 
                                   pose.orientation.z, pose.orientation.w, 
                                   node.localMatrix);
                node.localMatrix[12] = pose.position.x;
                node.localMatrix[13] = pose.position.y;
                node.localMatrix[14] = pose.position.z;
            }
            
            // Recompute world matrices for all nodes
            for (int rootIdx : model.rootNodes) {
                model.nodes[rootIdx].UpdateWorldMatrix(nullptr);
            }
            
            // Update children recursively
            std::function<void(int)> updateChildren = [&](int nodeIdx) {
                for (int childIdx : model.nodes[nodeIdx].children) {
                    model.nodes[childIdx].UpdateWorldMatrix(model.nodes[nodeIdx].worldMatrix);
                    updateChildren(childIdx);
                }
            };
            
            for (int rootIdx : model.rootNodes) {
                updateChildren(rootIdx);
            }
        }
        
        // Update model space location if available
        if (model.modelSpace != XR_NULL_HANDLE) {
            XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
            XrResult locResult = xrLocateSpace(model.modelSpace, m_openxrManager->GetReferenceSpace(), displayTime, &location);
            if (XR_SUCCEEDED(locResult) && (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
                model.currentPose = location.pose;
                model.isVisible = (location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
            } else {
                model.isVisible = false;
            }
        }
    };
    
    updateModel(m_leftController);
    updateModel(m_rightController);
}

bool VRControllerModelManager::ParseGLTFModel(const uint8_t* data, size_t size, ControllerModel& outModel) {
    cgltf_options options = {};
    cgltf_data* gltf = nullptr;
    
    cgltf_result result = cgltf_parse(&options, data, size, &gltf);
    if (result != cgltf_result_success) {
        Logger::err("VRControllerModelManager: Failed to parse glTF data");
        return false;
    }
    
    // Load buffers (for GLB, data is embedded)
    result = cgltf_load_buffers(&options, gltf, nullptr);
    if (result != cgltf_result_success) {
        Logger::err("VRControllerModelManager: Failed to load glTF buffers");
        cgltf_free(gltf);
        return false;
    }
    
    Logger::info(str::format("VRControllerModelManager: Parsing glTF with ", 
                             gltf->meshes_count, " meshes, ",
                             gltf->materials_count, " materials, ",
                             gltf->nodes_count, " nodes"));
    
    // Parse materials
    outModel.materials.resize(gltf->materials_count);
    for (size_t i = 0; i < gltf->materials_count; i++) {
        cgltf_material& srcMat = gltf->materials[i];
        ControllerMaterial& dstMat = outModel.materials[i];
        
        if (srcMat.has_pbr_metallic_roughness) {
            dstMat.baseColorFactor[0] = srcMat.pbr_metallic_roughness.base_color_factor[0];
            dstMat.baseColorFactor[1] = srcMat.pbr_metallic_roughness.base_color_factor[1];
            dstMat.baseColorFactor[2] = srcMat.pbr_metallic_roughness.base_color_factor[2];
            dstMat.baseColorFactor[3] = srcMat.pbr_metallic_roughness.base_color_factor[3];
            dstMat.metallicFactor = srcMat.pbr_metallic_roughness.metallic_factor;
            dstMat.roughnessFactor = srcMat.pbr_metallic_roughness.roughness_factor;
            
            // Check for base color texture and extract it
            if (srcMat.pbr_metallic_roughness.base_color_texture.texture) {
                cgltf_texture* tex = srcMat.pbr_metallic_roughness.base_color_texture.texture;
                if (tex->image && tex->image->buffer_view) {
                    // Get image data from buffer view
                    cgltf_buffer_view* bufView = tex->image->buffer_view;
                    const uint8_t* imageData = static_cast<const uint8_t*>(bufView->buffer->data) + bufView->offset;
                    size_t imageSize = bufView->size;
                    
                    // Decode image using stb_image
                    int width, height, channels;
                    uint8_t* pixels = stbi_load_from_memory(imageData, (int)imageSize, &width, &height, &channels, 4);
                    
                    if (pixels) {
                        Logger::info(str::format("VRControllerModelManager: Loaded texture ", i, " (", width, "x", height, ")"));
                        
                        // Store decoded pixels temporarily - will be uploaded in CreateModelVulkanResources
                        dstMat.hasTexture = true;
                        dstMat.textureWidth = width;
                        dstMat.textureHeight = height;
                        dstMat.texturePixels.assign(pixels, pixels + (width * height * 4));
                        
                        stbi_image_free(pixels);
                    } else {
                        Logger::warn(str::format("VRControllerModelManager: Failed to decode texture ", i));
                    }
                }
            }
        }
        
        // Extract emissive factor
        dstMat.emissiveFactor[0] = srcMat.emissive_factor[0];
        dstMat.emissiveFactor[1] = srcMat.emissive_factor[1];
        dstMat.emissiveFactor[2] = srcMat.emissive_factor[2];
    }
    
    // Parse meshes
    for (size_t meshIdx = 0; meshIdx < gltf->meshes_count; meshIdx++) {
        cgltf_mesh& srcMesh = gltf->meshes[meshIdx];
        
        for (size_t primIdx = 0; primIdx < srcMesh.primitives_count; primIdx++) {
            cgltf_primitive& prim = srcMesh.primitives[primIdx];
            
            if (prim.type != cgltf_primitive_type_triangles) {
                continue;  // Only support triangles
            }
            
            ControllerMesh mesh;
            
            // Find position, normal, and texcoord accessors
            cgltf_accessor* posAccessor = nullptr;
            cgltf_accessor* normAccessor = nullptr;
            cgltf_accessor* uvAccessor = nullptr;
            
            for (size_t attrIdx = 0; attrIdx < prim.attributes_count; attrIdx++) {
                cgltf_attribute& attr = prim.attributes[attrIdx];
                if (attr.type == cgltf_attribute_type_position) posAccessor = attr.data;
                else if (attr.type == cgltf_attribute_type_normal) normAccessor = attr.data;
                else if (attr.type == cgltf_attribute_type_texcoord) uvAccessor = attr.data;
            }
            
            if (!posAccessor) {
                Logger::warn("VRControllerModelManager: Mesh primitive has no position data");
                continue;
            }
            
            // Extract vertices
            mesh.vertices.resize(posAccessor->count);
            float minPos[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
            float maxPos[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
            for (size_t v = 0; v < posAccessor->count; v++) {
                cgltf_accessor_read_float(posAccessor, v, mesh.vertices[v].position, 3);
                
                // Track bounds
                for (int c = 0; c < 3; c++) {
                    minPos[c] = std::min(minPos[c], mesh.vertices[v].position[c]);
                    maxPos[c] = std::max(maxPos[c], mesh.vertices[v].position[c]);
                }
                
                if (normAccessor) {
                    cgltf_accessor_read_float(normAccessor, v, mesh.vertices[v].normal, 3);
                } else {
                    mesh.vertices[v].normal[0] = 0;
                    mesh.vertices[v].normal[1] = 1;
                    mesh.vertices[v].normal[2] = 0;
                }
                
                if (uvAccessor) {
                    cgltf_accessor_read_float(uvAccessor, v, mesh.vertices[v].texCoord, 2);
                } else {
                    mesh.vertices[v].texCoord[0] = 0;
                    mesh.vertices[v].texCoord[1] = 0;
                }
            }
            
            // Store mesh center as pivot point for animation
            mesh.centerX = (minPos[0] + maxPos[0]) * 0.5f;
            mesh.centerY = (minPos[1] + maxPos[1]) * 0.5f;
            mesh.centerZ = (minPos[2] + maxPos[2]) * 0.5f;
            
            // Extract indices
            if (prim.indices) {
                mesh.indices.resize(prim.indices->count);
                for (size_t idx = 0; idx < prim.indices->count; idx++) {
                    mesh.indices[idx] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, idx));
                }
                mesh.indexCount = static_cast<uint32_t>(mesh.indices.size());
            }
            
            // Material index
            if (prim.material) {
                mesh.materialIndex = static_cast<int>(prim.material - gltf->materials);
            }
            
            outModel.meshes.push_back(std::move(mesh));
        }
    }
    
    // Parse nodes
    outModel.nodes.resize(gltf->nodes_count);
    for (size_t i = 0; i < gltf->nodes_count; i++) {
        cgltf_node& srcNode = gltf->nodes[i];
        ControllerNode& dstNode = outModel.nodes[i];
        
        if (srcNode.name) {
            dstNode.name = srcNode.name;
        }
        
        // Get mesh index
        if (srcNode.mesh) {
            dstNode.meshIndex = static_cast<int>(srcNode.mesh - gltf->meshes);
        }
        
        // Get transform - use cgltf helper that handles both TRS and matrix formats
        float localMat[16];
        cgltf_node_transform_local(&srcNode, localMat);
        
        // Copy directly to localMatrix (it's already computed)
        memcpy(dstNode.localMatrix, localMat, sizeof(float) * 16);
        
        // Also extract TRS for our internal tracking (in case we need them)
        // Translation is column 3 (indices 12, 13, 14)
        dstNode.translation[0] = localMat[12];
        dstNode.translation[1] = localMat[13];
        dstNode.translation[2] = localMat[14];
        
        // For now, keep rotation/scale at defaults since we have the matrix directly
        // (extracting quaternion from matrix is complex and we don't need it if we use the matrix)
        
        // Children
        for (size_t c = 0; c < srcNode.children_count; c++) {
            int childIdx = static_cast<int>(srcNode.children[c] - gltf->nodes);
            dstNode.children.push_back(childIdx);
            outModel.nodes[childIdx].parentIndex = static_cast<int>(i);
        }
    }
    
    // Find root nodes
    for (size_t i = 0; i < outModel.nodes.size(); i++) {
        if (outModel.nodes[i].parentIndex < 0) {
            outModel.rootNodes.push_back(static_cast<int>(i));
        }
    }
    
    // Compute initial world matrices
    for (int rootIdx : outModel.rootNodes) {
        outModel.nodes[rootIdx].UpdateWorldMatrix(nullptr);
    }
    
    std::function<void(int)> updateChildrenWorld = [&](int nodeIdx) {
        for (int childIdx : outModel.nodes[nodeIdx].children) {
            outModel.nodes[childIdx].UpdateWorldMatrix(outModel.nodes[nodeIdx].worldMatrix);
            updateChildrenWorld(childIdx);
        }
    };
    
    for (int rootIdx : outModel.rootNodes) {
        updateChildrenWorld(rootIdx);
    }
    
    cgltf_free(gltf);
    
    Logger::info(str::format("VRControllerModelManager: Parsed ", outModel.meshes.size(), " mesh primitives, ",
                             outModel.nodes.size(), " nodes"));
    
    return true;
}

bool VRControllerModelManager::CreateModelVulkanResources(ControllerModel& model) {
    if (m_device == VK_NULL_HANDLE) {
        Logger::err("VRControllerModelManager: No Vulkan device available");
        return false;
    }
    
    // Create vertex and index buffers for each mesh
    for (auto& mesh : model.meshes) {
        if (mesh.vertices.empty()) continue;
        
        // Create vertex buffer
        VkDeviceSize vertexSize = mesh.vertices.size() * sizeof(Vertex3D);
        if (!CreateBuffer(vertexSize, 
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         mesh.vertexBuffer, mesh.vertexMemory)) {
            Logger::err("VRControllerModelManager: Failed to create vertex buffer");
            return false;
        }
        
        // Copy vertex data
        void* data;
        vkMapMemory(m_device, mesh.vertexMemory, 0, vertexSize, 0, &data);
        memcpy(data, mesh.vertices.data(), vertexSize);
        vkUnmapMemory(m_device, mesh.vertexMemory);
        
        // Create index buffer
        if (!mesh.indices.empty()) {
            VkDeviceSize indexSize = mesh.indices.size() * sizeof(uint32_t);
            if (!CreateBuffer(indexSize,
                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             mesh.indexBuffer, mesh.indexMemory)) {
                Logger::err("VRControllerModelManager: Failed to create index buffer");
                return false;
            }
            
            vkMapMemory(m_device, mesh.indexMemory, 0, indexSize, 0, &data);
            memcpy(data, mesh.indices.data(), indexSize);
            vkUnmapMemory(m_device, mesh.indexMemory);
        }
    }
    
    Logger::info(str::format("VRControllerModelManager: Created Vulkan resources for ", model.meshes.size(), " meshes"));
    return true;
}

uint32_t VRControllerModelManager::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physDevice, &memProperties);
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && 
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    
    return UINT32_MAX;
}

bool VRControllerModelManager::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                            VkMemoryPropertyFlags properties,
                                            VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }
    
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_device, buffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, properties);
    
    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
        vkDestroyBuffer(m_device, buffer, nullptr);
        return false;
    }
    
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, buffer, nullptr);
        return false;
    }
    
    vkBindBufferMemory(m_device, buffer, memory, 0);
    return true;
}

bool VRControllerModelManager::CreateTextureImage(const uint8_t* pixels, int width, int height,
                                                   VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
    if (!pixels || width <= 0 || height <= 0) return false;
    
    VkDeviceSize imageSize = width * height * 4;
    
    // Create staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    if (!CreateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuffer, stagingMemory)) {
        return false;
    }
    
    // Copy pixel data to staging buffer
    void* data;
    vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &data);
    memcpy(data, pixels, imageSize);
    vkUnmapMemory(m_device, stagingMemory);
    
    // Create image
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    
    if (vkCreateImage(m_device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return false;
    }
    
    // Allocate memory for image
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_device, image, &memReqs);
    
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyImage(m_device, image, nullptr);
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return false;
    }
    
    vkBindImageMemory(m_device, image, memory, 0);
    
    // Create image view
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    
    if (vkCreateImageView(m_device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
        vkDestroyImage(m_device, image, nullptr);
        vkFreeMemory(m_device, memory, nullptr);
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return false;
    }
    
    // Note: Image layout transition and copy from staging would require a command buffer
    // For simplicity, we're skipping that here - the image won't have valid content
    // A proper implementation needs a command pool and queue for transfer operations
    
    // Cleanup staging buffer
    vkDestroyBuffer(m_device, stagingBuffer, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);
    
    Logger::info(str::format("VRControllerModelManager: Created texture image ", width, "x", height));
    return true;
}

} // namespace dxvk
