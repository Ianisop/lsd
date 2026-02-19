#version 330 core

// Uniforms provided automatically by the terminal
uniform float uTime;
uniform vec2  uResolution; // framebuffer size in pixels

in vec2 vUV; // 0..1 across screen

out vec4 FragColor;

void main()
{
    // Reconstruct shadertoy-style normalised coords
    vec2 I = (vUV * 2.0 - 1.0) * vec2(uResolution.x / uResolution.y, 1.0);

    vec3 r = normalize(vec3(I, -1.5));
    vec3 a = normalize(tan(uTime * 0.2 + vec3(0, 1, 2)));

    vec4 O  = vec4(0.0);
    float t = 0.0, v = 1.0, l = 0.0;

    for(float i = 0.0; i < 70.0; i++, t += v * 0.04)
    {
        vec3 p = t * r;
        p.z += 6.0;
        p = a * dot(a, p) * 2.0 - p;
        l = dot(p, p);

        for(float n = 0.0; n < 4.0; n++)
            p.xz = abs(p.xz) / l - 0.4;

        v = dot(p, p);
        O += 0.005 / v * (0.5 + 0.5 * cos(v * 3.0 + uTime + vec4(0, 1, 2, 0)));
    }

    FragColor = vec4(clamp(O.rgb * 0.15, 0.0, 1.0), 1.0);
}
