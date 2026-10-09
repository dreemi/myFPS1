#version 330
// Shared vertex shader for everything lit: level, characters, guns.

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;

uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
uniform mat4 lightVP;   // sun's view-projection, for shadow lookups

out vec3 fragPosition;  // world (or camera, for the viewmodel) space
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragNormal;
out vec4 fragLightPos;

void main()
{
    vec4 world = matModel * vec4(vertexPosition, 1.0);
    fragPosition = world.xyz;
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
    fragLightPos = lightVP * world;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
