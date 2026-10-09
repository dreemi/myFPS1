#version 330
// Sun + sky ambient + shadows + fog, filmic tonemapping.
// World surfaces (worldMapped = 1) compute their texture coordinates from the
// world position and normal (boxes are axis-aligned), so the same mapping
// works for every box and the normal map needs no vertex tangents.

in vec3 fragPosition;
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;
in vec4 fragLightPos;

uniform sampler2D texture0;   // albedo (sRGB)
uniform sampler2D texture1;   // roughness (world surfaces)
uniform sampler2D texture2;   // normal map, OpenGL convention (world surfaces)
uniform sampler2D shadowMap;
uniform vec4 colDiffuse;

uniform int worldMapped;
uniform float tileSize;
uniform float specular;       // highlight strength for this surface

uniform vec3 sunDir;          // direction the sunlight travels
uniform vec3 sunColor;        // linear, can exceed 1
uniform vec3 skyAmbient;      // linear
uniform vec3 groundAmbient;   // linear
uniform vec3 viewPos;
uniform vec3 fogColor;        // sRGB, matches the sky's horizon
uniform float fogDensity;
uniform int useShadows;
uniform float shadowTexel;
uniform float sunVisibility;  // multiplies sunlight when shadows aren't sampled (viewmodel)

// Two short-lived point lights (muzzle flashes); radius 0 = off
uniform vec3 lightPos[2];
uniform vec3 lightColor[2];
uniform float lightRadius[2];

out vec4 finalColor;

vec3 ToLinear(vec3 c) { return pow(c, vec3(2.2)); }

vec3 Tonemap(vec3 c)
{
    // ACES filmic approximation, then gamma
    c = (c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14);
    return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

float SunShadow(vec3 n)
{
    if (useShadows == 0) return sunVisibility;
    vec3 p = fragLightPos.xyz / fragLightPos.w * 0.5 + 0.5;
    if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z > 1.0) return 1.0;
    float facing = max(dot(n, -sunDir), 0.0);
    float bias = max(0.0012 * (1.0 - facing), 0.00025);
    float lit = 0.0;
    for (int x = -1; x <= 1; x++)
        for (int y = -1; y <= 1; y++)
            lit += (p.z - bias > texture(shadowMap, p.xy + vec2(x, y) * shadowTexel).r) ? 0.0 : 1.0;
    return lit / 9.0;
}

void main()
{
    vec3 n = normalize(fragNormal);
    vec2 uv = fragTexCoord;
    vec3 N = n;
    float rough = 0.7;

    if (worldMapped == 1) {
        // Same mapping as the CPU side used: pick the face's axes from its normal
        vec3 a = abs(n);
        vec3 r, u;
        if (a.y >= a.x && a.y >= a.z) { r = vec3(1, 0, 0); u = n.y > 0.0 ? vec3(0, 0, -1) : vec3(0, 0, 1); }
        else if (a.x >= a.z)          { r = n.x > 0.0 ? vec3(0, 0, -1) : vec3(0, 0, 1); u = vec3(0, 1, 0); }
        else                          { r = n.z > 0.0 ? vec3(1, 0, 0) : vec3(-1, 0, 0); u = vec3(0, 1, 0); }
        uv = vec2(dot(fragPosition, r), -dot(fragPosition, u)) / tileSize;
        vec3 nm = texture(texture2, uv).xyz * 2.0 - 1.0;
        N = normalize(r * nm.x + u * nm.y + n * nm.z);
        rough = texture(texture1, uv).r;
    }

    vec4 tex = texture(texture0, uv);
    vec3 albedo = ToLinear(tex.rgb * colDiffuse.rgb * fragColor.rgb);

    vec3 L = -sunDir;
    vec3 V = normalize(viewPos - fragPosition);
    float shadow = SunShadow(n);
    float ndl = max(dot(N, L), 0.0) * smoothstep(0.0, 0.15, dot(n, L) + 0.05); // no light leaking past the geometric edge
    vec3 ambient = mix(groundAmbient, skyAmbient, N.y * 0.5 + 0.5);
    vec3 H = normalize(L + V);
    float gloss = mix(48.0, 6.0, rough);
    float spec = pow(max(dot(N, H), 0.0), gloss) * specular * (1.0 - rough) * ndl;

    vec3 color = albedo * (ambient + sunColor * ndl * shadow) + sunColor * spec * shadow;

    for (int i = 0; i < 2; i++) {
        if (lightRadius[i] <= 0.0) continue;
        vec3 d = lightPos[i] - fragPosition;
        float dist = length(d);
        float att = clamp(1.0 - dist / lightRadius[i], 0.0, 1.0);
        color += albedo * lightColor[i] * max(dot(N, d / max(dist, 0.001)), 0.0) * att * att;
    }

    float fog = 1.0 - exp(-length(viewPos - fragPosition) * fogDensity);
    color = mix(color, ToLinear(fogColor), fog);

    finalColor = vec4(Tonemap(color), tex.a * colDiffuse.a * fragColor.a);
}
