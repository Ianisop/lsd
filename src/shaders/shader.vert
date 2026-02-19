#version 330 core

layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 fgColor;
layout(location = 3) in vec3 bgColor;
layout(location = 4) in float isGlyph;
layout(location = 5) in float styleIndex;  // Add this

out vec2 TexCoords;
out vec3 vFg;
out vec3 vBg;
out float vIsGlyph;
out float vStyleIndex;  // Pass to fragment shader

void main()
{
    gl_Position = vec4(pos, 0.0, 1.0);
    TexCoords   = uv;
    vFg         = fgColor;
    vBg         = bgColor;
    vIsGlyph    = isGlyph;
    vStyleIndex = styleIndex;  // Pass through
}