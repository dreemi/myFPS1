#version 330
// Shared vertex shader for everything lit: level, characters, guns.
// Characters are skinned here (skinned = 1): each vertex follows up to four bones.

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
in vec4 vertexBoneIds;
in vec4 vertexBoneWeights;

uniform int skinned;
uniform mat4 boneMatrices[64];

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
    vec4 position = vec4(vertexPosition, 1.0);
    vec3 normal = vertexNormal;
    if (skinned == 1) {
        mat4 skin = boneMatrices[int(vertexBoneIds.x)] * vertexBoneWeights.x +
                    boneMatrices[int(vertexBoneIds.y)] * vertexBoneWeights.y +
                    boneMatrices[int(vertexBoneIds.z)] * vertexBoneWeights.z +
                    boneMatrices[int(vertexBoneIds.w)] * vertexBoneWeights.w;
        position = skin * position;
        normal = mat3(skin) * normal;
    }
    vec4 world = matModel * position;
    fragPosition = world.xyz;
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal * vec4(normal, 0.0)));
    fragLightPos = lightVP * world;
    gl_Position = mvp * position;
}
