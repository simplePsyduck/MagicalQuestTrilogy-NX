// MagicalQuest_nx: the application (the game, menus), the same on every platform (see app.h).
#include "app/app.h"
#ifdef __SWITCH__
#include "platform/switch/perf_nx.h"
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include "app/msupack.h"
#include "app/sys.h"
#include "app/vkeys.h"
#include "core/fileio.h"
#include "core/png.h"

const wchar_t* kAppTitle = L"The Magical Quest - Widescreen";
const char* kAppVersion = "0.1.0";
namespace {
constexpr uint32_t kKnownCrc = 0x10874c70;  // the (headerless) ROM the port was built and tested with
}  // namespace

App* g_app = nullptr;
FILE* g_log = nullptr;  // (opened by the platform)

void logf(const char* fmt, ...) {
  if (!g_log) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
}

std::string narrow(const std::wstring& w) { return narrowUtf8(w); }

bool readAll(const std::wstring& path, std::vector<uint8_t>& out) { return readFile(path, out, 16u << 20); }

bool writeAll(const std::wstring& path, const void* d, size_t n) { return writeFileSafely(path, d, n); }

static uint32_t crc32(const uint8_t* d, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t v = i;
      for (int k = 0; k < 8; k++) v = (v & 1) ? 0xedb88320u ^ (v >> 1) : v >> 1;
      table[i] = v;
    }
    init = true;
  }
  uint32_t c = 0xffffffffu;
  for (size_t i = 0; i < n; i++) c = table[(c ^ d[i]) & 0xff] ^ (c >> 8);
  return ~c;
}

// Validate that the data is The Magical Quest's ROM (headered or not).
bool isMqRom(const std::vector<uint8_t>& d, bool* exact) {
  size_t off = (d.size() & 0x3ff) == 0x200 ? 0x200 : 0;
  if (d.size() < off + 0x8000) return false;
  if (!MqGame::looksLikeMagicalQuest(d.data() + off, d.size() - off)) return false;
  if (exact) *exact = (d.size() - off == 0x100000) && crc32(d.data() + off, d.size() - off) == kKnownCrc;
  return true;
}

bool App::loadRom() {
  std::wstring path;
  // remembered location
  std::string rp;
  if (readText(pathJoin(exeDir, L"rompath.txt"), rp)) {
    while (!rp.empty() && (rp.back() == '\n' || rp.back() == '\r')) rp.pop_back();
    std::wstring w = widenUtf8(rp);
    std::vector<uint8_t> data;
    if (readAll(w, data) && isMqRom(data, nullptr)) path = w;
  }
  // (Not found: the screen says what is needed, and it is looked for again while it is shown.)
  if (path.empty() && !findRom(&path)) return false;
  std::vector<uint8_t> data;
  bool exact = false;
  if (!readAll(path, data) || !isMqRom(data, &exact)) return false;
  // (Another version or dump than the USA one: it runs, the card's ROM line in
  // amber; widescreen, cheats and the level select are tuned for the USA one.)
  const std::string remembered = narrow(path) + "\n";
  writeAll(pathJoin(exeDir, L"rompath.txt"), remembered.data(), remembered.size());
  romPath = path;
  romExact = exact;
  game.reset();  // (before the Snes it points at: its destructor uses it)
  snes = std::make_unique<Snes>();
#ifdef __SWITCH__
  snes->ppu.setThreaded(cfg.threadedVideo);  // (the picture's layers mixed on a second core)
#endif
  // (the console's copy patched for an MSU-1 pack if one is chosen)
  std::vector<uint8_t> run = data;
  applyMsu(run);
  if (!snes->loadRom(run)) {
    snes.reset();
    return false;
  }
  romData = std::move(data);
  game = std::make_unique<MqGame>(*snes);
  game->install();
  snes->reset(true);
  loadSram();
  return true;
}

// The chosen MSU-1 pack (see app/msupack.h): its patch applied to the
// console's ROM and the chip attached, or why not (msuStatus).
void App::applyMsu(std::vector<uint8_t>& rom) {
  if (msuDir.empty()) msuDir = pathJoin(exeDir, L"msu");
  makeDir(msuDir);
  msuLoaded = cfg.msuPack;
  msuStatus.clear();
  msuBase.clear();
  if (cfg.msuPack.empty()) return;
  MsuPack pk;
  std::string err;
  std::vector<uint8_t> patch;
  if (findMsuPack(msuDir, cfg.msuPack, pk, &err)) {
    if (!readFile(pk.patch, patch, 16u << 20)) {
      err = "its patch can't be read";
    } else {
      std::vector<uint8_t> patched = rom;
      if ((patched.size() & 0x3ff) == 0x200) patched.erase(patched.begin(), patched.begin() + 0x200);
      if (applyRomPatch(patch, patched, &err)) {
        rom.swap(patched);
        snes->msu = std::make_unique<Msu1>();
        snes->msu->open(pk.base);
        snes->msu->setGain(cfg.msuVolume / 100.0f * cfg.musicVolume / 100.0f);
        msuBase = pk.base;
        msuStatus = "Playing " + narrow(cfg.msuPack) + ": " + std::to_string(pk.tracks) + " tracks, patched with " +
                    narrow(pathName(pk.patch)) + ".";
        logf("MSU-1 pack %s: %d tracks, patch %s", narrow(cfg.msuPack).c_str(), pk.tracks,
             narrow(pk.patch).c_str());
        return;
      }
    }
  }
  msuStatus = "Not used: " + err + ".";
  logf("MSU-1 pack %s not used: %s", narrow(cfg.msuPack).c_str(), err.c_str());
}

// Another pack chosen: the console is rebuilt with it (outside a game: the
// no ROM screen shows). Save files are kept; a game's save states belong with the
// pack they were made with.
void App::reloadForMsu() {
  if (!snes || cfg.msuPack == msuLoaded) return;
  saveSram();
  if (loadRom()) romReady();
}

void App::romReady() {
  applyConfig();
  if (!romExact) logf("ROM is not the dump the port was checked with (CRC differs): running it anyway");
  if (!menuHintShown && !test.on) {
    menuHintShown = true;
    menu.notify(cfg.selectMode == 0 ? "Press Select or Esc for the menu" : "Hold Select or press Esc for the menu", 4.0f,
                true);
  }
}

void App::pollRom(double dt) {
  if (snes || (romPoll -= dt) > 0) return;
  romPoll = 1.0;
  if (!loadRom()) return;
  logf("rom found: %s", narrow(romPath).c_str());
  romReady();
  menu.notify("The Magical Quest ROM found");
}

// Reset Game: the console as at power-on (the save file and the settings kept).
void App::resetGame() {
  if (!snes) return;
  reloadForMsu();  // (a pack chosen in a game: in use from this start)
  if (!snes) return;
  if (rewinding) endRewind();
  saveSram();
  snes->reset(false);
  game->onStateLoaded();
  rewind.clear();
  audio.flush();
  menu.closeAll();
  applyConfig();
}

// Black frame insertion's state, beside its switch.
int App::hfrSubFrames() const {
  if (!cfg.highFrameRate || cfg.blackFrames || pacing.hz < 89) return 1;
  return std::clamp((int)std::lround(pacing.hz / Snes::kFrameRate + 0.25), 2, Ppu::kMaxSubFrames);
}

std::string App::blackFramesNote() const {
  if (!cfg.blackFrames) return "";
  if (!cfg.vsync) return "needs VSync";
  char buf[48];
  if (pacing.perFrame >= 2) snprintf(buf, sizeof buf, "%.0f Hz", pacing.hz);
  else snprintf(buf, sizeof buf, "needs 120 Hz (now %.0f)", pacing.hz);
  return buf;
}

void App::loadSram() {
  std::vector<uint8_t> d;
  if (readAll(pathJoin(saveDir, L"mq.srm"), d) && d.size() >= sizeof(snes->sram)) {
    memcpy(snes->sram, d.data(), sizeof(snes->sram));
  }
  snes->sramDirty = false;
}

void App::saveSram() {
  if (!snes) return;
  if (writeAll(pathJoin(saveDir, L"mq.srm"), snes->sram, sizeof(snes->sram))) {
    snes->sramDirty = false;
    sramFailed = false;
  } else if (!sramFailed) {  // (still dirty: tried again later)
    sramFailed = true;
    logf("saving game progress failed");
    menu.notify("Could not save game progress (is the disk full?)");
  }
}

std::wstring App::statePath(int slot) const { return pathJoin(saveDir, L"state" + std::to_wstring(slot) + L".sst"); }

// A rewind snapshot back in the machine. Battery saves are kept as they are
// now: rewinding never takes back progress the game has saved.
void App::restoreSnapshot(const std::vector<uint8_t>& st) {
  std::vector<uint8_t> keep(snes->sram, snes->sram + sizeof(snes->sram));
  bool dirty = snes->sramDirty;
  if (!snes->loadState(st)) return;
  std::memcpy(snes->sram, keep.data(), keep.size());
  snes->sramDirty = dirty;
  game->onStateLoaded();
}

// While Rewind is held: `steps` snapshots back (each two frames, so twice the
// game's speed), the oldest staying on screen. Each is drawn by running a
// frame from it, silently; letting go returns to the last one shown (see
// endRewind). False (play on) when there is nothing to go back to.
bool App::stepRewind(int steps) {
  if (!rewinding && rewind.snapshots() < 2) return false;
  if (steps <= 0) return true;
  std::vector<uint8_t> st;
  bool moved = false;
  for (int i = 0; i < steps && rewind.stepBack(&st); i++) moved = true;
  if (!moved) {
    if (rewinding) return true;  // (at the oldest: stay there)
    return false;
  }
  if (!rewinding) audio.flush();
  rewinding = true;
  restoreSnapshot(st);
  game->beforeFrame();
  snes->runFrame();
  game->afterFrame();
  audioBuf.clear();
  snes->takeAudio(audioBuf);  // (not played)
  fpsFrames++;
  return true;
}

void App::endRewind() {
  rewinding = false;
  if (!rewind.newest().empty()) restoreSnapshot(rewind.newest());
  audio.flush();
}

void App::saveState(int slot) {
  auto st = snes->saveState();
  if (writeAll(statePath(slot), st.data(), st.size())) menu.notify("State " + std::to_string(slot) + " saved");
  else menu.notify("Could not save state " + std::to_string(slot) + " (is the disk full?)");
}

void App::loadState(int slot) {
  std::vector<uint8_t> d;
  if (!readAll(statePath(slot), d)) {
    menu.notify("State " + std::to_string(slot) + " is empty");
    return;
  }
  if (snes->loadState(d)) {
    game->onStateLoaded();
    rewind.clear();  // (the past belongs to another timeline now)
    menu.notify("State " + std::to_string(slot) + " loaded");
    audio.flush();
  } else {
    menu.notify("State " + std::to_string(slot) + " is not compatible");
  }
}

void App::screenshot() {
  makeDir(shotDir);
  const std::wstring name = widenUtf8(timeText("mq_%Y%m%d_%H%M%S.png"));
  int eL = snes->ppu.extL, eR = snes->ppu.extR;
  bool ok = writePng(narrow(pathJoin(shotDir, name)), snes->ppu.frame + (kMaxExt - eL), 256 + eL + eR,
                     snes->ppu.frameHeight(), kLineW);
  menu.notify(ok ? "Screenshot saved" : "Screenshot failed");
}

// ---------------------------------------------------------------------------
// Display


void App::applyConfig() {
  audio.setVolume(cfg.volume / 100.0f);
  audio.setLatency(cfg.audioLatency);
  menu.confirmRight = cfg.confirmRight;
  menu.style = cfg.menuStyle;
  if (snes) snes->ppu.noSpriteLimit = cfg.noSpriteLimit;
#ifdef __SWITCH__
  if (snes) snes->ppu.setThreaded(cfg.threadedVideo);
#endif
  // (a music pack's songs: its own volume)
  if (snes && snes->msu) snes->msu->setGain(cfg.msuVolume / 100.0f);
  if (game) {
    game->hudAnchor = cfg.hudAnchor;
    game->setEnemyRange(cfg.widescreenEnemies != 0);
  }
}

// Decide the widescreen margin and the destination rectangle.
void App::computeLayout(IRect* dest, int* extOut, int* srcW) {
  int W = video.width(), H = video.height();
  double par = cfg.pixelAspectCrt ? 8.0 / 7.0 : 1.0;
  double target;
  switch (cfg.aspect) {
    case kAspect4x3: target = 0; break;
    case kAspect16x10: target = 16.0 / 10.0; break;
    case kAspect16x9: target = 16.0 / 9.0; break;
    case kAspect21x9: target = 21.0 / 9.0; break;
    default: target = (double)W / std::max(H, 1); break;
  }
  int wantExt = 0;
  if (target > 0) {
    double needW = 224.0 * target / par;
    wantExt = (int)std::floor((needW - 256.0) / 2.0);
    wantExt = std::clamp(wantExt, 0, MqGame::kMaxSupportedExt);
  }
  // A minimised window has a 1x1 client area: keep the last layout, or the
  // game would switch to 4:3 (and its widescreen patches off) meanwhile.
  if (W < 16 || H < 16) wantExt = lastWantExt;
  lastWantExt = wantExt;
  game->setWidescreen(wantExt);
  int extL = game->activeExtL(), extR = game->activeExtR();
  *extOut = extL;
  int sw = 256 + extL + extR;
  *srcW = sw;
  int sh = snes->ppu.frameHeight();
  double imgW = sw * par, imgH = sh;
  // (The scale is the full wide picture's whatever is shown: a screen the
  // game shows 4:3 in a widescreen session (SMB2's open vertical areas, the
  // menus) keeps the size of the rest, beside it the pillarbox; it was drawn
  // a fifth bigger on a 16:9 display, the HUD jumping, as the view switched.)
  int fullSw = std::max(sw, 256 + 2 * wantExt);
  double dw, dh;
  if (cfg.scaling == 2) {  // stretch: fill the target aspect area
    double fullW = (target > 0 ? 224.0 * target : 256.0 * par);
    double s = std::min(W / fullW, H / imgH);
    dh = imgH * s;
    dw = imgW * s;
    if (extL + extR >= 2 * wantExt || target <= 0) {
      dw = std::min((double)W, fullW * s);
    }
  } else if (cfg.scaling == 1) {  // integer: whole multiples on both axes
    // (CRT pixels: the nearest whole width to 8/7 of the height)
    int s = std::max(1, (int)std::floor(H / imgH)), hs;
    for (;; s--) {
      hs = std::max(1, (int)std::lround(s * par));
      if (fullSw * hs <= W || s == 1) break;
    }
    dw = (double)sw * hs;
    dh = imgH * s;
  } else {
    double s = std::min(W / (fullSw * par), H / imgH);
    dw = imgW * s;
    dh = imgH * s;
    // The margins are whole pixels: fill the last bit of a display the
    // chosen aspect matches (a stretch of under two pixels).
    if (target > 0 && extL + extR >= 2 * wantExt && dw < W && W - dw < 2 * par * s + 1) dw = W;
  }
  int x0 = (int)std::lround((W - dw) / 2), y0 = (int)std::lround((H - dh) / 2);
  *dest = IRect{x0, y0, x0 + (int)std::lround(dw), y0 + (int)std::lround(dh)};
}

// ---------------------------------------------------------------------------
// Input

void App::readPlayer(int p, uint16_t* out, double dt) {
  float dz = cfg.deadzone / 100.0f;
  int padIdx = cfg.padIndex[p];
  bool act[kActCount];
  for (int a = 0; a < kActCount; a++) {
    act[a] = input.active(cfg.keys[p][a], padIdx, dz) || input.active(cfg.pad[p][a], padIdx, dz);
  }
  // Left stick always drives the D-pad
  if (const PadState* ps = input.pad(padIdx)) {
    act[kActLeft] |= ps->axes[0] < -dz;
    act[kActRight] |= ps->axes[0] > dz;
    act[kActUp] |= ps->axes[1] < -dz;
    act[kActDown] |= ps->axes[1] > dz;
  }
  // Select and the menu.
  // * The Menu bindings (Esc; the pad's View/Select by default) open the menu.
  // * selectMode 0: Select opens the menu too. The game's own Select is on the left
  //   stick button.
  // * selectMode 1: tap Select = game Select, hold Select = menu.
  auto isSelectBinding = [&](const Binding& b) { return b == cfg.keys[p][kActSelect] || b == cfg.pad[p][kActSelect]; };
  bool select = input.active(cfg.keys[p][kActSelect], padIdx, dz) || input.active(cfg.pad[p][kActSelect], padIdx, dz);
  bool menuBtn = false;
  for (const Binding* b : {&cfg.keys[p][kActMenu], &cfg.pad[p][kActMenu]})
    if (b->type != Binding::None && !isSelectBinding(*b) && input.active(*b, padIdx, dz)) menuBtn = true;
  bool menuPressed = menuBtn && !prevMenuBtn[p];
  bool selectOut = false;
  if (p != 0) {
    selectOut = select;  // player 2 has no menu button
  } else if (cfg.selectMode == 0) {
    if (select && !prevSelect[p]) menuPressed = true;
    bool lsFree = true;
    for (int a = 0; a < kActCount; a++)
      if (cfg.pad[p][a].type == Binding::PadButton && cfg.pad[p][a].code == 8) lsFree = false;
    const PadState* ps = input.pad(padIdx);
    if (lsFree && ps && ps->buttons[8]) selectOut = true;  // left stick press
  } else {
    if (select) {
      if (menu.open()) selectConsumed[p] = true;  // a press in the menu (Back) isn't a game tap
      selectHeld[p] += (float)dt;
      if (selectHeld[p] >= 0.4f && !selectConsumed[p]) {
        selectConsumed[p] = true;
        menuPressed = true;
      }
    } else {
      if (selectHeld[p] > 0 && !selectConsumed[p]) selectPulse[p] = 0.1f;  // tap -> game Select
      selectHeld[p] = 0;
      selectConsumed[p] = false;
    }
  }
  prevSelect[p] = select;
  prevMenuBtn[p] = menuBtn;
  if (selectPulse[p] > 0) {
    selectOut = true;
    selectPulse[p] -= (float)dt;
  }

  uint16_t v = 0;
  if (act[kActUp]) v |= kPadUp;
  if (act[kActDown]) v |= kPadDown;
  if (act[kActLeft]) v |= kPadLeft;
  if (act[kActRight]) v |= kPadRight;
  if (act[kActA]) v |= kPadA;
  if (act[kActB]) v |= kPadB;
  if (act[kActX]) v |= kPadX;
  if (act[kActY]) v |= kPadY;
  if (act[kActL]) v |= kPadL;
  if (act[kActR]) v |= kPadR;
  if (act[kActStart]) v |= kPadStart;
  if (selectOut) v |= kPadSelect;
  // no opposing directions
  if ((v & kPadUp) && (v & kPadDown)) v &= ~(kPadUp | kPadDown);
  if ((v & kPadLeft) && (v & kPadRight)) v &= ~(kPadLeft | kPadRight);
  *out = v;

  if (p == 0) {
    if (menuPressed && !menu.open() && !noRom()) {
      logf("menu opened by %s (select key=%d pad=%d, menu button=%d, pad %d)",
           select ? "Select" : "Menu button", (int)input.active(cfg.keys[p][kActSelect], padIdx, dz),
           (int)input.active(cfg.pad[p][kActSelect], padIdx, dz), (int)menuBtn, padIdx);
      openMenu();
    }
    auto edge = [&](int a) { return act[a] && !prevAction[p][a]; };
    if (!menu.open() && !noRom()) {
      if (edge(kActSaveState)) saveState(stateSlot);
      if (edge(kActLoadState)) loadState(stateSlot);
      if (edge(kActScreenshot)) screenshot();
    }
#if !defined(__ANDROID__) && !defined(__SWITCH__)
    if (!menu.open()) {
      if (edge(kActFullscreen)) {
        cfg.displayMode = cfg.displayMode == DisplayMode::Windowed ? DisplayMode::Borderless : DisplayMode::Windowed;
        applyDisplayMode();
      }
    }
#endif
    const bool inGame = !noRom() && game;
    act[kActRewind] |= touch.held(TouchControls::BtnRewind);
    act[kActFastForward] |= touch.held(TouchControls::BtnFF);
    rewindHeld = cfg.rewind && act[kActRewind] && !menu.open() && inGame;
    fastForward = cfg.fastForward && act[kActFastForward] && !menu.open() && !rewindHeld && inGame;
  }
  for (int a = 0; a < kActCount; a++) prevAction[p][a] = act[a];
}

MenuInput App::menuInput() {
  MenuInput m;
  float dz = 0.5f;
  bool nav[8] = {};
  nav[0] = input.keyDown(VK_UP);
  nav[1] = input.keyDown(VK_DOWN);
  nav[2] = input.keyDown(VK_LEFT);
  nav[3] = input.keyDown(VK_RIGHT);
  nav[4] = input.keyDown(VK_RETURN) || input.keyDown(VK_SPACE);
  nav[5] = input.keyDown(VK_ESCAPE) || input.keyDown(VK_BACK);
  // When Select opens the menu, Select also backs out of it.
  if (cfg.selectMode == 0)
    nav[5] |= input.active(cfg.keys[0][kActSelect], cfg.padIndex[0], 0.5f) ||
              input.active(cfg.pad[0][kActSelect], cfg.padIndex[0], 0.5f);
  nav[6] = input.keyDown(VK_PRIOR) || input.keyDown('Q');
  nav[7] = input.keyDown(VK_NEXT) || input.keyDown('E');
  for (int i = 0; i < input.padCount(); i++) {
    const PadState* p = input.pad(i);
    if (!p) continue;
    nav[0] |= p->buttons[10] || p->axes[1] < -dz;
    nav[1] |= p->buttons[11] || p->axes[1] > dz;
    nav[2] |= p->buttons[12] || p->axes[0] < -dz;
    nav[3] |= p->buttons[13] || p->axes[0] > dz;
    // (face buttons by position: 0 bottom, 1 right; Menu Buttons picks which confirms)
    const int ok = cfg.confirmRight ? 1 : 0;
    nav[4] |= p->buttons[ok] || p->buttons[7];
    nav[5] |= p->buttons[1 - ok] || p->buttons[6];
    nav[6] |= p->buttons[4];
    nav[7] |= p->buttons[5];
  }
  // directions are level-triggered (menu handles repeat); buttons are edges
  m.up = nav[0];
  m.down = nav[1];
  m.left = nav[2];
  m.right = nav[3];
  m.confirm = nav[4] && !prevMenuNav[4];
  m.back = nav[5] && !prevMenuNav[5];
  m.tabLeft = nav[6] && !prevMenuNav[6];
  m.tabRight = nav[7] && !prevMenuNav[7];
  for (int i = 0; i < 8; i++) prevMenuNav[i] = nav[i];
  return m;
}

void App::openMenu() {
  if (menu.open()) return;
  menu.push(pageMain());
  menuAnim = 0;
  // swallow the button that opened the menu
  menuInput();
  prevMenuNav[4] = prevMenuNav[5] = true;
}

void App::updateInput(double dt) {
  input.poll();
  for (int p = 0; p < 2; p++) readPlayer(p, &padState[p], dt);
  touchFrame();
  padState[0] |= touch.takeSnesButtons();
  if (!snes) return;
  snes->input[0] = menu.open() || noRom() ? 0 : padState[0];
  snes->input[1] = menu.open() || noRom() ? 0 : padState[1];
}

// ---------------------------------------------------------------------------
// Rebinding

void App::startBind(int player, bool pad, int action) {
  bindPlayer = player;
  bindPad = pad;
  bindAction = action;
  bindTimer = 6.0;
  menu.capturing = true;
  input.resetCapture();
}

void App::updateBind(double dt) {
  if (bindAction < 0) return;
  bindTimer -= dt;
  char buf[160];
  snprintf(buf, sizeof buf, "Press a %s for \"%s\"  (Esc to cancel, Delete to clear)  %d",
           bindPad ? "controller button" : "key", actionName(bindAction), (int)std::ceil(bindTimer));
  menu.captureText = buf;
  bool cancel = bindTimer <= 0;
  Binding b;
  if (input.keyDown(VK_ESCAPE)) cancel = true;
  else if (input.keyDown(VK_DELETE)) {
    (bindPad ? cfg.pad : cfg.keys)[bindPlayer][bindAction] = Binding();
    cancel = true;
  } else if (input.capture(&b, bindPad, cfg.padIndex[bindPlayer], cfg.deadzone / 100.0f)) {
    (bindPad ? cfg.pad : cfg.keys)[bindPlayer][bindAction] = b;
    cancel = true;
  }
  if (cancel) {
    bindAction = -1;
    menu.capturing = false;
    prevMenuNav[4] = prevMenuNav[5] = true;  // don't let the key also act in the menu
    menu.refresh();
    cfg.save(configPath);
  }
}

// ---------------------------------------------------------------------------
// Menu pages

std::shared_ptr<MenuPage> App::pageMain() {
  auto p = std::make_shared<MenuPage>();
  p->rebuild = [this](MenuPage& pg) {
    pg.title = "The Magical Quest";
    pg.subtitle = "Paused";
    pg.items.clear();
    pg.items.push_back(MenuItem::action("Resume", [this]() { menu.closeAll(); }, "Return to the game."));

    pg.items.push_back(MenuItem::header("Save states"));
    std::vector<std::string> slotNames;
    for (int i = 1; i <= kStateSlots; i++) slotNames.push_back(std::to_string(i));
    MenuItem slot = MenuItem::choice(
        "Slot", slotNames, [this]() { return std::clamp(stateSlot, 1, kStateSlots) - 1; },
        [this](int v) { stateSlot = std::clamp(v + 1, 1, kStateSlots); },
        "Choose the save-state slot (" + bindingsText(kActSaveState) + " saves, " + bindingsText(kActLoadState) +
            " loads in game).");
    slot.valueText = [this]() {
      // the slot and when it was saved (or that it's empty)
      std::string t = std::to_string(stateSlot) + "  \xE2\x80\xA2  ";
      int day, month, hour, minute;
      if (!fileModified(statePath(stateSlot), &day, &month, &hour, &minute)) return t + "Empty";
      static const char* kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
      char buf[48];
      snprintf(buf, sizeof buf, "%d %s, %02d:%02d", day, kMon[(month + 11) % 12], hour, minute);
      return t + buf;
    };
    pg.items.push_back(slot);
    pg.items.push_back(MenuItem::action("Save State", [this]() {
      saveState(stateSlot);
      menu.closeAll();
    }, "Save a snapshot of the game to the selected slot."));
    pg.items.push_back(MenuItem::action("Load State", [this]() {
      if (!fileExists(statePath(stateSlot))) {
        menu.notify("Slot " + std::to_string(stateSlot) + " is empty");
        return;
      }
      loadState(stateSlot);
      menu.closeAll();
    }, "Restore the snapshot in the selected slot."));

    pg.items.push_back(MenuItem::header("Game"));
    pg.items.push_back(MenuItem::submenu("Options", [this]() { menu.push(pageOptions()); },
                                         "Display (widescreen), graphics, colour, audio, controls and speed."));
    pg.items.push_back(MenuItem::submenu("Credits", [this]() { menu.push(pageCredits()); },
                                         "What this is built on."));

    pg.items.push_back(MenuItem::header("System"));
    pg.items.push_back(MenuItem::action("Take Screenshot", [this]() { screenshot(); },
                                        "Saves the game picture (without the menu) to the screenshots folder (" +
                                            bindingsText(kActScreenshot) + " in game)."));
    pg.items.push_back(MenuItem::action("Reset Game", [this]() {
      menu.confirm("Reset the game?", [this]() {
        resetGame();
      }, "Progress is kept only if the game saved it.");
    }, "Start the game again, as at power-on."));
    pg.items.push_back(MenuItem::action("Quit", [this]() {
      menu.confirm("Quit?", [this]() { running = false; },
                   "Progress is kept only if the game saved it.");
    }, "Save your settings and exit."));
  };
  p->onLeave = [this]() { cfg.save(configPath); };
  return p;
}

std::string App::bindingsText(int a) const {
  std::string k = bindingName(cfg.keys[0][a]), j = bindingName(cfg.pad[0][a]);
  bool hk = !k.empty() && k != "-", hj = !j.empty() && j != "-";
  return hk && hj ? k + " / " + j : hk ? k : hj ? j : "unbound";
}

std::shared_ptr<MenuPage> App::pageOptions() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Options";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    pg.items.push_back(MenuItem::submenu("Display", [this]() { menu.push(pageDisplay()); },
                                         "Fullscreen, monitor, widescreen and scaling."));
    pg.items.push_back(MenuItem::submenu("Graphics", [this]() { menu.push(pageGraphics()); },
                                         "Filters, CRT effects and sprite options."));
    pg.items.push_back(MenuItem::submenu("Colour", [this]() { menu.push(pageColor()); },
                                         "Colour profiles, picture adjustments and accessibility filters."));
    pg.items.push_back(MenuItem::submenu("Audio", [this]() { menu.push(pageAudio()); },
                                         "Volume, latency and custom soundtracks (MSU-1 music packs)."));
    pg.items.push_back(MenuItem::submenu("Controls", [this]() { menu.push(pageControls()); },
                                         "Keyboard and joypad bindings for both players."));
    pg.items.push_back(MenuItem::choice(
        "Menu Style", {"Storybook", "Retro", "Themed Dark", "Classic"}, [this]() { return cfg.menuStyle; },
        [this](int v) {
          cfg.menuStyle = v;
          applyConfig();
        },
        "How these menus look: Storybook (bright and chunky), Retro (black, bricks and a pixel font), Themed Dark "
        "(dark, under the game's own heading) or Classic (the original dark panel)."));
    pg.items.push_back(MenuItem::header("Troubleshooting"));
    const struct { const char* name; int group; const char* help; } kGroups[] = {
        {"Object Patch: On-Screen Window", MqGame::kOnScreen,
         "Objects count as on screen across the wide picture. Off: the 4:3 window (objects in the margins stay frozen "
         "or are removed). Takes effect at once."},
        {"Object Patch: Active Range", MqGame::kActiveRange,
         "Objects are kept alive across the wide picture instead of being removed when they leave a 4:3 sized "
         "range. Takes effect at once."},
        {"Object Patch: Spawn Window", MqGame::kSpawnWindow,
         "Level objects are placed across the wide picture, also when the camera scrolls up or down. Takes effect "
         "when the next level or room loads."},
        {"Object Patch: Level-Start Centre", MqGame::kLevelStartCentre,
         "The camera centre the object tests use is set before a level's objects are placed. Takes effect when the "
         "next level or room loads."},
    };
    for (const auto& g : kGroups) {
      const int bit = g.group;
      pg.items.push_back(MenuItem::toggle(g.name, [this, bit]() { return game && game->patchGroup(bit) ? 1 : 0; },
                                          [this, bit](int) {
                                            if (game) game->setPatchGroup(bit, !game->patchGroup(bit));
                                          },
                                          std::string(g.help) + " For finding which patch a problem comes from: leave all on to play."));
    }
    pg.items.push_back(MenuItem::toggle("Object Event Log", [this]() { return game && game->eventLog() ? 1 : 0; },
                                        [this](int) {
                                          if (game) game->setEventLog(!game->eventLog());
                                        },
                                        "Records every object spawn and removal while you play (a little slower). "
                                        "Turn it on, play to the problem, then use Log Debug Info."));
    pg.items.push_back(MenuItem::action("Log Debug Info", [this]() {
      if (!game) return;
      logf("---- debug info ----");
      std::string d = game->debugDump();
      size_t pos = 0;
      while (pos < d.size()) {
        size_t nl = d.find('\n', pos);
        if (nl == std::string::npos) nl = d.size();
        logf("%s", d.substr(pos, nl - pos).c_str());
        pos = nl + 1;
      }
      logf("---- end of debug info ----");
      menu.notify("Written to mq_port.log");
    }, "Writes the camera, spawn window, sprite budget, object table and recent events to mq_port.log: send it "
       "with a report of an object or boss that is missing."));
    pg.items.push_back(MenuItem::header("Speed"));
    MenuItem ff = MenuItem::toggle("Fast Forward", [this]() { return cfg.fastForward ? 1 : 0; },
                                   [this](int v) { cfg.fastForward = v != 0; },
                                   "Hold " + bindingsText(kActFastForward) + " to run the game at 4x speed (sound off).");
    pg.items.push_back(ff);
    MenuItem rw = MenuItem::toggle("Rewind", [this]() { return cfg.rewind ? 1 : 0; },
                                   [this](int v) {
                                     cfg.rewind = v != 0;
                                     if (!v) rewind.clear();
                                   },
                                   "Hold " + bindingsText(kActRewind) +
                                       " to step back through the last minute of play. Saved game progress is "
                                       "never undone.");
    pg.items.push_back(rw);
    MenuItem cpuItem = MenuItem::choice(
        "Game CPU Speed", {"100% (stock)", "125%", "150%", "175%", "200%", "225%", "250%", "275%", "300%", "325%", "350%", "375%", "400%"},
        [this]() { return (cfg.emuCpuPercent - 100) / 25; }, [this](int v) { cfg.emuCpuPercent = 100 + 25 * v; },
        "How fast the game's own processor runs, as a percentage of the real console's. Above 100% the busy scenes that slowed down on the console run smoother. It makes the Switch work harder (the game's code runs that many times more often), so use the lowest setting that is smooth enough; a few games may misbehave at the highest settings.");
    pg.items.push_back(cpuItem);
    pg.items.push_back(MenuItem::header("Hotkeys"));
    auto hot = [&](const char* what, int a) {
      MenuItem it = MenuItem::info(what);
      it.valueText = [this, a]() { return bindingsText(a); };
      pg.items.push_back(it);
    };
    hot("Menu", kActMenu);
    hot("Save state", kActSaveState);
    hot("Load state", kActLoadState);
    hot("Fast forward (hold)", kActFastForward);
    hot("Rewind (hold)", kActRewind);
    hot("Fullscreen", kActFullscreen);
    hot("Screenshot", kActScreenshot);
  };
  p->onLeave = [this]() { cfg.save(configPath); };
  return p;
}

// ---------------------------------------------------------------------------
// Achievements

std::shared_ptr<MenuPage> App::pageCredits() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Credits";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    pg.items.push_back(MenuItem::info("MagicalQuest_nx", "Widescreen for The Magical Quest Starring Mickey Mouse"));
    pg.items.push_back(MenuItem::header("Built on"));
    pg.items.push_back(MenuItem::info("The smas-v4 base project",
                                      "Its SNES emulation core, menus, MSU-1 music pack support and Switch layer."));
    pg.items.push_back(MenuItem::header("The game"));
    pg.items.push_back(MenuItem::info("Not included",
                                      "Put your own ROM (.sfc or .smc) in the roms folder next to the .nro."));
  };
  return p;
}

std::shared_ptr<MenuPage> App::pageDisplay() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Display";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
#if !defined(__ANDROID__) && !defined(__SWITCH__)
    pg.items.push_back(MenuItem::choice(
        "Display Mode", {"Windowed", "Fullscreen"},
        [this]() { return cfg.displayMode == DisplayMode::Windowed ? 0 : 1; },
        [this](int v) {
          cfg.displayMode = v == 0 ? DisplayMode::Windowed : DisplayMode::Borderless;
          applyDisplayMode();
          menu.refresh();
        },
        "F11 toggles windowed mode."));
#endif
    pg.items.push_back(MenuItem::choice(
        "Aspect Ratio", {"4:3 (Original)", "16:10 Widescreen", "16:9 Widescreen", "21:9 Ultrawide", "Auto (Fill Display)"},
        [this]() { return cfg.aspect; },
        [this](int v) {
          cfg.aspect = v;
          if (cfg.displayMode == DisplayMode::Windowed) applyDisplayMode();
        },
        "Widescreen shows more of each level to the left and right. Auto matches your display."));
    pg.items.push_back(MenuItem::choice("Pixel Shape", {"CRT (8:7, as on a TV)", "Square (1:1)"},
                                        [this]() { return cfg.pixelAspectCrt ? 0 : 1; },
                                        [this](int v) { cfg.pixelAspectCrt = v == 0; },
                                        "CRT matches how the game looked on a television."));
    pg.items.push_back(MenuItem::choice("Scaling", {"Fit", "Integer (Pixel Perfect)", "Stretch"},
                                        [this]() { return cfg.scaling; }, [this](int v) { cfg.scaling = v; },
                                        "Integer scaling keeps every pixel the same size (with CRT pixels, as close to "
                                        "8:7 as whole screen pixels allow)."));
    pg.items.push_back(MenuItem::toggle("Re-anchor HUD", [this]() { return cfg.hudAnchor ? 1 : 0; },
                                        [this](int v) {
                                          cfg.hudAnchor = v;
                                          applyConfig();
                                        },
                                        "In widescreen, move the score and the hi-score out to the screen edges, and the "
                                        "costume slots to the lower left. The time and lives stay in the middle."));
    pg.items.push_back(MenuItem::choice("Widescreen Enemies", {"Original Timing", "Active In Full View"},
                                        [this]() { return cfg.widescreenEnemies; },
                                        [this](int v) {
                                          cfg.widescreenEnemies = v;
                                          applyConfig();
                                        },
                                        "Active: enemies and objects are on screen and spawn across the whole wide view. "
                                        "Original: the 4:3 game's timing, so they pop in at the edges of its picture."));
    pg.items.push_back(MenuItem::choice("Side Borders", {"Black", "Blurred Game", "Dark Frame"},
                                        [this]() { return cfg.pillarbox; }, [this](int v) { cfg.pillarbox = v; },
                                        "What fills the screen where the game has nothing to show (menus, 4:3)."));
    pg.items.push_back(MenuItem::toggle("Show FPS", [this]() { return cfg.showFps ? 1 : 0; },
                                        [this](int v) { cfg.showFps = v; }));
  };
  return p;
}

std::shared_ptr<MenuPage> App::pageGraphics() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Graphics";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    pg.items.push_back(MenuItem::choice(
        "Filter", {"Nearest (Sharp Pixels)", "Sharp Bilinear", "Smooth (Bilinear)", "CRT Scanlines", "CRT Aperture Grille",
                   "LCD Grid", "Scale2x", "Scale4x", "xBR 2x", "xBR 4x"},
        [this]() { return cfg.filter; }, [this](int v) { cfg.filter = v; },
        "Sharp Bilinear keeps pixels crisp without uneven sizes. Scale2x/4x and xBR smooth the pixel-art stair-steps."));
    pg.items.push_back(MenuItem::slider("Scanline / Effect Strength", 0, 100, 5, [this]() { return cfg.scanlines; },
                                        [this](int v) { cfg.scanlines = v; }, "%",
                                        "Strength of the CRT and LCD effects."));
    pg.items.push_back(MenuItem::slider("Screen Curvature", 0, 100, 5, [this]() { return cfg.crtCurve; },
                                        [this](int v) { cfg.crtCurve = v; }, "%", "Simulated curved CRT glass."));
    pg.items.push_back(MenuItem::toggle("Remove Sprite Limit", [this]() { return cfg.noSpriteLimit ? 1 : 0; },
                                        [this](int v) {
                                          cfg.noSpriteLimit = v;
                                          applyConfig();
                                        },
                                        "Reduce sprite flicker by lifting the 32 sprites per line hardware limit."));
  };
  return p;
}

std::shared_ptr<MenuPage> App::pageColor() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Colour";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    pg.items.push_back(MenuItem::choice(
        "Colour Profile",
        {"Original", "Vivid", "CRT Television", "Muted", "Greyscale", "Sepia", "Pocket (Green)", "Virtual (Red)",
         "Night (Warm)", "Console Accurate"},
        [this]() { return cfg.colorProfile; }, [this](int v) { cfg.colorProfile = v; },
        "Overall colour style of the picture."));
    pg.items.push_back(MenuItem::slider("Brightness", 50, 150, 5, [this]() { return cfg.brightness; },
                                        [this](int v) { cfg.brightness = v; }, "%"));
    pg.items.push_back(MenuItem::slider("Contrast", 50, 150, 5, [this]() { return cfg.contrast; },
                                        [this](int v) { cfg.contrast = v; }, "%"));
    pg.items.push_back(MenuItem::slider("Saturation", 0, 200, 5, [this]() { return cfg.saturation; },
                                        [this](int v) { cfg.saturation = v; }, "%"));
    pg.items.push_back(MenuItem::slider("Gamma", 50, 150, 5, [this]() { return cfg.gamma; },
                                        [this](int v) { cfg.gamma = v; }, "%"));
    pg.items.push_back(MenuItem::slider("Colour Temperature", -100, 100, 10, [this]() { return cfg.colorTemp; },
                                        [this](int v) { cfg.colorTemp = v; }, "",
                                        "Negative values are cooler (bluer), positive values warmer."));
    pg.items.push_back(MenuItem::choice("Colour Vision Assist",
                                        {"Off", "Protanopia", "Deuteranopia", "Tritanopia"},
                                        [this]() { return cfg.colorblind; }, [this](int v) { cfg.colorblind = v; },
                                        "Shifts colours so they are easier to tell apart with colour blindness."));
    pg.items.push_back(MenuItem::action("Reset Picture Settings", [this]() {
      cfg.brightness = cfg.contrast = cfg.gamma = 100;
      cfg.saturation = 100;
      cfg.colorProfile = 0;
      cfg.colorblind = 0;
      cfg.colorTemp = 0;
      menu.refresh();
    }));
  };
  return p;
}

std::shared_ptr<MenuPage> App::pageAudio() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Audio";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    pg.items.push_back(MenuItem::slider("Volume", 0, 100, 5, [this]() { return cfg.volume; },
                                        [this](int v) {
                                          cfg.volume = v;
                                          applyConfig();
                                        },
                                        "%"));
    pg.items.push_back(MenuItem::slider("Latency", 20, 160, 10, [this]() { return cfg.audioLatency; },
                                        [this](int v) {
                                          cfg.audioLatency = v;
                                          applyConfig();
                                          audio.flush();  // take effect now, not after a slow drift
                                        },
                                        " ms", "Lower values respond faster; raise it if you hear crackling."));
    pg.items.push_back(MenuItem::toggle("Mute When Unfocused", [this]() { return cfg.muteUnfocused ? 1 : 0; },
                                        [this](int v) { cfg.muteUnfocused = v; }));
    // MSU-1 music packs (app/msupack.h): Off, then the msu folder's packs
    pg.items.push_back(MenuItem::header("Custom Soundtrack (MSU-1)"));
    if (msuDir.empty()) msuDir = pathJoin(exeDir, L"msu");
    auto packs = std::make_shared<std::vector<std::wstring>>(listMsuPacks(msuDir));
    if (!cfg.msuPack.empty() && std::find(packs->begin(), packs->end(), cfg.msuPack) == packs->end())
      packs->push_back(cfg.msuPack);  // (chosen, since gone: shown so it can be turned off)
    std::vector<std::string> names = {"Off"};
    for (const auto& n : *packs) names.push_back(narrow(n));
    std::string help;
    if (!cfg.msuPack.empty() && cfg.msuPack == msuLoaded && !msuStatus.empty()) help = msuStatus + " ";
    else if (cfg.msuPack != msuLoaded)
      help = noRom() ? "Starts when you next start a game. "
                        : "Choose Restart With This Pack below to hear it now (or it starts with your next game). ";
    help += packs->empty() ? "Put a pack in its own folder in the msu folder: its MSU-1 patch (.bps or .ips) and "
                             "its tracks (.pcm), then come back here."
                           : "A pack is a folder in the msu folder: its MSU-1 patch (.bps or .ips) and tracks (.pcm).";
    pg.items.push_back(MenuItem::choice(
        "Music Pack", names,
        [this, packs]() {
          for (size_t i = 0; i < packs->size(); i++)
            if ((*packs)[i] == cfg.msuPack) return (int)i + 1;
          return 0;
        },
        [this, packs](int v) {
          cfg.msuPack = v <= 0 ? std::wstring() : (*packs)[(size_t)std::min<int>(v, (int)packs->size()) - 1];
          if (noRom()) {
            reloadForMsu();
            if (!cfg.msuPack.empty() && !(snes && snes->msu)) menu.notify(msuStatus, 5.0f);
          }
          if (!test.on && test.runSeconds <= 0) cfg.save(configPath);
          menu.refresh();  // (its help says what became of it)
        },
        help));
    if (!noRom() && snes && cfg.msuPack != msuLoaded) {
      // (in a game: the console is rebuilt with the pack, so the game starts again)
      pg.items.push_back(MenuItem::action("Restart With This Pack", [this]() {
        menu.confirm("Restart the game with this music?", [this]() { resetGame(); },
                     "The game starts again from its title. Progress since your last save is lost.");
      }, "Starts the game again from its title with the chosen music pack (or none)."));
    }
    pg.items.push_back(MenuItem::slider("Music Pack Volume", 0, 100, 5, [this]() { return cfg.msuVolume; },
                                        [this](int v) {
                                          cfg.msuVolume = v;
                                          applyConfig();
                                        },
                                        "%"));
#if defined(__SWITCH__)
    pg.items.push_back(MenuItem::action("MSU-1 Folder", [this]() {
      makeDir(msuDir);
      menu.notify("Music packs go in " + narrow(msuDir) + " (one folder per pack)", 6.0f);
    }, "Where music packs go on the SD card: one folder per pack."));
#elif !defined(__ANDROID__)
    pg.items.push_back(MenuItem::action("Open MSU-1 Folder", [this]() {
      makeDir(msuDir);
      openFolder(msuDir);
    }, "Where music packs go: one folder per pack."));
#endif
  };
  return p;
}

std::shared_ptr<MenuPage> App::pageControls() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Controls";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    pg.items.push_back(MenuItem::submenu("Player 1 Keyboard", [this]() { menu.push(pageBindings(0, false)); }));
    pg.items.push_back(MenuItem::submenu("Player 1 Joypad", [this]() { menu.push(pageBindings(0, true)); }));
    pg.items.push_back(MenuItem::submenu("Player 2 Keyboard", [this]() { menu.push(pageBindings(1, false)); }));
    pg.items.push_back(MenuItem::submenu("Player 2 Joypad", [this]() { menu.push(pageBindings(1, true)); }));
    std::vector<std::string> pads;
    for (int i = 0; i < 8; i++) {
      const PadState* ps = input.pad(i);
      std::string n = "Pad " + std::to_string(i + 1);
      if (!ps) n += " (not connected)";
      else n += ": " + narrow(ps->name) + (ps->connected ? "" : " (unplugged)");
      pads.push_back(n);
    }
    pg.items.push_back(MenuItem::choice("Player 1 Uses", pads, [this]() { return cfg.padIndex[0]; },
                                        [this](int v) { cfg.padIndex[0] = v; }));
    pg.items.push_back(MenuItem::choice("Player 2 Uses", pads, [this]() { return cfg.padIndex[1]; },
                                        [this](int v) { cfg.padIndex[1] = v; }));
    pg.items.push_back(MenuItem::choice("Select Button", {"Opens Menu", "Tap: Game Select  -  Hold: Menu"},
                                        [this]() { return cfg.selectMode; },
                                        [this](int v) { cfg.selectMode = v; },
                                        "Opens Menu: Select brings up this menu; the game's own Select is on the "
                                        "left stick button or Menu > Press Select In Game."));
    pg.items.push_back(MenuItem::choice(
        "Menu Buttons", {"Bottom Confirms", "Right Confirms"}, [this]() { return cfg.confirmRight ? 1 : 0; },
        [this](int v) {
          cfg.confirmRight = v != 0;
          applyConfig();
        },
        "Which face button chooses in the menus: the bottom one (A on an Xbox pad), or the right "
        "one (A on a Nintendo pad); the other goes back. The games' own buttons are set by their TYPE A / TYPE B "
        "and Button Bindings."));
    pg.items.push_back(MenuItem::slider("Stick Deadzone", 10, 80, 5, [this]() { return cfg.deadzone; },
                                        [this](int v) { cfg.deadzone = v; }, "%"));
#if !defined(_WIN32) && !defined(__SWITCH__)
    pg.items.push_back(MenuItem::header("Touch screen"));
    pg.items.push_back(MenuItem::choice("On-Screen Controls", {"While Touching", "Always", "Never"},
                                        [this]() { return cfg.touchMode; }, [this](int v) { cfg.touchMode = v; },
                                        "While Touching: shown once you touch the screen, hidden when a controller "
                                        "or keyboard is used."));
    pg.items.push_back(MenuItem::slider("Controls Opacity", 10, 100, 10, [this]() { return cfg.touchOpacity; },
                                        [this](int v) { cfg.touchOpacity = v; }, "%"));
#endif
    pg.items.push_back(MenuItem::action("Restore Default Controls", [this]() {
      Config d;
      d.setDefaults();
      memcpy(cfg.keys, d.keys, sizeof(cfg.keys));
      memcpy(cfg.pad, d.pad, sizeof(cfg.pad));
      menu.notify("Controls reset");
    }));
  };
  return p;
}

std::shared_ptr<MenuPage> App::pageBindings(int player, bool pad) {
  auto p = std::make_shared<MenuPage>();
  p->title = "Player " + std::to_string(player + 1) + (pad ? " Joypad" : " Keyboard");
  p->subtitle = pad ? "Positional layout: the bottom face button is SNES B" : "";
  p->rebuild = [this, player, pad](MenuPage& pg) {
    pg.items.clear();
    int last = player == 0 ? kActCount : kActSelect + 1;
    for (int a = 0; a < last; a++) {
      MenuItem it;
      it.kind = MenuItem::Bind;
      it.label = actionName(a);
      it.valueText = [this, player, pad, a]() {
        return bindingName((pad ? cfg.pad : cfg.keys)[player][a]);
      };
      it.onActivate = [this, player, pad, a]() { startBind(player, pad, a); };
      it.help = "Press Enter / A to change.";
      pg.items.push_back(it);
    }
  };
  return p;
}


void App::frame(double dt, int gameFrames) {
  dt = std::min(dt, 0.1);
  menu.tickToasts((float)dt);
  updateInput(dt);
  if (bindAction >= 0) updateBind(dt);
  if (noRom()) {  // (no ROM yet: it is looked for again every second; Start quits)
    pollRom(dt);
    MenuInput mi = menuInput();
    if (menu.open()) {
      if (menu.top() && !menu.capturing) menu.update(mi, (float)dt);
      menuAnim = std::min(1.0f, menuAnim + (float)dt * 8.0f);
    }
    if ((padState[0] & kPadStart) && !(prevPadStart & kPadStart)) running = false;
    prevPadStart = padState[0];
    return;
  }

  bool menuOpen = menu.open();
  if (menuOpen) {
    MenuInput mi = menuInput();
    // Menu button closes the menu from the top level
    if (menu.top() && !menu.capturing) menu.update(mi, (float)dt);
    menuAnim = std::min(1.0f, menuAnim + (float)dt * 8.0f);
  } else {
    menuInput();  // keep edge state fresh
  }
  if (menuOpen && !menu.open()) {
    cfg.save(configPath);
    audio.flush();
  }

  bool emulate = !menu.open();
  if (rewinding && (!rewindHeld || !emulate)) endRewind();
  if (!cfg.rewind && rewind.snapshots()) rewind.clear();
  if (emulate && rewindHeld && stepRewind(gameFrames)) {
    // (showing the past)
  } else if (emulate) {
    const int runs = gameFrames * (fastForward ? 4 : 1);
    for (int i = 0; i < runs; i++) {
      // (Of the frames a pass runs, only the last is drawn: a device that falls behind runs two a pass, and
      // drawing is most of a frame's work. The game keeps its speed and its sound. Fast forward too.)
      snes->ppu.skipRender = i < runs - 1;
#ifdef __SWITCH__
      const auto emuT0 = std::chrono::steady_clock::now();
#endif
      snes->setCpuSpeed(cfg.emuCpuPercent);
      game->beforeFrame();
      snes->runFrame();
      game->afterFrame();
#ifdef __SWITCH__
      g_nxPerf.emuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - emuT0).count();
      g_nxPerf.emuFrames++;
#endif
      audioBuf.clear();
      snes->takeAudio(audioBuf);
      bool mute = (cfg.muteUnfocused && !focused) || fastForward;
      if (!mute) audio.push(audioBuf.data(), audioBuf.size() / 2, Snes::kSampleRate);
      fpsFrames++;
      if (cfg.rewind && (++rewindCount & 1) == 0) {
#ifdef __SWITCH__
        const auto rwT0 = std::chrono::steady_clock::now();
#endif
        rewind.push(snes->saveState());
#ifdef __SWITCH__
        g_nxPerf.rewindMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rwT0).count();
        g_nxPerf.rewindPushes++;
#endif
      }
    }
    snes->ppu.skipRender = false;
  }
  sramTimer += dt;
  if (sramTimer > 5.0 && snes->sramDirty) {
    sramTimer = 0;
    saveSram();
  }
  fpsTimer += dt;
  if (fpsTimer >= 0.5) {
    fps = fpsFrames / fpsTimer;
    fpsFrames = 0;
    fpsTimer = 0;
  }
}

void App::render() {
  Canvas* uiCtx = video.canvas();
  const RectF all = rectF(0, 0, (float)video.width(), (float)video.height());
  if (!snes) {
    video.clear();
    video.beginUi();
    menu.setAccent(colorF(0.95f, 0.76f, 0.25f));
    menu.setGameArt(0);
    if (uiCtx) {
      const float W = (float)video.width(), H = (float)video.height(), u = H / 720.0f;
      uiCtx->fillGradient(all, colorF(0.06f, 0.09f, 0.20f), colorF(0.17f, 0.29f, 0.55f));
      uiCtx->titleText(L"The Magical Quest", rectF(0, H * 0.20f, W, H * 0.20f + 100 * u), 84 * u,
                       colorF(1.0f, 0.82f, 0.28f), kAlignCenter);
      uiCtx->text(L"Widescreen for the Switch", rectF(0, H * 0.20f + 100 * u, W, H * 0.20f + 140 * u), 26 * u, 600,
                  colorF(0.85f, 0.9f, 1.0f), kAlignCenter);
      const wchar_t* lines[] = {L"Put your The Magical Quest ROM (.sfc or .smc)",
                                L"in a \"roms\" folder next to this .nro on the SD card.",
                                L"It is looked for again every second.",
                                L"Press + to quit."};
      for (int i = 0; i < 4; i++) {
        const float y = H * 0.52f + i * 38 * u;
        uiCtx->text(lines[i], rectF(0, y, W, y + 34 * u), 24 * u, i == 3 ? 700 : 500,
                    i == 3 ? colorF(1.0f, 0.82f, 0.28f) : colorF(0.92f, 0.94f, 1.0f), kAlignCenter);
      }
    }
    if (menu.open()) menu.draw(video.width(), video.height(), menuAnim);
    char fpsText[32] = "";
    if (cfg.showFps) snprintf(fpsText, sizeof fpsText, "%.1f FPS", fps);
    menu.drawOsd(video.width(), video.height(), fpsText);
    if (blackFrame && uiCtx) uiCtx->fillRect(all, colorF(0, 0, 0, 1));
    video.endUi();
    return;
  }
  IRect dest;
  int ext, srcW;
  computeLayout(&dest, &ext, &srcW);
  // High frame rate: on the refreshes after a game frame, the in-between
  // picture nearest the refresh's moment, the last frame k / K of the way to
  // this one (shown a frame late: what lies between two frames is known once
  // the second is drawn); the frame itself at its own moment.
  const uint32_t* pic = snes->ppu.frame;
  const int subs = snes->ppu.subFrames;
  if (subs > 1 && !menu.open() && hfrT >= 0 && snes->ppu.subTimed) {
    // (drawn at the refreshes' own moments: the one nearest this, or the frame)
    double best = std::fabs(1.0 - hfrT);
    for (int k = 1; k < subs; k++)
      if (std::fabs(snes->ppu.subAt[k - 1] - hfrT) < best)
        best = std::fabs(snes->ppu.subAt[k - 1] - hfrT), pic = snes->ppu.subFrame[k - 1];
    if (hfrPrevOk && hfrPrev.size() == (size_t)(kLineW * kFrameH) && hfrT < best) pic = hfrPrev.data();
  } else if (subs > 1 && !menu.open() && hfrT >= 0) {
    int k = std::clamp((int)std::lround(hfrT * subs), 1, subs);
    if (k < subs) pic = snes->ppu.subFrame[k - 1];
  }
  // (the last frame's picture, shown for a moment near it, at its own left
  // margin: shown at this frame's, a room sliding to the middle jumped back
  // and forth)
  int from = kMaxExt - ext;
  if (pic == hfrPrev.data()) from = std::clamp(kMaxExt - hfrPrevExt, 0, kLineW - srcW);
  video.upload(pic + from, srcW, snes->ppu.frameHeight(), kLineW);
  VideoParams vp;
  vp.srcW = srcW;
  vp.srcH = snes->ppu.frameHeight();
  vp.dest = dest;
  vp.filter = cfg.filter;
  vp.scanlines = cfg.scanlines / 100.0f;
  vp.curve = cfg.crtCurve / 100.0f;
  vp.colorProfile = cfg.colorProfile;
  vp.brightness = cfg.brightness / 100.0f;
  vp.contrast = cfg.contrast / 100.0f;
  vp.saturation = cfg.saturation / 100.0f;
  vp.gamma = cfg.gamma / 100.0f;
  vp.colorblind = cfg.colorblind;
  vp.temperature = cfg.colorTemp / 100.0f;
  vp.pillarbox = cfg.pillarbox;
  vp.frame = snes->frameCount();
  video.draw(vp);
  video.beginUi();
  menu.setGameArt(0);
  menu.setAccent(colorF(0.95f, 0.76f, 0.25f));  // (gold)
  if (cfg.pillarbox == 2) {
    // dark frame: subtle border around the picture
    RectF r = rectF((float)dest.left - 2, (float)dest.top - 2, (float)dest.right + 2, (float)dest.bottom + 2);
    if (Canvas* c = video.canvas()) c->strokeRect(r, 3.0f, colorF(0.25f, 0.25f, 0.3f, 1.0f));
  }
  touch.draw(video.canvas(), cfg.touchOpacity / 100.0f);
  if (menu.open()) menu.draw(video.width(), video.height(), menuAnim);
  char fpsText[32] = "";
  if (cfg.showFps) snprintf(fpsText, sizeof fpsText, "%.1f FPS", fps);
  menu.drawOsd(video.width(), video.height(), fpsText,
               rewinding ? "<< Rewind" : fastForward ? ">> Fast Forward" : "");
  if (blackFrame && uiCtx) uiCtx->fillRect(all, colorF(0, 0, 0, 1));
  video.endUi();
}

