#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec3 fragColor;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;      // Main VGUI content texture
layout(binding = 1) uniform sampler2D alphaMaskSampler; // Rounded corner mask texture

layout(push_constant) uniform ShaderParams {
    mat4 mvpMatrix;          // 64 bytes (already used by vertex shader)
    float hasAlphaMask;      // 4 bytes offset 64
    float fullOpacity;       // 4 bytes offset 68  
    float padding1;          // 4 bytes offset 72
    float padding2;          // 4 bytes offset 76
} pc;

void main() {
    // Sample base texture (VGUI content)
    vec4 baseColor = texture(texSampler, fragTexCoord);
    
    // Apply vertex color blending for flexibility
    vec4 result = mix(vec4(fragColor, 1.0), baseColor, baseColor.a);
    
    // Apply rounded corner alpha mask if enabled
    if (pc.hasAlphaMask > 0.5) {
        // Sample the alpha mask texture
        vec4 alphaMask = texture(alphaMaskSampler, fragTexCoord);
        
        if (pc.fullOpacity > 0.5) {
            // Full opacity mode: use mask as cutout (0 = transparent, >0 = fully opaque)
            result.a = (alphaMask.r > 0.5) ? result.a : 0.0;
        } else {
            // Normal blending mode: use mask to modulate existing alpha
            result.a *= alphaMask.r;
        }
    }
    
    outColor = result;
}