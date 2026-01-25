#version 450

// Inputs from vertex shader
layout(location = 0) in vec3 fragWorldNormal;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragWorldPos;

// Output color
layout(location = 0) out vec4 outColor;

// Push constants
layout(push_constant) uniform PushConstants {
    mat4 mvpMatrix;
    mat4 modelMatrix;
    vec4 baseColor;
} pc;

void main() {
    // DEBUG: Force bright magenta output to verify geometry renders
    outColor = vec4(1.0, 0.0, 1.0, 1.0);
    return;
    
    // Normalize the interpolated normal
    vec3 normal = normalize(fragWorldNormal);
    
    // Simple directional lighting from above
    vec3 lightDir = normalize(vec3(0.3, 1.0, 0.5));
    float ambient = 0.3;
    float diffuse = max(dot(normal, lightDir), 0.0);
    float lighting = ambient + diffuse * 0.7;
    
    // Apply lighting to base color
    vec3 finalColor = pc.baseColor.rgb * lighting;
    
    // Add slight rim lighting for better depth perception
    vec3 viewDir = normalize(-fragWorldPos);
    float rim = 1.0 - max(dot(viewDir, normal), 0.0);
    rim = pow(rim, 3.0) * 0.15;
    finalColor += vec3(rim);
    
    outColor = vec4(finalColor, pc.baseColor.a);
}
