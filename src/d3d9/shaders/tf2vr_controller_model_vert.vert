#version 450

// Vertex inputs
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;  // Kept for vertex format compatibility
layout(location = 2) in vec2 inTexCoord;

// Outputs to fragment shader
layout(location = 0) out vec2 fragTexCoord;

// Push constants for MVP and model matrices
layout(push_constant) uniform PushConstants {
    mat4 mvpMatrix;      // View-Projection matrix
    mat4 modelMatrix;    // Controller pose transformation
    vec4 baseColor;      // Base color factor
} pc;

void main() {
    // Transform local vertex to world space, then to clip space
    vec4 worldPos = pc.modelMatrix * vec4(inPosition, 1.0);
    gl_Position = pc.mvpMatrix * worldPos;
    
    fragTexCoord = inTexCoord;
}
