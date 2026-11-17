# Testing on the Switch

## Build and install
```
export DEVKITPRO=/opt/devkitpro
cd switch && make clean && make          # -> MagicalQuest_nx.nro (add LTO=0 if the LTO plugin is missing)
```
Copy `MagicalQuest_nx.nro` over the old one in `sdmc:/switch/MagicalQuest_nx/`. Settings, save states and
`mq_port.log` are in that folder; nothing there needs to be deleted.

## This build: 4-1 cutscene, boss-room margins, stage 3-1 layers
- **Stage 4-1** (Pete's Peak): the in-game cutscene (Goofy: "Gawrsh, Mickey...") should start a few seconds in.
- **Boss rooms**: the picture should stay put while the boss moves.
- **Stage 3-1** (the cave): the ceiling and the rock strips should run across the whole wide picture while you walk.
- Also try the world 2 mid boss again: it needs the same pooled-object fix as the cutscene.
If something is still wrong, the earlier bisect steps (below) and the save-state report still apply.

## Earlier build: the HDMA band and Mode 7
The invisible boss and the blank cutscenes were not the widescreen: the game starts an HDMA band mid-frame
(an IRQ at line 36 enables the channels) that switches those lines to Mode 7, and the emulator only started
HDMA at the top of the frame. Fixed in the core (`src/core/snes.cpp`, writing `$420C`). Please check:
- The opening cutscenes (after the title): the picture and the caption text should show.
- The world 2 mid boss and boss, and the snow boss: the fight starts and the boss (a Mode 7 picture) is drawn.
- Stage 2's water surface has a scroll band that now runs: it should look like waves, not glitchy.
- Anything else that is Mode 7 or a band across the screen (the map, other cutscenes, bonus rooms).
Your two save states are still good: they load in this build.

## What the earlier builds changed (so you know what to look for)
1. **Vertical scenes**: when the camera scrolls up or down the game spawns the objects of the rows that
   come into view, going through a fixed 13 buckets of the spawn window; with the wider window the right part
   of the picture got none. Reproduced headless in stage 1's second scene: at camera (310, 295) an enemy at
   x=392 inside the picture never spawned (tomatoes and items there likewise); now it does.
2. Earlier fixes still in: the level-start camera centre, the spawn window kept inside the active range, the
   per-class active ranges and the on-screen window widened by the picture's full extra width.
3. **Options > Troubleshooting** now has a switch for each object patch group (below).

## Checks
- Stage 1, second (vertical) scene: scroll down and up; tomatoes and enemies should be there the first time,
  not only after leaving and coming back.
- Walk back and forth along a stretch: enemies may respawn when you return (the game does that), but should not
  pile up.
- The mid boss and boss of world 2, and the snow boss: does the fight start, and is the boss drawn?

## If a boss (or anything) is still wrong: find which part causes it
Each step is one run of the problem. Stop at the first one that changes the result.

1. **Aspect Ratio 4:3** (Options > Display). The original game. If the problem is still there, it is not the
   widescreen at all (try it with and without Rewind: Options > Speed).
2. **Widescreen Enemies: Original** (Options > Display) with a wide aspect ratio. All object patches off; only
   the picture is wide. If the boss is fine now, the cause is one of the object patch groups: continue.
3. Back on **Active**, in Options > Troubleshooting switch **one** of the four *Object Patch* switches off and
   redo the problem:
   - *On-Screen Window* and *Active Range* take effect at once.
   - *Spawn Window* and *Level-Start Centre* take effect when the next level or room loads (switch off, then
     enter the stage again).
   The one whose switch changes the result is the cause.
4. If it is still wrong with all four off and Widescreen Enemies Original, the cause is in the picture itself:
   turn **HUD Anchor** off (Options > Display) and check again.

## What to send me
- The group (or step) that changed it, the stage and where in it.
- A **save state** from the moment (Menu > Save State; `state1.sst` in the folder above): they load in the
  headless tool, so I can look at the exact frame.
- `mq_port.log` after Options > Troubleshooting > *Object Event Log* on, play to the problem, *Log Debug Info*.
