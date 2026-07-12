#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in float inLayer;
layout(location = 3) in float inLight;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} cam;

layout(push_constant) uniform Push {
    mat4 model; // per-block gui display transform (from the model json)
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out float vLayer;
layout(location = 2) out float vLight;

void main() {
    gl_Position = cam.proj * cam.view * pc.model * vec4(inPos, 1.0);
    vUV = inUV;
    vLayer = inLayer;
    vLight = inLight;
}
