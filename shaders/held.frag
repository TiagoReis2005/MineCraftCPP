#version 450

layout(set = 0, binding = 1) uniform sampler2DArray uTex;

layout(location = 0) in vec2 vUV;
layout(location = 1) in float vLayer;
layout(location = 2) in float vLight;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(uTex, vec3(vUV, vLayer));
    if (tex.a < 0.5) discard; // cutout (leaves/glass)
    outColor = vec4(tex.rgb * vLight, 1.0);
}
