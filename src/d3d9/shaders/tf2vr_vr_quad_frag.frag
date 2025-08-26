#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec3 fragColor;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

void main() {
    vec4 texColor = texture(texSampler, fragTexCoord);
    
    // Blend texture with vertex color for flexibility
    // If texture is transparent or missing, fall back to vertex color
    outColor = mix(vec4(fragColor, 1.0), texColor, texColor.a);
}