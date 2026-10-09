#version 330
// Full-screen sky: gradient from horizon to zenith, sun disc and glow.
// Drawn first each frame; the lit shader's fog fades into fogColor (= horizon).

uniform vec2 resolution;
uniform vec3 camForward;
uniform vec3 camRight;
uniform vec3 camUp;
uniform float tanHalfFov;
uniform vec3 sunDir;      // direction sunlight travels (so the sun is at -sunDir)
uniform vec3 horizon;     // sRGB
uniform vec3 zenith;      // sRGB
uniform vec3 sunTint;     // sRGB

out vec4 finalColor;

vec3 ToLinear(vec3 c) { return pow(c, vec3(2.2)); }
vec3 Tonemap(vec3 c)
{
    c = (c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14);
    return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

void main()
{
    vec2 ndc = (gl_FragCoord.xy / resolution) * 2.0 - 1.0;
    float aspect = resolution.x / resolution.y;
    vec3 dir = normalize(camForward + camRight * ndc.x * tanHalfFov * aspect + camUp * ndc.y * tanHalfFov);

    float h = clamp(dir.y, 0.0, 1.0);
    vec3 sky = mix(ToLinear(horizon), ToLinear(zenith), pow(h, 0.55));

    float s = max(dot(dir, -sunDir), 0.0);
    vec3 sun = ToLinear(sunTint);
    sky += sun * (pow(s, 8.0) * 0.25 + pow(s, 64.0) * 0.6);          // glow around the sun
    sky += sun * smoothstep(0.9993, 0.9997, s) * 12.0;                // the disc itself

    // Below the horizon (rarely seen): fade to a dusty ground tone
    if (dir.y < 0.0) sky = mix(sky, ToLinear(horizon) * 0.6, clamp(-dir.y * 4.0, 0.0, 1.0));

    // Same tonemapping as the lit shader, so fogged geometry blends into the horizon exactly
    finalColor = vec4(Tonemap(sky), 1.0);
}
