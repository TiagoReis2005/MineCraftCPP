#version 450

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    // Solid quads sample a 1x1 white texture, so the result is just vColor.
    // Textured quads (block icons, M5b) sample an atlas and tint by vColor.
    outColor = texture(uTex, vUV) * vColor;
}
