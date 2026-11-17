// SNES system: bus, timing, DMA/HDMA, interrupts, joypads, cartridge (LoROM).
#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include "cpu65816.h"
#include "ppu.h"
#include "apu.h"
#include "msu1.h"
#include <memory>

// Joypad bit layout (matches the 16-bit auto-read format)
enum : uint16_t {
  kPadB = 0x8000, kPadY = 0x4000, kPadSelect = 0x2000, kPadStart = 0x1000,
  kPadUp = 0x0800, kPadDown = 0x0400, kPadLeft = 0x0200, kPadRight = 0x0100,
  kPadA = 0x0080, kPadX = 0x0040, kPadL = 0x0020, kPadR = 0x0010,
};

class Snes {
public:
  Snes();
  bool loadRom(const std::vector<uint8_t>& data);
  void reset(bool hard);
  // Emulate until the start of the next vertical blank.
  void runFrame();

  // CPU bus interface
  uint8_t cpuRead(uint32_t adr);
  void cpuWrite(uint32_t adr, uint8_t val);
  void cpuIdle() { cpuTick(6); }
  // The emulated CPU's speed as a percentage of the real one (100 = stock, up to 400): each of its cycles takes
  // 100/pct of the time, while the PPU, DMA and sound chip run as ever. Scenes that overran the frame on the
  // real console (slowdown) get done in time. Set before each frame; it costs the host more CPU time, since the
  // game's code (and its waiting loops) run that many times more often.
  void setCpuSpeed(int pct) {
    pct = pct < 100 ? 100 : pct > 400 ? 400 : pct;
    if (pct != cpuPct_) {
      cpuPct_ = pct;
      cpuAcc_ = 0;
    }
  }
  int cpuSpeed() const { return cpuPct_; }
  void setCpuFast(bool fast) { setCpuSpeed(fast ? 200 : 100); }  // (kept for the old on/off use)
  bool cpuFast() const { return cpuPct_ > 100; }

  // Side-effect-free memory access for tools and game hooks
  uint8_t peek(uint32_t adr) const;
  void poke(uint32_t adr, uint8_t v);
  uint16_t peek16(uint32_t adr) const { return peek(adr) | (peek(adr + 1) << 8); }

  // Audio: returns samples generated since last call (stereo, 32040 Hz)
  void takeAudio(std::vector<int16_t>& out);
  static constexpr double kSampleRate = 32040.0;
  static constexpr double kFrameRate = 21477272.727272 / 357366.0;

  Cpu65816 cpu;
  Ppu ppu;
  Apu apu;

  uint8_t wram[0x20000];
  std::vector<uint8_t> rom;
  uint8_t sram[0x2000];
  bool sramDirty = false;
  // Extra ROM space mapped at banks $60-$6F:$8000-$FFFF for patch code.
  std::vector<uint8_t> patchRom;
  // MSU-1 (an MSU-1 patch's music pack, see core/msu1.h): absent unless one is chosen.
  std::unique_ptr<Msu1> msu;

  uint16_t input[2] = {0, 0};

  // Execution hooks: called before executing an instruction at a marked PC.
  // Return true if the hook changed control flow (PC) and the instruction
  // at the old PC should not execute.
  using ExecHook = bool (*)(void* ctx, uint32_t pc);
  // Extra data the game layer keeps in save states (widescreen state). size 0
  // on load: the state has none (saved by an older version).
  struct StateExt {
    virtual std::vector<uint8_t> saveExt() const = 0;
    virtual void loadExt(const uint8_t* data, size_t size) = 0;

  protected:
    ~StateExt() = default;
  };
  StateExt* stateExt = nullptr;

  void setExecHook(ExecHook fn, void* ctx) {
    execHook_ = fn;
    execCtx_ = ctx;
  }
  void markExec(uint32_t pc, bool on);
  void clearExecMarks();
  // Write watch (debugging): called for CPU writes in [lo,hi]
  using WatchFn = void (*)(void* ctx, uint32_t adr, uint8_t v, uint32_t pc);
  void setWriteWatch(uint32_t lo, uint32_t hi, WatchFn fn, void* ctx) {
    watchLo_ = lo;
    watchHi_ = hi;
    watchFn_ = fn;
    watchCtx_ = ctx;
  }
  // Game-layer write hooks (WRAM addresses normalized to $7Exxxx), called
  // after the write has been performed.
  void addWriteHook(uint32_t lo, uint32_t hi, WatchFn fn, void* ctx) {
    if (nWriteHooks_ < 8) writeHooks_[nWriteHooks_++] = {lo, hi, fn, ctx};
  }
  void clearWriteHooks() { nWriteHooks_ = 0; }
  void setReadWatch(uint32_t lo, uint32_t hi, WatchFn fn, void* ctx) {
    rwatchLo_ = lo;
    rwatchHi_ = hi;
    rwatchFn_ = fn;
    rwatchCtx_ = ctx;
  }
  // Called for APU port writes (debug / music tracking)
  using PortFn = void (*)(void* ctx, int port, uint8_t v);
  void setPortWatch(PortFn fn, void* ctx) {
    portFn_ = fn;
    portCtx_ = ctx;
  }

  uint32_t frameCount() const { return frames_; }
  uint16_t vPos() const { return vPos_; }
  uint16_t hPos() const { return hPos_; }
  uint64_t masterCycles() const { return cycles_; }

  // Save states
  std::vector<uint8_t> saveState() const;
  bool loadState(const std::vector<uint8_t>& data);  // leaves the machine unchanged on failure
  bool loadStateRaw(const std::vector<uint8_t>& data);

  void stepInstruction();

private:
  friend class Cpu65816;
  struct DmaChannel {
    uint8_t dmap = 0xff, bbad = 0xff;
    uint16_t a1t = 0xffff;
    uint8_t a1b = 0xff;
    uint16_t das = 0xffff;
    uint8_t dasb = 0xff;
    uint16_t a2a = 0xffff;
    uint8_t ntrl = 0xff;
    uint8_t unused = 0xff;
    bool doTransfer = false;
    bool terminated = true;
  };
  // Channels with a general DMA in progress. HDMA on a channel stops it, as on
  // hardware (it reuses the byte counter). Only set inside runDma, so not saved.
  uint8_t dmaRunning_ = 0;

  // timing
  void sanitizeState();
  uint64_t cycles_ = 0;
  uint16_t hPos_ = 0, vPos_ = 0;
  uint16_t lineLen_ = 1364;
  int cpuPct_ = 100;  // (see setCpuSpeed)
  int cpuAcc_ = 0;    // the part of a master cycle owed, in 1/pct units, so that 6 cycles at 125% are 4.8 on average
  // The master cycles of one CPU cycle that takes `mc` at stock speed.
  void cpuTick(int mc) {
    if (cpuPct_ == 100) {
      tick(mc);
      return;
    }
    cpuAcc_ += mc * 100;
    const int s = cpuAcc_ / cpuPct_;
    if (s > 0) {
      cpuAcc_ -= s * cpuPct_;
      tick(s);
    }
  }
  uint32_t frames_ = 0;
  bool frameDone_ = false;
  bool pendingVblank_ = false;
  bool inVblank_ = false;
  int evPos_[8];
  int evCount_ = 0, evIdx_ = 0;
  int autoJoyTimer_ = 0;

  // registers
  uint8_t nmitimen_ = 0;
  bool nmiFlag_ = false;
  bool irqFlag_ = false;
  uint16_t htime_ = 0x1ff, vtime_ = 0x1ff;
  uint8_t wrio_ = 0xff;
  uint8_t mulA_ = 0xff;
  uint16_t divA_ = 0xffff;
  uint16_t rdDiv_ = 0, rdMpy_ = 0;
  bool fastRom_ = false;
  uint8_t hdmaen_ = 0;
  uint32_t wmAdr_ = 0;
  uint16_t joyAuto_[4] = {};
  uint16_t joyLatch_[2] = {};
  bool joyStrobe_ = false;
  uint8_t openBus_ = 0;
  DmaChannel dma_[8];

  // hooks
  ExecHook execHook_ = nullptr;
  void* execCtx_ = nullptr;
  std::vector<uint8_t> execMap_;
  bool anyExecMarks_ = false;
  uint32_t watchLo_ = 1, watchHi_ = 0;
  WatchFn watchFn_ = nullptr;
  void* watchCtx_ = nullptr;
  struct WriteHook {
    uint32_t lo, hi;
    WatchFn fn;
    void* ctx;
  } writeHooks_[8];
  int nWriteHooks_ = 0;
  uint32_t rwatchLo_ = 1, rwatchHi_ = 0;
  WatchFn rwatchFn_ = nullptr;
  void* rwatchCtx_ = nullptr;
  PortFn portFn_ = nullptr;
  void* portCtx_ = nullptr;

  size_t romMask_ = 0;

  // The master cycles of one bus access or internal cycle. Nearly always nothing happens inside them (no
  // scanline end, no scheduled event): that case is done here, inlined into every access; tickSlow runs the events.
  void tick(int mc) {
    if (mc < evPos_[evIdx_] - hPos_) {
      hPos_ += mc;
      cycles_ += mc;
      if (autoJoyTimer_ > 0) autoJoyTimer_ -= mc;
      return;
    }
    tickSlow(mc);
  }
  void tickSlow(int mc);
  void buildLineEvents();
  void processEvent(int pos, int& extra);
  void newLine();
  void vblankStart();
  void checkIrq();
  int irqPos() const;
  int accessTime(uint32_t adr) const;
  const uint8_t* rdPage_[2048] = {};  // (see buildPages)
  uint8_t accTab_[2048] = {};         // (see buildAccess)
  void buildPages();
  void buildAccess();
  uint8_t busRead(uint32_t adr);
  void busWrite(uint32_t adr, uint8_t v);
  uint8_t readIo(uint16_t adr);
  void writeIo(uint16_t adr, uint8_t v);
  uint8_t readBBus(uint8_t adr);
  void writeBBus(uint8_t adr, uint8_t v);
  uint8_t dmaReadA(uint32_t adr);
  void dmaWriteA(uint32_t adr, uint8_t v);
  void runDma(uint8_t mask);
  void hdmaInit(uint8_t mask = 0xff);
  int hdmaRun();
  void syncApu();
  bool execCheck(uint32_t pc);
};
