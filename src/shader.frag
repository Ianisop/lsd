#version 330 core

uniform sampler2D atlasRegular;
uniform sampler2D atlasBold;
uniform sampler2D atlasItalic;
uniform sampler2D atlasBoldItalic;

in vec2  vUV;
in vec3  vFG;
in vec3  vBG;
in float vIsGlyph;
in float vStyleIndex;

out vec4 FragColor;

void main()
{
    int style = int(round(vStyleIndex));

    // Colored cell background (semi-transparent, vFG carries the bg color)
    if(style == 96)
    {
        FragColor = vec4(vFG, 0.8);
        return;
    }

    // Cursor solid white block
    if(style == 99)
    {
        FragColor = vec4(1.0, 1.0, 1.0, 1.0);
        return;
    }

    // Cursor glyph — black ink on white block
    if(style == 98)
    {
        float alpha = texture(atlasRegular, vUV).r;
        FragColor = vec4(0.0, 0.0, 0.0, alpha);
        return;
    }

    // Normal glyph — transparent except at ink pixels
    if(vIsGlyph > 0.5)
    {
        float alpha;
        if     (style == 0) alpha = texture(atlasRegular,    vUV).r;
        else if(style == 1) alpha = texture(atlasBold,       vUV).r;
        else if(style == 2) alpha = texture(atlasItalic,     vUV).r;
        else                alpha = texture(atlasBoldItalic, vUV).r;
        FragColor = vec4(vFG, alpha);
        return;
    }

    FragColor = vec4(0.0);
}
