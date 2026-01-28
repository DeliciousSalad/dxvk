#version 450

// Inputs from vertex shader
layout(location = 0) in vec2 fragTexCoord;

// Output color
layout(location = 0) out vec4 outColor;

// Texture sampler (binding 0, set 0)
layout(set = 0, binding = 0) uniform sampler2D baseColorTexture;

// Push constants
layout(push_constant) uniform PushConstants {
    mat4 mvpMatrix;
    mat4 modelMatrix;
    vec4 baseColor;
    vec4 emissive;  // RGB = emissive color, W = hasTexture flag
} pc;

void main() {
    // Sample texture if available, otherwise use base color
    vec4 albedo;
    if (pc.emissive.w > 0.5) {
        albedo = texture(baseColorTexture, fragTexCoord) * pc.baseColor;
    } else {
        albedo = pc.baseColor;
    }
    
    // Unlit rendering - output albedo + emissive
    vec3 finalColor = min(albedo.rgb + pc.emissive.rgb, vec3(1.0));
    
    outColor = vec4(finalColor, albedo.a);
}
