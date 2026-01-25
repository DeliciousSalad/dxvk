#version 450

// Inputs from vertex shader
layout(location = 0) in vec3 fragWorldNormal;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragWorldPos;

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
    float hasTexture = pc.emissive.w;
    if (hasTexture > 0.5) {
        vec4 texColor = texture(baseColorTexture, fragTexCoord);
        albedo = texColor * pc.baseColor;
    } else {
        albedo = pc.baseColor;
    }
    
    // Normalize the interpolated normal
    vec3 normal = normalize(fragWorldNormal);
    
    // SteamVR-style lighting - balanced ambient with good contrast
    // Primary light from upper-front-right
    vec3 lightDir1 = normalize(vec3(0.4, 0.8, 0.4));
    float diffuse1 = max(dot(normal, lightDir1), 0.0);
    
    // Secondary fill light from upper-left
    vec3 lightDir2 = normalize(vec3(-0.3, 0.6, 0.2));
    float diffuse2 = max(dot(normal, lightDir2), 0.0) * 0.4;
    
    // Balanced ambient - not too dark, not too bright
    float ambient = 0.25;
    float lighting = ambient + diffuse1 * 0.65 + diffuse2;
    
    // Apply lighting to albedo
    vec3 finalColor = albedo.rgb * lighting;
    
    // Add emissive (unaffected by lighting - for LED indicators, buttons, etc.)
    finalColor += pc.emissive.rgb;
    
    // Subtle rim lighting for depth (reduced intensity)
    vec3 viewDir = normalize(-fragWorldPos);
    float rim = 1.0 - max(dot(viewDir, normal), 0.0);
    rim = pow(rim, 4.0) * 0.08;
    finalColor += vec3(rim);
    
    // Clamp to avoid over-brightening
    finalColor = min(finalColor, vec3(1.0));
    
    outColor = vec4(finalColor, albedo.a);
}
