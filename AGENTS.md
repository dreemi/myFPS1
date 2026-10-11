# Instructions for agents working on this project

## Geometry: avoid z-fighting

Never put two faces on the exact same level so that you properly avoid z-fighting. Always triple check your designs to avoid z-fighting.

In practice:
- Two boxes may touch (faces back to back), but no two visible faces may lie in the same plane facing the same way while overlapping. Where pieces meet at a corner, one piece takes the corner and the other stops at its face (e.g. side walls fit between end walls, not through them).
- Trim, bands, posts, shutters and sills stand a few millimetres proud of the surface they sit on, or stop just short of a top face, rather than ending flush with it.
- A slab covering a wall's top (a cornice, a ceiling) means the wall stops inside the slab, not level with its top.
- `tests/player_test.cpp` ("No z-fighting") checks every map's visible boxes for faces sharing a plane; it must pass. Meshes that aren't boxes (round walls, roofs, trees, the ground) aren't covered by it: check those by hand, and look at the result in the game.
