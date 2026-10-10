#pragma once
#include "raylib.h"
#include "character.hpp"
#include "game/types.hpp"

// Third-person characters (bots and other players): the character model in its
// animation, the gun in its right hand, flinching when hit and falling when
// killed. Drawing only; the game decides what each SoldierView says.

// Gun space -> world for the gun in its hand (barrel along -Z, tilted to its aim)
Matrix SoldierGunTransform(const CharacterAssets* character, const SoldierView& s);
Vector3 SoldierMuzzle(const CharacterAssets* character, const SoldierView& s);
// Which animation a player-controlled soldier plays, from how it moves
CharAnim SoldierAnim(float speed, bool crouching, bool grounded);
// Stride length (m per animation cycle) of an animation, so feet don't slide
float SoldierStride(CharAnim anim);

// Draws it (null or unloaded character: simple placeholder shapes)
void DrawSoldier(CharacterAssets* character, const SoldierView& s);
