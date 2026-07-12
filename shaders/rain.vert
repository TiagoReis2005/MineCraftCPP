#version 450

layout(location = 0) in vec3 inPos; // world space, rebuilt per frame around the player
layout(location = 1) in vec2 inUV;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} cam;

layout(push_constant) uniform Push {
    vec4 tint;   // a = alpha (rain intensity / cloud opacity)
    vec4 offset; // world offset (cloud anchor + drift; zero for rain)
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vTint;

void main() {
    gl_Position = cam.proj * cam.view * vec4(inPos + pc.offset.xyz, 1.0);
    vUV = inUV;
    vTint = pc.tint;
}
