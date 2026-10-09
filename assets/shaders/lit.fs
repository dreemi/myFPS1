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
uniform int faceMapped;       // world-mapped surface whose texture covers each face once (vertex UVs)
uniform float tileSize;
uniform float specular;       // highlight strength for this surface

// Physically based objects (materialMode = 1): metal/roughness, optional maps
uniform int materialMode;     // 0 = simple (characters), 1 = PBR (weapons, casings)
uniform float metallic;
uniform float roughness;      // multiplies the roughness map when there is one
uniform int useRoughMap;      // texture1.r
uniform int useNormalMap;     // texture2, OpenGL convention, in the model's UV space
uniform float occlusion;      // ambient/reflection scale (1 = open sky)

uniform vec3 sunDir;          // direction the sunlight travels
uniform vec3 sunColor;        // linear, can exceed 1
uniform vec3 skyAmbient;      // linear
uniform vec3 groundAmbient;   // linear
uniform vec3 viewPos;
uniform vec3 envUp;           // world up in the space being drawn (camera space for the viewmodel)
uniform vec3 horizonColor;    // sRGB sky colours, for reflections
uniform vec3 zenithColor;
uniform vec3 envGround;       // linear, the sunlit ground seen in reflections
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

// The sky (and ground) seen in direction d, blurred toward the average by roughness
vec3 Environment(vec3 d, float rough)
{
    float y = dot(d, envUp);
    vec3 horizon = ToLinear(horizonColor), zenith = ToLinear(zenithColor);
    vec3 sky = mix(horizon, zenith, sqrt(clamp(y, 0.0, 1.0)));
    sky = mix(sky, vec3(dot(sky, vec3(0.3, 0.55, 0.15))), 0.6); // reflections read less saturated than the sky
    vec3 ground = mix(horizon * 0.6, envGround, smoothstep(0.0, 0.25, -y));
    vec3 sharp = mix(ground, sky, smoothstep(-0.04, 0.04, y)); // soft horizon line
    vec3 blurred = mix(envGround, mix(horizon, zenith, 0.5), y * 0.5 + 0.5);
    return mix(sharp, blurred, rough * rough);
}

// Split-sum environment BRDF approximation (Karis)
vec3 EnvBRDF(vec3 f0, float rough, float ndv)
{
    vec4 r = rough * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

// GGX highlight from one light, already multiplied by N.L
vec3 SpecularGGX(vec3 N, vec3 V, vec3 L, vec3 f0, float rough)
{
    vec3 H = normalize(L + V);
    float ndl = max(dot(N, L), 0.0), ndv = max(dot(N, V), 1e-3), ndh = max(dot(N, H), 0.0);
    float a = max(rough * rough, 0.002), a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159 * d * d);
    float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    float G = (ndl / (ndl * (1.0 - k) + k)) * (ndv / (ndv * (1.0 - k) + k));
    vec3 F = f0 + (1.0 - f0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
    return D * G * F / max(4.0 * ndl * ndv, 1e-3) * ndl;
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
        uv = faceMapped == 1 ? fragTexCoord : vec2(dot(fragPosition, r), -dot(fragPosition, u)) / tileSize;
        vec3 nm = texture(texture2, uv).xyz * 2.0 - 1.0;
        N = normalize(r * nm.x + u * nm.y + n * nm.z);
        rough = texture(texture1, uv).r;
    }

    vec4 tex = texture(texture0, uv);
    vec3 albedo = ToLinear(tex.rgb * colDiffuse.rgb * fragColor.rgb);

    if (materialMode == 1) {
        if (useNormalMap == 1) {
            // Tangent frame from screen-space derivatives (no vertex tangents needed)
            vec3 dp1 = dFdx(fragPosition), dp2 = dFdy(fragPosition);
            vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
            vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
            vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
            vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
            float invmax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
            vec3 nm = texture(texture2, uv).xyz * 2.0 - 1.0;
            nm.y = -nm.y; // image rows run down the texture; OpenGL normal maps have green pointing up
            N = normalize(mat3(T * invmax, B * invmax, n) * nm);
        }
        float r = useRoughMap == 1 ? texture(texture1, uv).r * roughness : roughness;
        r = clamp(r, 0.05, 1.0);
        vec3 V = normalize(viewPos - fragPosition);
        if (dot(N, V) < 0.0) N = normalize(N - V * dot(N, V) * 1.01); // no black rims from the normal map
        vec3 L = -sunDir;
        float ndv = max(dot(N, V), 1e-3);
        vec3 f0 = mix(vec3(0.04), albedo, metallic);
        vec3 diffuse = albedo * (1.0 - metallic);
        float shadow = SunShadow(n) * smoothstep(-0.05, 0.1, dot(n, L));
        vec3 ambient = mix(groundAmbient, skyAmbient, dot(N, envUp) * 0.5 + 0.5);

        vec3 color = diffuse * (ambient * occlusion + sunColor * max(dot(N, L), 0.0) * shadow);
        color += sunColor * SpecularGGX(N, V, L, f0, r) * shadow;
        vec3 R = reflect(-V, N);
        color += Environment(R, r) * EnvBRDF(f0, r, ndv) * occlusion;
        for (int i = 0; i < 2; i++) {
            if (lightRadius[i] <= 0.0) continue;
            vec3 d = lightPos[i] - fragPosition;
            float dist = length(d);
            float att = clamp(1.0 - dist / lightRadius[i], 0.0, 1.0);
            vec3 Ld = d / max(dist, 0.001);
            color += lightColor[i] * att * att * (diffuse * max(dot(N, Ld), 0.0) + SpecularGGX(N, V, Ld, f0, r));
        }
        float fog = 1.0 - exp(-length(viewPos - fragPosition) * fogDensity);
        color = mix(color, ToLinear(fogColor), fog);
        finalColor = vec4(Tonemap(color), tex.a * colDiffuse.a);
        return;
    }

    vec3 L = -sunDir;
    vec3 V = normalize(viewPos - fragPosition);
    float shadow = SunShadow(n);
    float ndl = max(dot(N, L), 0.0) * smoothstep(0.0, 0.15, dot(n, L) + 0.05); // no light leaking past the geometric edge
    vec3 ambient = mix(groundAmbient, skyAmbient, dot(N, envUp) * 0.5 + 0.5);
    vec3 H = normalize(L + V);
    float gloss = mix(48.0, 6.0, rough);
    float spec = pow(max(dot(N, H), 0.0), gloss) * specular * (1.0 - rough) * ndl;

    // Walls darken toward the ground, where little sky light reaches them
    float contact = 1.0;
    if (worldMapped == 1 && abs(n.y) < 0.5) contact = mix(0.45, 1.0, smoothstep(0.0, 0.9, fragPosition.y));
    vec3 color = albedo * (ambient * contact + sunColor * ndl * shadow * mix(0.8, 1.0, contact)) + sunColor * spec * shadow;

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
