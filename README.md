# MagicalQuestTrilogy-NX
A native Nintendo Switch port of Disney's Magical Quest Trilogy (Magical Quest, Great Circus Mystary, &amp; Mickey to Donald Magical Adventure 3) with widescreen, save states, filters, and an in-game menu. Bring your own ROMs.

_Based off [Darhkfox's Super Mario All Stars PC/Android port](https://github.com/Darhkfox/SuperMarioAllStars) but adapted for the Magical Quest SNES games_

Currently only supports Magical Quest 1 with support for 2 (Great Circus Mystery) and 3 coming at a later date. Expect bugs and issues this is an early PoC port created with Nintendo Switch as the primary target. Eventual Android/PC port is planned but the primary goal was for Nintendo Switch. Magical Quest 1 is currently fully playable with widescreen 16:9 tested with some issues but works quite well so far!

**To build (devkitpro is required):** Clone repo & run make in MSYS2 (windows) from the /switch/ subfolder to generate the .nro

.nro file goes to _SD:\switch\MagicalQuest_nx_

place a USA good ROM dump in the same _MagicalQuest_nx_ folder

Run via homebrew launcher with title override (hold R while launching a game)

The Magical Quest games, its characters, and all ROM data is copyright of Disney & Capcom. No data from the ROM is included in this project and a good ROM dump must be provided to play. We hold no affiliation.
