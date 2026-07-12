#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} cam;

layout(push_constant) uniform Push {
    mat4 model; // camera-centered celestial rotation + placement
    vec4 tint;  // rgb tint, a = fade (stars)
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vTint;

void main() {
    gl_Position = cam.proj * cam.view * pc.model * vec4(inPos, 1.0);
    vUV = inUV;
    vTint = pc.tint;
}
