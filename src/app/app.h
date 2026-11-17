// MagicalQuest_nx: the application (game, menus), the same on every platform. The platform's part
// (window, main loop, video, sound, controllers) is platform/switch/app_switch.cpp.
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "app/canvas.h"
#include "app/config.h"
#include "app/menu.h"
#include "core/rewind.h"
#include "core/snes.h"
#include "game/mq.h"
#include "platform/android/audio_sdl.h"
#include "platform/android/input_sdl.h"
#include "platform/android/touch.h"
#include "platform/android/video_gles.h"

extern const wchar_t* kAppTitle;
extern const char* kAppVersion;

struct App {
  bool running = true;
  bool focused = true;
  Config cfg;
  std::wstring exeDir, configPath, saveDir, shotDir, romPath;
  std::wstring msuDir;     // MSU-1 music packs (see app/msupack.h); exeDir/msu unless the platform says
  std::wstring msuLoaded;  // the pack the console runs with (empty: none)
  std::string msuStatus;   // what became of the chosen pack, for the Audio page
  std::wstring msuBase;    // its tracks' path without "-N.pcm" (empty: none in use)
  std::unique_ptr<Snes> snes;
  std::unique_ptr<MqGame> game;
  Video video;
  Audio audio;
  Input input;
  Menu menu;
  // No ROM found yet: a screen says where to put it (see render); it is looked for again every second.
  bool noRom() const { return !snes; }
  uint16_t prevPadStart = 0;  // (the no-ROM screen: Start quits)
  bool menuHintShown = false;
  std::vector<uint8_t> romData;
  bool romExact = true;
  double romPoll = 0;
  bool blackFrame = false;  // this refresh is black (black frame insertion)
  int hfrStep = 0;          // high frame rate without VSync: the pass within the frame
  std::vector<uint32_t> hfrPrev;  // high frame rate, timed: the last frame's picture (shown for a moment near it)
  bool hfrPrevOk = false;
  int hfrPrevExt = 0;
  bool vblankWaited = false;  // the last pass waited for a vertical blank (VSync)
  static constexpr int kHfrMaxDrawn = 2;  // in-between pictures drawn with a frame, at most
  double hfrT = -1;         // high frame rate: this refresh's moment, from the last frame (0) to this one (1); -1 off
  int hfrSubFrames() const;  // in-between pictures per frame for this display (1: none)
  TouchControls touch;  // (on-screen controls: see platform/android/touch.h)
  bool touchActive = true;  // touch is the input in use (else a controller or keys)
  struct SDL_Window* window = nullptr;
  bool pickingRom = false;
  void pickRom();   // the system file picker, the file copied into the app (see SmasActivity)
  void pollPick();
  void pickArt(int card);  // a cover (card 0-4) or the background (-1) chosen with the system picker (Android)
  void pollArtPick();
  bool pickingArt = false;
  void touchFrame();  // taps and drags on the launcher and menus, the on-screen pad's buttons
  bool mouseShown = false;  // (Windows: the mouse moved on the launcher, its pointer shown till a key)
  int lastWantExt = 0;  // widescreen margin chosen by the last usable layout
  bool pendingDisplayMode = false;  // re-apply the display mode from the main loop
  float menuAnim = 0;
  // input state
  uint16_t padState[2] = {0, 0};
  bool prevAction[2][kActCount] = {};
  bool prevSelect[2] = {false, false};
  bool prevMenuBtn[2] = {false, false};
  float selectHeld[2] = {0, 0};
  bool selectConsumed[2] = {false, false};
  float selectPulse[2] = {0, 0};  // seconds of game Select still to send
  bool menuWasOpen = false;
  bool prevMenuNav[8] = {};
  // timing
  struct Pacing {
    int mode = -1;        // kPace* in effect
    double hz = 60;       // display refresh
    int perFrame = 0;     // refreshes per game frame when locked (0 = not a whole number)
    double vbResidual = 0;
    int vblanks = 0;      // refreshes since the last game frame (locked mode)
    double accum = 0;     // game time owed (unlocked vsync)
    double deadline = 0;  // next frame time (timer-paced modes)
    std::string cadence;  // refreshes per game frame, for the test log
    // (display.pacing_log: refreshes per pass seen, 0 1 2 3+; the passes' spacing)
    int vbSeen[4] = {}, logPasses = 0, logFrames = 0;
    double dtLo = 1e9, dtHi = 0, logAt = 0;
    int offBeatCount = 0;           // passes in a row whose wait wasn't a whole number of refreshes
    bool offBeatLogged = false;
    double sumReal = 0, sumCounted = 0;  // (this window's time gone by, and its refreshes counted)
    int nWin = 0;
    bool clockMode = false;         // game time from the clock: the refreshes counted drifted
    unsigned logPresents = 0, logRefreshes = 0;
  } pacing;
  double fpsTimer = 0;
  int fpsFrames = 0;
  double fps = 0;
  static constexpr int kStateSlots = 20;  // save-state slots (state1.sst .. state20.sst)
  int stateSlot = 1;
  double sramTimer = 0;
  bool sramFailed = false;  // the last save of game progress failed (reported once)
  std::vector<int16_t> audioBuf;
  bool fastForward = false;
  // Rewind: a snapshot every other frame (see stepRewind).
  Rewind rewind;
  bool rewindHeld = false;  // the Rewind button is held (and rewinding is on)
  bool rewinding = false;   // showing the past: the machine is at a snapshot (plus a frame to draw it)
  unsigned rewindCount = 0;
  bool stepRewind(int steps);
  void endRewind();
  void restoreSnapshot(const std::vector<uint8_t>& st);
  // automated test mode (--test-shot)
  struct Test {
    bool on = false;
    std::wstring shot, state, menuPage, config;
    int frames = 120;
    int w = 1920, h = 1080;
    std::string input;
    double runSeconds = 0;  // --test-run: drive the real main loop for N seconds
    double loseDeviceAt = 0;  // --test-device-lost: pretend the GPU was reset at N seconds
    int displayMode = -1;   // --test-display 0/1/2 (default windowed)
    int sync = -1;          // --test-sync 0/1/2
  } test;
  void runTest();

  // ---------------------------------------------------------------------
  bool init();
  void shutdown();
  void run();
  void frame(double dt, int gameFrames);
  void render();
  bool loadRom();
  bool findRom(std::wstring* out);
  void romReady();                    // a ROM was loaded: the settings applied to the console
  void resetGame();                   // the console as at power-on
  void pollRom(double dt);            // looks for the ROM again while it is missing
  void applyMsu(std::vector<uint8_t>& rom);  // (loadRom: the chosen pack's patch and chip)
  void reloadForMsu();
  std::string blackFramesNote() const;
  void applyDisplayMode();
  void applyConfig();
  // The slang shader preset in cfg.slangPreset loaded (or dropped) if it
  // isn't what the video layer has; force after the device was recreated.
  void computeLayout(IRect* dest, int* ext, int* srcW);
  void updateInput(double dt);
  void readPlayer(int p, uint16_t* snesPad, double dt);
  MenuInput menuInput();
  void openMenu();
  void saveSram();
  void loadSram();
  void saveState(int slot);
  void loadState(int slot);
  void screenshot();
  std::wstring statePath(int slot) const;
  // menu pages
  std::shared_ptr<MenuPage> pageMain();
  std::shared_ptr<MenuPage> pageDisplay();
  std::shared_ptr<MenuPage> pageGraphics();
  std::shared_ptr<MenuPage> pageColor();
  std::shared_ptr<MenuPage> pageAudio();
  std::shared_ptr<MenuPage> pageControls();
  std::shared_ptr<MenuPage> pageBindings(int player, bool pad);
  std::shared_ptr<MenuPage> pageCredits();
  std::shared_ptr<MenuPage> pageOptions();
  std::string bindingsText(int action) const;  // "F5 / LB": what does it (player 1)
  // rebinding
  int bindPlayer = 0, bindAction = -1;
  bool bindPad = false;
  double bindTimer = 0;
  void startBind(int player, bool pad, int action);
  void updateBind(double dt);
};


extern App* g_app;
extern FILE* g_log;
void logf(const char* fmt, ...);
std::string narrow(const std::wstring& w);
bool readAll(const std::wstring& path, std::vector<uint8_t>& out);
bool writeAll(const std::wstring& path, const void* d, size_t n);
// Whether the data is The Magical Quest's ROM (headered or not); `exact`:
// the dump the port was built and tested with.
bool isMqRom(const std::vector<uint8_t>& d, bool* exact);
// The pacing modes (see App::run).
enum { kPaceVsync = 0, kPaceTimer = 1 };
