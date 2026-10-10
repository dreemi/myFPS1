#pragma once
#include "raylib.h"
#include "character.hpp"

// A plain steel combat helmet (M1-style shell with a flared rim), built to fit
// the character's head. Everyone wears one: it rides on the head bone, and when
// a shot to the back of the head knocks it off it tumbles away as debris.
// Helmet space: origin at the centre of the dome, +Y up, +Z the way the face looks.

void InitHelmet(const CharacterAssets* character); // after Render::Init (uses the lit shader)
void UnloadHelmet();
void DrawHelmet(Matrix transform);                 // helmet space -> world
// Helmet space -> the character's model space, worn, in that animation frame
Matrix HelmetOnHead(const CharacterAssets& character, CharAnim anim, int frame);
// Lying rim-down on the ground: how high its origin sits
float HelmetRestHeight();
