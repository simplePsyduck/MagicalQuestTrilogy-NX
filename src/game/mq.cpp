#include "game/mq.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace {

// Where the game keeps what the layer reads (all WRAM offsets in bank $7E unless noted).
constexpr int kLoopPc = 0x018472;   // the gameplay loop's top ($01:8472): runs once a frame in a level
constexpr int kSpawnInitPc = 0x02F1B0;  // the spawn window's set-up at a level's start (see onExec)
constexpr int kCenterX = 0x1DBC;    // camera X + 128 and Y + 128: what the object range tests measure from
constexpr int kCenterY = 0x1DBE;
constexpr int kSpawnEntryPc = 0x02F2EC;   // the spawn routine for one bucket (X = bucket)
constexpr int kSpawnExitPcA = 0x02F2EB;   // its two RTS
constexpr int kSpawnExitPcB = 0x02F3E3;
constexpr int kPooledModeA = 0x02F4B6;    // the two create-object checks that allocate from the 4-entry pool
constexpr int kPooledModeB = 0x02F560;
constexpr int kInjectRet = 0x02FF00;      // where a deferred spawn call returns to (never executed: caught by the hook)
// The sprite list. The game draws an object with $01:928A (D = its struct); each of its pieces is written to the
// OAM buffer ($7E:FA80, X = the offset) by STA $FA80,X at one of two places, A holding the piece's X on the screen
// as a signed 16-bit number: only its low 9 bits reach the PPU. The list is started at $01:91A7 and sent to the
// PPU in NMI ($00:816A).
constexpr int kObjDrawPc = 0x01928A;
constexpr int kObjPiecePcA = 0x019309;
constexpr int kObjPiecePcB = 0x019371;
constexpr int kOamResetPc = 0x0191A7;
constexpr int kOamDmaPc = 0x00816A;
constexpr int kDrawCamX = 0x1DB5;         // the camera X the sprites are drawn against
// The level title card ("STAGE 4 / PETE'S PEAK", $09:F8xx): its four wait-for-the-frame calls, the first being the
// loop Mickey walks in with (from X = -32 until X = 80).
constexpr int kTitleWalkPc = 0x09F89C;
constexpr int kTitleWaitPcs[3] = {0x09F8C5, 0x09F8D5, 0x09F8EB};
// The level-clear screen ("DARK FOREST CLEAR!": a mode 7 spotlight on black, the score in sprites): its loop.
constexpr int kClearLoopPc = 0x01F212;
constexpr int kPlayerX = 0x049C;
constexpr int kCamLock = 0x1D8D;          // 1 while a mid-boss holds the camera, 2 for a boss, else 0
// The mid-boss's controller object waits here ($02:91DD: LDA $1AF1 / CMP $40) until the camera has reached the
// place it is to be held at, then sets $1D8D: the mid-boss and its parts already exist while it waits.
constexpr int kLockWaitPc = 0x0291DD;
constexpr int kStage = 0x02AD;
constexpr int kMoveXPc = 0x01A0DA;           // ($01:A0DA: the object at D moves by its speed: X ($0B-$0D) += ($19-$1B), or -= when $14)
constexpr int kKindBossCtl = 0x6C;            // the object that waits for the camera at a stage boss's room and locks it ($03:E6B9)
constexpr int kBossEntryPad = 40;             // a mid-boss makes its entrance from this far outside the wide picture
constexpr int kBossKinds[] = {0x4C};          // (the objects that are mid-bosses whose flight is spread over the arena: stage 1's)

constexpr int kInsideL = 16, kInsideR = 240;  // an object is on the 4:3 screen when it is this far inside it
constexpr int kEntrancePad = 40;              // a waiting object past the 4:3 edge, by up to this, ...
constexpr int kSlideFrames = 36;              // a mid-boss put back where it waited: drawn sliding in from the edge
constexpr int kNewFrames = 4;                 // an object this new is not drawn in a margin
// The routine that keeps an object within the camera's view ($01:E503, D = the object): X between the camera plus
// $00:C78B,X and plus $00:C78D,X (X = $1995; 0: 16 and 240, the 4:3 screen less 16 px each side).
constexpr int kClampPc = 0x01E509;
constexpr int kClampDonePc = 0xE520;          // (its SEP #$20 / RTS)
constexpr int kKindGoofy = 0x9A, kKindGoofyItem = 0x6A;
constexpr int kExtStep = 6;               // the margins grow at most this much a frame
constexpr int kCamX = 0x1AF1;
constexpr int kCamY = 0x1AF5;
constexpr int kBg1HofsW = 0x1D6B;   // the game's own copy of the BG scroll it writes in NMI ($86D2)
constexpr int kBg1VofsW = 0x1D6F;
constexpr int kBg2HofsW = 0x1D73;
constexpr int kBg2VofsW = 0x1D77;
constexpr int kCamMaxX = 0x1D82;    // the camera's right limit
constexpr int kLayerDp[2] = {0x1AB6, 0x1C0C};   // the per-layer scroll structs
constexpr int kPageTable[2] = {0xFC22, 0xFE22};  // (in bank $7F) 16-bit page pointers, 0x20 per row of pages

// ROM tables the layer widens (file offsets of a LoROM image).
constexpr int kCullOff = 0x39CC;    // $00:B9CC  pairs (a, b): on screen when (cx + 128 - x + a) < b
constexpr int kCullLen = 40;
// $00:CD02: the "active range" of an object, which the object code tests (JSR $03:9213, $03:9280) and kills
// the object when it is out of: 8-byte entries, a horizontal pair (a, b) then a vertical pair, an object
// being in range when (centre - x + a) < b. And $00:CD5A: four horizontal-only pairs the same way (they
// overlap the first table's last entries).
constexpr int kRangeOff = 0x4D02;
constexpr int kRangeLen = 0x6C;
constexpr int kRangeEntries = 14;
constexpr int kSpawnRightOff = 0x171E3;  // $02:F1E3  right edge of the spawn window: camera + this ($0140)
constexpr int kSpawnLeftOff = 0x171F6;   // $02:F1F6  left edge: camera - this ($0040)
constexpr int kSpawnCountOff = 0x17206;  // $02:F206  buckets spawned at a level's start ($0D)
constexpr int kSpawnVCountOff = 0x172D6; // $02:F2D6  buckets spawned when the camera moves vertically ($0D): LDY #$000D

inline int floorDiv8(int v) { return v >> 3; }

}  // namespace

MqGame::MqGame(Snes& snes) : snes_(snes) {}

bool MqGame::looksLikeMagicalQuest(const uint8_t* rom, size_t size) {
  // The internal header of a LoROM image is at $7FC0: 21 bytes of title, then the map mode ($20 = LoROM).
  if (size < 0x8000) return false;
  const char kTitle[] = "THE MAGICAL QUEST";
  if (std::memcmp(rom + 0x7FC0, kTitle, sizeof kTitle - 1) != 0) return false;
  return rom[0x7FD5] == 0x20 || rom[0x7FD5] == 0x30;
}

void MqGame::install() {
  snes_.ppu.hooks = this;
  snes_.clearExecMarks();
  snes_.markExec(kLoopPc, true);
  snes_.markExec(kSpawnInitPc, true);
  for (int pc : {kSpawnEntryPc, kSpawnExitPcA, kSpawnExitPcB, kPooledModeA, kPooledModeB, kInjectRet}) snes_.markExec(pc, true);
  for (int pc : {kObjDrawPc, kObjPiecePcA, kObjPiecePcB, kOamResetPc, kOamDmaPc, kTitleWalkPc}) snes_.markExec(pc, true);
  for (int pc : kTitleWaitPcs) snes_.markExec(pc, true);
  snes_.markExec(kLockWaitPc, true);
  snes_.markExec(kClearLoopPc, true);
  snes_.markExec(kClampPc, true);
  snes_.markExec(kMoveXPc, true);
  snes_.setExecHook(execThunk, this);
  snes_.stateExt = this;
  snes_.clearWriteHooks();
  // The original bytes of the tables the layer widens (kept to put them back at 4:3).
  if (snes_.rom.size() >= (size_t)kSpawnCountOff + 1) {
    origCull_.assign(snes_.rom.begin() + kCullOff, snes_.rom.begin() + kCullOff + kCullLen);
    origRange_.assign(snes_.rom.begin() + kRangeOff, snes_.rom.begin() + kRangeOff + kRangeLen);
    origSpawn_ = {snes_.rom[kSpawnRightOff], snes_.rom[kSpawnRightOff + 1], snes_.rom[kSpawnLeftOff],
                  snes_.rom[kSpawnLeftOff + 1], snes_.rom[kSpawnCountOff], snes_.rom[kSpawnVCountOff]};
  }
  patchedKey_ = -1;
  applyRomPatches();
}

bool MqGame::execThunk(void* ctx, uint32_t pc) { return static_cast<MqGame*>(ctx)->onExec(pc); }

bool MqGame::onExec(uint32_t pc) {
  switch (pc) {
    case kObjPiecePcA:
    case kObjPiecePcB: {
      // (only when the game is in the state the routine runs in: 16-bit A and X)
      const CpuRegs& r = snes_.cpu.r;
      if (r.mf || r.xf) return false;
      const int idx = (r.x & 0x1FF) >> 2;
      pendX_[idx] = (int16_t)r.a;
      pendShift_[idx] = (int16_t)curShift_;
      pendSlot_[idx] = (int8_t)curSlot_;
      pendSet_[idx] = true;
      return false;
    }
    case kObjDrawPc: {
      const int d = snes_.cpu.r.dp;
      curShift_ = 0;
      curSlot_ = (d >= 0x490 && d <= 0x490 + 0x50 * 30 && (d - 0x490) % 0x50 == 0) ? (d - 0x490) / 0x50 : -1;
      if (curSlot_ > 0) curShift_ = spawnHide(curSlot_);
      return false;
    }
    case kClampPc:
      return onClamp();
    case kClearLoopPc:
      clearFrame_ = snes_.frameCount();
      clearSeen_ = true;
      return false;
    case kLockWaitPc: {
      lockWaitFrame_ = snes_.frameCount();
      lockWaitSeen_ = true;
      // Stage 1's mid-boss holds the camera at $0C00, the 4:3 screen's last stretch of the level. In the wide picture
      // the fight is centred a little further on, where Mickey stands when the camera has got there ($0C36).
      const int d = snes_.cpu.r.dp;
      if (reqExt_ > 0 && snes_.wram[kStage] == 1 && rd16(d + 0x40) == 0x0C00) {
        snes_.wram[d + 0x40] = 0x36;
        snes_.wram[d + 0x42] = 0xB6;
      }
      return false;
    }
    case kMoveXPc:
      return onMoveX();
    case kOamResetPc:
      std::memset(pendSet_, 0, sizeof pendSet_);
      curShift_ = 0;
      curSlot_ = -1;
      return false;
    case kOamDmaPc:
      std::memcpy(liveX_, pendX_, sizeof liveX_);
      std::memcpy(liveShift_, pendShift_, sizeof liveShift_);
      std::memcpy(liveSet_, pendSet_, sizeof liveSet_);
      std::memcpy(liveSlot_, pendSlot_, sizeof liveSlot_);
      return false;
    case kTitleWalkPc:
      // The title card is drawn wide: Mickey starts his walk as far past the picture's left edge as he was past the
      // 4:3 one (the loop ends when he is at X = 80, wherever he started).
      // (once, on the loop's first pass: he walks through X = -32 again on his way)
      if (reqExt_ > 0 && rd16(kPlayerX) == 0xFFE0 && !(titleSeen_ && snes_.frameCount() - titleFrame_ <= 2)) {
        const uint16_t x = (uint16_t)(0xFFE0 - reqExt_);
        snes_.wram[kPlayerX] = x & 0xFF;
        snes_.wram[kPlayerX + 1] = x >> 8;
      }
      [[fallthrough]];
    case 0x09F8C5:
    case 0x09F8D5:
    case 0x09F8EB:
      titleFrame_ = snes_.frameCount();
      titleSeen_ = true;
      return false;
    case kLoopPc:
      loopFrame_ = snes_.frameCount();
      scanObjects();
      return runDeferredSpawn();
    case kSpawnEntryPc:
      inSpawn_ = true;
      spawnBucket_ = snes_.cpu.r.x & 0xFF;
      return false;
    case kSpawnExitPcA:
    case kSpawnExitPcB:
      inSpawn_ = false;
      return false;
    case kPooledModeA:
    case kPooledModeB:
      return denyPooledSpawn();
    case kInjectRet:
      if (!injecting_) return false;
      {  // the deferred spawn is done: the code it interrupted goes on
        CpuRegs& r = snes_.cpu.r;
        r.a = saved_.a; r.x = saved_.x; r.y = saved_.y; r.sp = saved_.sp; r.dp = saved_.dp; r.pc = saved_.pc;
        r.db = saved_.db; r.pb = saved_.pb;
        r.c = saved_.c; r.z = saved_.z; r.v = saved_.v; r.n = saved_.n; r.i = saved_.i; r.d = saved_.d;
        r.xf = saved_.xf; r.mf = saved_.mf; r.e = saved_.e;
        std::memcpy(&snes_.wram[0x1A], savedDp_, sizeof savedDp_);
        injecting_ = false;
      }
      return true;
    case kSpawnInitPc:
      std::memset(deferred_, 0, sizeof deferred_);  // a new level or room: nothing is being held back
      if (enemyRange_ && reqExt_ > 0 && (patchMask_ & kLevelStartCentre)) {
        // A level's objects in the spawn window are placed here, and from the next frame on the object code
        // tests each one against the camera's centre ($1DBC/$1DBE) and kills it when it is out of its range.
        // But the game only sets that centre from its main loop, some 16 frames later (while the level's name
        // is shown), and until then it still holds 0 (or the last level's): the original window is small
        // enough for that to go unseen, but the wide one reaches objects that, measured from 0, are out of
        // range, so they were killed (their spawn flag cleared) and did not come back until the camera had
        // left their bucket and returned. The centre is made right before they are placed.
        const uint16_t cx = (uint16_t)(rd16(kCamX) + 0x80), cy = (uint16_t)(rd16(kCamY) + 0x80);
        snes_.wram[kCenterX] = cx & 0xFF;
        snes_.wram[kCenterX + 1] = cx >> 8;
        snes_.wram[kCenterY] = cy & 0xFF;
        snes_.wram[kCenterY + 1] = cy >> 8;
      }
      return false;
    default:
      return false;
  }
}

// The original spawn window is 13 buckets of 32 px from (camera - 64) on.
bool MqGame::bucketInOriginalWindow(int bucket) const {
  const int left = ((uint16_t)(rd16(kCamX) - 64) >> 5) & 0xFF;
  return ((bucket - left) & 0xFF) <= 12;
}

// Create-object checks of the kinds with a limited number of instances (they allocate from a pool of 4): refuse
// when the spawn routine asks for a bucket the original window would not have reached yet (the bucket is kept
// to be spawned later, see runDeferredSpawn). Returns true when it did (the check is skipped: it fails).
bool MqGame::denyPooledSpawn() {
  if (!inSpawn_ || !enemyRange_ || reqExt_ <= 0 || !(patchMask_ & kSpawnWindow)) return false;
  if (bucketInOriginalWindow(spawnBucket_)) return false;
  deferred_[spawnBucket_ & 0xFF] = true;
  CpuRegs& r = snes_.cpu.r;
  const uint16_t ret = (uint16_t)(snes_.wram[(r.sp + 1) & 0x1FFF] | (snes_.wram[(r.sp + 2) & 0x1FFF] << 8));
  r.sp += 2;
  r.pc = (uint16_t)(ret + 1);  // (an RTS)
  r.c = true;                  // the creation failed
  return true;
}

// Spawns a held back bucket once it is inside the original window, by calling the game's own routine ($02:F2EC) from
// the top of the main loop: the interrupted code's registers are put back when the routine returns to kInjectRet.
bool MqGame::runDeferredSpawn() {
  if (injecting_ || !enemyRange_ || reqExt_ <= 0) return false;
  for (int b = 0; b < 256; b++) {
    if (!deferred_[b] || !bucketInOriginalWindow(b)) continue;
    deferred_[b] = false;
    CpuRegs& r = snes_.cpu.r;
    saved_ = {r.a, r.x, r.y, r.sp, r.dp, r.pc, r.db, r.pb, r.c, r.z, r.v, r.n, r.i, r.d, r.xf, r.mf, r.e};
    std::memcpy(savedDp_, &snes_.wram[0x1A], sizeof savedDp_);
    snes_.wram[0x22] = 0;  // (the vertical window is checked)
    r.dp = 0;
    r.db = 2;
    r.xf = true;
    r.mf = true;
    r.d = false;
    r.x = (uint16_t)b;
    r.y = 0;
    const uint16_t ret = (uint16_t)((kInjectRet & 0xFFFF) - 1);
    snes_.wram[r.sp & 0x1FFF] = ret >> 8;
    r.sp--;
    snes_.wram[r.sp & 0x1FFF] = ret & 0xFF;
    r.sp--;
    r.pb = 2;
    r.pc = kSpawnEntryPc & 0xFFFF;
    injecting_ = true;
    return true;
  }
  return false;
}

std::vector<uint8_t> MqGame::saveExt() const {
  std::vector<uint8_t> v;
  v.push_back(1);  // version (the block below; what v3 adds follows it and is skipped by older builds)
  v.push_back(injecting_ ? 1 : 0);
  auto p16 = [&](uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); };
  p16(saved_.a); p16(saved_.x); p16(saved_.y); p16(saved_.sp); p16(saved_.dp); p16(saved_.pc);
  v.push_back(saved_.db); v.push_back(saved_.pb);
  v.push_back((saved_.c ? 1 : 0) | (saved_.z ? 2 : 0) | (saved_.v ? 4 : 0) | (saved_.n ? 8 : 0) | (saved_.i ? 16 : 0) |
              (saved_.d ? 32 : 0) | (saved_.xf ? 64 : 0) | (saved_.mf ? 128 : 0));
  v.push_back(saved_.e ? 1 : 0);
  v.insert(v.end(), savedDp_, savedDp_ + sizeof savedDp_);
  for (int i = 0; i < 256; i += 8) {
    uint8_t b = 0;
    for (int k = 0; k < 8; k++) b |= deferred_[i + k] ? (1 << k) : 0;
    v.push_back(b);
  }
  // (v3: which objects have come on screen, and which have been looked at)
  v.push_back(0xE3);
  for (int n = 0; n < 32; n++) {
    const Entrance& e = entr_[n];
    v.push_back((e.tracked ? 1 : 0) | (e.inside ? 2 : 0) | (seen_[n] ? 4 : 0) | (e.pushed ? 8 : 0));
    v.push_back(e.kind);
    v.push_back(0);  // (v3: the place it waited at, no longer used)
    v.push_back(0);
    v.push_back(seenKind_[n]);
    for (int k = 0; k < 4; k++) v.push_back(0);  // (v3: when it set off)
  }
  return v;
}

void MqGame::loadExt(const uint8_t* d, size_t n) {
  extLoaded_ = false;
  injecting_ = false;
  std::memset(deferred_, 0, sizeof deferred_);
  if (!d || n < 2 + 12 + 2 + 2 + sizeof savedDp_ + 32 || d[0] != 1) return;  // (none, or not ours)
  size_t i = 1;
  injecting_ = d[i++] != 0;
  auto g16 = [&]() { uint16_t x = (uint16_t)(d[i] | (d[i + 1] << 8)); i += 2; return x; };
  saved_.a = g16(); saved_.x = g16(); saved_.y = g16(); saved_.sp = g16(); saved_.dp = g16(); saved_.pc = g16();
  saved_.db = d[i++]; saved_.pb = d[i++];
  const uint8_t f = d[i++];
  saved_.c = f & 1; saved_.z = f & 2; saved_.v = f & 4; saved_.n = f & 8; saved_.i = f & 16; saved_.d = f & 32;
  saved_.xf = f & 64; saved_.mf = f & 128;
  saved_.e = d[i++] != 0;
  std::memcpy(savedDp_, d + i, sizeof savedDp_);
  i += sizeof savedDp_;
  for (int k = 0; k < 256; k++) deferred_[k] = (d[i + k / 8] >> (k % 8)) & 1;
  i += 32;
  if (n >= i + 1 + 32 * 9 && d[i] == 0xE3) {
    i++;
    for (int k = 0; k < 32; k++, i += 9) {
      Entrance& e = entr_[k];
      e.tracked = d[i] & 1;
      e.inside = d[i] & 2;
      seen_[k] = d[i] & 4;
      e.pushed = d[i] & 8;
      e.kind = d[i + 1];
      seenKind_[k] = d[i + 4];
    }
    extLoaded_ = true;
  }
}

void MqGame::setWidescreen(int ext) {
  ext = std::clamp(ext, 0, kMaxSupportedExt);
  if (ext == reqExt_) return;
  reqExt_ = ext;
  applyRomPatches();
}

void MqGame::setPatchGroup(int group, bool on) {
  const int m = on ? (patchMask_ | group) : (patchMask_ & ~group);
  if (m == patchMask_) return;
  patchMask_ = m & kAllGroups;
  applyRomPatches();
}

void MqGame::setEnemyRange(bool on) {
  if (on == enemyRange_) return;
  enemyRange_ = on;
  applyRomPatches();
}

void MqGame::onStateLoaded() {
  loopFrame_ = snes_.frameCount();
  titleSeen_ = false;
  clearSeen_ = false;
  frozenOk_ = captured_ = false;
  lockStart_ = snes_.frameCount();
  pagesKept_ = pagesValid_ = false;
  lockWaitSeen_ = false;
  snapExt_ = true;
  std::memset(pendSet_, 0, sizeof pendSet_);
  std::memset(liveSet_, 0, sizeof liveSet_);
  curShift_ = 0;
  if (!extLoaded_) {
    // (a state from before v3: every object there is has been looked at; none is known to have come on screen)
    for (int n = 0; n < 32; n++) {
      entr_[n] = Entrance();
      seen_[n] = snes_.wram[0x490 + 0x50 * n] != 0;
      seenKind_[n] = snes_.wram[0x490 + 0x50 * n + 6];
    }
  }
  // (what v3 marked as come in was also by time: an object outside the 4:3 screen now is taken as not having been)
  const int cam = (int16_t)rd16(kCamX);
  for (int n = 1; n < 31; n++) {
    const int sx = (int16_t)rd16(0x490 + 0x50 * n + 0xC) - cam;
    if (entr_[n].inside && (sx < kInsideL || sx > kInsideR) && !entr_[n].pushed) entr_[n].inside = false;
  }
  extLoaded_ = false;
  if (eventLog_)
    for (int i = 0; i < 32; i++) slotActive_[i] = snes_.wram[0x490 + 0x50 * i];
  patchedKey_ = -1;
  applyRomPatches();
}

// The tables the layer widens, in the in-memory ROM (nothing on disk changes).
//
//  * Object cull ($00:B9CC): the game works out each frame which objects are on screen. For an object of
//    class X: on screen when (camX + 128 - objX + tbl[X]) < tbl[X+2], i.e. objX within +-tbl[X] of the
//    middle of the screen (160 px for most classes: 32 px past either edge). Widening both by the
//    extra width w (tbl[X] + w, tbl[X+2] + 2w) keeps the 32 px over the edge of the wider picture.
//  * Spawn window ($02:F1B0): objects are placed in 32 px buckets as the camera nears them; the window
//    is camX-64 .. camX+320 (64 px past the 4:3 edges), set up here at a level's start and then moved
//    with the camera by $02:F21D. The window is made 64 + the margin (rounded up to whole buckets) past
//    each edge, and the number of buckets filled at the start to match.
void MqGame::applyRomPatches() {
  // The 4:3 window slides inside the wide picture, so the picture can reach all of the extra width (twice the
  // margin) past either edge of it: the object windows are widened by that, not by the margin.
  const int want = enemyRange_ ? 2 * reqExt_ : 0;
  const int key = want | (patchMask_ << 16);
  if (key == patchedKey_ || origCull_.empty()) return;
  patchedKey_ = key;
  std::vector<uint8_t>& rom = snes_.rom;
  // (only a ROM whose bytes are the ones looked at: another dump or hack is left alone)
  auto w16 = [&](int off) { return rom[off] | (rom[off + 1] << 8); };
  std::memcpy(&rom[kCullOff], origCull_.data(), kCullLen);
  std::memcpy(&rom[kRangeOff], origRange_.data(), kRangeLen);
  rom[kSpawnRightOff] = origSpawn_[0];
  rom[kSpawnRightOff + 1] = origSpawn_[1];
  rom[kSpawnLeftOff] = origSpawn_[2];
  rom[kSpawnLeftOff + 1] = origSpawn_[3];
  rom[kSpawnCountOff] = origSpawn_[4];
  rom[kSpawnVCountOff] = origSpawn_[5];
  if (want == 0) return;
  if (w16(kSpawnRightOff) != 0x0140 || w16(kSpawnLeftOff) != 0x0040 || rom[kSpawnCountOff] != 0x0D ||
      rom[kSpawnVCountOff] != 0x0D || rom[kSpawnVCountOff - 1] != 0xA0)
    return;

  for (int i = 0; (patchMask_ & kOnScreen) && i + 1 < kCullLen / 2; i += 2) {
    const int a = w16(kCullOff + i * 2), b = w16(kCullOff + i * 2 + 2);
    if (a >= 0x80 && b == 2 * a) {
      const int na = a + want, nb = b + 2 * want;
      rom[kCullOff + i * 2] = na & 0xFF;
      rom[kCullOff + i * 2 + 1] = na >> 8;
      rom[kCullOff + i * 2 + 2] = nb & 0xFF;
      rom[kCullOff + i * 2 + 3] = nb >> 8;
    }
  }
  // (the active ranges: the horizontal pairs of the first table, and the second table's)
  auto widenPair = [&](int off) {
    const int a = w16(off), b = w16(off + 2);
    if (a < 0x80 || b != 2 * a) return;
    const int na = a + want, nb = b + 2 * want;
    rom[off] = na & 0xFF;
    rom[off + 1] = na >> 8;
    rom[off + 2] = nb & 0xFF;
    rom[off + 3] = nb >> 8;
  };
  for (int k = 0; (patchMask_ & kActiveRange) && k < kRangeEntries; k++) widenPair(kRangeOff + 8 * k);
  for (int j = 0; (patchMask_ & kActiveRange) && j < 4; j++) {
    const int off = kRangeOff + 0x58 + 4 * j;
    // (the pairs shared with the first table's horizontal ones are already done: not twice)
    if ((off - kRangeOff) % 8 != 0) widenPair(off);
  }

  // (Exactly 64 + w, not rounded up to whole buckets: an object spawned past the edge of its class's active
  // range would be killed at once, its spawn flag cleared, and not come back until the camera left and
  // re-entered its bucket. The original keeps its spawn window 32 px inside the range: so does this.)
  if (!(patchMask_ & kSpawnWindow)) return;
  const int pad = 64 + want;
  const int right = 256 + pad, left = pad;
  const int count = (256 + 2 * pad + 31) / 32 + 1;
  rom[kSpawnRightOff] = right & 0xFF;
  rom[kSpawnRightOff + 1] = right >> 8;
  rom[kSpawnLeftOff] = left & 0xFF;
  rom[kSpawnLeftOff + 1] = left >> 8;
  rom[kSpawnCountOff] = (uint8_t)std::min(count, 40);
  // (When the camera moves up or down the objects of the rows that come into view are spawned too, by going
  // through the buckets of the window from its left edge: a fixed 13 of them. With a wider window the right
  // part of the picture got none of them: a vertical scene's items and enemies were missing there.)
  rom[kSpawnVCountOff] = (uint8_t)std::min(count, 40);
}

void MqGame::objWriteThunk(void* ctx, uint32_t adr, uint8_t v, uint32_t pc) {
  static_cast<MqGame*>(ctx)->onObjWrite(adr, v, pc);
}

// The object table is 31 slots of 0x50 bytes from $0490 (slot 0 the player); byte 0 is non-zero while the slot is
// in use.
void MqGame::onObjWrite(uint32_t adr, uint8_t v, uint32_t pc) {
  adr &= 0xFFFF;
  if (adr < 0x490 || adr > 0xE40 || (adr - 0x490) % 0x50 != 0) return;
  const int slot = (int)((adr - 0x490) / 0x50);
  if (slot > 31) return;
  if ((slotActive_[slot] != 0) == (v != 0)) {
    slotActive_[slot] = v;
    return;
  }
  slotActive_[slot] = v;
  ObjEvent e;
  e.frame = snes_.frameCount();
  e.pc = pc;
  e.slot = (uint8_t)slot;
  e.spawn = v ? 1 : 0;
  const int b = 0x490 + 0x50 * slot;
  e.x = (int16_t)((int16_t)rd16(b + 0xC) - (int16_t)rd16(kCamX));  // (on the screen, as it was then)
  e.y = (int16_t)((int16_t)rd16(b + 0xF) - (int16_t)rd16(kCamY));
  if (events_.size() < 256) events_.push_back(e);
  else events_[eventHead_++ % 256] = e;
}

void MqGame::setEventLog(bool on) {
  if (on == eventLog_) return;
  eventLog_ = on;
  snes_.clearWriteHooks();
  if (on) {
    for (int i = 0; i < 32; i++) slotActive_[i] = snes_.wram[0x490 + 0x50 * i];
    events_.clear();
    eventHead_ = 0;
    snes_.addWriteHook(0x7E0490, 0x7E0E40, objWriteThunk, this);
  }
}

std::string MqGame::debugDump() const {
  std::string s;
  char buf[256];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
  auto add = [&](const char* f, auto... a) {
    std::snprintf(buf, sizeof buf, f, a...);
    s += buf;
    s += '\n';
  };
#pragma GCC diagnostic pop
  const int cam = rd16(kCamX), camY = rd16(kCamY);
  add("frame %u gameplay=%d widescreen=%d (picture %d+%d) enemyRange=%d hud=%d", snes_.frameCount(), gameplay_ ? 1 : 0,
      reqExt_, extL_, extR_, enemyRange_ ? 1 : 0, hudOn_ ? 1 : 0);
  add("camera %d,%d  limits x<=%d y<=%d  centre $1DBC=%d $1DBE=%d  stage $02AD=%d", cam, camY, rd16(kCamMaxX),
      rd16(0x1D84), rd16(kCenterX), rd16(kCenterY), rd16(0x02AD));
  add("spawn window buckets right=%d left=%d last=%d  (patched for %d)  difficulty=%d", rd16(0x1F6E), rd16(0x1F70),
      rd16(0x1F72), patchedKey_ < 0 ? -1 : (patchedKey_ & 0xFFFF), snes_.wram[0x18]);
  add("object patch groups on-screen=%d active-range=%d spawn-window=%d level-start-centre=%d", patchGroup(kOnScreen),
      patchGroup(kActiveRange), patchGroup(kSpawnWindow), patchGroup(kLevelStartCentre));
  add("sprite budget left=%d of 128  objsel/size: see PPU; scroll BG1=%d,%d BG2=%d,%d", rd16(0x403),
      snes_.ppu.bgHofs(0), snes_.ppu.bgVofs(0), snes_.ppu.bgHofs(1), snes_.ppu.bgVofs(1));
  add("slot state flags   x(screen) y(screen) kind  [on-screen = flag bit $4000]");
  int alive = 0;
  for (int n = 0; n < 31; n++) {
    const int b = 0x490 + 0x50 * n;
    if (!snes_.wram[b]) continue;
    alive++;
    const int x = (int16_t)rd16(b + 0xC) - cam, y = (int16_t)rd16(b + 0xF) - camY;
    add(" %2d   %02X   %04X  %6d %6d     %02X  %s", n, snes_.wram[b], rd16(b + 8), x, y, snes_.wram[b + 6],
        (rd16(b + 8) & 0x4000) ? "ON" : "off");
  }
  add("%d objects alive", alive);
  if (eventLog_) {
    add("last object events (oldest first):  frame  spawn/kill slot  x,y(screen)  writer pc");
    const size_t n = events_.size();
    for (size_t i = 0; i < n; i++) {
      const ObjEvent& e = events_[n < 256 ? i : (eventHead_ + i) % 256];
      add(" %u %s slot %d  %d,%d  %06X", e.frame, e.spawn ? "SPAWN" : "KILL ", e.slot, e.x, e.y, e.pc);
    }
  } else {
    add("%s", "(the event log is off: switch it on under Options > Troubleshooting, then play to the problem)");
  }
  return s;
}

void MqGame::beforeFrame() {}
void MqGame::afterFrame() {}

// A scroll value the game keeps as 16 bits ($1D6B...) the way the PPU has it (10 bits): the 16-bit one
// is the nearest to the register's, so the game's NMI being a frame behind its main loop (or ahead)
// does not matter.
int MqGame::readNear(int ram, uint16_t reg) const {
  const int w = rd16(ram);
  int d = ((int)(reg & 0x3FF) - (w & 0x3FF)) & 0x3FF;
  if (d >= 512) d -= 1024;
  return w + d;
}

uint8_t MqGame::romByte(uint8_t bank, uint16_t off) const {
  if (bank == 0x7F) return snes_.wram[0x10000 + off];
  if (bank == 0x7E) return snes_.wram[off];
  const size_t o = ((size_t)(bank & 0x7F) << 15) | (off & 0x7FFF);
  return o < snes_.rom.size() ? snes_.rom[o] : 0;
}

// The tile entry the game's own column builder ($01:C201...) puts in the tilemap for the tile at (tx, ty)
// (in 8-px tiles from the level's top left):
//   page      = the 256x256 px page the tile is in; its pointer is a word at $7F:table + 0x20*pageY + 2*pageX
//   metatile  = 16x16 px: a word in that page's block (16x16 words, ROM-pointer to the definition)
//   definition = four tile entries (TL, TR, BL, BR), 8 bytes, at (word | $8000) in the metatile bank.
uint16_t MqGame::decodeTile(const Layer& L, int tx, int ty) const {
  const int px = tx >> 5, py = ty >> 5;
  const int tbl = L.table + 0x20 * py + 2 * px;
  int ptr = pageByte(tbl) | (pageByte(tbl + 1) << 8);
  if (ptr & 0x8000) ptr = 0;  // (the game's BPL: a negative pointer is none)
  const int a = ptr + ((ty >> 1) & 15) * 0x20 + ((tx >> 1) & 15) * 2;
  const uint8_t lo = romByte(L.pageBank, (uint16_t)a), hi = romByte(L.pageBank, (uint16_t)(a + 1));
  const int def = (lo | (hi << 8) | 0x8000) + (ty & 1) * 4 + (tx & 1) * 2;
  return (uint16_t)(romByte(L.metaBank, (uint16_t)def) | (romByte(L.metaBank, (uint16_t)(def + 1)) << 8));
}

// Once a frame (the picture's first line): is this a level, how wide is the picture, and what are the
// scroll values the margins are worked out from.
void MqGame::frameSetup(Ppu& ppu) {
  gameplay_ = (snes_.frameCount() - loopFrame_) <= 2 && ppu.bgMode() == 1;
  // A door (or anything else that fades the room out) leaves the game's loop for its own: the room is still on the
  // screen, so it stays as wide as it was until the screen is dark, rather than jumping to 4:3 for the fade.
  fadeHold_ = !gameplay_ && extL_ + extR_ > 0 && wide_ && reqExt_ > 0 && ppu.bgMode() == 1 && !ppu.forcedBlank() &&
              ppu.brightness() > 0 && snes_.frameCount() - loopFrame_ <= 150;
  if (fadeHold_) gameplay_ = true;
  staticRoom_ = false;
  wide_ = false;
  hudOn_ = false;
  title_ = false;
  capture_ = freeze_ = false;
  arenaEnd_ = -1;
  bg2Adj_ = 0;
  clear_ = false;
  int eL = 0, eR = 0;
  if (gameplay_) {
    if (!snes_.wram[kCamLock]) {
      std::memcpy(keptPages_, &snes_.wram[0x10000 + kPageTable[0]], sizeof keptPages_);
      pagesKept_ = false;
      pagesValid_ = true;
    } else {
      pagesKept_ = pagesValid_;
    }
    const int r[2][2] = {{kBg1HofsW, kBg1VofsW}, {kBg2HofsW, kBg2VofsW}};
    for (int l = 0; l < 2; l++) {
      Layer& L = layer_[l];
      L.cam = readNear(r[l][0], ppu.bgHofs(l));
      L.camY = readNear(r[l][1], ppu.bgVofs(l));
      L.dp = kLayerDp[l];
      L.table = kPageTable[l];
      L.pageBank = snes_.wram[L.dp + 0x46];
      L.metaBank = snes_.wram[L.dp + 0x4C];
    }
    // (a layer whose tilemap is 32 tiles wide is not streamed level data but a small map that repeats, like the
    // clouds of stage 4-2 that drift on their own: the decoder happens to agree with it wherever its pages are all
    // the same, and then drew stripes left of the map's start. Its margins are the tilemap wrapping.)
    for (int l = 0; l < 2; l++) layerOk_[l] = (ppu.bgMapSize(l) & 1) && layerMatchesVram(layer_[l], l);
    const int M = reqExt_;
    if (M > 0) {
      // The margins slide with the game's camera ($1AF1). BG1's scroll is the same in a level, but in a boss room it is
      // whatever the boss effects make of it (and moves with the boss): that must not move the picture.
      int cam = layer_[0].cam;
      const int camGame = (int16_t)rd16(kCamX);
      if (std::abs(cam - camGame) > 24 || snes_.wram[kCamLock]) cam = camGame;
      // A fight that takes the level's layers off the screen (the mode 7 bosses: the 4:3 screen's scenery is copied
      // to BG3 and the rest of VRAM is the boss's) leaves the margins with nothing: while the camera is held and the
      // level's layers are still there, their margins are kept every frame (Ppu::wideCapture), and shown again on
      // the lines that have them off (Ppu::wideFreeze), for as long as the camera stays where it was.
      const bool held = snes_.wram[kCamLock] != 0;
      if (frozenOk_ && cam != frozenCam_) frozenOk_ = false;
      if (captured_) {  // (last frame's margins were kept)
        frozenOk_ = true;
        frozenCam_ = capturedCam_;
        frozenL_ = capturedL_;
        frozenR_ = capturedR_;
        frozenFull_ = capturedFull_;
        mirrorBuilt_ = false;
      }
      captured_ = false;
      // (a fight that takes the layers away does so as the camera is taken, or within a few frames of it: a fight
      // that has kept them for two seconds keeps them, and its margins need not be kept every frame)
      if (!held) lockStart_ = snes_.frameCount();
      // (and the fights that do are at a room's end: elsewhere nothing is kept)
      const bool nearEnd = rightRoom(std::max(0, cam), (int16_t)rd16(kCamY), 2 * M) < M;
      capture_ = (ppu.mainScreen() & 1) && layerOk_[0] && (nearEnd || held) && snes_.frameCount() - lockStart_ < 120;
      layersAway_ = !(ppu.mainScreen() & 1) || ppu.bgMode() == 7;  // (a mode 7 fight)
      freeze_ = held && frozenOk_;
      capturedCam_ = cam;
      cam = std::max(0, cam);
      wantedMargins(cam, (int16_t)rd16(kCamY), &eL, &eR, &arenaEnd_);
      // A room that never scrolls (the shop behind a door: the camera's limit is 0, the HUD is up, nothing is level
      // data) is built for 4:3 alone: its scenery's edge tiles are repeated into the margins.
      if (!held && cam == 0 && (int)rd16(kCamMaxX) == 0 && hudAnchor && ppu.bgHofs(2) == 0 && ppu.bgVofs(2) == 0 &&
          !layersAway_ && ppu.bgMode() == 1) {
        staticRoom_ = true;
        eL = eR = M;
      }
      // (A margin that has to go goes at once: what is behind it must not be seen. One that can come back comes
      // back a few pixels a frame, so the view glides when a room's edge leaves the picture, as it does with the
      // camera at a level's ends.)
      if (!snapExt_) {
        eL = std::min(eL, extL_ + kExtStep);
        eR = std::min(eR, extR_ + kExtStep);
      }
      // (while the margins are the kept ones, they are as wide as they were)
      if (freeze_ && !capture_) {
        eL = frozenL_;
        eR = frozenR_;
        // (a fight at a room's end: centred like the others, the kept picture mirrored at the room's end)
        const int roomR = rightRoom(cam, (int16_t)rd16(kCamY), 2 * M);
        if (roomR < M && frozenL_ >= M) {
          const int end = 256 + roomR;
          if (!mirrorBuilt_) buildArenaMirror(ppu, end, 256 + M);
          eL = eR = M;
        }
        arenaEnd_ = -1;
      }
      bg2Adj_ = staticRoom_ ? 0 : parallaxAdjust(cam, eL);
      layer_[1].cam += bg2Adj_;
      capturedL_ = eL;
      capturedR_ = eR;
      capturedFull_ = rightRoom(cam, (int16_t)rd16(kCamY), 2 * M) < M;  // (the 4:3 picture too, near a room's end)
      wide_ = eL + eR > 0;
      // BG3 is the HUD when it is the fixed 32x32 map the game puts it in
      hudOn_ = hudAnchor && ppu.bgHofs(2) == 0 && ppu.bgVofs(2) == 0;
    }
    snapExt_ = false;
  } else {
    snapExt_ = true;
    // The level title card: drawn wide on black, so Mickey walks in from the picture's edge (see onExec).
    title_ = titleSeen_ && snes_.frameCount() - titleFrame_ <= 2 && reqExt_ > 0;
    // The level-clear screen: drawn wide too. Its spotlights are mode 7, which goes on over the margins by itself
    // (black past its map), so they sweep in from the picture's edges.
    clear_ = !title_ && clearSeen_ && snes_.frameCount() - clearFrame_ <= 2 && reqExt_ > 0 && ppu.bgMode() == 7;
    if (title_ || clear_) {
      eL = eR = reqExt_;
      wide_ = true;
    }
  }
  extL_ = eL;
  extR_ = eR;
  centerShift_ = (eR - eL) / 2;
  ppu.setExt(eL, eR);
  collectTrueX(ppu);
}

// The right side of a mode 7 fight's kept margins, past the room's end (screen x `end`): the last picture before
// the fight mirrored there. That picture is in the PPU's frame buffer still (this is the next frame's first line):
// it has the sprites too, which some arenas are built of (5-3's half-pipe). Mickey, and the HUD's top rows (the
// hi-score), are not taken from it but from the kept BG layers.
void MqGame::buildArenaMirror(Ppu& ppu, int end, int to) {
  // (the sprites of objects that move - Mickey, enemies - are left out: what is behind them is the kept BG layers;
  // an object that stays put, as 5-3's half-pipe does, is scenery and kept)
  static const uint8_t kSize[8][4] = {{8, 8, 16, 16}, {8, 8, 32, 32}, {8, 8, 64, 64}, {16, 16, 32, 32},
                                      {16, 16, 64, 64}, {32, 32, 64, 64}, {16, 32, 32, 64}, {16, 32, 32, 32}};
  static uint8_t skip[kFrameH][256];
  std::memset(skip, 0, sizeof skip);
  const int sel = ppu.objSizeSel() & 7;  // (OBSEL bits 5-7, as the PPU keeps them)
  const uint32_t now = (uint32_t)snes_.frameCount();
  for (int i = 0; i < 128; i++) {
    if (!liveSet_[i]) continue;
    const int x9 = ppu.oam[i * 4] | (((ppu.oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 1) << 8);
    if ((liveX_[i] & 0x1FF) != x9) continue;
    const int slot = liveSlot_[i];
    if (slot > 0 && now - lastMove_[slot] > 60) continue;  // (an object that has not moved for a second: scenery)
    const bool big = (ppu.oam[512 + (i >> 2)] >> ((i & 3) * 2 + 1)) & 1;
    const int w = kSize[sel][big ? 2 : 0], h = kSize[sel][big ? 3 : 1];
    const int sx = liveX_[i], sy = (ppu.oam[i * 4 + 1] + 1) & 0xFF;
    for (int y = sy; y < sy + h; y++)
      for (int x = std::max(0, sx); x < std::min(256, sx + w); x++) skip[y & 0xFF][x] = 1;
  }
  const bool lit = ppu.brightness() == 15;
  for (int row = 0; row < kFrameH; row++) {
    const uint32_t* fb = ppu.frame + row * kLineW;
    const int ty = (row + 1 + ppu.bgVofs(2)) >> 3;
    for (int x = end; x < to; x++) {
      const int sx = 2 * end - 1 - x;
      uint16_t c = ppu.frozen[row][sx + kMaxExt];
      if (lit && sx >= 0 && sx < 256 && !(ty >= 1 && ty <= 4) && !skip[row & 0xFF][sx]) {
        const uint32_t v = fb[sx + kMaxExt];
        c = (uint16_t)(((v >> 19) & 31) | (((v >> 11) & 31) << 5) | (((v >> 3) & 31) << 10));
      }
      ppu.frozen[row][x + kMaxExt] = c;
    }
  }
  mirrorBuilt_ = true;
}

// The parallax layer (BG2) scrolls at its own pace, r of the camera's (a half, in most levels): r * cam + c. Near a
// level's (or a room's) ends the wide picture's view stops before the camera does - the 4:3 window slides across the
// picture instead (eL from M to 2M) - and BG2, still following the camera, ran the other way across the still picture,
// twice as fast as anywhere else. BG2 is made to follow the picture instead: its left edge is at c + s * (its left
// edge in the level), s chosen so the picture's ends meet BG2's ends at the level's ends (a little less than r: the
// picture is wider than 4:3). Where the camera and BG2 go together (r = 1) nothing changes. The difference is BG2's
// extra scroll, on every line and in the margins' tiles (the whole layer then comes from the level data).
int MqGame::parallaxAdjust(int cam, int eL) {
  if (!layerOk_[1] || layersAway_) return 0;
  const int bg2 = layer_[1].cam;
  const int M = reqExt_;
  // The pace is the game's own, not guessed from the scroll values: $03F7 picks the handler at $01:C5A8 that works
  // BG2's X out of the camera's: modes 0 and 8 use cam >> 1, modes 2, 4 and 6 use cam itself (no other offset).
  // (Guessing it from bg2 / cam rounded the pace to a quarter and put a 1-2 px step backwards into the scroll.)
  double r;
  const int mode = snes_.wram[0x03F7];
  if (mode == 0 || mode == 8) r = 0.5;
  else if (mode == 2 || mode == 4 || mode == 6) r = 1.0;
  else if (std::abs(bg2 - cam) <= 2) r = 1.0;
  else if (cam >= 128) r = std::round(4.0 * bg2 / cam) / 4.0;
  else r = parallaxR_;
  if (r <= 0 || r > 1) return 0;
  if (cam >= 128) parallaxR_ = r;
  if (r == 1.0) return 0;
  // (a layer that is not at that pace - a drifting one, an effect - is left to the game)
  if (std::abs(bg2 - (int)std::floor(r * cam)) > 2 && mode != 0 && mode != 8) return 0;
  const int maxCam = std::max(0, (int)rd16(kCamMaxX));
  const int W = 256 + 2 * M;
  const double span = maxCam + 256 - W;
  const double s = span > 0 ? std::max(0.0, (r * maxCam + 256 - W) / span) : r;
  // BG2's left edge is s * (the picture's left edge in the level): a straight line of the picture's position, so it
  // never steps back and never moves while the picture is held at a level's end (the old rounded form wobbled by 1-2 px).
  return (int)std::lround(s * (cam - eL)) + eL - bg2;
}

// A byte of the page tables ($7F:FC22 BG1, $7F:FE22 BG2). While the camera is held for a fight the game clears
// pages it no longer needs there (the stage $0F boss drops the page left of its arena); what the margins show
// then is the level as it was when the camera was taken: the tables as they were then.
uint8_t MqGame::pageByte(int adr) const {
  const int a = adr & 0xFFFF;
  if (pagesKept_ && a >= kPageTable[0] && a < kPageTable[0] + 0x400) return keptPages_[a - kPageTable[0]];
  return snes_.wram[0x10000 + a];
}

// BG1's page (256x256 px) at this column and row of the level is missing: the page table's pointer is negative.
// The game's camera stops at such a page ($01:C5FB, $01:C651: it tests the page its far edge would enter, the
// indices taken to 4 bits as here), which is how a level is cut into rooms: nothing is behind it but another
// room's scenery or nothing at all.
bool MqGame::pageHole(int col, int row) const {
  const int t = kPageTable[0] + 0x20 * (row & 15) + 2 * (col & 15);
  return (pageByte(t + 1) & 0x80) != 0;
}

// How many pixels of level there are to show left of the 4:3 window (up to `most`): to the level's start, or to
// the nearest missing page in the rows of pages the picture is in.
int MqGame::leftRoom(int cam, int camY, int most) const {
  int room = std::min(cam, most);
  const int r0 = std::max(0, camY) >> 8, r1 = (std::max(0, camY) + 223) >> 8;
  for (int r = r0; r <= r1; r++) {
    for (int c = (cam - 1) >> 8; c >= 0 && (c + 1) * 256 > cam - room; c--) {
      if (!pageHole(c, r)) continue;
      room = std::min(room, std::max(0, cam - (c + 1) * 256));
      break;
    }
  }
  return room;
}

int MqGame::rightRoom(int cam, int camY, int most) const {
  const int maxCam = std::max(0, (int)rd16(kCamMaxX));
  int room = std::min(most, std::max(0, maxCam - cam));
  const int r0 = std::max(0, camY) >> 8, r1 = (std::max(0, camY) + 223) >> 8;
  const int edge = cam + 256;
  for (int r = r0; r <= r1; r++) {
    for (int c = edge >> 8; c * 256 < edge + room; c++) {
      if (!pageHole(c, r)) continue;
      room = std::min(room, std::max(0, c * 256 - edge));
      break;
    }
  }
  return room;
}

// The margins for a camera position: the 4:3 window slides inside the wide picture so the view stays in the level
// (or the room): all the extra width on the right where there is nothing to show on the left, split evenly where
// there is room on both sides, all on the left at the other end.
void MqGame::wantedMargins(int cam, int camY, int* eL, int* eR, int* arenaEnd) const {
  const int M = reqExt_, total = 2 * M;
  const int roomL = leftRoom(cam, camY, total), roomR = rightRoom(cam, camY, total);
  int l = std::min(roomL, M);
  int r = std::min(total - l, roomR);
  l = std::min(roomL, total - r);
  if (arenaEnd) *arenaEnd = -1;
  // A fight at the end of a room (the camera held where the room stops: every stage's boss) is centred: the view is
  // let past the room's end by what it lacks on the right, and the scenery there is the arena's, mirrored at the
  // end (fetchWideTile; the kept margins of a mode 7 fight, frameSetup).
  // (and so is the approach to it, once the controller that will take the camera is there: the picture is already
  // centred when the camera is taken, so nothing jumps)
  if ((snes_.wram[kCamLock] || bossCtl_) && roomR < M && roomL >= M) {
    l = r = M;
    if (arenaEnd) *arenaEnd = 256 + roomR;
  }
  *eL = l;
  *eR = r;
}


// What the PPU is to draw each sprite at: the game's own X for it, which it cut to 9 bits (a sprite 512 px away
// from another has the same 9 bits: with the objects' windows widened, an object far off one side of the wide
// picture was drawn again in the other side's margin), plus its object's entrance shift.
void MqGame::collectTrueX(Ppu& ppu) {
  ppu.clearObjTrueX();
  if (!wide_) return;
  for (int i = 0; i < 128; i++) {
    if (!liveSet_[i]) continue;
    const int x9 = ppu.oam[i * 4] | (((ppu.oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 1) << 8);
    if ((liveX_[i] & 0x1FF) != x9) continue;  // (not the list this entry is from: left to the PPU)
    ppu.objTrueX[i] = (int16_t)(liveX_[i] + liveShift_[i]);
    ppu.objTrueXSet[i] = true;
  }
}

// An object made this frame (the game's loop has not looked at it yet) and drawn in a margin: some are drawn there
// for that one frame before anything else is done with them, a mid-boss's parts with their graphics not loaded yet
// (2-1's snake); off the 4:3 screen nobody saw them. Drawn out of sight; from the next frame on it is the object's.
int MqGame::spawnHide(int slot) const {
  if (!wide_ || title_ || clear_) return 0;
  const Entrance& e = entr_[slot];
  const uint32_t now = (uint32_t)snes_.frameCount();
  const int b = 0x490 + 0x50 * slot;
  const int sx = (int16_t)(rd16(b + 0xC) - rd16(kDrawCamX));
  if ((!seen_[slot] || now - e.made < kNewFrames) && !e.inside && (sx < kInsideL || sx > kInsideR)) return 2048;
  if (e.slide && now - e.slideStart < kSlideFrames) return e.slide * (int)(kSlideFrames - (now - e.slideStart)) / kSlideFrames;
  return 0;
}

// A mid-boss holds the camera ($1D8D = 1, or its controller waits for the camera at $02:91DD) somewhere in a level,
// with the level going on on both sides of the wide picture: its arena is the wide picture, not the 4:3 screen.
bool MqGame::heldArena() const {
  const bool held = snes_.wram[kCamLock] == 1 || (lockWaitSeen_ && snes_.frameCount() - lockWaitFrame_ <= 2);
  return held && gameplay_ && wide_ && arenaEnd_ < 0 && extL_ > 0 && extR_ > 0 && !layersAway_;
}

static bool isBossKind(int k) {
  for (int b : kBossKinds)
    if (b == k) return true;
  return false;
}

// A mid-boss's flight is worked out for the 4:3 screen (it starts 20 px outside its left edge and flies across the
// 256 px). In the wide arena the same flight is spread over the wide picture: it begins outside the picture's left
// edge and every step of its X is as much longer as the picture is wider. The game's X is what changes, so what the
// boss hits and is hit by is where it is drawn.
bool MqGame::onMoveX() {
  CpuRegs& r = snes_.cpu.r;
  if (!r.mf || reqExt_ <= 0) return false;
  const int d = r.dp;
  if (d < 0x490 + 0x50 || d > 0x490 + 0x50 * 30 || (d - 0x490) % 0x50 != 0) return false;
  const int slot = (d - 0x490) / 0x50;
  if (!snes_.wram[d] || !isBossKind(snes_.wram[d + 6]) || !heldArena()) return false;
  const int cam = (int16_t)rd16(kCamX);
  const int wideW = 256 + extL_ + extR_;
  auto getX = [&]() {
    int v = snes_.wram[d + 0xB] | (snes_.wram[d + 0xC] << 8) | (snes_.wram[d + 0xD] << 16);
    return (v & 0x800000) ? v - 0x1000000 : v;
  };
  auto setX = [&](int v) {
    snes_.wram[d + 0xB] = v & 0xFF;
    snes_.wram[d + 0xC] = (v >> 8) & 0xFF;
    snes_.wram[d + 0xD] = (v >> 16) & 0xFF;
  };
  Entrance& e = entr_[slot];
  const int num = wideW - 24;  // (the flight's scale is num / 256)
  if (!e.mapped) {
    // (put where the same flight would be: its left start, -20 from the 4:3 screen, is now outside the picture)
    const int rel = getX() - (cam << 8);
    const int start = -(extL_ + kBossEntryPad);
    const int nrel = (start << 8) + (int)((int64_t)(rel + (20 << 8)) * num / 256);
    setX((cam << 8) + nrel);
    e.mapped = true;
  }
  int v = snes_.wram[d + 0x19] | (snes_.wram[d + 0x1A] << 8) | (snes_.wram[d + 0x1B] << 16);
  if (v & 0x800000) v -= 0x1000000;
  const int step = (int)((int64_t)v * num / 256);
  setX(getX() + (snes_.wram[d + 0x14] ? -step : step));
  r.pc = 0x01A0F1;  // (its RTS)
  return true;
}

// The game keeps every object (Mickey above all) within the 4:3 screen less 16 px. In a mid-boss's arena, which is
// the wide picture, that is the wide picture less 16 px: Mickey can go into the margins, and so can the mid-boss.
bool MqGame::onClamp() {
  CpuRegs& r = snes_.cpu.r;
  if (r.mf || r.x != 0 || !heldArena()) return false;
  const int cam = (int16_t)rd16(kCamX);
  const int d = r.dp;
  const int x = (int16_t)rd16(d + 0xC);
  const int lo = cam - extL_ + 16, hi = cam + 256 + extR_ - 16;
  int nx = x;
  if (x < lo) nx = lo;
  if (x > hi) nx = hi;
  if (nx != x) {
    snes_.wram[d + 0xC] = nx & 0xFF;
    snes_.wram[d + 0xD] = (nx >> 8) & 0xFF;
  }
  r.pc = kClampDonePc;
  return true;
}

// Once a frame, from the top of the game's loop: the slots that were freed, and the objects that are new.
void MqGame::scanObjects() {
  for (int n = 0; n < 31; n++) {  // (which objects move: buildArenaMirror)
    const int b = 0x490 + 0x50 * n;
    const uint32_t pos = (uint32_t)rd16(b + 0xC) | ((uint32_t)rd16(b + 0xF) << 16);
    if (!snes_.wram[b] || pos != lastPos_[n]) lastMove_[n] = (uint32_t)snes_.frameCount();
    lastPos_[n] = pos;
  }
  // Mid-bosses wait for their cue just past the 4:3 screen's edge (the waiting place is in the wide picture, where
  // they stood in plain sight, some with their graphics not loaded yet) and then jump or fly in. In a mid-boss's
  // arena (heldArena) an object that waits there, never yet on the 4:3 screen, is put as far again past the edge:
  // outside the wide picture, it makes its entrance from there.
  const int camNow = (int16_t)rd16(kCamX);
  const bool arena = heldArena();
  bossCtl_ = false;
  for (int n = 1; n < 31; n++) bossCtl_ |= snes_.wram[0x490 + 0x50 * n] && snes_.wram[0x490 + 0x50 * n + 6] == kKindBossCtl;
  for (int n = 1; n < 31; n++) {
    const int b = 0x490 + 0x50 * n;
    Entrance& e = entr_[n];
    if (!snes_.wram[b]) {
      e = Entrance();
      continue;
    }
    const int x = (int16_t)rd16(b + 0xC);
    if (!e.tracked || e.kind != snes_.wram[b + 6]) {
      e = Entrance();
      e.tracked = true;
      e.kind = snes_.wram[b + 6];
      e.made = (uint32_t)snes_.frameCount();
      e.prevX = (int16_t)x;
    }
    const int sx = x - camNow;
    // (one that was put out and is then put back by the game where it waited - 3-2's mid-boss, when its cue comes -
    // is drawn sliding in from past the picture's edge to where it is, over kSlideFrames)
    if (isBossKind(e.kind)) {
      e.prevX = (int16_t)x;
      continue;
    }
    if (e.pushed && !e.inside && std::abs(x - e.prevX) >= 32 && arena) {
      if (sx >= 256 && sx < 256 + extR_) e.slide = (int16_t)(256 + extR_ + kEntrancePad - sx);
      else if (sx < 0 && sx > -extL_) e.slide = (int16_t)(-extL_ - kEntrancePad - sx);
      e.slideStart = (uint32_t)snes_.frameCount();
    }
    e.prevX = (int16_t)x;
    if (sx >= kInsideL && sx <= kInsideR) e.inside = true;
    if (!arena || e.inside || e.pushed) continue;
    int dx = 0;
    if (sx > kInsideR && sx < 256 + extR_ + kEntrancePad) dx = extR_ + kEntrancePad;
    else if (sx < kInsideL && sx > -extL_ - kEntrancePad) dx = -(extL_ + kEntrancePad);
    if (!dx) continue;
    const uint16_t nx = (uint16_t)(rd16(b + 0xC) + dx);
    snes_.wram[b + 0xC] = nx & 0xFF;
    snes_.wram[b + 0xD] = nx >> 8;
    e.pushed = true;
    e.prevX = (int16_t)nx;
  }
  bool goofy = false;
  for (int n = 1; n < 31; n++) goofy |= snes_.wram[0x490 + 0x50 * n] && snes_.wram[0x490 + 0x50 * n + 6] == kKindGoofy;
  for (int n = 1; n < 31; n++) {
    const int b = 0x490 + 0x50 * n;
    if (!snes_.wram[b]) {
      seen_[n] = false;
      continue;
    }
    const uint8_t kind = snes_.wram[b + 6];
    if (seen_[n] && seenKind_[n] == kind) continue;
    seen_[n] = true;
    seenKind_[n] = kind;
    // Goofy (and what he carries) is put 32 px past the 4:3 screen's right edge and walks in to where he talks
    // from: in the wide picture that is in view, and he appeared out of thin air. He is put as far past the
    // picture's edge instead. (He walks until he is at his mark, so the cutscene goes on as before, a little later.)
    if (reqExt_ <= 0 || !(kind == kKindGoofy || (kind == kKindGoofyItem && goofy))) continue;
    const int cam = (int16_t)rd16(kCamX);
    const int sx = (int16_t)rd16(b + 0xC) - cam;
    if (sx <= 256 || sx > 256 + 64) continue;
    int eL = 0, eR = 0;
    wantedMargins(std::max(0, cam), (int16_t)rd16(kCamY), &eL, &eR);
    const uint16_t x = (uint16_t)(rd16(b + 0xC) + eR);
    snes_.wram[b + 0xC] = x & 0xFF;
    snes_.wram[b + 0xD] = x >> 8;
  }
}

void MqGame::beginLine(Ppu& ppu, int row) {
  if (row == 0) frameSetup(ppu);
  if (traceLines_) {
    if (row == 0) lineLog_.clear();
    lineLog_.push_back((uint32_t)ppu.bgMode() | ((uint32_t)ppu.mainScreen() << 8) | ((uint32_t)ppu.subScreen() << 16) |
                       ((uint32_t)ppu.setIni() << 24));
  }
  if (!wide_) return;
  if (clear_) {  // (the spotlights are mode 7 and go on by themselves; the text's layers stay on the 4:3 screen)
    for (int l = ppu.bgMode() == 7 ? 2 : 0; l < 4; l++) ppu.hideWideBg[l] = true;  // (mode 7: BG1, and BG2 as EXTBG)
    return;
  }
  if (title_) {  // (the title card: its layers are not continued over the margins, which stay black)
    for (int l = 0; l < 4; l++) ppu.hideWideBg[l] = true;
    return;
  }
  // A layer the game has on the subscreen only (the boss of 3-3 fading in: BG1, the cave wall left of the arena, is
  // blended onto BG2 there) would leave the margin it fills empty: the margin shows it as it is.
  if (!(ppu.mainScreen() & 1) && (ppu.subScreen() & 1)) ppu.wideSubFill = true;
  if (bg2Adj_) {
    ppu.bgHofsAdj[1] = (int16_t)bg2Adj_;
    ppu.wideFetchAll[1] = true;
  }
  // A boss room's fade (3-3: BG2, the arena's wall, has the fixed colour taken from it, up to black) leaves BG1 as
  // it is: on the 4:3 screen BG1 is the boss there, but in the margins it is the room's scenery, which goes dark too.
  {
    const uint8_t cs = ppu.colorMathSel(), cd = ppu.colorMathDesig();
    if ((cd & 0x80) && (cd & 2) && !(cd & 1) && !(cs & 2) && ((cs >> 4) & 3) == 0) ppu.wideMathLayers = 1;
  }
  if (capture_ && ppu.bgMode() == 1 && (ppu.mainScreen() & 1)) {
    ppu.wideCapture = true;
    // (BG2 too when it is off the screen for the fight's start, while it is still the level's: not repointed)
    ppu.wideCaptureLayers = (uint8_t)(1 | (ppu.bgMapBase(1) != ppu.bgMapBase(0) ? 2 : 0));
    ppu.wideCaptureAll = capturedFull_;
    // (on the 4:3 screen the game may already have the scenery on BG3, its copy for the fight: BG3 too, but not on
    // the HUD's top rows, whose right end is the hi-score)
    const int ty = (row + 1 + ppu.bgVofs(2)) >> 3;
    ppu.wideCaptureInner = (uint8_t)(ppu.wideCaptureLayers | ((ty >= 1 && ty <= 4) ? 0 : 4));
    captured_ = true;
  }
  if (freeze_ && (ppu.bgMode() == 7 || !(ppu.mainScreen() & 1))) {
    // (the level's layers are not the level's on this line: what they would draw in the margins is not scenery;
    // a mode 7 line's BG1 is the boss, which goes on over the margins)
    ppu.wideFreeze = true;
    if (ppu.bgMode() != 7) ppu.hideWideBg[0] = ppu.hideWideBg[1] = true;
  }
  if (hudOn_) {
    ppu.hideWideBg[2] = true;
    // The HUD's rows: tile rows 1-4 hold the score (left), the time and lives (middle) and the hi-score
    // (right); rows 24-27 the costume slots (left). Everything else (the level banner, the pause text)
    // is centred on the wide picture.
    const int ty = (row + 1 + ppu.bgVofs(2)) >> 3;
    PpuLayerRemap& rm = ppu.layerRemap[2];
    if (ty >= 1 && ty <= 4) {
      rm.count = 3;
      rm.seg[0] = {(int16_t)-extL_, (int16_t)(80 - extL_), 0, 0};
      rm.seg[1] = {(int16_t)(80 + centerShift_), (int16_t)(184 + centerShift_), 80, 0};
      rm.seg[2] = {(int16_t)(184 + extR_), (int16_t)(256 + extR_), 184, 0};
    } else if (ty >= 24 && ty <= 27) {
      rm.count = 1;
      rm.seg[0] = {(int16_t)-extL_, (int16_t)(256 - extL_), 0, 0};
    } else if (ppu.mainScreenWindow() & 4) {
      // (a speech box: the text is cut out by a window the game puts at its place on the 4:3 screen; moved to the
      // picture's middle the words slid out of their box. It stays where the box is.)
    } else if (centerShift_ != 0) {
      rm.count = 1;
      rm.seg[0] = {(int16_t)centerShift_, (int16_t)(256 + centerShift_), 0, 0};
    }
  }
}

int MqGame::fetchWideTile(Ppu& ppu, int layer, int mapX, int mapY, int screenX, uint16_t* entry) {
  (void)mapX;
  if (!wide_) return kWideVram;
  if (layer == 2) return hudOn_ ? kWideBlank : kWideVram;
  if (layer > 1) return kWideVram;
  if (staticRoom_) {
    // (the 4:3 screen's own columns, the first and last repeated outward)
    const int c0 = floorDiv8(layer_[layer].cam), tx = floorDiv8(layer_[layer].cam + screenX);
    const int ty = floorDiv8(layer_[layer].camY) + ((mapY - floorDiv8(layer_[layer].camY)) & 127);
    *entry = vramTile(layer, std::min(std::max(tx, c0), c0 + 31), ty);
    return kWideEntry;
  }
  if (!layerOk_[layer]) return kWideVram;  // (not level data the decoder knows: the layer's own tilemap wraps)
  const Layer& L = layer_[layer];
  const int tx = floorDiv8(L.cam + screenX);
  // (the PPU gives the row modulo 128: put it back next to the layer's real scroll)
  const int base = floorDiv8(L.camY);
  const int ty = base + ((mapY - base) & 127);
  if (ty < 0) return kWideBlank;
  if (tx < 0) {
    // (left of the level: only the parallax layer gets here, its margin reaches up to half a margin past its own
    // start: the layer's first tiles mirrored, which carries the scenery on; one column repeated drew stripes)
    *entry = decodeTile(L, -1 - tx, ty) ^ 0x4000;
    return kWideEntry;
  }
  if (arenaEnd_ >= 0 && screenX >= arenaEnd_) {
    // (past the end of a centred arena: the arena mirrored at its end, where the layer's tiles line up with it; a
    // layer that scrolls at its own pace (the parallax) has its last 256 px carried on instead)
    const int endPx = L.cam + arenaEnd_;
    if ((endPx & 7) == 0) *entry = decodeTile(L, (endPx >> 3) * 2 - 1 - tx, ty) ^ 0x4000;
    else *entry = decodeTile(L, tx - 32, ty);
    return kWideEntry;
  }
  *entry = decodeTile(L, tx, ty);
  return kWideEntry;
}

// The tile entry the PPU reads for tile (tx, ty) of BG layer `layer`, with its tilemap size and base as they are now.
uint16_t MqGame::vramTile(int layer, int tx, int ty) const {
  const int size = snes_.ppu.bgMapSize(layer);
  const int col = tx & ((size & 1) ? 63 : 31), row = ty & ((size & 2) ? 63 : 31);
  int adr = snes_.ppu.bgMapBase(layer) + (col & 31) + (row & 31) * 32;
  if (col >= 32) adr += 0x400;
  if (row >= 32) adr += (size & 1) ? 0x800 : 0x400;
  return snes_.ppu.vram[adr & 0x7FFF];
}

// Does the level-data decoder give the tiles the game has in this layer's VRAM (a sample of the visible ones)? It
// does for the streamed layers of a level; a layer that is a small repeating map (stage 3-1's rock strips) or
// anything else is not level data: its margins are the tilemap wrapping.
bool MqGame::layerMatchesVram(const Layer& L, int layer) const {
  int bad = 0, n = 0;
  for (int gy = 24; gy < 224; gy += 48) {
    for (int gx = 0; gx < 256; gx += 24) {
      const int tx = (L.cam + gx) >> 3, ty = (L.camY + gy) >> 3;
      if (ty < 0) continue;
      n++;
      if (tx < 0) {  // (a scroll that is not a place in the level: not level data)
        bad++;
        continue;
      }
      if (vramTile(layer, tx, ty) != decodeTile(L, tx, ty)) bad++;
    }
  }
  return n > 0 && bad * 10 <= n;  // (up to a tenth: the streamed edge, a block just broken)
}

int MqGame::verifyLayer(int layer, int* total) const {
  // (the layer set up as a frame's setup does it, from the PPU as it is now)
  Layer L;
  const int r[2][2] = {{kBg1HofsW, kBg1VofsW}, {kBg2HofsW, kBg2VofsW}};
  L.cam = readNear(r[layer][0], snes_.ppu.bgHofs(layer));
  L.camY = readNear(r[layer][1], snes_.ppu.bgVofs(layer));
  L.dp = kLayerDp[layer];
  L.table = kPageTable[layer];
  L.pageBank = snes_.wram[L.dp + 0x46];
  L.metaBank = snes_.wram[L.dp + 0x4C];
  int bad = 0, n = 0;
  for (int y = 0; y < 224; y += 8) {
    for (int x = 0; x < 256; x += 8) {
      const int tx = (L.cam + x) >> 3, ty = (L.camY + y) >> 3;
      n++;
      if (vramTile(layer, tx, ty) != decodeTile(L, tx, ty)) bad++;
    }
  }
  if (total) *total = n;
  return bad;
}

std::string MqGame::verifyMap(int layer) const {
  Layer L;
  const int r[2][2] = {{kBg1HofsW, kBg1VofsW}, {kBg2HofsW, kBg2VofsW}};
  L.cam = readNear(r[layer][0], snes_.ppu.bgHofs(layer));
  L.camY = readNear(r[layer][1], snes_.ppu.bgVofs(layer));
  L.dp = kLayerDp[layer];
  L.table = kPageTable[layer];
  L.pageBank = snes_.wram[L.dp + 0x46];
  L.metaBank = snes_.wram[L.dp + 0x4C];
  std::string s;
  for (int y = 0; y < 224; y += 8) {
    for (int x = 0; x < 256; x += 8) {
      const int tx = (L.cam + x) >> 3, ty = (L.camY + y) >> 3;
      s += vramTile(layer, tx, ty) != decodeTile(L, tx, ty) ? '#' : '.';
    }
    s += '\n';
  }
  return s;
}

std::string MqGame::describe() const {
  char buf[200];
  std::snprintf(buf, sizeof buf, "gameplay=%d ext=%d+%d (req %d) hud=%d cam=%d,%d bg2=%d", gameplay_, extL_, extR_, reqExt_,
                hudOn_, layer_[0].cam, layer_[0].camY, layer_[1].cam);
  return buf;
}
