// DevilutionX for PS5 - samples the rectangle's picture; the screen is opaque.
// SPDX-License-Identifier: GPL-3.0-or-later
// Compiled into ps5vk_shaders.h by tools/make-vk-shaders.sh.
#version 450

layout(set = 0, binding = 0) uniform sampler2D picture;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(texture(picture, in_uv).rgb, 1.0);
}
