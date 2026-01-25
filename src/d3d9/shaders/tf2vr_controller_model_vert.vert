#version 450

// Vertex inputs
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

// Outputs to fragment shader
layout(location = 0) out vec3 fragWorldNormal;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec3 fragWorldPos;

// Push constants for MVP and model matrices
layout(push_constant) uniform PushConstants {
    mat4 mvpMatrix;      // Model-View-Projection matrix
    mat4 modelMatrix;    // Model matrix (for normal transform)
    vec4 baseColor;      // Base color factor
} pc;

void main() {
    // The push constants now contain:
    // - mvpMatrix: actually contains viewProj (same as quad uses)
    // - modelMatrix: the controller pose transformation
    
    // Step 1: Transform local vertex to world space using model matrix
    vec4 worldPos = pc.modelMatrix * vec4(inPosition, 1.0);
    
    // Step 2: Transform world position to clip space using viewProj
    // This is the same transformation the quad uses
    gl_Position = pc.mvpMatrix * worldPos;
    
    // Transform normal to world space
    mat3 normalMatrix = mat3(pc.modelMatrix);
    fragWorldNormal = normalize(normalMatrix * inNormal);
    
    fragTexCoord = inTexCoord;
    fragWorldPos = worldPos.xyz;
}
