#version 450

layout(set = 0, binding = 1) uniform sampler2D uSkin;

layout(push_constant) uniform Push {
    layout(offset = 64) vec4 tint; // rgb multiplier + alpha (spectator ghost = 0.5)
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in float vShade;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = texture(uSkin, vUV);
    if (c.a < 0.1) discard; // transparent parts of the skin's outer layer
    outColor = vec4(c.rgb * vShade * pc.tint.rgb, pc.tint.a);
}
