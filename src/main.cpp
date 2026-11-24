#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

// ============================================================================
//  CardLyre Pro v3.0 — 6-Mode Polyphonic Workstation (Cardputer-Adv & V1)
//  Raw 4x14 Physical Matrix Input:
//    Play Grid (3x8 = 24 Pads):
//      Row 1: Q   W   E   R   T   Y   U   I   (Pads 1..8)
//      Row 2: Aa  A   S   D   F   G   H   J   (Pads 9..16)
//      Row 3: Opt Alt Z   X   C   V   B   N   (Pads 17..24)
//    Right-Side FX & Controls (completely separate from Play Grid!):
//      O=Reverb(H), P=Echo(H), K=Mod(H), L=Filter(H), M=Sustain Toggle (1-tap!)
//      [/]=Volume 0..200% (30 steps + auto ES8311 boost), -/==Transpose(H)
//      1..6=Instruments(H), 7=Wave/Drive(H), 8=Vis(H), 9/0=Scale(H), '=Theme(H)
// ============================================================================

constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 135;

constexpr uint32_t SAMPLE_RATE      = 32000;
constexpr int      AUDIO_BUF_FRAMES = 256;   // 8 ms smooth M5Unified DMA
constexpr int      MAX_VOICES       = 6;
constexpr int      KS_MAX_LEN       = 400;
constexpr uint32_t HOLD_LOCK_MS     = 1300;  // 1.3s hold prevents accidental switches
constexpr int      PIN_SD_SCK       = 40;
constexpr int      PIN_SD_MISO      = 39;
constexpr int      PIN_SD_MOSI      = 14;
constexpr int      PIN_SD_CS        = 12;
static const char* REC_DIR          = "/MMCTool_Rec";
static const char* SONG_DIR         = "/MMCTool_Songs";

#ifndef RGB565
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#endif

// ============================================================================
//  HAL: ES8311 Hardware Amp Boost (Cardputer-Adv only; auto-skipped on V1)
// ============================================================================
class HalES8311 {
public:
  static constexpr uint8_t I2C_ADDR = 0x18;

  static bool isPresent() {
    static int8_t cached = -1;
    if (cached < 0) cached = M5.In_I2C.scanID(I2C_ADDR) ? 1 : 0;
    return cached == 1;
  }

  static void setBoost(uint8_t level) {
    if (!isPresent()) return;
    uint8_t reg = (level == 2) ? 0xD5 : ((level == 1) ? 0xCB : 0xBF);
    M5.In_I2C.writeRegister8(I2C_ADDR, 0x32, reg, 400000);
  }
};

// ============================================================================
//  6 CRT Color Themes
// ============================================================================
enum ThemeStyle : uint8_t {
  STYLE_RETRO = 0, // Retro 84: pixel brackets, double borders, phosphor green
  STYLE_AMBER = 1, // Amber CRT: scanlines, rounded warm CRT glow
  STYLE_CYBER = 2, // Cyber 2077: chamfered corners, Night City yellow/cyan/red
  STYLE_GHOST = 3  // Transparent Whites: misty monochrome dithered translucency
};

struct Theme {
  const char* name;
  ThemeStyle style;
  uint16_t bg;
  uint16_t scan;
  uint16_t panel;
  uint16_t border;
  uint16_t primary;
  uint16_t secondary;
  uint16_t accent;
  uint16_t text;
  uint16_t dim;
};

static const Theme THEMES[] = {
  {"RETRO 84",    STYLE_RETRO, RGB565(4, 14, 6),   RGB565(8, 26, 12),  RGB565(10, 32, 16), RGB565(45, 155, 75),  RGB565(90, 255, 130), RGB565(235, 245, 175), RGB565(255, 180, 60),  RGB565(215, 255, 220), RGB565(75, 155, 95)},
  {"AMBER CRT",   STYLE_AMBER, RGB565(8, 4, 0),    RGB565(20, 10, 0),  RGB565(24, 14, 2),  RGB565(135, 75, 0),  RGB565(255, 195, 20), RGB565(80, 255, 140),  RGB565(255, 90, 30),   RGB565(255, 240, 190), RGB565(140, 105, 50)},
  {"CYBER 2077",  STYLE_CYBER, RGB565(6, 4, 14),   RGB565(14, 12, 32), RGB565(18, 14, 38), RGB565(0, 175, 215), RGB565(255, 230, 15), RGB565(0, 245, 255),   RGB565(255, 40, 95),   RGB565(245, 250, 255), RGB565(120, 135, 170)},
  {"GHOST WHITE", STYLE_GHOST, RGB565(12, 12, 14), RGB565(24, 24, 28), RGB565(34, 34, 40), RGB565(175, 175, 185),RGB565(250, 250, 255),RGB565(180, 190, 205), RGB565(120, 125, 135), RGB565(255, 255, 255), RGB565(125, 128, 138)}
};
constexpr uint8_t THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);

// ============================================================================
//  Instruments, Scales, Drum Types & 24-Pad Maps
// ============================================================================
enum Instrument : uint8_t {
  INST_BASS = 0,
  INST_EGTR,
  INST_LYRE,
  INST_SYNTH,
  INST_DRUMS,
  INST_DRUMPAD,
  INST_REESE,    // Hold Tab: Sewerslvt / DnB Reese Bass
  INST_CHAINSAW, // Hold Fn:  4x Detuned EDM / Hyperpop Supersaw Chainsaw
  INST_ACID303,  // Hold Ctrl: Resonant TB-303 Acid Squelch Synth
  INST_PIANO,    // Hold Esc (`): Real Acoustic Grand Piano
  INST_COUNT
};
static const char* INST_NAMES[INST_COUNT] = {
  "BASS", "E-GTR", "LYRE", "SYNTH", "DRUMS", "DPAD", "REESE", "CHAIN", "ACID", "PIANO"
};

enum SynthWave : uint8_t {
  WAVE_SAW = 0,
  WAVE_SQR,
  WAVE_TRI,
  WAVE_PAD,
  WAVE_BELL, // Phonk 808 Cowbell (chromatic!)
  WAVE_COUNT
};
static const char* WAVE_NAMES[WAVE_COUNT] = {"SAW", "SQR", "TRI", "PAD", "BELL"};

enum DrumKit : uint8_t {
  KIT_AMEN = 0, // Breakcore / Amen Break
  KIT_TEKK = 1, // Hardtekk / Gabber / Sirens
  KIT_PHNK = 2, // Phonk / 808 Trap
  KIT_COUNT = 3
};
static const char* KIT_NAMES[KIT_COUNT] = {"AMEN", "TEKK", "PHNK"};

enum ScaleLayout : uint8_t {
  LAYOUT_MAJ   = 0, // Genshin Lyre Major
  LAYOUT_MIN   = 1, // Emotional Natural Minor
  LAYOUT_PIANO = 2, // Real Piano: Mid=White (C4..C5), Top=Sharps (#), Bot=Bass (C3..C4)
  LAYOUT_BIG   = 3, // Wide Double-Keys 3x4 (12 big pads)
  LAYOUT_CHORD = 4, // 1-Key Live Strummed Chords
  LAYOUT_COUNT = 5
};
static const char* LAYOUT_NAMES[LAYOUT_COUNT] = {"MAJ", "MIN", "PNO", "BIG", "CHD"};
static const char* NOTE_NAMES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

enum VisMode : uint8_t {
  VIS_RIPPLE_GRID = 0, // 24 numbered circles (3x8) + expanding neon waves
  VIS_SCOPE_GRID  = 1, // Live CRT oscilloscope + compact 3x8 pad grid
  VIS_COUNT       = 2
};

enum DrumType : uint8_t {
  DRM_NONE = 0,
  DRM_KICK808,
  DRM_KICKPUNCH,
  DRM_KICKTEKK,
  DRM_KICKPIEPs,
  DRM_SNARE,
  DRM_SNARE_FAT,
  DRM_CLAP,
  DRM_RIM,
  DRM_CHH,
  DRM_OHH,
  DRM_CRASH,
  DRM_TOM_LO,
  DRM_TOM_MID,
  DRM_TOM_HI,
  DRM_ROLL,
  DRM_SUBDROP,
  DRM_SIREN,
  DRM_COWBELL,
  DRM_ZAP
};

// 8-note scale per row (full octave Do..Do on 8 columns!)
static const int8_t SCALE_MAJ8[8] = {0, 2, 4, 5, 7, 9, 11, 12};
static const int8_t SCALE_MIN8[8] = {0, 2, 3, 5, 7, 8, 10, 12};

// Full 24-pad Drum Kit (3 rows x 8 cols)
static const DrumType DRUM_KITS[KIT_COUNT][3][8] = {
  // Kit 0: AMEN (Breakcore)
  {
    {DRM_CRASH,    DRM_OHH,       DRM_CHH,      DRM_ROLL,      DRM_RIM,       DRM_TOM_HI,  DRM_TOM_MID, DRM_TOM_LO},
    {DRM_TOM_LO,   DRM_TOM_MID,   DRM_TOM_HI,   DRM_SNARE,     DRM_SNARE_FAT, DRM_CLAP,    DRM_RIM,     DRM_CHH},
    {DRM_KICK808,  DRM_KICKPUNCH, DRM_SNARE,    DRM_CLAP,      DRM_CHH,       DRM_OHH,     DRM_SUBDROP, DRM_CRASH}
  },
  // Kit 1: TEKK (Hardtekk / Gabber / Rave Sirens)
  {
    {DRM_SIREN,    DRM_CRASH,     DRM_ZAP,      DRM_ROLL,      DRM_OHH,       DRM_CHH,     DRM_COWBELL, DRM_SUBDROP},
    {DRM_KICKTEKK, DRM_KICKPIEPs, DRM_CLAP,     DRM_SNARE_FAT, DRM_RIM,       DRM_TOM_HI,  DRM_TOM_LO,  DRM_ZAP},
    {DRM_KICKTEKK, DRM_KICKPIEPs, DRM_KICKPUNCH,DRM_CLAP,      DRM_CHH,       DRM_OHH,     DRM_SIREN,   DRM_SUBDROP}
  },
  // Kit 2: PHNK (Phonk / 808 Trap)
  {
    {DRM_COWBELL,  DRM_CRASH,     DRM_OHH,      DRM_CHH,       DRM_ZAP,       DRM_RIM,     DRM_ROLL,    DRM_SUBDROP},
    {DRM_TOM_LO,   DRM_TOM_MID,   DRM_SNARE,    DRM_SNARE_FAT, DRM_CLAP,      DRM_COWBELL, DRM_RIM,     DRM_CHH},
    {DRM_KICK808,  DRM_SUBDROP,   DRM_KICKPUNCH,DRM_SNARE,     DRM_CLAP,      DRM_CHH,     DRM_OHH,     DRM_COWBELL}
  }
};

// DRUMPAD (MPC Launchpad):
//   Row 0 (Pads 1..8):  8 Emotional Chords (Am, F, C, G, Dm, Em, E7, Am7)
//   Row 1 (Pads 9..16): 8 Matching Deep 808 Sub-Bass Notes (A1, F1, C2, G1, D2, E2, E1, A2)
//   Row 2 (Pads 17..24): 8 Fat Beat Pads (Kick808, PunchKick, Snare, FatSnare, Clap, CHH, OHH, Crash)
static const uint8_t DPAD_CHORDS[8][3] = {
  {57, 60, 64}, // 1 (Q): Am
  {53, 57, 60}, // 2 (W): F
  {60, 64, 67}, // 3 (E): C
  {55, 59, 62}, // 4 (R): G
  {50, 53, 57}, // 5 (T): Dm
  {52, 55, 59}, // 6 (Y): Em
  {52, 56, 62}, // 7 (U): E7
  {57, 60, 67}  // 8 (I): Am7
};
static const char* DPAD_CHORD_NAMES[8] = {"Am","F","C","G","Dm","Em","E7","A7"};
static const uint8_t DPAD_BASS_NOTES[8] = {45, 41, 48, 43, 50, 52, 40, 57};
static const DrumType DPAD_BEATS[8] = {
  DRM_KICK808, DRM_KICKPUNCH, DRM_SNARE, DRM_SNARE_FAT,
  DRM_CLAP,    DRM_CHH,       DRM_OHH,   DRM_CRASH
};

// Real Piano layout across 3x8:
//   Row 0 (Top Q..I):   C#4 D#4 E5  F#4 G#4 A#4 B5  C#5  (Black keys # right above white keys!)
//   Row 1 (Mid Aa..J):  C4  D4  E4  F4  G4  A4  B4  C5   (White keys!)
//   Row 2 (Bot Opt..N): C3  D3  E3  F3  G3  A3  B3  C4   (Left-hand Bass / Lower octave!)
static const uint8_t PIANO_MAP[3][8] = {
  {61, 63, 76, 66, 68, 70, 83, 73},
  {60, 62, 64, 65, 67, 69, 71, 72},
  {48, 50, 52, 53, 55, 57, 59, 60}
};

static int8_t octaveShift = 0; // -3..+3 octaves (- / = or G0 1x/2x!)

static int mapPadToMidi(int r, int c, ScaleLayout layout, Instrument inst, int8_t tr) {
  int transpose = (int)tr + (int)octaveShift * 12;
  int octShift = (inst == INST_BASS || inst == INST_EGTR || inst == INST_REESE) ? -12 : 0;
  if (layout == LAYOUT_PIANO) {
    return constrain((int)PIANO_MAP[r][c] + octShift + transpose, 33, 96);
  }
  if (layout == LAYOUT_BIG) {
    static const int8_t BIG_MIN[12] = {12, 14, 15, 17, 5, 7, 8, 10, 0, 2, 3, 5};
    return constrain(45 + BIG_MIN[r * 4 + (c / 2)] + octShift + transpose, 33, 96);
  }
  const int8_t* sc = (layout == LAYOUT_MAJ) ? SCALE_MAJ8 : SCALE_MIN8;
  int baseRoot = (layout == LAYOUT_MAJ) ? 48 : 45;
  int octave = 2 - r;
  int m = baseRoot + octave * 12 + sc[c] + octShift + transpose;
  return constrain(m, 33, 96);
}

// ============================================================================
//  Ripple Wave Animation System
// ============================================================================
struct Ripple {
  bool     active = false;
  int16_t  x = 0;
  int16_t  y = 0;
  float    radius = 0.0f;
  float    maxRadius = 46.0f;
  uint16_t color = 0xFFFF;
};
constexpr int MAX_RIPPLES = 16;
static Ripple ripples[MAX_RIPPLES];

static void spawnRipple(int x, int y, uint16_t color, float maxR = 46.0f) {
  int slot = 0;
  float biggest = -1.0f;
  for (int i = 0; i < MAX_RIPPLES; ++i) {
    if (!ripples[i].active) { slot = i; break; }
    if (ripples[i].radius > biggest) { biggest = ripples[i].radius; slot = i; }
  }
  ripples[slot].active    = true;
  ripples[slot].x         = (int16_t)x;
  ripples[slot].y         = (int16_t)y;
  ripples[slot].radius    = 5.0f;
  ripples[slot].maxRadius = maxR;
  ripples[slot].color     = color;
}

static uint16_t blend565(uint16_t fg, uint16_t bg, float alpha) {
  if (alpha <= 0.0f) return bg;
  if (alpha >= 1.0f) return fg;
  int r1 = (fg >> 11) & 0x1F, g1 = (fg >> 5) & 0x3F, b1 = fg & 0x1F;
  int r2 = (bg >> 11) & 0x1F, g2 = (bg >> 5) & 0x3F, b2 = bg & 0x1F;
  int r = r2 + (int)((r1 - r2) * alpha);
  int g = g2 + (int)((g1 - g2) * alpha);
  int b = b2 + (int)((b1 - b2) * alpha);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void updateRipples() {
  for (int i = 0; i < MAX_RIPPLES; ++i) {
    if (!ripples[i].active) continue;
    ripples[i].radius += 2.2f;
    if (ripples[i].radius >= ripples[i].maxRadius) ripples[i].active = false;
  }
}

// ============================================================================
//  6-Voice Polyphonic DSP Engine (Studio Bass, E-Gtr, Lyre, Synth, Real Drums)
// ============================================================================
struct Voice {
  volatile bool active   = false;
  volatile bool held     = false;
  uint8_t  midiNote      = 60;
  int8_t   padId         = -1; // 0..23
  uint32_t age           = 0;
  Instrument inst        = INST_LYRE;
  DrumType drum          = DRM_NONE;
  SynthWave wave         = WAVE_SAW;

  float freq             = 440.0f;
  float phase            = 0.0f;
  float phase2           = 0.0f;
  float phase3           = 0.0f;
  float phase4           = 0.0f;
  float env              = 0.0f;
  float pitchEnv         = 0.0f;
  float fltState         = 0.0f;
  float hpState          = 0.0f;
  uint32_t sampleAge     = 0;

  // Karplus-Strong ring buffer (Lyre & E-Guitar)
  float ksBuf[KS_MAX_LEN];
  int   ksLen            = 120;
  int   ksPos            = 0;
  float ksPrev           = 0.0f;
};

constexpr int ECHO_LEN = 10240; // 320 ms stereo ping-pong
static int16_t echoBufL[ECHO_LEN];
static int16_t echoBufR[ECHO_LEN];

constexpr int REV1_LEN = 1117;
constexpr int REV2_LEN = 1487;
constexpr int REV3_LEN = 1789;
static int16_t revBuf1[REV1_LEN];
static int16_t revBuf2[REV2_LEN];
static int16_t revBuf3[REV3_LEN];


// ============================================================================
//  SD Card WAV Recorder & Overdub Player (/MMCTool_Rec/REC_XXX.wav)
// ============================================================================
static SPIClass sdSPI(HSPI);
static bool     sdReady       = false;
static File     recFile;
static File     playFile;
static volatile bool isRecording = false;
static volatile bool isPlayingRec = false;
static uint32_t recDataBytes  = 0;
static uint32_t recStartMs    = 0;
static int      recListCount  = 0;
static int      recSelIdx     = 0;
static char     recNames[32][24];
static uint16_t recDurs[32];
static bool     recMenuOpen   = false;
static uint32_t entLastTapMs  = 0;
static bool     entDoubleHold = false;

// Lock-free SPSC ring buffers (Core 0 <-> Core 1) so SD I/O NEVER stutters audio!
constexpr uint32_t REC_RING_SAMPLES = 8192; // 16 KB smooth lag-free buffer
static int16_t  recRing[REC_RING_SAMPLES];
static volatile uint32_t recWriteIdx = 0;
static volatile uint32_t recReadIdx  = 0;

constexpr uint32_t PLAY_RING_SAMPLES = 4096; // 8 KB
static int16_t  playRing[PLAY_RING_SAMPLES];
static volatile uint32_t playWriteIdx = 0;
static volatile uint32_t playReadIdx  = 0;

static void writeWavHeader(File& f, uint32_t dataBytes) {
  uint8_t hdr[44] = {};
  uint32_t chunkSize = 36 + dataBytes;
  uint32_t byteRate  = SAMPLE_RATE * 2 * 2; // 32kHz * stereo * 16-bit
  memcpy(hdr + 0,  "RIFF", 4);
  memcpy(hdr + 4,  &chunkSize, 4);
  memcpy(hdr + 8,  "WAVEfmt ", 8);
  uint32_t sub1 = 16; uint16_t pcm = 1, ch = 2, align = 4, bps = 16;
  uint32_t sr = SAMPLE_RATE;
  memcpy(hdr + 16, &sub1, 4);
  memcpy(hdr + 20, &pcm, 2);
  memcpy(hdr + 22, &ch, 2);
  memcpy(hdr + 24, &sr, 4);
  memcpy(hdr + 28, &byteRate, 4);
  memcpy(hdr + 32, &align, 2);
  memcpy(hdr + 34, &bps, 2);
  memcpy(hdr + 36, "data", 4);
  memcpy(hdr + 40, &dataBytes, 4);
  f.seek(0);
  f.write(hdr, 44);
}

static void scanRecFolder() {
  recListCount = 0;
  if (!sdReady) return;
  if (!SD.exists(REC_DIR)) SD.mkdir(REC_DIR);
  File dir = SD.open(REC_DIR);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  File f = dir.openNextFile();
  while (f && recListCount < 32) {
    if (!f.isDirectory()) {
      const char* raw = f.name();
      const char* base = strrchr(raw, '/');
      base = base ? (base + 1) : raw;
      size_t ln = strlen(base);
      if (ln > 4 && strcasecmp(base + ln - 4, ".wav") == 0) {
        strncpy(recNames[recListCount], base, 23);
        recNames[recListCount][23] = '\0';
        uint32_t sz = f.size();
        recDurs[recListCount] = (sz > 44) ? (uint16_t)((sz - 44) / (SAMPLE_RATE * 4)) : 0;
        recListCount++;
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  if (recSelIdx >= recListCount) recSelIdx = max(0, recListCount - 1);
}

static void stopRecording() {
  if (!isRecording) return;
  isRecording = false;
  delay(15);
  // Drain remaining samples
  while (recReadIdx != recWriteIdx && recFile) {
    uint32_t avail = recWriteIdx - recReadIdx;
    uint32_t rPos  = recReadIdx % REC_RING_SAMPLES;
    uint32_t chunk = min(avail, REC_RING_SAMPLES - rPos);
    recFile.write((const uint8_t*)&recRing[rPos], chunk * 2);
    recDataBytes += chunk * 2;
    recReadIdx += chunk;
  }
  if (recFile) {
    writeWavHeader(recFile, recDataBytes);
    recFile.close();
  }
  scanRecFolder();
  if (recListCount > 0) recSelIdx = recListCount - 1;
}

static void startRecording() {
  if (!sdReady || isRecording) return;
  if (!SD.exists(REC_DIR)) SD.mkdir(REC_DIR);
  char path[48];
  for (int n = 1; n <= 999; ++n) {
    snprintf(path, sizeof(path), "%s/REC_%03d.wav", REC_DIR, n);
    if (!SD.exists(path)) break;
  }
  recFile = SD.open(path, FILE_WRITE);
  if (!recFile) return;
  writeWavHeader(recFile, 0);
  recDataBytes = 0;
  recReadIdx = recWriteIdx = 0;
  recStartMs = millis();
  isRecording = true;
}

static void stopPlaybackRec() {
  isPlayingRec = false;
  if (playFile) playFile.close();
  playReadIdx = playWriteIdx = 0;
}

static void startPlaybackRec() {
  if (isRecording) stopRecording();
  if (!sdReady || recListCount == 0) return;
  stopPlaybackRec();
  char path[48];
  snprintf(path, sizeof(path), "%s/%s", REC_DIR, recNames[recSelIdx]);
  playFile = SD.open(path, FILE_READ);
  if (!playFile) return;
  if (playFile.size() <= 44) { playFile.close(); return; }
  playFile.seek(44); // Skip WAV header
  playReadIdx = playWriteIdx = 0;
  isPlayingRec = true;
}

static void serviceSDStreams() {
  if (isRecording && recFile) {
    uint32_t avail = recWriteIdx - recReadIdx;
    // Write small 512-sample (1 KB) blocks so UI & video stay super smooth!
    if (avail >= 512) {
      uint32_t rPos  = recReadIdx % REC_RING_SAMPLES;
      uint32_t chunk = min(avail, REC_RING_SAMPLES - rPos);
      if (chunk > 512) chunk = 512;
      recFile.write((const uint8_t*)&recRing[rPos], chunk * 2);
      recDataBytes += chunk * 2;
      recReadIdx += chunk;
    }
  }
  if (isPlayingRec && playFile) {
    uint32_t used = playWriteIdx - playReadIdx;
    uint32_t freeSp = PLAY_RING_SAMPLES - used;
    if (freeSp >= 512) {
      uint32_t wPos  = playWriteIdx % PLAY_RING_SAMPLES;
      uint32_t chunk = min(freeSp, PLAY_RING_SAMPLES - wPos);
      if (chunk > 1024) chunk = 1024;
      int rd = playFile.read((uint8_t*)&playRing[wPos], chunk * 2);
      if (rd > 0) {
        playWriteIdx += (uint32_t)(rd / 2);
      } else {
        stopPlaybackRec();
      }
    }
  }
}


// ============================================================================
//  GUIDE / SYNTHESIA SONG TRAINER (/CardLyre_Songs/*.txt)
// ============================================================================
constexpr int MAX_GUIDE_NOTES = 256;
static bool     guideActive   = false;
static uint8_t  guideNotes[MAX_GUIDE_NOTES];
static int      guideLen      = 0;
static int      guideStep     = 0;
static int      songListCount = 0;
static int      songSelIdx    = 0;
static char     songNames[24][24];

// Built-in demo melody (emotional minor arpeggio) if SD has no songs yet
static const uint8_t DEMO_GUIDE_NOTES[] = {
  57, 64, 60, 64, 57, 64, 60, 64,
  53, 60, 57, 60, 55, 62, 59, 62,
  57, 64, 69, 67, 64, 60, 62, 64
};

static int parseNoteToken(const char* tok) {
  if (!tok || !tok[0]) return -1;
  char n = (char)toupper((unsigned char)tok[0]);
  int semi = -1;
  switch (n) {
    case 'C': semi = 0; break;
    case 'D': semi = 2; break;
    case 'E': semi = 4; break;
    case 'F': semi = 5; break;
    case 'G': semi = 7; break;
    case 'A': semi = 9; break;
    case 'B': semi = 11; break;
    default: return -1;
  }
  int pos = 1;
  if (tok[pos] == '#') { semi++; pos++; }
  else if (tok[pos] == 'b') { semi--; pos++; }
  int oct = 4;
  if (tok[pos] >= '0' && tok[pos] <= '8') oct = tok[pos] - '0';
  return constrain((oct + 1) * 12 + semi, 36, 84);
}

static void scanSongFolder() {
  songListCount = 0;
  if (!sdReady) return;
  if (!SD.exists(SONG_DIR)) SD.mkdir(SONG_DIR);
  File dir = SD.open(SONG_DIR);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  File f = dir.openNextFile();
  while (f && songListCount < 24) {
    if (!f.isDirectory()) {
      const char* raw = f.name();
      const char* base = strrchr(raw, '/');
      base = base ? (base + 1) : raw;
      size_t ln = strlen(base);
      if (ln > 4 && strcasecmp(base + ln - 4, ".txt") == 0) {
        strncpy(songNames[songListCount], base, 23);
        songNames[songListCount][23] = '\0';
        songListCount++;
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  if (songSelIdx >= songListCount) songSelIdx = 0;
}

static void loadGuideSong() {
  guideLen = 0;
  guideStep = 0;
  if (sdReady && songListCount > 0) {
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", SONG_DIR, songNames[songSelIdx]);
    File f = SD.open(path, FILE_READ);
    if (f) {
      char tok[16];
      int tLen = 0;
      while (f.available() && guideLen < MAX_GUIDE_NOTES) {
        char ch = (char)f.read();
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == ',' || ch == '\t') {
          if (tLen > 0) {
            tok[tLen] = '\0';
            int m = parseNoteToken(tok);
            if (m >= 0) guideNotes[guideLen++] = (uint8_t)m;
            tLen = 0;
          }
        } else if (tLen < 14) {
          tok[tLen++] = ch;
        }
      }
      if (tLen > 0 && guideLen < MAX_GUIDE_NOTES) {
        tok[tLen] = '\0';
        int m = parseNoteToken(tok);
        if (m >= 0) guideNotes[guideLen++] = (uint8_t)m;
      }
      f.close();
    }
  }
  if (guideLen == 0) {
    guideLen = (int)(sizeof(DEMO_GUIDE_NOTES) / sizeof(DEMO_GUIDE_NOTES[0]));
    memcpy(guideNotes, DEMO_GUIDE_NOTES, guideLen);
  }
}

// Find which (r, c) pad matches a target MIDI note in current layout (by exact note or octave-folded pitch class)
static bool findGuideTargetPad(uint8_t targetMidi, ScaleLayout layout, Instrument inst, int8_t tr, int& outR, int& outC) {
  // 1st pass: exact MIDI match
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 8; ++c) {
      if (mapPadToMidi(r, c, layout, inst, tr) == (int)targetMidi) {
        outR = r; outC = c; return true;
      }
    }
  }
  // 2nd pass: pitch-class match (same note name in nearest octave)
  int pc = targetMidi % 12;
  for (int r = 1; r >= 0; --r) {
    for (int c = 0; c < 8; ++c) {
      if ((mapPadToMidi(r, c, layout, inst, tr) % 12) == pc) {
        outR = r; outC = c; return true;
      }
    }
  }
  for (int c = 0; c < 8; ++c) {
    if ((mapPadToMidi(2, c, layout, inst, tr) % 12) == pc) {
      outR = 2; outC = c; return true;
    }
  }
  return false;
}

class SynthEngine {
public:
  Voice voices[MAX_VOICES];
  uint32_t noteCounter = 0;

  volatile Instrument instrument   = INST_LYRE;
  volatile SynthWave  synthWave    = WAVE_SAW;
  volatile DrumKit    drumKit      = KIT_AMEN;
  volatile uint8_t    driveLevel   = 1; // 0=CLN, 1=CRN, 2=HVY
  volatile uint8_t    reverbMode   = 1; // 0=OFF, 1=ROOM, 2=HALL
  volatile uint8_t    echoMode     = 1; // 0=OFF, 1=ECH1, 2=ECH2
  volatile uint8_t    modMode      = 0; // 0=OFF, 1=VIB, 2=TREM
  volatile uint8_t    filterMode   = 0; // 0=BRIT, 1=WARM, 2=LOFI
  volatile bool       sustainPedal = false;
  volatile bool       sustainLatch = false;
  volatile float      masterVol    = 0.90f;

  volatile int8_t  scopeBuf[120];
  volatile uint8_t scopeIdx = 0;

  uint32_t rng = 0x24681357;
  inline float nextNoise() {
    rng = rng * 1664525U + 1013904223U;
    return ((int32_t)(rng >> 16) - 32768) * (1.0f / 32768.0f);
  }

  int echoPos = 0;
  int rPos1 = 0, rPos2 = 0, rPos3 = 0;
  float lpL = 0.0f, lpR = 0.0f;
  float lfoPhase = 0.0f;

  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

  void begin() {
    memset(echoBufL, 0, sizeof(echoBufL));
    memset(echoBufR, 0, sizeof(echoBufR));
    memset(revBuf1, 0, sizeof(revBuf1));
    memset(revBuf2, 0, sizeof(revBuf2));
    memset(revBuf3, 0, sizeof(revBuf3));

    auto spk = M5Cardputer.Speaker.config();
    spk.sample_rate = SAMPLE_RATE;
    spk.stereo = true;
    spk.dma_buf_len = AUDIO_BUF_FRAMES;
    spk.dma_buf_count = 4;
    M5Cardputer.Speaker.config(spk);
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(255);

    xTaskCreatePinnedToCore(audioTask, "SynthProV3", 8192, this, 3, nullptr, 0);
  }

  int allocSlot(uint8_t midiNote, bool isDrum) {
    int slot = -1;
    uint32_t oldestAge = 0xFFFFFFFF;
    int oldestSlot = 0;
    for (int i = 0; i < MAX_VOICES; ++i) {
      if (!isDrum && voices[i].active && voices[i].drum == DRM_NONE && voices[i].midiNote == midiNote) {
        return i;
      }
      if (!voices[i].active && slot < 0) slot = i;
      if (voices[i].age < oldestAge) { oldestAge = voices[i].age; oldestSlot = i; }
    }
    return (slot >= 0) ? slot : oldestSlot;
  }

  void noteOn(uint8_t midiNote, int8_t padId, Instrument forceInst) {
    float freq = 440.0f * powf(2.0f, ((float)midiNote - 69.0f) / 12.0f);
    portENTER_CRITICAL(&mux);
    int slot = allocSlot(midiNote, false);
    Voice& v = voices[slot];
    v.active    = true;
    v.held      = true;
    v.midiNote  = midiNote;
    v.padId     = padId;
    v.age       = ++noteCounter;
    v.inst      = forceInst;
    v.drum      = DRM_NONE;
    v.wave      = synthWave;
    v.freq      = freq;
    v.phase     = 0.0f;
    v.phase2    = 0.0f;
    v.phase3    = 0.0f;
    v.phase4    = 0.0f;
    v.env       = (forceInst == INST_SYNTH && v.wave == WAVE_PAD) ? 0.02f : 1.0f;
    v.pitchEnv  = 1.0f;
    v.fltState  = 0.0f;
    v.hpState   = 0.0f;
    v.sampleAge = 0;

    if (forceInst == INST_LYRE || forceInst == INST_EGTR) {
      int len = (int)((float)SAMPLE_RATE / freq + 0.5f);
      if (len < 16) len = 16;
      if (len >= KS_MAX_LEN) len = KS_MAX_LEN - 1;
      v.ksLen = len;
      v.ksPos = 0;
      v.ksPrev = 0.0f;
      float last = 0.0f;
      for (int j = 0; j < len; ++j) {
        float n = nextNoise();
        float s = sinf(6.2831853f * (float)j / (float)len);
        if (forceInst == INST_EGTR) {
          last = last * 0.25f + n * 0.75f;
          float saw = (2.0f * (float)j / (float)len) - 1.0f;
          v.ksBuf[j] = last * 0.65f + saw * 0.35f;
        } else {
          // Crystalline Windsong Lyre harp
          last = last * 0.45f + n * 0.55f;
          v.ksBuf[j] = last * 0.52f + s * 0.48f;
        }
      }
    }
    portEXIT_CRITICAL(&mux);
  }

  void triggerDrum(DrumType dt, int8_t padId) {
    if (dt == DRM_NONE) return;
    portENTER_CRITICAL(&mux);
    int slot = allocSlot(0, true);
    Voice& v = voices[slot];
    v.active    = true;
    v.held      = false;
    v.midiNote  = 0;
    v.padId     = padId;
    v.age       = ++noteCounter;
    v.inst      = INST_DRUMS;
    v.drum      = dt;
    v.phase     = 0.0f;
    v.phase2    = 0.0f;
    v.phase3    = 0.0f;
    v.phase4    = 0.0f;
    v.env       = 1.0f;
    v.pitchEnv  = 1.0f;
    v.fltState  = 0.0f;
    v.hpState   = 0.0f;
    v.sampleAge = 0;
    portEXIT_CRITICAL(&mux);
  }

  void padOff(int8_t padId) {
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < MAX_VOICES; ++i) {
      if (voices[i].active && voices[i].drum == DRM_NONE && voices[i].padId == padId) {
        voices[i].held = false;
      }
    }
    portEXIT_CRITICAL(&mux);
  }

  void releaseAll() {
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < MAX_VOICES; ++i) voices[i].held = false;
    portEXIT_CRITICAL(&mux);
  }

private:
  static void audioTask(void* arg) {
    static_cast<SynthEngine*>(arg)->renderLoop();
  }

  void renderLoop() {
    static int16_t ringBuf[3][AUDIO_BUF_FRAMES * 2];
    uint8_t bufIdx = 0;
    uint8_t scopeDiv = 0;

    for (;;) {
      while (M5Cardputer.Speaker.isPlaying(0) >= 2) {
        vTaskDelay(1);
      }
      int16_t* outBuf = ringBuf[bufIdx];

      bool sustain   = sustainPedal || sustainLatch;
      uint8_t rMode  = reverbMode;
      uint8_t eMode  = echoMode;
      uint8_t mMode  = modMode;
      uint8_t fMode  = filterMode;
      uint8_t drv    = driveLevel;
      float vol      = masterVol;

      int echoDelay  = (eMode == 2) ? 9800 : 6200;
      float echoMix  = (eMode == 0) ? 0.0f : ((eMode == 2) ? 0.40f : 0.26f);
      float echoFb   = (eMode == 2) ? 0.45f : 0.32f;
      float revMix   = (rMode == 0) ? 0.0f : ((rMode == 2) ? 0.44f : 0.24f);
      float revFb    = (rMode == 2) ? 0.78f : 0.60f;
      float fltAlpha = (fMode == 0) ? 0.92f : ((fMode == 1) ? 0.38f : 0.16f);

      portENTER_CRITICAL(&mux);
      for (int s = 0; s < AUDIO_BUF_FRAMES; ++s) {
        lfoPhase += (5.2f * 6.2831853f) / (float)SAMPLE_RATE;
        if (lfoPhase > 6.2831853f) lfoPhase -= 6.2831853f;
        float lfo      = sinf(lfoPhase);
        float pitchMul = (mMode == 1) ? (1.0f + 0.0085f * lfo) : 1.0f;
        float tremL    = (mMode == 2) ? (0.72f + 0.28f * lfo) : 1.0f;
        float tremR    = (mMode == 2) ? (0.72f - 0.28f * lfo) : 1.0f;

        float mixL = 0.0f, mixR = 0.0f;

        for (int i = 0; i < MAX_VOICES; ++i) {
          Voice& v = voices[i];
          if (!v.active) continue;
          v.sampleAge++;

          float sample = 0.0f;

          if (v.drum != DRM_NONE) {
            // --- FAT, REALISTIC SYNTHESIZED DRUMS (TR-808 / 909 STYLE) ---
            switch (v.drum) {
              case DRM_KICK808: {
                float f = 44.0f + 150.0f * v.pitchEnv * v.pitchEnv;
                v.phase += (f * 6.2831853f) / (float)SAMPLE_RATE;
                if (v.phase > 6.2831853f) v.phase -= 6.2831853f;
                float click = (v.sampleAge < 90) ? (nextNoise() * 0.45f) : 0.0f;
                float body = sinf(v.phase) * 2.1f + click;
                sample = (body / (1.0f + fabsf(body) * 0.65f)) * v.env * 1.15f;
                v.pitchEnv *= 0.9968f;
                v.env *= 0.99945f; // Long booming 808 tail!
                break;
              }
              case DRM_KICKPUNCH: {
                float f = 54.0f + 220.0f * v.pitchEnv * v.pitchEnv;
                v.phase += (f * 6.2831853f) / (float)SAMPLE_RATE;
                if (v.phase > 6.2831853f) v.phase -= 6.2831853f;
                float click = (v.sampleAge < 70) ? (nextNoise() * 0.70f) : 0.0f;
                float body = sinf(v.phase) * 2.4f + click;
                sample = (body / (1.0f + fabsf(body) * 0.75f)) * v.env * 1.15f;
                v.pitchEnv *= 0.9920f;
                v.env *= 0.9986f;
                break;
              }
              case DRM_SNARE:
              case DRM_SNARE_FAT: {
                // Breakcore / Amen Break Cracky Piccolo Snare!
                float f1 = ((v.drum == DRM_SNARE_FAT) ? 185.0f : 255.0f) + 160.0f * v.pitchEnv;
                float f2 = f1 * 1.62f;
                v.phase  += (f1 * 6.2831853f) / (float)SAMPLE_RATE;
                v.phase2 += (f2 * 6.2831853f) / (float)SAMPLE_RATE;
                if (v.phase  > 6.2831853f) v.phase  -= 6.2831853f;
                if (v.phase2 > 6.2831853f) v.phase2 -= 6.2831853f;
                float n = nextNoise();
                v.fltState += (n - v.fltState) * 0.62f;
                v.hpState  += (v.fltState - v.hpState) * 0.18f;
                float wire = (v.fltState - v.hpState) * 3.1f;
                float shell = (sinf(v.phase) * 0.95f + sinf(v.phase2) * 0.45f) * v.pitchEnv;
                float raw = (shell + wire) * 1.4f;
                sample = (raw / (1.0f + fabsf(raw) * 0.55f)) * v.env * 1.15f;
                v.pitchEnv *= 0.9968f;
                v.env *= (v.drum == DRM_SNARE_FAT) ? 0.9990f : 0.9985f;
                break;
              }
              case DRM_CLAP: {
                // Classic 3-burst TR-909 Clap + resonant bandpass tail
                float burst = 0.0f;
                if (v.sampleAge < 380) {
                  uint32_t m = v.sampleAge % 380;
                  burst = (m < 140) ? 1.3f : 0.2f;
                } else if (v.sampleAge < 760) {
                  burst = ((v.sampleAge - 380) < 140) ? 1.4f : 0.25f;
                } else {
                  burst = 1.0f;
                }
                float n = nextNoise();
                v.fltState += (n - v.fltState) * 0.42f;
                v.hpState  += (v.fltState - v.hpState) * 0.18f;
                float bp = (v.fltState - v.hpState) * 2.8f;
                sample = bp * burst * v.env * 0.95f;
                if (v.sampleAge > 760) v.env *= 0.9983f;
                break;
              }
              case DRM_RIM: {
                v.phase  += (460.0f * 6.2831853f) / (float)SAMPLE_RATE;
                v.phase2 += (1680.0f * 6.2831853f) / (float)SAMPLE_RATE;
                sample = (sinf(v.phase) * 0.7f + sinf(v.phase2) * 0.5f) * v.env * 0.95f;
                v.env *= 0.9955f;
                break;
              }
              case DRM_CHH:
              case DRM_OHH:
              case DRM_CRASH: {
                // Ringing Breakcore Cymbal Crash ("Бздыщь!") & Crisp Metallic Hats!
                v.phase  += 311.0f / (float)SAMPLE_RATE; if (v.phase  >= 1.0f) v.phase  -= 1.0f;
                v.phase2 += 587.0f / (float)SAMPLE_RATE; if (v.phase2 >= 1.0f) v.phase2 -= 1.0f;
                v.phase3 += 1244.0f / (float)SAMPLE_RATE; if (v.phase3 >= 1.0f) v.phase3 -= 1.0f;
                float ring = sinf(v.phase * 6.2831853f + 2.6f * sinf(v.phase2 * 6.2831853f)) * 0.55f
                           + ((v.phase3 < 0.5f) ? 0.35f : -0.35f);
                float splash = ring * 0.62f + nextNoise() * 0.58f;
                v.hpState += (splash - v.hpState) * 0.28f;
                float hi = (splash - v.hpState) * 1.85f;
                sample = (hi / (1.0f + fabsf(hi) * 0.4f)) * v.env * ((v.drum == DRM_CRASH) ? 1.05f : 0.85f);
                float dec = (v.drum == DRM_CHH) ? 0.9958f : ((v.drum == DRM_OHH) ? 0.9991f : 0.99962f);
                v.env *= dec;
                break;
              }
              case DRM_TOM_LO:
              case DRM_TOM_MID:
              case DRM_TOM_HI: {
                float baseF = (v.drum == DRM_TOM_LO) ? 72.0f : ((v.drum == DRM_TOM_MID) ? 115.0f : 165.0f);
                float f = baseF + 90.0f * v.pitchEnv;
                v.phase += (f * 6.2831853f) / (float)SAMPLE_RATE;
                if (v.phase > 6.2831853f) v.phase -= 6.2831853f;
                float body = sinf(v.phase) * 1.6f;
                sample = (body / (1.0f + fabsf(body) * 0.5f)) * v.env;
                v.pitchEnv *= 0.9960f;
                v.env *= 0.9989f;
                break;
              }
              case DRM_ROLL: {
                // Rapid 3-hit Breakcore / Amen Snare Stutter ("ту-ту-туц!")
                uint32_t sub = v.sampleAge % 2200; // ~68ms stutter hits
                float hitEnv = (v.sampleAge < 6600) ? expf(-(float)sub * 0.0022f) : 0.0f;
                float f = 250.0f + 130.0f * hitEnv;
                v.phase += (f * 6.2831853f) / (float)SAMPLE_RATE;
                if (v.phase > 6.2831853f) v.phase -= 6.2831853f;
                float n = nextNoise();
                v.fltState += (n - v.fltState) * 0.6f;
                v.hpState  += (v.fltState - v.hpState) * 0.16f;
                sample = (sinf(v.phase) * 0.7f + (v.fltState - v.hpState) * 2.6f) * hitEnv * 1.05f;
                if (v.sampleAge >= 6600) v.env = 0.0f;
                break;
              }
              case DRM_KICKTEKK: {
                // Hardtekk / Gabber Distorted Punch Kick!
                float f = 48.0f + 260.0f * v.pitchEnv * v.pitchEnv;
                v.phase += (f * 6.2831853f) / (float)SAMPLE_RATE;
                if (v.phase > 6.2831853f) v.phase -= 6.2831853f;
                float raw = sinf(v.phase) * 5.2f + ((v.sampleAge < 80) ? nextNoise() * 1.2f : 0.0f);
                sample = (raw / (1.0f + fabsf(raw) * 0.85f)) * v.env * 1.15f;
                v.pitchEnv *= 0.9935f;
                v.env *= 0.9988f;
                break;
              }
              case DRM_KICKPIEPs: {
                // Uptempo Piep / Zaag Kick!
                float f = 62.0f + 520.0f * v.pitchEnv * v.pitchEnv * v.pitchEnv;
                v.phase += f / (float)SAMPLE_RATE;
                if (v.phase >= 1.0f) v.phase -= 1.0f;
                float saw = (2.0f * v.phase - 1.0f) * 3.8f + sinf(v.phase * 6.2831853f) * 2.5f;
                sample = (saw / (1.0f + fabsf(saw) * 0.8f)) * v.env * 1.05f;
                v.pitchEnv *= 0.9910f;
                v.env *= 0.9984f;
                break;
              }
              case DRM_SIREN: {
                // Rave Dub / Hardtekk Siren!
                v.phase2 += 4.5f / (float)SAMPLE_RATE;
                if (v.phase2 >= 1.0f) v.phase2 -= 1.0f;
                float f = 540.0f + 380.0f * sinf(v.phase2 * 6.2831853f);
                v.phase += f / (float)SAMPLE_RATE;
                if (v.phase >= 1.0f) v.phase -= 1.0f;
                float sq = (v.phase < 0.45f) ? 0.85f : -0.85f;
                v.fltState += (sq - v.fltState) * 0.25f;
                sample = v.fltState * v.env * 0.72f;
                v.env *= 0.99945f;
                break;
              }
              case DRM_COWBELL: {
                // Classic 808 Phonk Cowbell
                v.phase  += (545.0f * 6.2831853f) / (float)SAMPLE_RATE;
                v.phase2 += (815.0f * 6.2831853f) / (float)SAMPLE_RATE;
                float sq = ((sinf(v.phase) > 0.0f ? 0.5f : -0.5f) + (sinf(v.phase2) > 0.0f ? 0.5f : -0.5f));
                v.fltState += (sq - v.fltState) * 0.28f;
                sample = v.fltState * v.env * 0.95f;
                v.env *= 0.9982f;
                break;
              }
              case DRM_ZAP: {
                // Laser / Psy Zap
                float f = 120.0f + 1850.0f * v.pitchEnv * v.pitchEnv;
                v.phase += (f * 6.2831853f) / (float)SAMPLE_RATE;
                if (v.phase > 6.2831853f) v.phase -= 6.2831853f;
                sample = sinf(v.phase) * v.env * 0.85f;
                v.pitchEnv *= 0.9925f;
                v.env *= 0.9965f;
                break;
              }
              case DRM_SUBDROP: {
                float f = 36.0f + 110.0f * v.pitchEnv;
                v.phase += (f * 6.2831853f) / (float)SAMPLE_RATE;
                if (v.phase > 6.2831853f) v.phase -= 6.2831853f;
                sample = sinf(v.phase) * v.env * 1.15f;
                v.pitchEnv *= 0.9990f;
                v.env *= 0.9995f;
                break;
              }
              default: break;
            }
            if (v.env < 0.002f) { v.active = false; continue; }
            mixL += sample * 0.65f;
            mixR += sample * 0.65f;
            continue;
          }

          // --- MELODIC VOICES ---
          bool ringing = v.held || sustain;

          if (v.inst == INST_BASS) {
            // REAL FAT PUNCHY BASS GUITAR / 808 SUB + PLUCKED FILTER ENVELOPE!
            float dp = (v.freq * pitchMul) / (float)SAMPLE_RATE;
            v.phase += dp; if (v.phase >= 1.0f) v.phase -= 1.0f;
            float sub = sinf(v.phase * 6.2831853f);
            float saw = 2.0f * v.phase - 1.0f;
            float raw = sub * 0.65f + saw * 0.45f;
            // Filter envelope: bright finger/pick attack fading into warm sub body
            float cut = 0.06f + 0.42f * v.pitchEnv;
            v.fltState += (raw - v.fltState) * cut;
            v.pitchEnv *= 0.9965f;
            float gainB = (drv == 2) ? 3.2f : ((drv == 1) ? 2.1f : 1.4f);
            float x = v.fltState * v.env * gainB;
            sample = (x / (1.0f + fabsf(x) * 0.65f)) * 1.05f;
            v.env *= ringing ? 0.99993f : 0.9975f;
          } else if (v.inst == INST_LYRE || v.inst == INST_EGTR) {
            // Karplus-Strong plucked string (Lyre Harp & Overdriven Electric Guitar)
            float cur = v.ksBuf[v.ksPos];
            float damp = ringing ? 0.9968f : 0.985f;
            float nxt = 0.5f * (cur + v.ksPrev) * damp;
            v.ksPrev = cur;
            v.ksBuf[v.ksPos] = nxt;
            if (++v.ksPos >= v.ksLen) v.ksPos = 0;
            v.env *= ringing ? 0.99993f : 0.9980f;

            if (v.inst == INST_EGTR) {
              float gainD = (drv == 2) ? 8.5f : ((drv == 1) ? 4.8f : 1.8f);
              float x = cur * v.env * gainD;
              float dist = x / (1.0f + fabsf(x));
              // Guitar speaker cabinet warmth filter
              v.fltState += (dist - v.fltState) * 0.45f;
              sample = v.fltState * 0.85f;
            } else {
              sample = cur * v.env * 0.82f;
            }
          } else if (v.inst == INST_REESE) {
            // Sewerslvt / DnB Reese Bass (heavy detuned sub-saws + lowpass sweep)
            float f = v.freq * pitchMul * 0.5f;
            float dp1 = (f * 0.991f) / (float)SAMPLE_RATE;
            float dp2 = (f * 1.009f) / (float)SAMPLE_RATE;
            v.phase  += dp1; if (v.phase  >= 1.0f) v.phase  -= 1.0f;
            v.phase2 += dp2; if (v.phase2 >= 1.0f) v.phase2 -= 1.0f;
            float raw = (2.0f * v.phase - 1.0f) * 0.55f + (2.0f * v.phase2 - 1.0f) * 0.55f
                      + sinf(v.phase * 6.2831853f) * 0.45f;
            float cut = 0.08f + 0.16f * v.pitchEnv;
            v.fltState += (raw - v.fltState) * cut;
            v.pitchEnv *= 0.9985f;
            float drvR = (drv == 2) ? 3.2f : ((drv == 1) ? 2.1f : 1.3f);
            float x = v.fltState * drvR;
            sample = (x / (1.0f + fabsf(x) * 0.6f)) * v.env * 0.95f;
            v.env *= ringing ? 0.99993f : 0.9978f;
          } else if (v.inst == INST_CHAINSAW) {
            // 4-Oscillator Detuned Supersaw / Chainsaw Lead!
            float f = v.freq * pitchMul;
            v.phase  += (f * 0.985f) / (float)SAMPLE_RATE; if (v.phase  >= 1.0f) v.phase  -= 1.0f;
            v.phase2 += (f * 0.996f) / (float)SAMPLE_RATE; if (v.phase2 >= 1.0f) v.phase2 -= 1.0f;
            v.phase3 += (f * 1.004f) / (float)SAMPLE_RATE; if (v.phase3 >= 1.0f) v.phase3 -= 1.0f;
            v.phase4 += (f * 1.015f) / (float)SAMPLE_RATE; if (v.phase4 >= 1.0f) v.phase4 -= 1.0f;
            float stack = ((2.0f * v.phase  - 1.0f) + (2.0f * v.phase2 - 1.0f) +
                           (2.0f * v.phase3 - 1.0f) + (2.0f * v.phase4 - 1.0f)) * 0.25f;
            float drvC = (drv == 2) ? 4.5f : ((drv == 1) ? 2.4f : 1.1f);
            float x = stack * drvC;
            sample = (x / (1.0f + fabsf(x) * 0.7f)) * v.env * 0.65f;
            v.env *= ringing ? 0.99990f : 0.9972f;
          } else if (v.inst == INST_ACID303) {
            // Bulletproof Resonant TB-303 Acid Squelch (clamped feedback, zero NaN lockup!)
            float f = v.freq * pitchMul;
            v.phase += f / (float)SAMPLE_RATE;
            if (v.phase >= 1.0f) v.phase -= 1.0f;
            float saw = 2.0f * v.phase - 1.0f;
            float cut = 0.04f + 0.48f * v.pitchEnv * v.pitchEnv;
            float res = (drv == 2) ? 1.25f : ((drv == 1) ? 0.95f : 0.65f);
            float fb  = constrain(v.fltState - v.hpState, -1.8f, 1.8f) * res;
            v.fltState = constrain(v.fltState + ((saw + fb) - v.fltState) * cut, -2.5f, 2.5f);
            v.hpState  = constrain(v.hpState  + (v.fltState - v.hpState) * (cut * 0.45f), -2.5f, 2.5f);
            v.pitchEnv *= 0.9976f;
            sample = (v.fltState / (1.0f + fabsf(v.fltState) * 0.55f)) * v.env * 0.78f;
            v.env *= ringing ? 0.99988f : 0.9970f;
          } else if (v.inst == INST_PIANO) {
            // Real Acoustic Grand Piano: hammer transient + 3 warm partials + dynamic lowpass!
            float f = v.freq * pitchMul;
            float dp = (f * 6.2831853f) / (float)SAMPLE_RATE;
            v.phase  += dp;        if (v.phase  > 6.2831853f) v.phase  -= 6.2831853f;
            v.phase2 += dp * 2.0f; if (v.phase2 > 6.2831853f) v.phase2 -= 6.2831853f;
            v.phase3 += dp * 3.0f; if (v.phase3 > 6.2831853f) v.phase3 -= 6.2831853f;
            float body = sinf(v.phase) * 0.62f + sinf(v.phase2) * 0.28f * v.pitchEnv + sinf(v.phase3) * 0.14f * v.pitchEnv * v.pitchEnv;
            float hammer = (v.sampleAge < 110) ? (nextNoise() * 0.18f * (1.0f - (float)v.sampleAge / 110.0f)) : 0.0f;
            v.pitchEnv *= 0.99945f;
            sample = (body + hammer) * v.env * 0.85f;
            v.env *= ringing ? 0.999935f : 0.9974f;
          } else {
            // Universal Synth (SAW / SQR / TRI / PAD / PHONK BELL)
            float dp1 = (v.freq * pitchMul * 0.997f) / (float)SAMPLE_RATE;
            float dp2 = (v.freq * pitchMul * 1.003f) / (float)SAMPLE_RATE;
            v.phase  += dp1; if (v.phase  >= 1.0f) v.phase  -= 1.0f;
            v.phase2 += dp2; if (v.phase2 >= 1.0f) v.phase2 -= 1.0f;

            if (v.wave == WAVE_PAD) {
              if (ringing) { if (v.env < 0.88f) v.env += 0.00028f; }
              else         { v.env *= 0.9991f; }
              float s1 = sinf(v.phase * 6.2831853f);
              float s2 = sinf(v.phase2 * 6.2831853f);
              sample = (0.55f * s1 + 0.45f * s2 + 0.22f * sinf(v.phase * 12.56637f)) * v.env * 0.42f;
            } else if (v.wave == WAVE_BELL) {
              // Chromatic Phonk 808 Cowbell!
              v.phase3 += (v.freq * 1.495f) / (float)SAMPLE_RATE;
              if (v.phase3 >= 1.0f) v.phase3 -= 1.0f;
              float sq = ((v.phase < 0.5f ? 0.55f : -0.55f) + (v.phase3 < 0.5f ? 0.45f : -0.45f));
              v.fltState += (sq - v.fltState) * 0.28f;
              sample = v.fltState * v.env * 0.72f;
              v.env *= ringing ? 0.99980f : 0.9975f;
            } else if (v.wave == WAVE_SAW) {
              float w = ((2.0f * v.phase - 1.0f) + (2.0f * v.phase2 - 1.0f)) * 0.5f;
              sample = w * v.env * 0.40f;
              v.env *= ringing ? 0.99990f : 0.9975f;
            } else if (v.wave == WAVE_SQR) {
              float w = ((v.phase < 0.35f ? 1.0f : -1.0f) + (v.phase2 < 0.5f ? 0.6f : -0.6f)) * 0.5f;
              sample = w * v.env * 0.35f;
              v.env *= ringing ? 0.99988f : 0.9968f;
            } else {
              float t1 = 1.0f - 4.0f * fabsf(v.phase - 0.5f);
              float t2 = 1.0f - 4.0f * fabsf(v.phase2 - 0.5f);
              sample = (t1 + t2) * 0.5f * v.env * 0.52f;
              v.env *= ringing ? 0.99990f : 0.9978f;
            }
          }

          if (v.env < 0.0012f) { v.active = false; continue; }

          float panR = 0.36f + 0.055f * (float)i;
          mixL += sample * (1.0f - panR);
          mixR += sample * panR;
        }

        // Tone Filter + Tremolo (with NaN/Inf firewall so audio can NEVER lock up!)
        if (!isfinite(mixL)) mixL = 0.0f;
        if (!isfinite(mixR)) mixR = 0.0f;
        lpL += (mixL - lpL) * fltAlpha;
        lpR += (mixR - lpR) * fltAlpha;
        if (!isfinite(lpL)) lpL = 0.0f;
        if (!isfinite(lpR)) lpR = 0.0f;
        float dryL = lpL * tremL;
        float dryR = lpR * tremR;

        // Stereo Ping-Pong Echo
        int readPos = echoPos - echoDelay;
        if (readPos < 0) readPos += ECHO_LEN;
        float eL = (float)echoBufL[readPos] * (1.0f / 32767.0f);
        float eR = (float)echoBufR[readPos] * (1.0f / 32767.0f);

        echoBufL[echoPos] = (int16_t)(constrain(dryL + eR * echoFb, -1.0f, 1.0f) * 32000.0f);
        echoBufR[echoPos] = (int16_t)(constrain(dryR + eL * echoFb, -1.0f, 1.0f) * 32000.0f);
        if (++echoPos >= ECHO_LEN) echoPos = 0;

        float sigL = dryL + eL * echoMix;
        float sigR = dryR + eR * echoMix;

        // 3-Comb Stereo Hall Reverb
        float monoIn = (sigL + sigR) * 0.5f;
        float c1 = (float)revBuf1[rPos1] * (1.0f / 32767.0f);
        float c2 = (float)revBuf2[rPos2] * (1.0f / 32767.0f);
        float c3 = (float)revBuf3[rPos3] * (1.0f / 32767.0f);

        revBuf1[rPos1] = (int16_t)(constrain(monoIn + c1 * revFb, -1.0f, 1.0f) * 32000.0f);
        revBuf2[rPos2] = (int16_t)(constrain(monoIn + c2 * revFb, -1.0f, 1.0f) * 32000.0f);
        revBuf3[rPos3] = (int16_t)(constrain(monoIn + c3 * revFb, -1.0f, 1.0f) * 32000.0f);
        if (++rPos1 >= REV1_LEN) rPos1 = 0;
        if (++rPos2 >= REV2_LEN) rPos2 = 0;
        if (++rPos3 >= REV3_LEN) rPos3 = 0;

        float revL = (c1 + c2 * 0.7f - c3 * 0.5f) * 0.5f;
        float revR = (c2 + c3 * 0.7f - c1 * 0.5f) * 0.5f;

        float finL = (sigL + revL * revMix) * vol * 1.45f;
        float finR = (sigR + revR * revMix) * vol * 1.45f;
        finL = finL / (1.0f + fabsf(finL) * 0.55f);
        finR = finR / (1.0f + fabsf(finR) * 0.55f);

        // Mix recorded WAV playback (allows live overdubbing!)
        if (isPlayingRec && (playWriteIdx - playReadIdx) >= 2) {
          float pL = (float)playRing[playReadIdx % PLAY_RING_SAMPLES] * (1.0f / 32768.0f);
          float pR = (float)playRing[(playReadIdx + 1) % PLAY_RING_SAMPLES] * (1.0f / 32768.0f);
          playReadIdx += 2;
          finL = constrain(finL + pL * 0.85f, -1.0f, 1.0f);
          finR = constrain(finR + pR * 0.85f, -1.0f, 1.0f);
        }

        int16_t sL = (int16_t)(constrain(finL, -1.0f, 1.0f) * 32000.0f);
        int16_t sR = (int16_t)(constrain(finR, -1.0f, 1.0f) * 32000.0f);
        outBuf[s * 2]     = sL;
        outBuf[s * 2 + 1] = sR;

        // Push stereo sample to lock-free WAV recording ring buffer
        if (isRecording && (recWriteIdx - recReadIdx) <= (REC_RING_SAMPLES - 2)) {
          recRing[recWriteIdx % REC_RING_SAMPLES]       = sL;
          recRing[(recWriteIdx + 1) % REC_RING_SAMPLES] = sR;
          recWriteIdx += 2;
        }

        if ((++scopeDiv & 3) == 0) {
          uint8_t idx = scopeIdx;
          scopeBuf[idx] = (int8_t)(constrain((finL + finR) * 0.5f, -1.0f, 1.0f) * 120.0f);
          scopeIdx = (idx + 1) % 120;
        }
      }
      portEXIT_CRITICAL(&mux);

      M5Cardputer.Speaker.playRaw(outBuf, AUDIO_BUF_FRAMES * 2, SAMPLE_RATE, true, 1, 0, false);
      bufIdx = (bufIdx + 1) % 3;
    }
  }
};

// ============================================================================
//  Globals, 30-Step Unified Volume & Raw 4x14 Matrix State
// ============================================================================
static SynthEngine synth;
static M5Canvas canvas(&M5Cardputer.Display);

static Preferences prefs;
static ScaleLayout curLayout   = LAYOUT_MIN;
static VisMode     visMode     = VIS_RIPPLE_GRID;
static int8_t      transpose   = 0;
static uint8_t     themeIdx    = 1; // Default AMBER CRT (saved in NVS!)

static uint8_t  g0Clicks     = 0;
static uint32_t g0LastMs     = 0;

static void saveThemeToNVS() {
  prefs.begin("mmctool", false);
  prefs.putUChar("theme", themeIdx);
  prefs.end();
}

static void loadThemeFromNVS() {
  prefs.begin("mmctool", true);
  themeIdx = prefs.getUChar("theme", 1) % THEME_COUNT;
  prefs.end();
}
static int         volStep   = 18; // 1..30 (1..15 = 0..100%, 16..22 = 150%+, 23..30 = 200%!!)
static int         batteryPct = 100;
static uint32_t    lastBatMs  = 0;
static uint32_t    lastDrawMs = 0;

// Raw 4x14 physical keyboard matrix (y = 0..3, x = 0..13)
static bool curMat[4][14]  = {};
static bool prevMat[4][14] = {};

// Hold-Lock state using physical (x, y)
static int8_t   holdX       = -1;
static int8_t   holdY       = -1;
static uint32_t holdStartMs = 0;
static bool     holdFired   = false;

static void applyUnifiedVolume() {
  volStep = constrain(volStep, 1, 30);
  int pct = (volStep <= 15) ? (volStep * 100 / 15) : (100 + ((volStep - 15) * 100) / 15);
  uint8_t bLvl = (pct > 150) ? 2 : ((pct > 100) ? 1 : 0);
  HalES8311::setBoost(bLvl);
  if (volStep <= 15) {
    synth.masterVol = 0.08f + 0.92f * ((float)volStep / 15.0f);
  } else {
    synth.masterVol = 1.00f + 0.75f * ((float)(volStep - 15) / 15.0f);
  }
}

// Perfectly straight symmetric 3x8 pad grid (cols 0..7 -> x = 16..219)
static void getPadCenter(int r, int c, int& cx, int& cy) {
  cx = 18 + c * 29; // 18, 47, 76, 105, 134, 163, 192, 221 (centered in 240px!)
  if (visMode == VIS_RIPPLE_GRID) {
    cy = 48 + r * 28;
  } else {
    cy = 85 + r * 14;
  }
}

static bool isHoldProtectedPos(int x, int y) {
  if (y == 0 && x == 0) return true;            // Hold Esc (`) -> REAL PIANO!
  if (y == 0 && x >= 1 && x <= 10) return true;  // 1..0 (Note: - and = are now 1-tap Octave -3..+3!)
  if (x == 0 && y >= 1 && y <= 3) return true;   // Left column: Tab(REESE), Fn(CHAIN), Ctrl(ACID)
  if (y == 1 && (x == 9 || x == 10 || x == 13)) return true; // O, P, \ (REC)
  if (y == 2 && (x == 9 || x == 10 || x == 12 || x == 13)) return true; // K, L, ', Enter
  if (y == 3 && x == 12) return true; // / (GUIDE)
  return false;
}

static const char* holdPosLabel(int x, int y) {
  if (x == 0) {
    if (y == 1) return "REESE";
    if (y == 2) return "CHAINSAW";
    if (y == 3) return "ACID 303";
  }
  if (y == 0) {
    if (x == 0) return "PIANO";
    if (x >= 1 && x <= 6) return INST_NAMES[x - 1];
    if (x == 7) return (synth.instrument == INST_DRUMS) ? "DRUM KIT" : "WAVE/DRV";
    if (x == 8) return "VIS MODE";
    if (x == 9 || x == 10) return "SCALE";
    if (x == 11 || x == 12) return "TRANSPOSE";
  }
  if (y == 1 && x == 9)  return "REVERB";
  if (y == 1 && x == 10) return "ECHO";
  if (y == 1 && x == 13) return isRecording ? "STOP REC" : "REC WAV";
  if (y == 2 && x == 9)  return "MOD FX";
  if (y == 2 && x == 10) return "FILTER";
  if (y == 2 && x == 12) return "THEME";
  if (y == 2 && x == 13) return isPlayingRec ? "STOP PLAY" : "PLAY REC";
  if (y == 3 && x == 12) return guideActive ? "GUIDE OFF" : "GUIDE ON";
  return "LOCK";
}

static void fireHoldPos(int x, int y) {
  if (x == 0) {
    if (y == 1)      synth.instrument = INST_REESE;
    else if (y == 2) synth.instrument = INST_CHAINSAW;
    else if (y == 3) synth.instrument = INST_ACID303;
    synth.releaseAll();
  } else if (y == 0) {
    if (x == 0) {
      synth.instrument = INST_PIANO;
      curLayout = LAYOUT_PIANO;
      synth.releaseAll();
    } else if (x >= 1 && x <= 6) {
      synth.instrument = (Instrument)(x - 1);
      synth.releaseAll();
    } else if (x == 7) {
      if (synth.instrument == INST_DRUMS) {
        synth.drumKit = (DrumKit)((synth.drumKit + 1) % KIT_COUNT);
      } else if (synth.instrument == INST_SYNTH) {
        synth.synthWave = (SynthWave)((synth.synthWave + 1) % WAVE_COUNT);
      } else {
        synth.driveLevel = (synth.driveLevel + 1) % 3;
      }
    } else if (x == 8) {
      visMode = (VisMode)((visMode + 1) % VIS_COUNT);
    } else if (x == 9) {
      curLayout = (ScaleLayout)((curLayout + LAYOUT_COUNT - 1) % LAYOUT_COUNT);
      synth.releaseAll();
    } else if (x == 10) {
      curLayout = (ScaleLayout)((curLayout + 1) % LAYOUT_COUNT);
      synth.releaseAll();
    } else if (x == 11) {
      if (transpose > -12) transpose--;
    } else if (x == 12) {
      if (transpose < 12) transpose++;
    }
  } else if (y == 1) {
    if (x == 9)       synth.reverbMode = (synth.reverbMode + 1) % 3;
    else if (x == 10) synth.echoMode   = (synth.echoMode + 1) % 3;
    else if (x == 13) { if (isRecording) stopRecording(); else startRecording(); }
  } else if (y == 2) {
    if (x == 9)       synth.modMode    = (synth.modMode + 1) % 3;
    else if (x == 10) synth.filterMode = (synth.filterMode + 1) % 3;
    else if (x == 12) { themeIdx = (themeIdx + 1) % THEME_COUNT; saveThemeToNVS(); }
    else if (x == 13) { if (isPlayingRec) stopPlaybackRec(); else startPlaybackRec(); }
  } else if (y == 3 && x == 12) {
    guideActive = !guideActive;
    if (guideActive) {
      curLayout = LAYOUT_PIANO;
      if (synth.instrument == INST_DRUMS || synth.instrument == INST_DRUMPAD) synth.instrument = INST_LYRE;
      loadGuideSong();
    }
  }
}


// ============================================================================
//  Pro Music Theory HUD: Real Key Signature + Live Chord & Note+Octave Detector
// ============================================================================
static int getKeyRootPitchClass() {
  int base = (curLayout == LAYOUT_MIN || curLayout == LAYOUT_BIG || curLayout == LAYOUT_CHORD) ? 9 : 0;
  return ((base + transpose) % 12 + 12) % 12;
}

static void formatKeySignature(char* out, size_t outSz) {
  int rootPc = getKeyRootPitchClass();
  bool isMinor = (curLayout == LAYOUT_MIN || curLayout == LAYOUT_BIG || curLayout == LAYOUT_CHORD);
  snprintf(out, outSz, "%s%s", NOTE_NAMES[rootPc], isMinor ? "m" : "");
}

static bool buildLiveChordHUD(char* out, size_t outSz) {
  uint8_t notes[MAX_VOICES];
  int cnt = 0;
  for (int i = 0; i < MAX_VOICES; ++i) {
    if (synth.voices[i].active && synth.voices[i].drum == DRM_NONE && synth.voices[i].env > 0.08f) {
      notes[cnt++] = synth.voices[i].midiNote;
    }
  }
  if (cnt == 0) return false;

  // Sort ascending
  for (int i = 0; i < cnt - 1; ++i) {
    for (int j = i + 1; j < cnt; ++j) {
      if (notes[j] < notes[i]) { uint8_t t = notes[i]; notes[i] = notes[j]; notes[j] = t; }
    }
  }

  // Build pitch-class bitmask for chord recognition
  uint16_t mask = 0;
  for (int i = 0; i < cnt; ++i) mask |= (1U << (notes[i] % 12));

  char chordName[12] = "";
  if (cnt >= 2) {
    for (int r = 0; r < 12 && !chordName[0]; ++r) {
      if (!(mask & (1U << r))) continue;
      bool m3  = (mask & (1U << ((r + 3) % 12))) != 0;
      bool M3  = (mask & (1U << ((r + 4) % 12))) != 0;
      bool P5  = (mask & (1U << ((r + 7) % 12))) != 0;
      bool m7  = (mask & (1U << ((r + 10) % 12))) != 0;
      bool M7  = (mask & (1U << ((r + 11) % 12))) != 0;
      bool d5  = (mask & (1U << ((r + 6) % 12))) != 0;
      if (m3 && P5 && m7)      snprintf(chordName, sizeof(chordName), "%sm7",   NOTE_NAMES[r]);
      else if (M3 && P5 && m7) snprintf(chordName, sizeof(chordName), "%s7",    NOTE_NAMES[r]);
      else if (M3 && P5 && M7) snprintf(chordName, sizeof(chordName), "%smaj7", NOTE_NAMES[r]);
      else if (m3 && P5)       snprintf(chordName, sizeof(chordName), "%sm",    NOTE_NAMES[r]);
      else if (M3 && P5)       snprintf(chordName, sizeof(chordName), "%s",     NOTE_NAMES[r]);
      else if (m3 && d5)       snprintf(chordName, sizeof(chordName), "%sdim",  NOTE_NAMES[r]);
      else if (P5 && cnt == 2) snprintf(chordName, sizeof(chordName), "%s5",    NOTE_NAMES[r]);
    }
  }

  int pos = 0;
  if (chordName[0]) {
    pos += snprintf(out + pos, outSz - pos, "[%s] ", chordName);
  }
  for (int i = 0; i < cnt && i < 4 && pos < (int)outSz - 5; ++i) {
    int oct = (int)(notes[i] / 12) - 1;
    pos += snprintf(out + pos, outSz - pos, "%s%d ", NOTE_NAMES[notes[i] % 12], oct);
  }
  return true;
}

static void renderRecMenu(const Theme& th) {
  canvas.fillSprite(th.bg);
  for (int y = 0; y < SCREEN_H; y += 3) canvas.drawFastHLine(0, y, SCREEN_W, th.scan);

  canvas.fillRect(0, 0, SCREEN_W, 15, th.panel);
  canvas.drawFastHLine(0, 15, SCREEN_W, th.border);
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(1);
  canvas.setTextColor(th.primary, th.panel);
  canvas.setCursor(5, 4);
  canvas.printf("RECORDINGS (%d)  /MMCTool_Rec", recListCount);

  if (!sdReady) {
    canvas.setTextColor(th.accent, th.bg);
    canvas.drawCenterString("NO MICROSD CARD DETECTED", 120, 56);
    canvas.setTextColor(th.dim, th.bg);
    canvas.drawCenterString("Insert FAT32 SD & reboot", 120, 72);
  } else if (recListCount == 0) {
    canvas.setTextColor(th.text, th.bg);
    canvas.drawCenterString("No recordings yet!", 120, 52);
    canvas.setTextColor(th.secondary, th.bg);
    canvas.drawCenterString("Hold \\ (1.3s) to record WAV", 120, 68);
  } else {
    int visible = 7;
    int topIdx = max(0, recSelIdx - visible / 2);
    if (topIdx + visible > recListCount) topIdx = max(0, recListCount - visible);

    for (int i = 0; i < visible; ++i) {
      int idx = topIdx + i;
      if (idx >= recListCount) break;
      int ry = 20 + i * 14;
      bool sel = (idx == recSelIdx);
      bool playingThis = (isPlayingRec && sel);

      if (sel) {
        canvas.fillRoundRect(4, ry, SCREEN_W - 8, 13, 2, th.primary);
        canvas.setTextColor(th.bg, th.primary);
      } else {
        canvas.setTextColor(th.text, th.bg);
      }
      canvas.setCursor(10, ry + 3);
      canvas.printf("%s %s", playingThis ? ">" : " ", recNames[idx]);

      char durStr[12];
      snprintf(durStr, sizeof(durStr), "%u:%02u", (unsigned)(recDurs[idx] / 60), (unsigned)(recDurs[idx] % 60));
      canvas.setCursor(SCREEN_W - 38, ry + 3);
      canvas.print(durStr);
    }
  }

  canvas.fillRect(0, SCREEN_H - 13, SCREEN_W, 13, th.panel);
  canvas.drawFastHLine(0, SCREEN_H - 13, SCREEN_W, th.border);
  canvas.setTextColor(th.dim, th.panel);
  canvas.setCursor(4, SCREEN_H - 10);
  canvas.print(";/.:Move  ENT:Play/Stop  ESC/TAB:Back");
  canvas.pushSprite(0, 0);
}

// ============================================================================
//  CRT Boot Splash
// ============================================================================
static void showBootSplash() {
  const Theme& th = THEMES[themeIdx % THEME_COUNT];
  for (int f = 0; f < 30; ++f) {
    canvas.fillSprite(th.bg);

    if (th.style == STYLE_RETRO) {
      // Theme 0: RETRO 84 — Pixel terminal frame + falling blocks
      for (int y = 0; y < SCREEN_H; y += 2) canvas.drawFastHLine(0, y, SCREEN_W, th.scan);
      canvas.drawRect(4, 4, SCREEN_W - 8, SCREEN_H - 8, th.border);
      canvas.drawRect(7, 7, SCREEN_W - 14, SCREEN_H - 14, th.primary);
      for (int i = 0; i < 8; ++i) {
        int bx = 18 + i * 28;
        int by = 14 + ((f * 3 + i * 11) % 20);
        canvas.fillRect(bx, by, 10, 6, (i & 1) ? th.secondary : th.primary);
      }
      canvas.setFont(&fonts::Font2);
      canvas.setTextColor(th.primary);
      canvas.drawCenterString("MMCTOOL // 1984", 120, 44);
      canvas.setFont(&fonts::Font0);
      canvas.setTextColor(th.secondary);
      canvas.drawCenterString("[ SYSTEM READY - 4096 HZ ]", 120, 74);
      int pw = (f * 160) / 29;
      canvas.drawRect(38, 94, 164, 10, th.primary);
      canvas.fillRect(40, 96, pw, 6, th.primary);
    } else if (th.style == STYLE_CYBER) {
      // Theme 2: CYBER 2077 — Glitch RGB bars + chamfered Night City frame
      for (int y = 0; y < SCREEN_H; y += 3) canvas.drawFastHLine(0, y, SCREEN_W, th.scan);
      int gx = (f < 14 && (f % 2 == 0)) ? ((f & 1) ? 4 : -4) : 0;
      canvas.drawLine(10, 6, 230, 6, th.secondary);
      canvas.drawLine(6, 128, 224, 128, th.accent);
      canvas.drawLine(230, 6, 234, 14, th.secondary);
      canvas.setFont(&fonts::Font4);
      if (gx != 0) {
        canvas.setTextColor(th.accent);
        canvas.drawCenterString("MMCTOOL", 120 + gx, 28);
        canvas.setTextColor(th.secondary);
        canvas.drawCenterString("MMCTOOL", 120 - gx, 28);
      }
      canvas.setTextColor(th.primary);
      canvas.drawCenterString("MMCTOOL", 120, 28);
      canvas.setFont(&fonts::Font0);
      canvas.fillRect(44, 58, 152, 12, th.accent);
      canvas.setTextColor(th.bg);
      canvas.drawCenterString("NIGHT CITY SYNTH // v2.0", 120, 60);
      for (int i = 0; i < 12; ++i) {
        int bh = 6 + ((f * 2 + i * 5) % 22);
        canvas.fillRect(24 + i * 16, 118 - bh, 11, bh, (i & 1) ? th.primary : th.secondary);
      }
    } else if (th.style == STYLE_GHOST) {
      // Theme 3: GHOST WHITE — Misty monochrome translucent rings & fog
      for (int y = 0; y < SCREEN_H; y += 2) {
        for (int x = (y & 2); x < SCREEN_W; x += 4) canvas.drawPixel(x, y, th.scan);
      }
      int r = (f * 3) % 54;
      canvas.drawCircle(120, 68, r + 6, th.dim);
      canvas.drawCircle(120, 68, r + 18, th.secondary);
      canvas.drawCircle(120, 68, r + 32, th.primary);
      canvas.setFont(&fonts::Font4);
      canvas.setTextColor(th.primary);
      canvas.drawCenterString("MMCTOOL", 120, 32);
      canvas.setFont(&fonts::Font0);
      canvas.setTextColor(th.secondary);
      canvas.drawCenterString("TRANSPARENT WHITES // EDITION", 120, 64);
      canvas.drawRoundRect(20, 14, 200, 106, 10, th.border);
    } else {
      // Theme 1: AMBER CRT — Classic glowing amber phosphor waves
      for (int y = 0; y < SCREEN_H; y += 3) canvas.drawFastHLine(0, y, SCREEN_W, th.scan);
      int r = (f * 3) % 48;
      canvas.drawCircle(120, 68, r + 10, th.primary);
      canvas.drawCircle(120, 68, r + 24, th.secondary);
      canvas.setFont(&fonts::Font4);
      canvas.setTextColor(th.primary);
      canvas.drawCenterString("MMCTOOL", 120, 22);
      canvas.setFont(&fonts::Font0);
      canvas.fillRoundRect(26, 54, 188, 12, 2, th.primary);
      canvas.setTextColor(th.bg);
      canvas.drawCenterString("Cardputer & ADV Workstation", 120, 56);
      canvas.setTextColor(th.secondary);
      canvas.drawCenterString("G0: 1x/2x Oct | 3x Theme | ESC:Piano", 120, 108);
    }

    canvas.pushSprite(0, 0);
    delay(20);
  }
}

// ============================================================================
//  UI Renderer (3x8 Straight Numbered Grid + Ripple Waves + CRT Scope)
// ============================================================================
static void drawPill(int x, int y, int w, const char* label, bool on, const Theme& th) {
  uint16_t bg = on ? th.primary : th.panel;
  uint16_t fg = on ? th.bg      : th.dim;
  canvas.fillRoundRect(x, y, w, 11, 2, bg);
  if (!on) canvas.drawRoundRect(x, y, w, 11, 2, th.border);
  canvas.setTextColor(fg, bg);
  canvas.drawCenterString(label, x + w / 2, y + 2);
}

static void renderUI() {
  const Theme& th = THEMES[themeIdx % THEME_COUNT];
  if (recMenuOpen) {
    renderRecMenu(th);
    return;
  }
  canvas.fillSprite(th.bg);

  if (th.style == STYLE_GHOST) {
    // Translucent misty checkerboard dither
    for (int y = 0; y < SCREEN_H; y += 2) {
      for (int x = (y & 2); x < SCREEN_W; x += 4) canvas.drawPixel(x, y, th.scan);
    }
    canvas.drawRoundRect(1, 15, SCREEN_W - 2, SCREEN_H - 16, 6, th.dim);
  } else {
    for (int y = 0; y < SCREEN_H; y += 3) canvas.drawFastHLine(0, y, SCREEN_W, th.scan);
    if (th.style == STYLE_RETRO) {
      // Retro pixel corner brackets
      canvas.drawRect(1, 15, SCREEN_W - 2, SCREEN_H - 16, th.border);
      canvas.fillRect(1, 15, 5, 5, th.primary);
      canvas.fillRect(SCREEN_W - 6, 15, 5, 5, th.primary);
    } else if (th.style == STYLE_CYBER) {
      // Cyber 2077 chamfered HUD corners
      canvas.drawLine(2, 24, 10, 16, th.accent);
      canvas.drawLine(10, 16, 48, 16, th.accent);
      canvas.drawLine(SCREEN_W - 48, 16, SCREEN_W - 10, 16, th.secondary);
      canvas.drawLine(SCREEN_W - 10, 16, SCREEN_W - 2, 24, th.secondary);
    }
  }

  // --- Top Bar (0..14) ---
  canvas.fillRect(0, 0, SCREEN_W, 14, th.panel);
  canvas.drawFastHLine(0, 14, SCREEN_W, th.border);
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(1);

  char liveChord[48] = "";
  bool hasLiveChord = buildLiveChordHUD(liveChord, sizeof(liveChord));

  if (holdX >= 0 && !holdFired) {
    uint32_t elapsed = millis() - holdStartMs;
    uint32_t reqMs = entDoubleHold ? 450 : HOLD_LOCK_MS;
    int pct = (int)min(100UL, (elapsed * 100UL) / reqMs);
    canvas.setTextColor(th.primary, th.panel);
    canvas.setCursor(4, 3);
    canvas.printf("HOLD -> %s", entDoubleHold ? "REC MENU" : holdPosLabel(holdX, holdY));
    canvas.drawRect(108, 3, 126, 8, th.border);
    canvas.fillRect(109, 4, (pct * 124) / 100, 6, th.secondary);
  } else if (hasLiveChord && synth.instrument != INST_DRUMS) {
    canvas.fillRoundRect(3, 2, 38, 10, 2, th.primary);
    canvas.setTextColor(th.bg, th.primary);
    canvas.drawCenterString(INST_NAMES[synth.instrument], 22, 3);
    canvas.setTextColor(th.secondary, th.panel);
    canvas.setCursor(46, 3);
    canvas.print(liveChord);
  } else {
    canvas.fillRoundRect(3, 2, 38, 10, 2, th.primary);
    canvas.setTextColor(th.bg, th.primary);
    canvas.drawCenterString(INST_NAMES[synth.instrument], 22, 3);

    canvas.setTextColor(th.secondary, th.panel);
    canvas.setCursor(45, 3);
    if (synth.instrument == INST_DRUMS) {
      canvas.printf("KIT:%s", KIT_NAMES[synth.drumKit]);
    } else if (synth.instrument == INST_SYNTH) {
      canvas.printf("%s/%s", LAYOUT_NAMES[curLayout], WAVE_NAMES[synth.synthWave]);
    } else if (synth.instrument == INST_EGTR || synth.instrument == INST_BASS ||
               synth.instrument == INST_REESE || synth.instrument == INST_CHAINSAW ||
               synth.instrument == INST_ACID303) {
      const char* dLbl = (synth.driveLevel == 2) ? "HVY" : ((synth.driveLevel == 1) ? "CRN" : "CLN");
      canvas.printf("%s/%s", LAYOUT_NAMES[curLayout], dLbl);
    } else {
      canvas.print(LAYOUT_NAMES[curLayout]);
    }

    char keyStr[8];
    formatKeySignature(keyStr, sizeof(keyStr));
    canvas.setTextColor(th.accent, th.panel);
    canvas.setCursor(104, 3);
    if (octaveShift != 0) canvas.printf("%s%+d", keyStr, (int)octaveShift);
    else                  canvas.printf("K:%s", keyStr);

    int vPct = (volStep <= 15) ? (volStep * 100 / 15) : (100 + ((volStep - 15) * 100) / 15);
    canvas.setCursor(146, 3);
    canvas.printf("V:%d%%%s", vPct, vPct > 150 ? "!!" : (vPct > 100 ? "+" : ""));

    canvas.setTextColor(batteryPct > 25 ? th.primary : RGB565(255, 60, 60), th.panel);
    canvas.setCursor(208, 3);
    canvas.printf("%3d%%", batteryPct);
  }

  // --- FX Pills Row (17..28) ---
  const char* revTxt = (synth.reverbMode == 2) ? "HALL" : ((synth.reverbMode == 1) ? "ROOM" : "O:RV");
  const char* echTxt = (synth.echoMode   == 2) ? "ECH2" : ((synth.echoMode   == 1) ? "ECH1" : "P:EC");
  const char* modTxt = (synth.modMode    == 2) ? "TREM" : ((synth.modMode    == 1) ? "VIB"  : "K:MD");
  const char* fltTxt = (synth.filterMode == 2) ? "LOFI" : ((synth.filterMode == 1) ? "WARM" : "L:FL");
  bool susOn = synth.sustainPedal || synth.sustainLatch;

  drawPill(4,   17, 43, revTxt, synth.reverbMode > 0, th);
  drawPill(51,  17, 43, echTxt, synth.echoMode > 0,   th);
  drawPill(98,  17, 43, modTxt, synth.modMode > 0,    th);
  drawPill(145, 17, 43, fltTxt, synth.filterMode > 0, th);
  drawPill(192, 17, 44, synth.sustainLatch ? "SUS*" : "M:SU", susOn, th);

  // --- Expanding & Smoothly Dissolving Ripple Waves (0..2/3 bright, 2/3..3/3 smooth dim fade) ---
  updateRipples();
  for (int i = 0; i < MAX_RIPPLES; ++i) {
    if (!ripples[i].active) continue;
    float prog = ripples[i].radius / ripples[i].maxRadius;
    int rInt = (int)ripples[i].radius;
    if (prog <= 0.666f) {
      canvas.drawCircle(ripples[i].x, ripples[i].y, rInt, ripples[i].color);
      if (rInt > 3) canvas.drawCircle(ripples[i].x, ripples[i].y, rInt - 2, blend565(ripples[i].color, th.bg, 0.45f));
    } else {
      // Smooth evaporation from 2/3 to 3/3 distance!
      float fade = 1.0f - (prog - 0.666f) / 0.334f;
      uint16_t dimCol = blend565(ripples[i].color, th.bg, fade * 0.85f);
      if (fade > 0.35f) {
        canvas.drawCircle(ripples[i].x, ripples[i].y, rInt, dimCol);
      } else {
        // Dotted evaporating sparks at the very end
        for (int a = 0; a < 16; ++a) {
          float ang = (float)a * 0.3927f + prog * 2.0f;
          int px = ripples[i].x + (int)(cosf(ang) * ripples[i].radius);
          int py = ripples[i].y + (int)(sinf(ang) * ripples[i].radius);
          canvas.drawPixel(px, py, dimCol);
        }
      }
    }
  }

  if (visMode == VIS_SCOPE_GRID) {
    // --- CRT Oscilloscope Box (32..71) ---
    constexpr int OX = 4, OY = 32, OW = 232, OH = 39;
    canvas.drawRoundRect(OX, OY, OW, OH, 3, th.border);
    int midY = OY + OH / 2;
    canvas.drawFastHLine(OX + 2, midY, OW - 4, th.panel);

    uint8_t head = synth.scopeIdx;
    int prevX = OX + 3, prevY = midY;
    for (int i = 0; i < 114; ++i) {
      int8_t sv = synth.scopeBuf[(head + i) % 120];
      int px = OX + 3 + i * 2;
      int py = constrain(midY - (sv * (OH / 2 - 4)) / 120, OY + 2, OY + OH - 3);
      if (i > 0) {
        canvas.drawLine(prevX, prevY, px, py, th.secondary);
        canvas.drawLine(prevX, prevY + 1, px, py + 1, th.primary);
      }
      prevX = px; prevY = py;
    }

    // Compact 3x8 numbered circles (78..122)
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 8; ++c) {
        int cx, cy;
        getPadCenter(r, c, cx, cy);
        bool down = curMat[r + 1][c + 1];
        int num = r * 8 + c + 1;
        uint16_t fillC = down ? th.primary : th.panel;
        uint16_t rimC  = down ? th.secondary : th.border;
        canvas.fillCircle(cx, cy, 6, fillC);
        canvas.drawCircle(cx, cy, 6, rimC);
        canvas.setTextColor(down ? th.bg : th.text, fillC);
        char nb[4];
        snprintf(nb, sizeof(nb), "%d", num);
        canvas.drawCenterString(nb, cx, cy - 3);
      }
    }
  } else {
    // --- FULLSCREEN STRAIGHT 3x8 NOTE & GUIDE GRID ---
    int gR = -1, gC = -1;
    if (guideActive && guideLen > 0) {
      findGuideTargetPad(guideNotes[guideStep % guideLen], curLayout, synth.instrument, transpose, gR, gC);
    }
    bool guideBlink = ((millis() / 180) & 1) == 0;

    int rootPc = getKeyRootPitchClass();
    for (int r = 0; r < 3; ++r) {
      // Draw subtle octave number (5/4/3) on far left of each melodic row
      if (synth.instrument != INST_DRUMS && synth.instrument != INST_DRUMPAD) {
        int rowMidi = mapPadToMidi(r, 0, curLayout, synth.instrument, transpose);
        int rowOct = (rowMidi / 12) - 1;
        int dummyX, rowY;
        getPadCenter(r, 0, dummyX, rowY);
        canvas.setTextColor(th.dim, th.bg);
        canvas.setCursor(1, rowY - 4);
        canvas.printf("%d", rowOct);
      }
      for (int c = 0; c < 8; ++c) {
        int cx, cy;
        getPadCenter(r, c, cx, cy);
        bool down = curMat[r + 1][c + 1];
        bool isTarget = (guideActive && r == gR && c == gC);
        int midi = mapPadToMidi(r, c, curLayout, synth.instrument, transpose);
        bool isMelodic = (synth.instrument != INST_DRUMS && synth.instrument != INST_DRUMPAD);
        bool isSharp = isMelodic && (NOTE_NAMES[midi % 12][1] == '#');
        bool isRoot  = isMelodic && ((midi % 12) == rootPc);

        int rad = down ? 11 : (isTarget ? 11 : 9);
        uint16_t fillC = down ? th.primary :
                         (isTarget ? (guideBlink ? th.secondary : th.accent) :
                         (isSharp ? th.bg : th.panel));
        uint16_t rimC  = down ? th.secondary :
                         (isTarget ? RGB565(255, 255, 255) :
                         (isRoot ? th.secondary :
                         ((synth.instrument == INST_DRUMPAD && r == 0) ? th.accent :
                         ((synth.instrument == INST_DRUMPAD && r == 1) ? th.secondary :
                         (isSharp ? th.accent : th.border)))));
        if (th.style == STYLE_RETRO) {
          canvas.fillRect(cx - rad, cy - rad + 1, rad * 2, rad * 2 - 2, fillC);
          canvas.drawRect(cx - rad, cy - rad + 1, rad * 2, rad * 2 - 2, rimC);
        } else if (th.style == STYLE_CYBER) {
          canvas.fillRoundRect(cx - rad, cy - rad + 1, rad * 2, rad * 2 - 2, 3, fillC);
          canvas.drawRoundRect(cx - rad, cy - rad + 1, rad * 2, rad * 2 - 2, 3, rimC);
        } else {
          canvas.fillCircle(cx, cy, rad, fillC);
          canvas.drawCircle(cx, cy, rad, rimC);
        }
        if (down || isTarget || isRoot) {
          canvas.drawCircle(cx, cy, rad + 2, isTarget ? RGB565(255, 255, 255) : (down ? th.accent : th.secondary));
        }

        canvas.setTextColor((down || isTarget) ? th.bg : (isSharp ? th.primary : th.text), fillC);
        if (synth.instrument == INST_DRUMPAD && r == 0) {
          canvas.drawCenterString(DPAD_CHORD_NAMES[c], cx, cy - 4);
        } else if (synth.instrument == INST_DRUMS || (synth.instrument == INST_DRUMPAD && r == 2)) {
          char nb[4];
          snprintf(nb, sizeof(nb), "%d", r * 8 + c + 1);
          canvas.drawCenterString(nb, cx, cy - 4);
        } else if (synth.instrument == INST_DRUMPAD && r == 1) {
          canvas.drawCenterString(NOTE_NAMES[(DPAD_BASS_NOTES[c] + transpose + 120) % 12], cx, cy - 4);
        } else {
          canvas.drawCenterString(NOTE_NAMES[midi % 12], cx, cy - 4);
        }
      }
    }
  }

  // --- Footer (with SD REC / PLAY status) ---
  canvas.setCursor(4, 126);
  if (isRecording) {
    uint32_t sec = (millis() - recStartMs) / 1000;
    canvas.setTextColor(RGB565(255, 45, 45), th.bg);
    canvas.printf("* REC %02u:%02u (Hold \\ to Stop)", (unsigned)(sec / 60), (unsigned)(sec % 60));
  } else if (guideActive && guideLen > 0) {
    canvas.setTextColor(th.secondary, th.bg);
    const char* sName = (songListCount > 0) ? songNames[songSelIdx] : "DEMO";
    canvas.printf("GUIDE:%s %d/%d ,/.:Sng", sName, guideStep + 1, guideLen);
  } else if (isPlayingRec && recListCount > 0) {
    canvas.setTextColor(th.secondary, th.bg);
    canvas.printf("> PLAY %s (Hold ENT Stop)", recNames[recSelIdx]);
  } else if (sdReady && recListCount > 0) {
    canvas.setTextColor(th.dim, th.bg);
    canvas.printf("H\\:Rec HENT:Play ,/.:%s (%d/%d)", recNames[recSelIdx], recSelIdx + 1, recListCount);
  } else {
    canvas.setTextColor(th.dim, th.bg);
    canvas.print("H1-6:Inst O/P/K/L:FX M:Sus H\\:Rec");
  }

  canvas.pushSprite(0, 0);
}

// ============================================================================
//  Raw Physical 4x14 Matrix Keyboard Scanner (Q..I / Aa..J / Opt..N!)
// ============================================================================
static void processKeyboard() {
  M5Cardputer.update();

  // G0 side button (1x = Oct Up, 2x = Oct Down, 3x = Cycle Theme + Save NVS!)
  if (M5Cardputer.BtnA.wasPressed()) {
    g0Clicks++;
    g0LastMs = millis();
  }
  if (g0Clicks > 0 && (millis() - g0LastMs >= 360)) {
    uint8_t cl = g0Clicks;
    g0Clicks = 0;
    if (cl == 1) {
      if (octaveShift < 3) { octaveShift++; synth.releaseAll(); }
    } else if (cl == 2) {
      if (octaveShift > -3) { octaveShift--; synth.releaseAll(); }
    } else {
      themeIdx = (themeIdx + 1) % THEME_COUNT;
      saveThemeToNVS();
    }
  }
  memset(curMat, 0, sizeof(curMat));

  // Read exact raw physical (x, y) matrix positions — Aa/Opt/Alt work cleanly with ZERO Shift conflicts!
  const auto& kl = M5Cardputer.Keyboard.keyList();
  for (const auto& pt : kl) {
    if (pt.y >= 0 && pt.y < 4 && pt.x >= 0 && pt.x < 14) {
      curMat[pt.y][pt.x] = true;
    }
  }

  synth.sustainPedal = curMat[3][13]; // Spacebar (x=13, y=3)
  const Theme& th = THEMES[themeIdx % THEME_COUNT];

  // --- Recordings Menu Navigation Mode ---
  if (recMenuOpen) {
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 14; ++x) {
        if (curMat[y][x] && !prevMat[y][x]) {
          if ((y == 0 && x == 0) || (y == 1 && x == 0)) {
            // Esc (`) or Tab -> close menu
            recMenuOpen = false;
          } else if ((y == 2 && x == 11) || (y == 1 && x == 2)) {
            // ; or W -> Up
            if (recListCount > 0) recSelIdx = (recSelIdx - 1 + recListCount) % recListCount;
          } else if ((y == 3 && x == 11) || (y == 2 && x == 3)) {
            // . or S -> Down
            if (recListCount > 0) recSelIdx = (recSelIdx + 1) % recListCount;
          } else if (y == 2 && x == 13) {
            // Enter -> Play selected recording and immediately close menu!
            startPlaybackRec();
            recMenuOpen = false;
          }
        }
      }
    }
    memcpy(prevMat, curMat, sizeof(prevMat));
    return;
  }

  // Update active Hold-Lock timer (including Tap + Hold Enter -> Recordings Menu!)
  if (holdX >= 0 && holdY >= 0) {
    if (curMat[holdY][holdX]) {
      uint32_t reqMs = entDoubleHold ? 450 : HOLD_LOCK_MS;
      if (!holdFired && (millis() - holdStartMs >= reqMs)) {
        holdFired = true;
        if (entDoubleHold) {
          entDoubleHold = false;
          if (isRecording) stopRecording();
          scanRecFolder();
          recMenuOpen = true;
          holdX = -1; holdY = -1;
          memcpy(prevMat, curMat, sizeof(prevMat));
          return;
        } else {
          fireHoldPos(holdX, holdY);
        }
      }
    } else {
      // Released before hold completed: if it was a short Enter tap, arm double-tap-hold window!
      if (holdX == 13 && holdY == 2 && !holdFired) {
        entLastTapMs = millis();
      }
      holdX = -1;
      holdY = -1;
      holdFired = false;
      entDoubleHold = false;
    }
  }

  // Scan all 4x14 matrix keys for press / release edges
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 14; ++x) {
      bool now  = curMat[y][x];
      bool prev = prevMat[y][x];
      if (now == prev) continue;

      // 1) Playable 3x8 Pad Grid: Rows 1..3, Cols 1..8 (Q..I, Aa..J, Opt..N)
      if (y >= 1 && y <= 3 && x >= 1 && x <= 8) {
        int r = y - 1;
        int c = x - 1;
        int8_t padId = (int8_t)(r * 8 + c);

        if (now) {
          int cx, cy;
          getPadCenter(r, c, cx, cy);
          uint16_t ripCol = (r == 0) ? th.secondary : ((r == 1) ? th.primary : th.accent);
          spawnRipple(cx, cy, ripCol, visMode == VIS_RIPPLE_GRID ? 69.0f : 36.0f);

          if (synth.instrument == INST_DRUMS) {
            synth.triggerDrum(DRUM_KITS[synth.drumKit][r][c], padId);
          } else if (synth.instrument == INST_DRUMPAD) {
            if (r == 0) {
              // Top Row: 8 Instant Emotional Chords (Warm Pad/Synth)
              for (int n = 0; n < 3; ++n) {
                uint8_t note = (uint8_t)constrain((int)DPAD_CHORDS[c][n] + transpose, 36, 96);
                synth.noteOn(note, padId, INST_SYNTH);
              }
            } else if (r == 1) {
              // Mid Row: 8 Matching Fat 808 Bass Notes!
              uint8_t note = (uint8_t)constrain((int)DPAD_BASS_NOTES[c] + transpose, 33, 84);
              synth.noteOn(note, padId, INST_BASS);
            } else {
              // Bottom Row: 8 Punchy Beat Pads!
              synth.triggerDrum(DPAD_BEATS[c], padId);
            }
          } else if (curLayout == LAYOUT_BIG) {
            int bigC = c / 2;
            int partnerX = 1 + bigC * 2 + ((c & 1) ? 0 : 1);
            if (!prevMat[y][partnerX]) {
              int midi = mapPadToMidi(r, c, LAYOUT_BIG, synth.instrument, transpose);
              synth.noteOn((uint8_t)midi, (int8_t)(r * 4 + bigC), synth.instrument);
            }
          } else if (curLayout == LAYOUT_CHORD) {
            int root = mapPadToMidi(r, c, LAYOUT_MIN, synth.instrument, transpose);
            int n2 = (synth.instrument == INST_EGTR) ? (root + 7)  : (root + ((c == 2 || c == 5 || c == 6) ? 4 : 3));
            int n3 = (synth.instrument == INST_EGTR) ? (root + 12) : (root + 7);
            synth.noteOn((uint8_t)constrain(root, 33, 96), padId, synth.instrument);
            delay(18);
            synth.noteOn((uint8_t)constrain(n2,   33, 96), padId, synth.instrument);
            delay(18);
            synth.noteOn((uint8_t)constrain(n3,   33, 96), padId, synth.instrument);
          } else {
            int midi = mapPadToMidi(r, c, curLayout, synth.instrument, transpose);
            synth.noteOn((uint8_t)midi, padId, synth.instrument);
            if (guideActive && guideLen > 0) {
              int gR = -1, gC = -1;
              if (findGuideTargetPad(guideNotes[guideStep % guideLen], curLayout, synth.instrument, transpose, gR, gC)) {
                if (r == gR && c == gC) {
                  guideStep = (guideStep + 1) % guideLen;
                }
              }
            }
          }
        } else {
          if (curLayout == LAYOUT_BIG) {
            int bigC = c / 2;
            if (!curMat[y][1 + bigC * 2] && !curMat[y][2 + bigC * 2]) {
              synth.padOff((int8_t)(r * 4 + bigC));
            }
          } else {
            synth.padOff(padId);
          }
        }
        continue;
      }

      // 2) Control & FX keys (on press)
      if (now) {
        if (isHoldProtectedPos(x, y)) {
          holdX = (int8_t)x;
          holdY = (int8_t)y;
          holdStartMs = millis();
          holdFired = false;
          entDoubleHold = (x == 13 && y == 2 && (millis() - entLastTapMs <= 550));
        } else if (y == 3 && x == 9) {
          // M key: instant 1-tap Sustain Latch toggle!
          synth.sustainLatch = !synth.sustainLatch;
        } else if (y == 1 && x == 11) {
          // [ key: Volume Down (30 steps)
          if (volStep > 1) { volStep--; applyUnifiedVolume(); }
        } else if (y == 1 && x == 12) {
          // ] key: Volume Up (30 steps -> 200% auto boost!)
          if (volStep < 30) { volStep++; applyUnifiedVolume(); }
        } else if (y == 0 && x == 11) {
          // - key: 1-tap Octave Down (-3..+3)
          if (octaveShift > -3) { octaveShift--; synth.releaseAll(); }
        } else if (y == 0 && x == 12) {
          // = key: 1-tap Octave Up (-3..+3)
          if (octaveShift < 3) { octaveShift++; synth.releaseAll(); }
        } else if (y == 0 && x == 13) {
          // Backspace / Del key: Force Stop recording playback immediately!
          if (isPlayingRec) stopPlaybackRec();
        } else if (y == 3 && x == 10) {
          if (guideActive && songListCount > 0) {
            songSelIdx = (songSelIdx - 1 + songListCount) % songListCount;
            loadGuideSong();
          } else if (recListCount > 0) {
            recSelIdx = (recSelIdx - 1 + recListCount) % recListCount;
          }
        } else if (y == 3 && x == 11) {
          if (guideActive && songListCount > 0) {
            songSelIdx = (songSelIdx + 1) % songListCount;
            loadGuideSong();
          } else if (recListCount > 0) {
            recSelIdx = (recSelIdx + 1) % recListCount;
          }
        }
      }
    }
  }

  memcpy(prevMat, curMat, sizeof(prevMat));
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = true;
  M5Cardputer.begin(cfg, true);

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(190);

  canvas.setColorDepth(8);
  canvas.createSprite(SCREEN_W, SCREEN_H);

  loadThemeFromNVS();
  showBootSplash();

  sdSPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  sdReady = SD.begin(PIN_SD_CS, sdSPI, 16000000);
  if (sdReady) { scanRecFolder(); scanSongFolder(); }

  synth.begin();
  delay(30);
  applyUnifiedVolume();
  synth.noteOn(69, -1, INST_LYRE); // Startup crystal harp chime
  batteryPct = M5Cardputer.Power.getBatteryLevel();
}

void loop() {
  processKeyboard();
  serviceSDStreams();

  uint32_t now = millis();
  if (now - lastBatMs >= 5000) {
    lastBatMs = now;
    batteryPct = M5Cardputer.Power.getBatteryLevel();
  }
  uint32_t frameMs = isRecording ? 40 : 25; // Smooth 25 FPS while recording to SD
  if (now - lastDrawMs >= frameMs) {
    lastDrawMs = now;
    renderUI();
  }
  delay(2);
}
