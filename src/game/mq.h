// The Magical Quest Starring Mickey Mouse (SNES, Capcom 1992): the widescreen game layer.
//
// What it does, in the order the picture is made:
//
//  * Margins. The game's own camera (BG1: $1AF1/$1AF5, BG2 half of it) is left alone. Each frame the
//    picture is made wider by `reqExt_` pixels on each side, and the 4:3 window slides inside it so the
//    wide view never leaves the level (at the level's left end the whole extra width is on the right,
//    in the middle it is split evenly, at the right end it is on the left).
//  * Level data. The game only streams the tile column just past the 4:3 edge into VRAM, so the margins
//    cannot be read from VRAM. fetchWideTile() works the tile out from the level data in WRAM the way
//    the game's own column builder does (see decodeTile()).
//  * HUD. The HUD is BG3 (fixed, 32x32). Its three groups (score / time+lives / hi-score) and the
//    costume slots are re-anchored to the wide picture's edges with the PPU's per-line layer remap.
//  * Objects. Two tables in the (in-memory) ROM are widened: the on-screen window for objects
//    ($00:B9CC, see applyRomPatches) and the spawn window ($02:F1E3/$F1F6/$F206). Nothing on disk is
//    changed; with the margin at 0 the original bytes are put back.
//  * Sprites. The game writes the 9-bit OAM X of an object up to 160 px either side of the screen's
//    middle; with extL+extR < 256 the PPU can tell a sprite at X=300 from one at X=-212.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/ppu.h"
#include "core/snes.h"

class MqGame : public PpuHooks, public Snes::StateExt {
 public:
  // The widest margin on each side: extL + extR must stay under 256 so a sprite's 9-bit X is unambiguous.
  static constexpr int kMaxSupportedExt = 112;

  explicit MqGame(Snes& snes);

  // Does this ROM (copier header already removed) look like the US release of The Magical Quest?
  static bool looksLikeMagicalQuest(const uint8_t* rom, size_t size);

  void install();
  void beforeFrame();
  void afterFrame();
  void onStateLoaded();

  // The margin wanted on each side, in pixels (0 = the original 4:3).
  void setWidescreen(int ext);
  int widescreen() const { return reqExt_; }
  // The margins the last frame was drawn with (they slide with the camera).
  int activeExtL() const { return extL_; }
  int activeExtR() const { return extR_; }
  bool inGameplay() const { return gameplay_; }

  bool hudAnchor = true;    // the HUD groups go to the edges of the wide picture
  // Objects are on-screen and spawn across the wide picture (off: the original timing, as at 4:3).
  void setEnemyRange(bool on);
  bool enemyRange() const { return enemyRange_; }

  std::string describe() const;

  // The object patches come in four groups that can be switched off one at a time to find which one a
  // problem comes from (Options > Troubleshooting). The first three change tables in the in-memory ROM; the
  // spawn window ones only take effect when the next level or room is loaded.
  enum PatchGroup { kOnScreen = 1, kActiveRange = 2, kSpawnWindow = 4, kLevelStartCentre = 8, kAllGroups = 15 };
  void setPatchGroup(int group, bool on);
  bool patchGroup(int group) const { return (patchMask_ & group) != 0; }

  // Diagnostics (for a bug report): a text dump of the camera, the spawn window, the sprite budget and the
  // object table, then the last spawn/kill events if the event log is on. Lines are separated by '\n'.
  std::string debugDump() const;
  // Records the BG mode, main/sub screen and SETINI the PPU has at the start of each visible line of the next frame
  // (they change down the screen when the game drives them with HDMA): for debugging.
  void traceLines(bool on) { traceLines_ = on; lineLog_.clear(); }
  const std::vector<uint32_t>& lineLog() const { return lineLog_; }
  // Records every object spawn and kill (a ring of the last 256) while on. A little CPU: for reports only.
  void setEventLog(bool on);
  bool eventLog() const { return eventLog_; }

  // Test aid: how many of the visible tiles of BG layer `layer` (0 = BG1, 1 = BG2) the level-data
  // decoder gets different from VRAM (0 when it is right). `total` gets the number compared.
  int verifyLayer(int layer, int* total) const;
  std::string verifyMap(int layer) const;  // (the same as a picture: '#' where a visible tile differs)

  // Snes::StateExt: only the deferred-spawn bookkeeping (see runDeferredSpawn) is kept in a save state
  std::vector<uint8_t> saveExt() const override;
  void loadExt(const uint8_t* data, size_t size) override;

  // The exec hook the layer installs (public so a debugging tool can chain to it).
  static bool execThunk(void* ctx, uint32_t pc);

  // PpuHooks
  void beginLine(Ppu& ppu, int row) override;
  int fetchWideTile(Ppu& ppu, int layer, int mapX, int mapY, int screenX, uint16_t* entry) override;

 private:
  struct Layer {
    int cam = 0;        // the layer's real (not 10-bit) horizontal scroll this frame
    int camY = 0;       // ... and vertical
    int dp = 0;         // the game's per-layer struct in WRAM ($1AB6 BG1, $1C0C BG2)
    int table = 0;      // the page table in bank $7F ($FC22 BG1, $FE22 BG2)
    uint8_t pageBank = 0;   // bank of the page definitions (WRAM $7F in every level seen)
    uint8_t metaBank = 0;   // bank of the metatile definitions (ROM)
  };

  bool onExec(uint32_t pc);
  // --- the wide view's own rules (see docs/magical-quest-widescreen.md, "v3")
  bool pageHole(int col, int row) const;       // BG1 has no page there (the game's camera never shows one)
  int leftRoom(int cam, int camY, int most) const;   // how far the view may reach left of the 4:3 window / right of it
  int rightRoom(int cam, int camY, int most) const;
  // (arenaEnd: where on the 4:3 screen the room ends when the view is let past it, a fight's arena; else -1)
  void wantedMargins(int cam, int camY, int* eL, int* eR, int* arenaEnd = nullptr) const;
  int spawnHide(int slot) const;               // how far an object is drawn from where it is (entrances: scanObjects)
  bool heldArena() const;                      // a mid-boss holds the camera mid-level: its arena is the wide picture
  bool onMoveX();                              // $01:A0DA: an object's X moved by its speed (a mid-boss's: over the wide arena)
  bool onClamp();                              // $01:E509: an object kept to the camera's 4:3 view
  void scanObjects();                          // once a frame: freed slots, and objects that are new
  void collectTrueX(Ppu& ppu);
  bool denyPooledSpawn();
  bool runDeferredSpawn();
  bool bucketInOriginalWindow(int bucket) const;
  uint16_t vramTile(int layer, int tx, int ty) const;
  bool layerMatchesVram(const Layer& L, int layer) const;
  static void objWriteThunk(void* ctx, uint32_t adr, uint8_t v, uint32_t pc);
  void onObjWrite(uint32_t adr, uint8_t v, uint32_t pc);

  void frameSetup(Ppu& ppu);
  void applyRomPatches();
  int readNear(int ram, uint16_t reg) const;  // the 16-bit WRAM value `ram` points to, taken to the register's 10 bits
  uint16_t decodeTile(const Layer& L, int tx, int ty) const;
  uint8_t romByte(uint8_t bank, uint16_t off) const;
  uint16_t rd16(int wramOff) const { return (uint16_t)(snes_.wram[wramOff] | (snes_.wram[wramOff + 1] << 8)); }

  Snes& snes_;
  int reqExt_ = 0;
  bool enemyRange_ = true;
  int extL_ = 0, extR_ = 0;
  bool gameplay_ = false;
  bool wide_ = false;      // this frame is drawn wide
  bool hudOn_ = false;     // this frame's BG3 is the HUD (fixed, at the screen's edges)
  uint64_t loopFrame_ = 0; // the frame the gameplay loop ($01:8472) last ran in
  int centerShift_ = 0;    // (extR - extL) / 2: where the middle of the wide picture is, from the 4:3 one
  Layer layer_[2];
  bool layerOk_[2] = {true, true};  // the level-data decoder agrees with the layer's VRAM this frame (else: tilemap wrap)

  // Objects of the kinds that come from a small pool (the cutscene NPCs and controllers, bosses, some enemies: at
  // most 4 alive) are only spawned from the wide part of the spawn window once the camera brings them into the
  // original sized window: spawned earlier they take the pool's entries before the game's own scripts can create
  // their objects (stage 4-1's cutscene never started: its NPC could not be created).
  bool inSpawn_ = false;            // inside the game's spawn routine for a bucket
  int spawnBucket_ = 0;
  bool injecting_ = false;          // a deferred bucket is being spawned by calling the game's routine
  struct SavedRegs {
    uint16_t a = 0, x = 0, y = 0, sp = 0, dp = 0, pc = 0;
    uint8_t db = 0, pb = 0;
    bool c = false, z = false, v = false, n = false, i = false, d = false, xf = false, mf = false, e = false;
  } saved_;
  uint8_t savedDp_[9] = {};
  bool deferred_[256] = {};         // spawn buckets whose pooled objects are being held back
  // Sprites: the X the game worked out for each OAM entry before cutting it to 9 bits (see onExec), as the game
  // builds its list (pend) and as the list the PPU now has (live).
  int16_t pendX_[128] = {}, pendShift_[128] = {}, liveX_[128] = {}, liveShift_[128] = {};
  bool pendSet_[128] = {}, liveSet_[128] = {};
  int8_t pendSlot_[128] = {}, liveSlot_[128] = {};  // (the object slot each entry was drawn for, -1: none)
  int curSlot_ = -1;
  uint32_t lastPos_[31] = {}, lastMove_[31] = {};   // (each slot's position last frame, and when it last moved)
  int curShift_ = 0;                // how far the object being drawn is drawn from where it is (spawnHide)
  // Each object: has it been well inside the 4:3 screen since it was made, and has it been put outside the wide
  // picture to make its entrance from there (scanObjects)
  struct Entrance {
    bool tracked = false, inside = false, pushed = false, mapped = false;
    uint8_t kind = 0;
    int16_t prevX = 0;              // (its X last frame)
    int16_t slide = 0;              // drawn this far from where it is, going down to 0 (see scanObjects)
    uint32_t slideStart = 0, made = 0;
  } entr_[32];
  bool seen_[32] = {};              // the slot's object has been looked at (scanObjects)
  uint8_t seenKind_[32] = {};
  uint64_t titleFrame_ = 0;         // the frame the level title card's loop last ran in
  bool titleSeen_ = false;
  uint64_t lockWaitFrame_ = 0;      // the frame a mid-boss's controller last waited for the camera in
  bool lockWaitSeen_ = false;
  bool title_ = false;              // this frame is the title card, drawn wide
  uint64_t clearFrame_ = 0;         // the frame the level-clear screen's loop last ran in
  bool clearSeen_ = false;
  bool clear_ = false;              // this frame is the level-clear screen, drawn wide
  // The margins kept for a fight that takes the level's layers away (see frameSetup)
  bool capture_ = false, captured_ = false, frozenOk_ = false, freeze_ = false, layersAway_ = false;
  uint8_t keptPages_[0x400] = {};   // both page tables as they were when the camera was last free
  bool pagesKept_ = false, pagesValid_ = false;
  uint8_t pageByte(int adr) const;
  void buildArenaMirror(Ppu& ppu, int end, int to);
  int arenaEnd_ = -1;               // this frame: the screen x past which the scenery is the arena's, mirrored
  int bg2Adj_ = 0;                  // this frame: BG2's extra scroll (parallaxAdjust)
  double parallaxR_ = 0.5;          // BG2's pace, as last worked out
  int parallaxAdjust(int cam, int eL);
  bool mirrorBuilt_ = false;        // the kept margins' right side has been made (a mode 7 fight's arena)
  int capturedCam_ = 0, capturedL_ = 0, capturedR_ = 0, frozenCam_ = 0, frozenL_ = 0, frozenR_ = 0;
  bool capturedFull_ = false, frozenFull_ = false;
  uint64_t lockStart_ = 0;          // the frame the camera was last free in
  bool extLoaded_ = false;          // the state just loaded had the v3 block
  bool bossCtl_ = false;            // a stage boss's camera-lock controller (kind $6C) is waiting for the camera: the room's end is its arena
  bool staticRoom_ = false;         // this frame: a room that never scrolls (the shop), its edge tiles repeated into the margins
  bool fadeHold_ = false;           // this frame: a door's fade-out, drawn wide like the room it is leaving
  bool snapExt_ = true;             // the next frame's margins are taken as they come (no easing): a new scene
  int patchedKey_ = -1;    // what the ROM tables are patched for: the extra width and the groups (-1 = not looked at yet)
  int patchMask_ = kAllGroups;
  std::vector<uint8_t> origCull_, origSpawn_, origRange_;
  struct ObjEvent {
    uint32_t frame;
    uint32_t pc;
    int16_t x, y;  // (where the object was on the screen)
    uint8_t slot, spawn;
  };
  bool eventLog_ = false;
  bool traceLines_ = false;
  std::vector<uint32_t> lineLog_;
  std::vector<ObjEvent> events_;  // (a ring)
  size_t eventHead_ = 0;
  uint8_t slotActive_[32] = {};
};
