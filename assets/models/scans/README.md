# Scanned buildings and walls

Photogrammetry scans reduced to game assets: at most 2,000 triangles each, with a 2048 colour
map and a 2048 normal map (JPEG) baked from the full scan. Model space is glTF's (Y up),
metres, standing on the ground at the origin.

Facades (`tools/scan_facade.py ... 2000 2048 --flat`): a wall, with its roof slope where it has
one, rebuilt as a grid, its ends closed with its profile (showing its own wall's plaster). The
face looks toward +Z; the back is open, so the game draws scans from both sides.
Objects (`tools/scan_object.py ... 2000 2048`): simplified 3D shapes.
All repacked with `tools/scan_repack.py` (JPEG, quality 90).

| Asset | Source scan | Options | Size (along x deep x tall) | Notes |
|---|---|---|---|---|
| house_a | hb_old_house_part_4.glb | facade | 9.6 x 3.3 x 5.1 m | plastered wall, door, window, green shutters, graffiti, tiled roof |
| house_b | hb_old_house_part_2.glb | facade | 7.8 x 3.6 x 4.9 m | same house, another side: door, two windows, downpipes |
| house_c | hb_old_house_part_3.glb | facade | 8.1 x 3.8 x 5.5 m | same house: two windows, gable end and chimney at one end |
| yellow_house | old_house_1.glb | facade, `--wall-height=2.7` (scan had no scale) | 11.8 x 3.2 x 5.6 m | two sections, yellow plaster, tiled roofs |
| door_wall | old_house_2.glb | facade, `--wall-height=3.4 --no-roof` (no scale) | 3.8 x 0 x 3.5 m | flat wall: double door, window, mailboxes |
| long_house | lowicz_center_old_house..glb | facade, `--wall-height=3.2` (no scale) | 29.5 x 0 x 3.7 m | long low brick-and-plaster front, several windows and doors |
| plaster_house | old_house_with_damaged_plaster_wall.glb | object, `--trim-ground` | 18.1 x 5.6 x 9.7 m | cracked plaster, hipped rusty metal roof, dormer |
| storage_building | old_industrial_storage_building.glb | object, `--trim-ground --voxel=0.1` | 19.0 x 14.7 x 7.0 m | whole building, all sides: rusty roofs, chimney, water tanks |
| ruined_wall | ruined_wall08.glb | object | 5.9 x 2.1 x 4.6 m | thick ruined brick wall, both sides, niches and an arch |
| winery_corner | abandoned_corridor_kyiv_winery_photogrammetry.glb | object | 7.1 x 1.9 x 3.7 m | inside wall of a room with its floor and a pile of junk |

Scans "without a real scale" were sized by eye from their doors and windows; the wall
heights above are guesses that can be changed by re-running with another `--wall-height`.

Collision for the whole objects comes from `tools/scan_colliders.py` (their footprint in 50 cm or
1 m squares, merged into boxes), pasted into the map that uses them (`src/map_oldtown.cpp`).
