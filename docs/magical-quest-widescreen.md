# The Magical Quest: how the widescreen works

Everything here was found by running the ROM headless (`tools/mqtool.cpp`) and reading the game's code
from execution traces. Addresses are SNES addresses (`$bank:offset`) or WRAM offsets in bank `$7E` unless
a bank is given. The ROM is the 1 MB LoROM; its internal header (`$00:FFC0`) title is `THE MAGICAL QUEST`.

## Picture

* The game writes the scroll registers in NMI from `$00:86D2`: BG1 H/V from `$1D6B`/`$1D6F`, BG2 from
  `$1D73`/`$1D77`, BG3 (the HUD) fixed. Those variables are copied from the camera in the main loop
  (`$01:87B9`): BG1's camera is `$1AF1` (X) and `$1AF5` (Y), BG2's is half of it.
* The camera's right limit is `$1D82` (bottom `$1D84`), set at a level's start from a three-step table at
  `$00:DFEA` indexed by the stage number in `$02AD`.
* `MqGame::frameSetup` makes the picture `2 x margin` pixels wider than 256 and slides the 4:3 window
  inside it so the wide view stays inside `0 .. $1D82 + 256`: all the extra width on the right at a level's
  start, split evenly in the middle, all on the left at its end. Rooms narrower than the picture (the
  limit under the margin) stay 4:3. `margin + margin` is kept under 256 so a sprite's 9-bit X is
  unambiguous for the PPU.

## Tiles beyond the 4:3 edges

The game streams one 32-tile column at a time into the 64x32 tilemaps (BG1 `$4800`, BG2 `$4000`) from the
NMI uploader at `$00:830C`, only one column past the right edge. So margins can't come from VRAM.

The column builder (`$01:C201`) reads a three-level structure which `MqGame::decodeTile` repeats:

1. **Page table** in WRAM bank `$7F`: BG1 `$FC22`, BG2 `$FE22`, one 16-bit pointer per 256x256 px page,
   `0x20` bytes per row of pages (`$7F:table + 0x20*pageY + 2*pageX`). A negative pointer means none.
2. **Page**: 16x16 words (`0x20` per row), one per 16x16 px metatile, at `pointer + row*0x20 + col*2`
   in the page bank (the layer struct's `$46`; always `$7F`, i.e. WRAM).
3. **Metatile definition**: four tile entries (TL, TR, BL, BR), 8 bytes, at `(word | $8000)` in the
   metatile bank (the layer struct's `$4C`; `$0B` in 1-1). Entry = `+ (ty&1)*4 + (tx&1)*2`.

The layer structs are at `$1AB6` (BG1) and `$1C0C` (BG2). The decoder matches the game's VRAM on all 896
visible tiles of both layers in every stage tried (`mqtool` command `verify`).

## HUD

The HUD is BG3 (map `$5C00`, fixed). Its groups: tile columns 0-9 (1P score), 10-22 (coin, time, lives,
hearts), 23-31 (hi-score) on tile rows 1-4, and the costume slots on rows 24-27, columns 5-17. With the
HUD anchor on, the PPU's per-line layer remap puts the score at the picture's left edge, the hi-score at
its right edge and the slots at the lower left; the rest is centered on the wide picture (so it follows
the sliding window). BG3's margins are left blank.

## Objects

All three tables below are widened by the picture's whole extra width `w = 2 x margin`, not by one margin:
the 4:3 window slides inside the picture, so at a level's start the wide view reaches `w` px past the 4:3
window's right edge (and at its end past the left edge). Sizing for one margin left objects in the outer
strip visible but not "on screen", where the game's own code then left them inert or killed them.

* **On screen** (`$01:910F`): each frame an object of class `X` is on screen when
  `(camX + 128 - objX + tbl[X]) < tbl[X+2]`, with the pairs at `$00:B9CC` (160/320 for most classes: 32 px
  past each edge). `MqGame::applyRomPatches` adds `w` to the first and `2w` to the second. Several kinds of
  object die when they are not on screen.
* **Active range** (`$03:9213`, `$03:9280`): the object code also asks "is this object in range?" and kills
  the object when it is not, with a second table at `$00:CD02` (8-byte entries: horizontal pair, vertical
  pair; and four horizontal-only pairs at `$00:CD5A`). Some classes are only +-136..160 px wide, narrower
  than the wide picture, so objects in the margins were removed while still visible. Their horizontal pairs
  are widened by `w` and `2w` (the vertical ones are left alone).
* **Spawn** (`$02:F1B0` at a level's start, `$02:F21D` each frame): objects are placed in 32 px buckets as
  the camera nears them. The window is camera -64 .. +320, from constants at `$02:F1F6` (64), `$02:F1E3`
  (320) and the bucket count at `$02:F206` (13). They are widened by exactly `w` (rounding up to whole
  buckets put the window's edge past the active range's: objects there were spawned and killed at once,
  their spawn flag cleared, and did not come back until the camera left and re-entered the bucket: the
  original keeps its spawn window 32 px inside the range, so must this). The bucket tables are read with an
  8-bit index that wraps; the entries it wraps onto, `$7E:C5F0..`, are empty.
* **Vertical moves**: when the camera moves up or down (`$02:F2CD`) the objects of the rows that come into view are
  spawned by going through the window's buckets from its left edge, a fixed 13 of them (`LDY #$000D` at
  `$02:F2D5`). It is set to the same bucket count as the level-start fill. With 13 the right part of a wide
  window got nothing when the camera crossed an object's row (stage 1's second scene: an enemy and tomatoes
  at x=392.. never spawned).
* **The camera centre at a level's start**: the range tests measure from `$1DBC`/`$1DBE` (camera + 128), which
  the main loop only sets some 16 frames after the spawn at `$02:F1B0`; until then it is 0 (or the last
  level's). The original window is small enough for that to go unseen; the wide one reaches objects that,
  measured from 0, are out of range, which were killed at once (a boss trigger, items, platforms missing
  until you left and came back). `MqGame::onExec` sets the centre right before the spawn.
* The in-memory ROM only is changed; with the margin at 0 (or Widescreen Enemies set to Original) the
  original bytes are put back.
* Sprites: the game writes every live object's OAM X without culling it (`$01:91CF`; only Y is culled,
  and an object is skipped when it doesn't fit the 128-sprite budget at `$0403`), so margin sprites need no
  other change. In sampled play the budget was never exhausted (at most 91 of 128 used).

Looked at and left alone: the generator at `$02:D158` drops objects above the player's own screen position
(stretching it over the wide picture would misplace them); children of objects (`$02:80A2`) appear at their
parent's position.

## Mode 7 scenes and HDMA started mid-frame (the boss and cutscene fix)

The world 2 boss (a giant spider), the snow boss and the opening cutscenes are drawn in Mode 7 for a band of
lines, not with sprites or BG1/BG2. The game gets the band with two HDMA channels (2 and 3) whose tables
write `$2105` (BG mode: 1, then 7 for 76-line stretches) and `$212C` (layers: `$14`, then `$12` = the Mode 7
plane with EXTBG), and it starts them part way down the screen: a V-IRQ at line 36 writes `$420C`. The core
only set HDMA channels up at line 0, so a channel enabled later stayed finished from the frame before: every
line stayed Mode 1, and the boss (and the cutscenes' pictures) never appeared although the objects were
all alive and could be fought. `Snes` now sets up a channel when it is newly enabled in `$420C` (as bsnes
does), not only at line 0. In the frames compared (title, menus, 19 stages at 4:3 and wide) the only other
change is a 66-pixel scroll band on stage 2's water surface, which is a mid-screen HDMA effect too.

## Pooled objects, boss rooms, and layers that are not level data

* **The 4-entry pool** (`$194F-$1952`, types `$1953-$1956`): objects of the limited-instance kinds (the cutscene
  controller and its NPC, bosses, some enemies) allocate from it in `$02:F4B6`/`$02:F560` -> `$02:F5EF`. The wider
  level-start spawn also placed a type-`77` enemy (stage 4-1) that the original window only reaches later; it took the
  last entry and the cutscene controller's NPC (`JSL $01A40A`, retried every frame) could never be created, so the
  cutscene never started. `MqGame::denyPooledSpawn` makes those two checks fail for buckets outside the original
  13-bucket window, remembers the bucket, and `runDeferredSpawn` spawns it later (calling `$02:F2EC` from the top of
  the main loop, with the interrupted registers put back at `kInjectRet`) once the camera brings it inside. The
  bookkeeping is kept in save states (`saveExt`).
* **Boss rooms**: BG1's scroll there is whatever the boss effects make of it and moved the picture's margins with the
  boss. The margins now follow the game's camera (`$1AF1`); BG1's scroll is only used while it agrees with it.
* **Layers that are not level data**: each frame the decoder is checked against a sample of the layer's VRAM
  (`layerMatchesVram`); a layer it does not match (stage 3-1's BG2 is a 32x32 repeating map: the rock strips of its
  ceiling and ledge) gets the tilemap's own wrap in the margins.

## v3: rooms, sprites that came round again, entrances, the title card

* **Rooms (missing pages).** A level is cut into rooms by pages that are missing from BG1's page table (a negative
  pointer): the game's camera tests the page its far edge would enter (`$01:C5FB`, `$01:C651`, through `$01:C69B`) and
  stops there. The wide view did not, and showed what is behind: nothing, or another room's tiles. `MqGame::leftRoom` /
  `rightRoom` now measure how far the level goes on either side of the 4:3 window in the rows of pages the picture is
  in (to the level's ends or the nearest missing page), and `wantedMargins` slides the window inside that the way it
  did inside the level. A margin that has to go goes at once; one that can come back grows `kExtStep` px a frame.
* **Layers 32 tiles wide** (stage 4-2's clouds, which drift on their own) are a small repeating map, not streamed level
  data; the decoder agreed with them by chance (all their pages are the same) and drew stripes left of the map's start.
  They always get the tilemap's own wrap now. BG2 left of its own start is the layer's first tiles mirrored.
* **Sprites drawn again on the other side.** The object windows are `2w` wider, so an object can be over 256 px left
  of the 4:3 window while still drawn; its 9-bit OAM X is then the same as that of a place in the right margin. The
  game works a piece's X out as a signed 16-bit number before it stores it (`STA $FA80,X` at `$01:9309` and
  `$01:9371`, A = the X, X = the OAM offset); the layer keeps those, hands them over when NMI sends the list to the PPU
  (`$00:816A`), and gives the PPU the true X of every entry whose 9 bits match (`Ppu::objTrueX`).
* **Mid-bosses.** They are created just outside the 4:3 screen and wait there (inside the wide picture, some with
  their graphics not loaded yet) until their controller object has the camera where it wants it (`$02:91DD`, which then
  sets `$1D8D` = 1; a boss sets 2). While that loop runs or `$1D8D` is 1, an object that has not been on the 4:3 screen
  is drawn further out (`MqGame::entranceShift`): its waiting place goes `kEntrancePad` px past the picture's edge and
  its way in to `kEnterL`/`kEnterR` is stretched to match, so it comes in from the picture's edge. Only the drawing
  moves; `kEntranceFrames` after it sets off the shift is eased out whatever it does. The shift rides on the true X.
* **Goofy** (object kind `$9A`, and the `$6A` he carries) is created 32 px past the 4:3 screen's right edge and walks
  to his mark. He is moved as far past the wide picture's edge when he is created (`MqGame::scanObjects`); the script
  waits for him, so nothing else changes. He walks at his own height, in front of the hillside the margin shows.
* **Speech boxes** are a PPU window at 4:3 coordinates that cuts the text out of BG3. The HUD rule moved every other
  BG3 row to the picture's middle, so the words left their box: rows on which BG3 is windowed (`TMW` bit 2) stay put.
* **The level title card** (`$09:F8xx`) is drawn wide on black (its layers hidden in the margins), and Mickey's walk
  starts `margin` px further left (`$049C` = -32 - margin when the walk loop at `$09:F89C` starts; it ends at X = 80).
* **The boss of 3-3** (stage `$02AD` = 11). While it fades in the game has BG1 (the wall left of the arena) on the
  subscreen only, blended onto BG2: the left margin, which only BG1 fills, showed the backdrop (purple).
  `Ppu::wideSubFill` makes a margin pixel with nothing on the main screen take the subscreen's layer as it is. The
  arena is the level's last screen, so the view had all its extra width on the left; it may now go one margin (80 px at
  most) past the level's end there, and BG2 is continued over it with its own tiles 256 px to the left (the wall and
  the fire repeat every 256 px).
* Save states carry which objects have come on screen (a block after the v1 one; older builds skip it).

## v4: level clear, boss rooms, mid-boss arenas, parallax

* **Level-clear screen** (its loop at `$01:F212`, a mode 7 spotlight on black): drawn wide. Mode 7 carries on over the
  margins by itself, so the spotlights sweep in from the picture's edges; BG3 (the "DARK FOREST CLEAR!" text) is
  not continued into the margins.
* **Boss fades.** 3-3's boss fades its room by taking the fixed colour from BG2 (`CGADSUB` = `$82`); BG1 is the boss
  on the 4:3 screen but the room's scenery in the margins, and stayed lit. `Ppu::wideMathLayers` puts BG1's margin
  pixels into the same colour maths.
* **Every boss arena is centred.** A fight at a room's end (camera held, `$1D8D` ≠ 0, nothing more of the room on the
  right) gets both margins; past the room's end the arena is mirrored at the end (`wantedMargins` / `arenaEnd_`;
  tile-exact, H-flipped, in `fetchWideTile`; a layer whose tiles don't line up there repeats its last 256 px).
* **Mode 7 bosses** (the spider, 5-3's, the stage `$0F` one) copy the 4:3 screen's scenery to BG3 or sprites, turn
  BG1/BG2 off and reuse their VRAM: the margins lost their scenery, or showed the boss's tiles as garbage. The PPU now
  keeps each line's margins (BG layers only, before colour maths: `Ppu::wideCapture`, every normal frame) and shows them
  again on the lines where the level's layers are off (`Ppu::wideFreeze`) for as long as the camera stays put. The
  page tables are kept from the last frame the camera was free (the game clears pages it no longer needs). A mode 7
  arena at a room's end is centred too: its right margin is the last full picture mirrored (`buildArenaMirror`; still
  objects such as 5-3's half-pipe kept, moving ones - Mickey, enemies - left out).
* **Mid-boss arenas are the wide picture.** While a mid-boss holds the camera mid-level (`heldArena`), the game's
  "keep the object in view" routine (`$01:E503`: camera + 16 .. + 240) is done by the layer with the margins added, so
  Mickey (and the mid-boss) can go into the margins. v3's drawn entrance (objects drawn further out than they were)
  is gone: a mid-boss part waiting just past the 4:3 edge is moved past the wide picture's edge instead
  (`scanObjects`), and one the game puts back where it waited when its cue comes (3-2's) is drawn sliding in from the
  edge for `kSlideFrames`. Objects in their first `kNewFrames` frames are not drawn near the 4:3 edges (2-1's snake was
  drawn for a frame with its graphics not loaded).
* **Parallax.** BG2 scrolls at r of the camera (½ in most levels). At a level's or room's ends the picture stops before
  the camera does and BG2 ran backwards across it. BG2 now follows the picture at a pace s a little under r, chosen so
  the picture's ends meet BG2's ends at the level's ends (`parallaxAdjust`, as `Ppu::bgHofsAdj`; BG2 is then drawn from
  the level data on the whole line). Levels where BG2 moves with the camera are unchanged.

## Known limits

* Kept margins (mode 7 fights) are a still picture: anything that moved there before the fight is frozen as it was,
  without its sprites. A save state loaded in the middle of such a fight has none kept: its margins are empty.
* A mid-boss whose arena is not entered through the `$02:91DD` controller is only treated as one once `$1D8D` is set.
* Mickey can walk into the margins only in mid-boss arenas; at a boss at a room's end the extra width on the right is
  mirrored scenery, not level, so the 4:3 limits stay.

* Rooms with no scrolling (the General Store, bonus rooms) and everything that isn't a level (title,
  cutscenes, map) stay 4:3.
* A save state taken at another margin can leave the spawn window at the old size until the next level.
* Only horizontal margins are done; the vertical view is the game's.

## Troubleshooting a missing object or boss

Options > Troubleshooting has *Object Event Log* (records every object spawn and removal, a little slower)
and *Log Debug Info*, which writes to `mq_port.log` the camera, the spawn window, the sprite budget (`$0403`),
the object table and the last 256 spawn/kill events (with the code address that wrote them). The same
dump is `debug` in `mqtool` (`eventlog 1` first). Save states (Menu > Save State) load in `mqtool`, so a state
taken at the problem is the best report.
