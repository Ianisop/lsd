#version 330 core

uniform sampler2D atlasRegular;
uniform sampler2D atlasBold;
uniform sampler2D atlasItalic;
uniform sampler2D atlasBoldItalic;

in vec2  TexCoords;  // Renamed to match vertex shader output
in vec3  vFg;
in vec3  vBg;
in float vIsGlyph;
in float vStyleIndex;  // Add this

out vec4 FragColor;

void main()
{
    int style = int(round(vStyleIndex));

    // Colored cell background (semi-transparent, vFG carries the bg color)
    if(style == 96)
    {
        FragColor = vec4(vFg, 0.8);  // vFG carries bg color for background cells
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
        float alpha = texture(atlasRegular, TexCoords).r;
        FragColor = vec4(0.0, 0.0, 0.0, alpha);
        return;
    }

    // Normal glyph — transparent except at ink pixels
    if(vIsGlyph > 0.5)
    {
        float alpha;
        if      (style == 0) alpha = texture(atlasRegular, TexCoords).r;
        else if (style == 1) alpha = texture(atlasBold, TexCoords).r;
        else if (style == 2) alpha = texture(atlasItalic, TexCoords).r;
        else if (style == 3) alpha = texture(atlasBoldItalic, TexCoords).r;
        else alpha = texture(atlasRegular, TexCoords).r;  // Fallback

        FragColor = vec4(vFg, alpha);
        return;
    }

    FragColor = vec4(0.0);
}