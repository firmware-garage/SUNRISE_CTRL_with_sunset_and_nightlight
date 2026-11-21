/*
  Sunrise Light v2
  ESP8266 + WS2815 strip (144 px), green-on-black CRT control page.

  - First boot: no credentials -> opens AP "Sunrise-Setup" with a captive
    portal (WiFiManager). Pick your network there; it's remembered.
  - Page lives at http://sunrise.local (mDNS). The IP is shown on the page too.
  - Time from NTP, US Central with DST.
  - OTA updates via PlatformIO env nodemcuv2_ota, or in a browser at
    http://sunrise.local/update (user "admin", password OTA_PASSWORD).
    Upload .pio/build/nodemcuv2/firmware.bin there.

  LED data: GPIO3 (the RX pin) by default, sent by the I2S DMA engine, so
  WiFi interrupts can't corrupt the timing and the CPU isn't blocked.
  GPIO3 is not a boot strapping pin, so nothing the strip does to the data
  line at power-up can stop the ESP8266 from booting. Serial is TX only
  (logging still works, serial input doesn't). See LED_DATA_GPIO below to go
  back to the old GPIO2 (D4) / UART1 wiring.
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <NeoPixelBus.h>
#include <time.h>

// ---------------- User config ----------------

#define SETUP_AP_NAME  "Sunrise-Setup"
// OTA_PASSWORD comes from secrets.ini through platformio.ini (build_flags and
// upload_flags read the same value), so the password lives in one gitignored
// file. This fallback only applies when building outside PlatformIO.
#ifndef OTA_PASSWORD
#define OTA_PASSWORD   "changeme"                // also the /update page and RESET WIFI password
#endif
#define UPDATE_USER    "admin"                   // /update page user name
#define TZ_STRING      "CST6CDT,M3.2.0,M11.1.0"  // US Central, DST 2nd Sun Mar to 1st Sun Nov. Change if not in US Central
#define NTP_1          "pool.ntp.org"
#define NTP_2          "time.nist.gov"

const uint16_t NUM_LEDS = 144;
const uint32_t FRAME_MS = 33;                     // ~30 fps; plenty for slow fades

// LED data pin. On the ESP8266, NeoPixelBus can only drive the strip from
// hardware on GPIO1, GPIO2 or GPIO3:
//   3 = RX pin, I2S DMA (default). Not a strapping pin; an input during boot.
//   2 = D4, UART1 (the old wiring). GPIO2 must be HIGH at reset or the chip
//       won't boot, and the boot loader also uses it as a TX output, so the
//       strip can see garbage until setup() runs. Only use it behind a
//       74AHCT125 powered from the same 5 V as the board.
// GPIO1 (TX) is not offered: the ROM prints its 74880-baud boot log there.
#define LED_DATA_GPIO  3

#if LED_DATA_GPIO == 3
  typedef NeoEsp8266DmaWs2812xMethod   LedMethod;
#elif LED_DATA_GPIO == 2
  typedef NeoEsp8266Uart1Ws2812xMethod LedMethod;
#else
  #error "LED_DATA_GPIO must be 3 (RX, DMA) or 2 (D4, UART1)"
#endif

// This strip takes RGB byte order. If red and green come out swapped, change to NeoGrbFeature.
// Ws2812x timing has the 300 us reset the WS2815 needs.
NeoPixelBus<NeoRgbFeature, LedMethod> strip(NUM_LEDS);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// ---------------- Settings (EEPROM) ----------------
// v2 layout first, night light fields appended, so a v2 block ("SUN2") still
// reads correctly and is upgraded in place instead of reset to defaults.
struct Settings {
  uint32_t magic;
  uint8_t  enabled;
  uint8_t  startHour, startMin;
  uint8_t  sunriseMaxPct;
  uint8_t  sunsetMaxPct;
  uint8_t  pad;
  uint16_t sunriseMin, holdMin, rampDownMin, sunsetMin;
  // --- added in v3 (night light) ---
  uint8_t  nlEnabled;                             // run the night light after a sunset
  uint8_t  nlIntensity;                           // 0..100 shimmer + sparkle rate
  uint8_t  nlBright;                              // 1..100
  uint8_t  pad2;
  char     mdnsHostname[32];                      // mDNS hostname (e.g. "sunrise")
};
static_assert(offsetof(Settings, sunsetMin) == 16, "v2 fields must keep their offsets");
static_assert(offsetof(Settings, nlEnabled) == 18, "v3 fields start right after v2");
const uint32_t SETTINGS_MAGIC_V2 = 0x53554E32;    // "SUN2"
const uint32_t SETTINGS_MAGIC    = 0x53554E33;    // "SUN3"
Settings cfg;

// Night light end time, kept apart from the settings so a power cut in the
// night can pick the night light back up. Written once when a night starts.
struct NightRec { uint32_t magic; uint32_t endEpoch; };
const int      NIGHT_REC_ADDR  = 64;
const uint32_t NIGHT_REC_MAGIC = 0x4E495445;      // "NITE"
const size_t   EEPROM_BYTES    = 128;

void nlDefaults() { cfg.nlEnabled = 1; cfg.nlIntensity = 40; cfg.nlBright = 10; cfg.pad2 = 0; strcpy(cfg.mdnsHostname, "sunrise"); }

void loadDefaults() {
  //      magic           en  hh mm  srMax ssMax pad  srMin hold ramp ssMin
  cfg = { SETTINGS_MAGIC, 1,  6, 0,  100,  40,   0,   30,   30,  10,  20, 1, 40, 10, 0 };
}

void clampSettings() {
  cfg.enabled       = cfg.enabled ? 1 : 0;
  cfg.startHour     = constrain(cfg.startHour, 0, 23);
  cfg.startMin      = constrain(cfg.startMin, 0, 59);
  cfg.sunriseMaxPct = constrain(cfg.sunriseMaxPct, 1, 100);
  cfg.sunsetMaxPct  = constrain(cfg.sunsetMaxPct, 1, 100);
  cfg.sunriseMin    = constrain(cfg.sunriseMin, 1, 180);
  cfg.holdMin       = constrain(cfg.holdMin, 0, 240);
  cfg.rampDownMin   = constrain(cfg.rampDownMin, 0, 120);
  cfg.sunsetMin     = constrain(cfg.sunsetMin, 1, 180);
  cfg.nlEnabled     = cfg.nlEnabled ? 1 : 0;
  cfg.nlIntensity   = constrain(cfg.nlIntensity, 0, 100);
  cfg.nlBright      = constrain(cfg.nlBright, 1, 100);
}

void saveSettings() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

void loadSettings() {
  EEPROM.get(0, cfg);
  if (cfg.magic == SETTINGS_MAGIC_V2) {           // keep the v2 settings, add the new ones
    nlDefaults();
    cfg.magic = SETTINGS_MAGIC;
    clampSettings();
    saveSettings();
  } else if (cfg.magic != SETTINGS_MAGIC) {
    loadDefaults();
    saveSettings();
  }
  clampSettings();
}

uint32_t loadNightEnd() {
  NightRec r;
  EEPROM.get(NIGHT_REC_ADDR, r);
  return r.magic == NIGHT_REC_MAGIC ? r.endEpoch : 0;
}

void storeNightEnd(uint32_t endEpoch) {
  if (loadNightEnd() == endEpoch) return;         // no flash write if nothing changed
  NightRec r = { NIGHT_REC_MAGIC, endEpoch };
  EEPROM.put(NIGHT_REC_ADDR, r);
  EEPROM.commit();
}

// ---------------- State machine ----------------
enum class Mode : uint8_t { Idle, Sunrise, Hold, RampDown, Sunset, Night };
Mode     mode      = Mode::Idle;
uint32_t modeStart = 0;                           // millis() when the current mode began
bool     otaActive = false;
uint32_t frameUsPeak = 0, frameUsMax = 0;        // render timing, for /api/state

// Night light timing
const uint32_t NL_TEST_MS      = 10UL * 60000UL;  // TEST NIGHT turns itself off after 10 min
const uint32_t NL_NOCLOCK_MS   = 12UL * 3600000UL;// no NTP time: run at most 12 h
const float    NL_FADE_IN_S    = 2.0f;            // after a sunset: glints appear within a couple of seconds
const float    NL_TEST_IN_S    = 2.0f;
const float    NL_TO_SUNRISE_S = 30.0f;           // overlaps the first 30 s of the sunrise
const float    NL_END_S        = 60.0f;           // START time with the alarm off
bool     nightTest  = false;
uint32_t nightDurMs = 0;                          // recomputed from the clock every second

const char* modeName(Mode m) {
  switch (m) {
    case Mode::Sunrise:  return "SUNRISE";
    case Mode::Hold:     return "HOLD";
    case Mode::RampDown: return "RAMP DOWN";
    case Mode::Sunset:   return "SUNSET";
    case Mode::Night:    return nightTest ? "NITE TEST" : "NIGHT";
    default:             return "IDLE";
  }
}

uint32_t modeDurationMs(Mode m) {
  switch (m) {
    case Mode::Sunrise:  return cfg.sunriseMin  * 60000UL;
    case Mode::Hold:     return cfg.holdMin     * 60000UL;
    case Mode::RampDown: return cfg.rampDownMin * 60000UL;
    case Mode::Sunset:   return cfg.sunsetMin   * 60000UL;
    case Mode::Night:    return nightDurMs;
    default:             return 0;
  }
}

// backdateMs lets a mode start "in the past" (catch-up after a reboot,
// or carrying overshoot into the next mode). Unsigned math handles wrap.
void setMode(Mode m, uint32_t backdateMs = 0) {
  mode = m;
  modeStart = millis() - backdateMs;
}

uint32_t modeElapsed() { return millis() - modeStart; }

float modeProgress() {
  uint32_t dur = modeDurationMs(mode);
  if (dur == 0) return 1.0f;
  uint32_t el = modeElapsed();
  return el >= dur ? 1.0f : (float)el / (float)dur;
}

void startNight(bool test, uint32_t backdateMs = 0, bool resumed = false);
void endNight(float fadeS);

// Chains through zero-length modes (hold = 0, ramp = 0) in one call.
void advanceMode() {
  while (mode != Mode::Idle) {
    uint32_t dur = modeDurationMs(mode);
    uint32_t el  = modeElapsed();
    if (el < dur) break;
    uint32_t over = el - dur;
    switch (mode) {
      case Mode::Sunrise: setMode(Mode::Hold, over);     break;
      case Mode::Hold:    setMode(Mode::RampDown, over); break;
      case Mode::Sunset:                               // sunset ends at black; night light takes over
        if (cfg.nlEnabled) startNight(false, over);
        else               setMode(Mode::Idle);
        break;
      case Mode::Night:   endNight(nightTest ? 2.0f : NL_END_S); break;  // test timeout or 12 h cap
      default:            setMode(Mode::Idle);           break;
    }
  }
}

// ---------------- Color ----------------
struct Col  { float r, g, b; };
struct Stop { float t; Col c; };

// Palette values are perceptual (pre-gamma).
const Stop SUNRISE_PAL[] = {
  { 0.00f, { 0.20f, 0.00f, 0.35f } },   // deep purple
  { 0.18f, { 0.45f, 0.00f, 0.30f } },   // plum
  { 0.36f, { 0.85f, 0.05f, 0.05f } },   // red
  { 0.55f, { 1.00f, 0.30f, 0.00f } },   // orange
  { 0.75f, { 1.00f, 0.70f, 0.10f } },   // yellow
  { 0.90f, { 1.00f, 0.88f, 0.55f } },   // pale gold
  { 1.00f, { 1.00f, 0.95f, 0.85f } },   // warm white
};

// Blue channel is zero in every stop: no blue wavelengths at night.
const Stop SUNSET_PAL[] = {
  { 0.00f, { 0.45f, 1.00f, 0.00f } },   // yellow-green
  { 0.25f, { 1.00f, 0.85f, 0.00f } },   // amber
  { 0.50f, { 1.00f, 0.45f, 0.00f } },   // orange
  { 0.75f, { 1.00f, 0.15f, 0.00f } },   // red-orange
  { 1.00f, { 0.70f, 0.00f, 0.00f } },   // deep red
};

static inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
static inline float smooth(float x)  { x = clamp01(x); return x * x * (3.0f - 2.0f * x); }

Col lerpCol(const Col& a, const Col& b, float t) {
  return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

Col samplePalette(const Stop* s, uint8_t n, float t) {
  t = clamp01(t);
  for (uint8_t i = 1; i < n; i++) {
    if (t <= s[i].t) {
      float span = s[i].t - s[i - 1].t;
      float f = span > 0.0f ? (t - s[i - 1].t) / span : 0.0f;
      return lerpCol(s[i - 1].c, s[i].c, f);
    }
  }
  return s[n - 1].c;
}

// Gamma 2.2 lookup. A LUT because powf() in soft-float would eat the CPU.
uint8_t gammaLut[1024];
void buildGamma() {
  for (int i = 0; i < 1024; i++)
    gammaLut[i] = (uint8_t)(powf(i / 1023.0f, 2.2f) * 255.0f + 0.5f);
}
static inline uint8_t toByte(float v) { return gammaLut[(uint16_t)(clamp01(v) * 1023.0f + 0.5f)]; }

// Both effects are symmetric about the center, so compute half and mirror.
void setPair(uint16_t i, const Col& c, float bright) {
  RgbColor px(toByte(c.r * bright), toByte(c.g * bright), toByte(c.b * bright));
  strip.SetPixelColor(i, px);
  strip.SetPixelColor(NUM_LEDS - 1 - i, px);
}

// p: 0..1 through the sunrise. bright: 0..1 overall level.
// Final frame: white across the middle half, end quarters blending back to purple.
void renderSunrise(float p, float bright) {
  const float c = (NUM_LEDS - 1) * 0.5f;
  const uint8_t n = sizeof(SUNRISE_PAL) / sizeof(SUNRISE_PAL[0]);
  for (uint16_t i = 0; i < (NUM_LEDS + 1) / 2; i++) {
    float d = (c - i) / c;                                   // 1 at the ends, ~0 at center
    float shape = (d <= 0.5f) ? 1.0f : 1.0f - smooth((d - 0.5f) * 2.0f);
    float k = 0.6f * d;                                      // outer pixels start later
    float front = smooth((p - k) / (1.0f - k));              // bloom from the middle
    setPair(i, samplePalette(SUNRISE_PAL, n, shape * front), bright);
  }
}

// q: 0..1 through the sunset. Lit band collapses toward the middle,
// dims, and slides green -> amber -> red. Edges run a little ahead in color.
void renderSunset(float q, float maxBright) {
  const float c = (NUM_LEDS - 1) * 0.5f;
  const uint8_t n = sizeof(SUNSET_PAL) / sizeof(SUNSET_PAL[0]);
  float halfWidth = 1.15f - 1.05f * q;                       // full strip -> small ember
  float bright = maxBright * (1.0f - q);
  for (uint16_t i = 0; i < (NUM_LEDS + 1) / 2; i++) {
    float d = (c - i) / c;
    float mask = clamp01((halfWidth - d) / 0.15f);           // soft edge
    float t = clamp01(q * 1.1f + 0.25f * d);
    setPair(i, samplePalette(SUNSET_PAL, n, t), bright * mask);
  }
}

// ---------------- Night light ----------------
// Works directly in PWM counts, not through gammaLut. At the bottom of an
// 8-bit channel the gamma LUT rounds a dim purple to (0,0,1) or (0,0,0): blue
// reaches its first count before red does. So a lit pixel here is either off
// or at least NL_FLOOR (1,0,2), and below that floor the light is dimmed by
// lighting fewer pixels, never by dropping the red channel.
const float   NL_RED_PER_BLUE = 0.99f;   // purple mix, in counts. Too blue: raise it. Too pink: lower it
const uint8_t NL_FLOOR_R      = 1;
const uint8_t NL_FLOOR_B      = 2;       // (1,0,2): dimmest step that still reads purple
const float   NL_MIN_B        = 0.5f;    // blue counts per glint at BRIGHT 1% (below the floor: only ~1/4 of glints show)
const float   NL_MAX_B        = 96.0f;   // blue counts per glint at BRIGHT 100%
const float   NL_JITTER       = 0.30f;   // each glint wanders +/-30% of the spacing
const uint8_t NL_MAX_SPARKS   = 4;

struct Spark { uint8_t pos; uint8_t green; uint16_t ageMs; uint16_t lifeMs; float peak; };

struct NightLayer {
  float    env = 0.0f;                   // 0..1 fade envelope of the whole layer
  float    fadeInS = NL_FADE_IN_S, fadeOutS = 1.0f;
  float    drift = 0.0f;                 // whole pattern creeps along the strip, px
  uint32_t jitN = 0; float jitF = 0.0f;  // lattice time for position wander
  uint32_t envN = 0; float envF = 0.0f;  // lattice time for brightness wander
  Spark    sp[NL_MAX_SPARKS];
  uint8_t  nSp = 0;
} nl;

float nlAcc[NUM_LEDS];                   // blue-equivalent counts per pixel, this frame
uint32_t rngState = 0x12345678;

static inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dUL; x ^= x >> 15; x *= 0x846ca68bUL; x ^= x >> 16; return x;
}
static inline float hashU(uint32_t a, uint32_t b) {      // 0..1, fixed per (a, b)
  return (hash32(a * 0x9E3779B1UL ^ hash32(b + 0x632BE5ABUL)) >> 8) * (1.0f / 16777216.0f);
}
static inline uint32_t rnd() { rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5; return rngState; }
static inline float rndU() { return (rnd() >> 8) * (1.0f / 16777216.0f); }

// Smooth 1-D value noise, -1..1. Each glint gets its own phase offset so they
// don't all turn around on the same frame.
static float vnoise(uint32_t k, uint32_t seed, uint32_t n, float f) {
  float u = f + hashU(k, seed);
  if (u >= 1.0f) { u -= 1.0f; n++; }
  float a = hashU(k ^ seed, n), b = hashU(k ^ seed, n + 1);
  return (a + (b - a) * smooth(u)) * 2.0f - 1.0f;
}

static void advanceLattice(uint32_t& n, float& f, float d) {
  f += d;
  while (f >= 1.0f) { f -= 1.0f; n++; }
}

// Blue counts per glint for a BRIGHT setting, on a 2.2 curve so the slider
// feels even: 1% -> 0.5 (below the floor), 10% -> 1.0, 25% -> 4.7, 50% -> 21, 100% -> 96.
float nlBaseBlue(uint8_t bright) {
  float x = (bright - 1) / 99.0f;
  return NL_MIN_B + (NL_MAX_B - NL_MIN_B) * powf(x, 2.2f);
}

bool nlVisible() { return nl.env > 0.0f || nl.nSp > 0; }

void nlCut() { nl.env = 0.0f; nl.nSp = 0; }      // LIGHTS OFF and manual commands: no fade

// Called once per frame. dt in seconds. Adds the layer on top of whatever the
// main mode already put in the strip buffer (black in NIGHT and IDLE, the
// start of the sunrise during the hand-off), saturating at 255.
void renderNightLayer(float dt) {
  bool on = (mode == Mode::Night);
  if (on) nl.env = nl.env + dt / nl.fadeInS;
  else    nl.env = nl.env - dt / nl.fadeOutS;
  nl.env = clamp01(nl.env);
  if (!nlVisible()) return;

  const float I     = cfg.nlIntensity / 100.0f;
  const float baseB = nlBaseBlue(cfg.nlBright);
  const float scale = nl.env * nl.env;           // perceptual fade

  // Motion speeds follow the SHIMMER slider. At 0 the pattern stands still.
  advanceLattice(nl.jitN, nl.jitF, 0.60f * I * dt);   // new wander target every ~4 s at 40%
  advanceLattice(nl.envN, nl.envF, 0.80f * I * dt);
  nl.drift += 0.25f * I * dt;                         // 0.1 px/s at 40%
  if (nl.drift >= NUM_LEDS) nl.drift -= NUM_LEDS;
  const float depth = 0.85f * I;                      // how far a glint dims while it wanders

  // Spacing: about every 3rd pixel at BRIGHT 1%, every 2nd at 100%.
  float sTarget = 3.0f - (cfg.nlBright - 1) / 99.0f;
  uint16_t nG = (uint16_t)(NUM_LEDS / sTarget + 0.5f);
  const float sp = (float)NUM_LEDS / nG;             // exact, so the wrap seam isn't uneven

  memset(nlAcc, 0, sizeof(nlAcc));
  for (uint16_t k = 0; k < nG; k++) {
    float x = (k + 0.5f) * sp + nl.drift + NL_JITTER * sp * vnoise(k, 0xA11CE, nl.jitN, nl.jitF);
    while (x >= NUM_LEDS) x -= NUM_LEDS;
    while (x < 0.0f)      x += NUM_LEDS;
    float e = 1.0f - depth * (0.5f + 0.5f * vnoise(k, 0xB0B, nl.envN, nl.envF));
    float v = baseB * e * scale;
    // Mostly one pixel per glint; crossfade only in a narrow zone as it hops.
    uint16_t p = (uint16_t)x;
    float w = smooth(((x - p) - 0.35f) / 0.30f);
    nlAcc[p] += v * (1.0f - w);
    nlAcc[(p + 1) % NUM_LEDS] += v * w;
  }

  // Sparkles: Poisson arrivals, rate tied to SHIMMER. 0.35/s at 100% (one every
  // ~3 s), one every ~7 s at 40%, none at 0. Only while the night light is on.
  if (on && I > 0.0f && nl.nSp < NL_MAX_SPARKS && rndU() < 0.35f * I * dt) {
    Spark& s = nl.sp[nl.nSp];
    uint8_t pos = rnd() % NUM_LEDS;
    for (uint8_t tries = 0; tries < 4 && nlAcc[pos] > 0.25f; tries++) pos = rnd() % NUM_LEDS;  // land in a gap
    s.pos = pos;
    s.green = rnd() & 1;
    s.ageMs = 0;
    s.lifeMs = 700 + rnd() % 700;
    // Green reads far brighter per count than red, so it gets fewer counts.
    s.peak = s.green ? constrain(2.0f + 0.6f * baseB, 2.0f, 80.0f)
                     : constrain(4.0f + 1.0f * baseB, 4.0f, 120.0f);
    nl.nSp++;
  }

  // Quantize. Each pixel has fixed dither thresholds, so a pixel that isn't
  // moving never flickers between counts, and the pattern's own motion hides
  // the dither. Below the floor a pixel is either off or exactly (1,0,2).
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    float v = nlAcc[i];
    if (v <= 0.0f) continue;
    float t1 = hashU(i, 0x51), t2 = hashU(i, 0x52);
    uint8_t r, b;
    if (v < NL_FLOOR_B) {
      if (v / NL_FLOOR_B <= t1) continue;
      r = NL_FLOOR_R; b = NL_FLOOR_B;
    } else {
      float fb = v + t1;           if (fb > 255.0f) fb = 255.0f;
      float fr = v * NL_RED_PER_BLUE + t2; if (fr > 255.0f) fr = 255.0f;
      b = (uint8_t)fb;
      r = (uint8_t)fr; if (r < NL_FLOOR_R) r = NL_FLOOR_R;
    }
    RgbColor c = strip.GetPixelColor(i);
    c.R = min(255, c.R + r);
    c.B = min(255, c.B + b);
    strip.SetPixelColor(i, c);
  }

  // Sparkles replace the purple on their pixel: fast rise, squared decay.
  uint16_t dms = (uint16_t)(dt * 1000.0f);
  for (uint8_t j = 0; j < nl.nSp; ) {
    Spark& s = nl.sp[j];
    const uint16_t rise = 90;
    float lv = s.ageMs < rise ? s.peak * s.ageMs / rise
                              : s.peak * sq(1.0f - (float)(s.ageMs - rise) / (s.lifeMs - rise));
    uint8_t q = (uint8_t)(lv * scale + hashU(s.pos, 0x53));
    if (q > 0) {
      RgbColor c = strip.GetPixelColor(s.pos);
      if (mode == Mode::Idle || mode == Mode::Night) c = RgbColor(0);
      if (s.green) c.G = min(255, c.G + q); else c.R = min(255, c.R + q);
      strip.SetPixelColor(s.pos, c);
    }
    s.ageMs += dms;
    if (s.ageMs >= s.lifeMs) { nl.sp[j] = nl.sp[--nl.nSp]; continue; }
    j++;
  }
}

void renderFrame(float dt) {
  float p  = modeProgress();
  float sr = cfg.sunriseMaxPct / 100.0f;
  float ss = cfg.sunsetMaxPct / 100.0f;
  switch (mode) {
    case Mode::Sunrise:  renderSunrise(p, sr * (0.3f + 0.7f * p)); break;
    case Mode::Hold:     renderSunrise(1.0f, sr);                   break;
    case Mode::RampDown: renderSunrise(1.0f, sr * (1.0f - p));      break;
    case Mode::Sunset:   renderSunset(p, ss);                       break;
    default:             strip.ClearTo(RgbColor(0));                break;  // IDLE, NIGHT
  }
  renderNightLayer(dt);
}

// ---------------- Schedule ----------------
bool timeValid() { return time(nullptr) > 1700000000; }

int  lastTriggerKey = -1;     // which day's sunrise already ran
bool catchUpChecked = false;  // first valid-time check after boot

// Seconds since today's (or yesterday's) START time, 0..86399, wall clock, so DST is handled.
long secondsSinceStart(time_t now) {
  struct tm lt;
  localtime_r(&now, &lt);
  long nowSec   = lt.tm_hour * 3600L + lt.tm_min * 60L + lt.tm_sec;
  long startSec = cfg.startHour * 3600L + cfg.startMin * 60L;
  long since    = nowSec - startSec;
  if (since < 0) since += 86400L;
  return since;
}

// Night light runs from the end of a sunset to the next START time.
void startNight(bool test, uint32_t backdateMs, bool resumed) {
  setMode(Mode::Night, backdateMs);
  nightTest = test;
  nl.fadeInS = test ? NL_TEST_IN_S : NL_FADE_IN_S;
  if (test) { nightDurMs = NL_TEST_MS; return; }
  if (timeValid()) {
    time_t now = time(nullptr);
    long until = 86400L - secondsSinceStart(now);
    nightDurMs = modeElapsed() + (uint32_t)until * 1000UL;
    if (!resumed) storeNightEnd((uint32_t)(now + until));   // one flash write per night
  } else {
    nightDurMs = NL_NOCLOCK_MS;
  }
}

void endNight(float fadeS) {
  nl.fadeOutS = fadeS;
  nightTest = false;
  setMode(Mode::Idle);
}

// Any manual command that stops a real night light: no comeback after a reboot.
void forgetNight() {
  if (loadNightEnd() != 0) storeNightEnd(0);
}

void checkSchedule() {
  if (!timeValid()) return;

  time_t now = time(nullptr);
  long since = secondsSinceStart(now);

  // Normal trigger window is the first minute. Right after boot, widen it to
  // the whole sunrise + hold so a power blip mid-sunrise picks up where it was.
  long window = 60;
  bool firstCheck = !catchUpChecked;
  if (firstCheck) {
    window = (long)(cfg.sunriseMin + cfg.holdMin) * 60L;
    catchUpChecked = true;
  }

  if (cfg.enabled) {
    time_t startedAt = now - since;
    struct tm st;
    localtime_r(&startedAt, &st);
    int key = st.tm_year * 400 + st.tm_yday;

    if (since < window && key != lastTriggerKey) {
      lastTriggerKey = key;
      if (mode == Mode::Night) nl.fadeOutS = NL_TO_SUNRISE_S;  // purple fades into the sunrise's purple start
      else                     nlCut();
      nightTest = false;
      setMode(Mode::Sunrise, (uint32_t)since * 1000UL);
      advanceMode();                           // jumps to HOLD if we're past the ramp
    }
  }

  if (mode == Mode::Night && !nightTest) {
    if (since < 60) endNight(NL_END_S);        // START time with the alarm off: fade out over a minute
    else nightDurMs = modeElapsed() + (uint32_t)(86400L - since) * 1000UL;  // follows START edits, DST, late NTP
  }

  // Power came back in the night: pick the night light back up, but only if
  // the saved end time still matches the current START time.
  if (firstCheck && mode == Mode::Idle) {
    uint32_t endE = loadNightEnd();
    long until = 86400L - since;
    if (endE > (uint32_t)now + 60 && labs((long)(endE - (uint32_t)now) - until) < 120)
      startNight(false, 0, true);
  }
}

// ---------------- Web ----------------
const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>SUNRISE</title>
<style>
:root{--g:#33ff66;--dim:#1c8a3a;--lab:#2fd65e;--panel:#020d04}
*{box-sizing:border-box}
html,body{margin:0;background:#000}
body{color:var(--g);font:bold 17px/1.3 "Courier New",Courier,monospace;text-shadow:0 0 2px rgba(51,255,102,.4);
padding:10px 10px;min-height:100vh;min-height:100dvh;background:radial-gradient(ellipse at center,#031a08 0%,#000 75%)}
body::after{content:"";position:fixed;inset:0;pointer-events:none;z-index:9;
background:repeating-linear-gradient(to bottom,rgba(0,0,0,.12) 0,rgba(0,0,0,.12) 1px,transparent 1px,transparent 3px)}
.wrap{max-width:560px;margin:0 auto}
header{display:flex;justify-content:space-between;align-items:baseline;gap:8px;margin:0 0 8px}
h1{font-size:20px;letter-spacing:2px;margin:0;font-weight:normal;white-space:nowrap}
.sub{color:var(--lab);font-size:13px;font-weight:normal;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.cur{display:inline-block;width:.6em;height:1em;background:var(--g);vertical-align:-2px;margin-left:3px;animation:b 1s steps(1) infinite}
@keyframes b{50%{opacity:0}}
fieldset{border:1px solid var(--dim);background:var(--panel);margin:0 0 9px;padding:3px 8px 8px;min-width:0}
legend{padding:0 4px;letter-spacing:1px;font-size:15px}
label{display:block}
.row{display:flex;justify-content:space-between;align-items:center;gap:8px;margin:6px 0}
.row label{display:flex;align-items:center;gap:6px}
.g3{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:6px;margin:6px 0}
.c2{grid-column:span 2}
.k{display:flex;justify-content:space-between;font-size:14px;color:var(--lab);margin-bottom:2px;white-space:nowrap;overflow:hidden}
.k b{font-weight:normal;color:var(--g)}
.v{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
input[type=number],input[type=time]{width:100%;background:#000;color:var(--g);border:1px solid var(--dim);
font:inherit;padding:6px 6px;text-shadow:inherit;color-scheme:dark}
input[type=time]{width:136px}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:24px;margin:2px 0;padding:0;background:transparent;cursor:pointer;--f:0}
input[type=range]:focus{outline:none}
input[type=range]::-webkit-slider-runnable-track{height:12px;border:1px solid var(--g);box-shadow:0 0 4px rgba(51,255,102,.45);
background:linear-gradient(to right,var(--g) calc(10px + var(--f)*(100% - 20px)),#000 0)}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:20px;height:20px;margin-top:-5px;border-radius:50%;
background:var(--g);border:2px solid #000;box-shadow:0 0 6px var(--g)}
input[type=range]::-moz-range-track{height:12px;border:1px solid var(--g);background:#000;box-shadow:0 0 4px rgba(51,255,102,.45)}
input[type=range]::-moz-range-progress{height:10px;background:var(--g)}
input[type=range]::-moz-range-thumb{width:16px;height:16px;border-radius:50%;background:var(--g);border:2px solid #000;box-shadow:0 0 6px var(--g)}
input[type=checkbox]{-webkit-appearance:none;appearance:none;margin:0;flex:none;width:24px;height:24px;background:#000;border:1px solid var(--g);box-shadow:0 0 4px rgba(51,255,102,.55);cursor:pointer;display:grid;place-content:center}
input[type=checkbox]:checked{background:var(--g);box-shadow:0 0 8px var(--g)}
input[type=checkbox]:checked::after{content:"";width:6px;height:12px;border:solid #000;border-width:0 3px 3px 0;transform:translateY(-1px) rotate(45deg)}
button{background:#000;color:var(--g);border:1px solid var(--g);font:inherit;font-size:13px;letter-spacing:.3px;padding:12px 2px;
cursor:pointer;text-shadow:inherit;white-space:nowrap}
button:hover,button:active{background:var(--g);color:#000;text-shadow:none}
.btns{display:flex;gap:6px}.btns button{flex:1;min-width:0}
.save{font-size:15px;letter-spacing:1px}
.prim{margin:0 0 9px}.prim button{padding:12px 2px}
button.on{background:var(--g);color:#000;text-shadow:none;box-shadow:0 0 8px var(--g)}
.g2{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;margin:2px 0 0}
legend label{display:flex;align-items:center;gap:6px}
.bar{border:1px solid var(--dim);height:10px;margin-top:6px}
.fill{height:100%;width:0;background:var(--g);box-shadow:0 0 8px var(--g);transition:width .5s}
#msg{position:fixed;left:50%;bottom:14px;transform:translateX(-50%);z-index:10;background:#000;border:1px solid var(--g);
padding:8px 14px;font-size:16px;white-space:nowrap;box-shadow:0 0 10px rgba(51,255,102,.5);display:none}
#msg.show{display:block}
.foot{color:var(--lab);font-size:13px;font-weight:normal}.foot a{color:var(--lab)}
@media(max-width:350px){body{padding:8px 6px;font-size:16px}.k{font-size:12px}button{font-size:12px;letter-spacing:0}fieldset{padding:3px 6px 8px}}
</style></head><body><div class="wrap">
<header><h1>&gt; SUNRISE_CTRL<span class="cur"></span></h1><div class="sub" id="host">sunrise.local</div></header>
<fieldset><legend>STATUS</legend>
<div class="g3">
<div><div class="k">CLOCK</div><div class="v" id="clock">--:--:--</div></div>
<div><div class="k">MODE</div><div class="v" id="mode">----</div></div>
<div><div class="k">REMAINING</div><div class="v" id="remain">--</div></div>
</div>
<div class="bar"><div class="fill" id="fill"></div></div>
</fieldset>
<form id="f">
<fieldset><legend>SUNRISE</legend>
<div class="row"><label for="enabled"><input type="checkbox" id="enabled" name="enabled">ALARM</label><label for="start">START<input type="time" id="start" name="start" required></label></div>
<div class="g3"><label><span class="k">RISE MIN</span><input type="number" id="sunriseMin" name="sunriseMin" min="1" max="180"></label>
<label><span class="k">HOLD MIN</span><input type="number" id="holdMin" name="holdMin" min="0" max="240"></label>
<label><span class="k">RAMP MIN</span><input type="number" id="rampDownMin" name="rampDownMin" min="0" max="120"></label>
</div>
<label><span class="k">MAX BRIGHT<b id="sunriseMaxV"></b></span><input type="range" id="sunriseMax" name="sunriseMax" min="1" max="100"></label>
</fieldset>
<fieldset><legend>SUNSET</legend>
<div class="g3">
<label><span class="k">SET MIN</span><input type="number" id="sunsetMin" name="sunsetMin" min="1" max="180"></label>
<label class="c2"><span class="k">MAX BRIGHT<b id="sunsetMaxV"></b></span><input type="range" id="sunsetMax" name="sunsetMax" min="1" max="100"></label>
</div>
</fieldset>
<fieldset><legend><label for="nlEnabled"><input type="checkbox" id="nlEnabled" name="nlEnabled">NIGHT LIGHT</label></legend>
<div class="g2">
<label><span class="k">SHIMMER<b id="nlIntensityV"></b></span><input type="range" id="nlIntensity" name="nlIntensity" min="0" max="100"></label>
<label><span class="k">BRIGHT<b id="nlBrightV"></b></span><input type="range" id="nlBright" name="nlBright" min="1" max="100"></label>
</div>
</fieldset>
<fieldset><legend>MDNS Hostname</legend>
<div class="row"><label for="mdnsHostname">MDNS Hostname</label><input type="text" id="mdnsHostname" name="mdnsHostname" minlength="1" maxlength="32" style="width:100%;background:#000;color:var(--g);border:1px solid var(--dim);font:inherit;padding:6px 6px;text-shadow:inherit;color-scheme:dark"></div>
</fieldset>
<div class="btns prim">
<button type="submit" class="save">SAVE SETTINGS</button>
<button type="button" class="save" onclick="act('sunset')">START SUNSET</button>
</div>
</form>
<div id="msg"></div>
<fieldset><legend>TEST / OFF</legend><div class="btns">
<button onclick="act('sunrise')">TEST SUNRISE</button>
<button id="nt" onclick="nightTest()">TEST NIGHT</button>
<button onclick="act('off')">LIGHTS OFF</button>
</div></fieldset>
<div class="foot"><span id="net"></span> // <a href="#" onclick="wifiReset();return false">RESET WIFI</a></div>
</div>
<script>
const $=id=>document.getElementById(id);
let loaded=false,curMode='';
const SL=['sunriseMax','sunsetMax','nlIntensity','nlBright'];
function fmt(s){if(!s||s<=0)return'--';if(s>=3600){const h=Math.floor(s/3600);return h+'h '+String(Math.floor(s%3600/60)).padStart(2,'0')+'m';}const m=Math.floor(s/60),x=s%60;return m+'m '+String(x).padStart(2,'0')+'s';}
let mt;function msg(t){const e=$('msg');e.textContent='> '+t;e.className='show';clearTimeout(mt);mt=setTimeout(()=>e.className='',3000);}
const nlOn=()=>curMode.startsWith('NI');
let pvT=0,pvQ=false;
function nlVals(){return{nlIntensity:$('nlIntensity').value,nlBright:$('nlBright').value};}
function preview(){if(!nlOn())return;if(pvT){pvQ=true;return;}
 fetch('/api/preview',{method:'POST',body:new URLSearchParams(nlVals())}).catch(()=>{});
 pvT=setTimeout(()=>{pvT=0;if(pvQ){pvQ=false;preview();}},150);}
function fillR(k){const e=$(k);e.style.setProperty('--f',(e.value-e.min)/(e.max-e.min));}
SL.forEach(k=>{fillR(k);$(k).addEventListener('input',()=>{$(k+'V').textContent=$(k).value+'%';fillR(k);if(k.startsWith('nl'))preview();});});
function showNt(){const b=$('nt'),t=curMode==='NITE TEST';b.classList.toggle('on',t);b.textContent=t?'STOP TEST':'TEST NIGHT';}
async function poll(){
 try{const s=await (await fetch('/api/state')).json();
  $('clock').textContent=s.timeValid?s.clock:'NTP WAIT';
  $('mode').textContent=s.mode;curMode=s.mode;showNt();$('remain').textContent=fmt(s.remainSec);
  $('fill').style.width=s.progress+'%';
  $('host').textContent=s.host;$('net').textContent=s.ip+' // RSSI '+s.rssi+' dBm';
  if(!loaded){loaded=true;$('enabled').checked=s.enabled;$('nlEnabled').checked=s.nlEnabled;$('start').value=s.start;
   ['sunriseMin','holdMin','rampDownMin','sunsetMin'].concat(SL).forEach(k=>$(k).value=s[k]);
   SL.forEach(k=>{$(k+'V').textContent=s[k]+'%';fillR(k);});}
 }catch(e){$('mode').textContent='LINK LOST';}
}
$('f').addEventListener('submit',async e=>{e.preventDefault();
 try{const r=await fetch('/api/settings',{method:'POST',body:new URLSearchParams(new FormData($('f')))});
  msg(r.ok?'SETTINGS SAVED':'SAVE FAILED');}catch(x){msg('SAVE FAILED');}poll();});
async function act(a,pw,extra){
 try{const b=Object.assign({do:a},extra||{});if(pw!==undefined)b.pw=pw;
  const r=await fetch('/api/action',{method:'POST',body:new URLSearchParams(b)});
  let j={};try{j=await r.json();}catch(e){}
  msg(r.ok?(j.msg||a.toUpperCase()+' OK'):(r.status==403?'WRONG PASSWORD':'COMMAND FAILED'));}catch(x){msg('COMMAND FAILED');}poll();}
function nightTest(){act('nighttest',undefined,nlVals());}
function wifiReset(){const p=prompt('Forget WiFi and reboot into setup mode?\nOTA password:');if(p)act('wifireset',p);}
poll();setInterval(poll,2000);
</script></body></html>)HTML";

void handleState() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  char clock[9] = "--:--:--";
  if (timeValid()) strftime(clock, sizeof(clock), "%H:%M:%S", &lt);

  uint32_t remain = 0;
  int progress = 0;
  if (mode != Mode::Idle) {
    uint32_t dur = modeDurationMs(mode), el = modeElapsed();
    remain   = el < dur ? (dur - el) / 1000UL : 0;
    progress = (int)(modeProgress() * 100.0f + 0.5f);
  }

  String ip = WiFi.localIP().toString();
  char buf[640];
  snprintf(buf, sizeof(buf),
    "{\"clock\":\"%s\",\"timeValid\":%s,\"mode\":\"%s\",\"progress\":%d,\"remainSec\":%lu,"
    "\"enabled\":%s,\"start\":\"%02u:%02u\",\"sunriseMin\":%u,\"holdMin\":%u,"
    "\"rampDownMin\":%u,\"sunriseMax\":%u,\"sunsetMax\":%u,\"sunsetMin\":%u,"
    "\"nlEnabled\":%s,\"nlIntensity\":%u,\"nlBright\":%u,\"frameUs\":%lu,"
    "\"ip\":\"%s\",\"host\":\"%s.local\",\"rssi\":%d}",
    clock, timeValid() ? "true" : "false", modeName(mode), progress, (unsigned long)remain,
    cfg.enabled ? "true" : "false", cfg.startHour, cfg.startMin,
    cfg.sunriseMin, cfg.holdMin, cfg.rampDownMin, cfg.sunriseMaxPct, cfg.sunsetMaxPct, cfg.sunsetMin,
    cfg.nlEnabled ? "true" : "false", cfg.nlIntensity, cfg.nlBright, (unsigned long)frameUsMax,
    ip.c_str(), cfg.mdnsHostname, (int)WiFi.RSSI());
  server.send(200, "application/json", buf);
}

uint16_t argU(const char* name, long lo, long hi, uint16_t cur) {
  if (!server.hasArg(name)) return cur;
  return (uint16_t)constrain(server.arg(name).toInt(), lo, hi);
}

void handleSettings() {
  uint8_t oldH = cfg.startHour, oldM = cfg.startMin;

  if (server.hasArg("start")) {                // "HH:MM" from <input type=time>
    String s = server.arg("start");
    int colon = s.indexOf(':');
    if (colon > 0) {
      cfg.startHour = constrain(s.substring(0, colon).toInt(), 0, 23);
      cfg.startMin  = constrain(s.substring(colon + 1).toInt(), 0, 59);
    }
  }
  cfg.enabled       = server.hasArg("enabled") ? 1 : 0;   // unchecked boxes aren't posted
  cfg.sunriseMin    = argU("sunriseMin", 1, 180, cfg.sunriseMin);
  cfg.holdMin       = argU("holdMin", 0, 240, cfg.holdMin);
  cfg.rampDownMin   = argU("rampDownMin", 0, 120, cfg.rampDownMin);
  cfg.sunriseMaxPct = argU("sunriseMax", 1, 100, cfg.sunriseMaxPct);
  cfg.sunsetMaxPct  = argU("sunsetMax", 1, 100, cfg.sunsetMaxPct);
  cfg.sunsetMin     = argU("sunsetMin", 1, 180, cfg.sunsetMin);
  cfg.nlEnabled     = server.hasArg("nlEnabled") ? 1 : 0;
  cfg.nlIntensity   = argU("nlIntensity", 0, 100, cfg.nlIntensity);
  cfg.nlBright      = argU("nlBright", 1, 100, cfg.nlBright);
  cfg.mdnsHostname[0] = '\0';
  if (server.hasArg("mdnsHostname")) {
    String h = server.arg("mdnsHostname");
    if (h.length() > 0) {
      h.toCharArray(cfg.mdnsHostname, sizeof(cfg.mdnsHostname));
    }
  }

  // Save settings to EEPROM
  saveSettings();

  // Re-apply hostname and restart mDNS dynamically (without reboot)
  WiFi.hostname(cfg.mdnsHostname);
  ArduinoOTA.setHostname(cfg.mdnsHostname);

  // Force mDNS to re-register with the new hostname
  MDNS.begin(cfg.mdnsHostname);

  // New start time: allow it to fire today even if today's already ran (handy for testing).
  if (cfg.startHour != oldH || cfg.startMin != oldM) lastTriggerKey = -1;

  // NIGHT LIGHT unticked and saved while it's on: that's the "turn it off" toggle.
  if (!cfg.nlEnabled && mode == Mode::Night && !nightTest) {
    endNight(2.0f);
    forgetNight();
  }

  // NIGHT LIGHT ticked and saved while the strip is idle (for example after
  // the sunset has already finished): start it now, running until the next
  // START time, instead of waiting for the next sunset.
  if (cfg.nlEnabled && mode == Mode::Idle && !nightTest && timeValid())
    startNight(false);

  saveSettings();
  server.send(200, "application/json", "{\"ok\":true}");
}

// Night light sliders while it's showing: applied live, in RAM only. SAVE
// writes them to flash, so dragging a slider doesn't wear the flash.
void applyNlArgs() {
  cfg.nlIntensity = argU("nlIntensity", 0, 100, cfg.nlIntensity);
  cfg.nlBright    = argU("nlBright", 1, 100, cfg.nlBright);
}

void handlePreview() {
  applyNlArgs();
  server.send(200, "application/json", "{\"ok\":true}");
}

// Manual commands cut the night light at once (no fade) and it stays off for
// the rest of the night.
void stopNightNow() {
  if (mode == Mode::Night && !nightTest) forgetNight();
  nightTest = false;
  nlCut();
}

void handleAction() {
  String a = server.arg("do");
  const char* reply = nullptr;
  if      (a == "sunset")  { stopNightNow(); setMode(Mode::Sunset); }
  else if (a == "sunrise") { stopNightNow(); setMode(Mode::Sunrise); }
  else if (a == "off")     { stopNightNow(); setMode(Mode::Idle); }
  else if (a == "nighttest") {
    if (mode == Mode::Night && nightTest) {          // second press: stop the test
      endNight(1.0f);
      reply = "NIGHT TEST OFF";
    } else if (mode == Mode::Night) {                // the real one is already on
      applyNlArgs();
      reply = "NIGHT LIGHT IS ON";
    } else {
      applyNlArgs();
      nl.nSp = 0;
      if (mode != Mode::Idle) nlCut();
      startNight(true);
      reply = "NIGHT TEST 10 MIN";
    }
  }
  else if (a == "wifireset") {
    // Wiping the credentials strands the light in setup-AP mode, so it takes
    // the OTA password. Anyone on the LAN could do it before.
    if (server.arg("pw") != OTA_PASSWORD) {
      server.send(403, "application/json", "{\"ok\":false}");
      return;
    }
    server.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    WiFiManager wm;
    wm.resetSettings();
    ESP.restart();
    return;
  } else {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  if (reply) {
    char b[64];
    snprintf(b, sizeof(b), "{\"ok\":true,\"msg\":\"%s\"}", reply);
    server.send(200, "application/json", b);
  } else {
    server.send(200, "application/json", "{\"ok\":true}");
  }
}

// Dim green center pixels while the setup portal is open, so you know it's waiting.
void portalIndicator(WiFiManager*) {
  strip.ClearTo(RgbColor(0));
  strip.SetPixelColor(NUM_LEDS / 2 - 1, RgbColor(0, 24, 0));
  strip.SetPixelColor(NUM_LEDS / 2, RgbColor(0, 24, 0));
  strip.Show();
}

// Send an all-off frame and wait until it has gone out.
void blankStrip() {
  strip.ClearTo(RgbColor(0));
  while (!strip.CanShow()) yield();
  strip.Show();
  while (!strip.CanShow()) yield();
}

// ---------------- Setup / loop ----------------
void setup() {
#if LED_DATA_GPIO == 3
  // TX only: GPIO3 belongs to the I2S output. Serial.begin() must come
  // before strip.Begin(), or it takes the pin back and no data goes out.
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
#else
  Serial.begin(115200);
#endif
  Serial.println(F("\nSunrise Light v2"));

  // Blank the strip before anything slow (WiFi can block for seconds).
  // Twice, 50 ms apart: on a shared 12 V supply the strip's chips can still
  // be coming out of undervoltage when the first frame goes out.
  strip.Begin();
  blankStrip();
  delay(50);
  blankStrip();
  buildGamma();

  static_assert(sizeof(Settings) <= (size_t)NIGHT_REC_ADDR, "settings overlap the night record");
  EEPROM.begin(EEPROM_BYTES);
  loadSettings();
  rngState = RANDOM_REG32 | 1;                 // hardware RNG seeds the sparkles

  WiFi.mode(WIFI_STA);
  WiFi.hostname(cfg.mdnsHostname);
  WiFiManager wm;
  wm.setHostname(cfg.mdnsHostname);
  wm.setConfigPortalTimeout(300);              // 5 min, then reboot and retry
  wm.setAPCallback(portalIndicator);
  if (!wm.autoConnect(SETUP_AP_NAME)) {
    ESP.restart();
  }
  blankStrip();
  Serial.print(F("IP: "));
  Serial.println(WiFi.localIP());

  configTime(TZ_STRING, NTP_1, NTP_2);

  ArduinoOTA.setHostname(cfg.mdnsHostname);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    otaActive = true;
    blankStrip();
  });
  ArduinoOTA.onError([](ota_error_t) { otaActive = false; });
  ArduinoOTA.begin();                          // also starts mDNS as cfg.mdnsHostname.local
  MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE); });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/action", HTTP_POST, handleAction);
  server.on("/api/preview", HTTP_POST, handlePreview);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  httpUpdater.setup(&server, "/update", UPDATE_USER, OTA_PASSWORD);  // browser OTA page
  server.begin();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  MDNS.update();
  if (otaActive) return;

  static uint32_t lastSched = 0;
  if (millis() - lastSched >= 1000) {
    lastSched = millis();
    checkSchedule();
  }

  static uint32_t lastFrame = 0;
  if (millis() - lastFrame >= FRAME_MS && strip.CanShow()) {
    uint32_t nowMs = millis();
    float dt = (nowMs - lastFrame) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;                  // after OTA or a long handler: no jump
    lastFrame = nowMs;
    uint32_t t0 = micros();
    advanceMode();
    renderFrame(dt);
    strip.Show();
    uint32_t us = micros() - t0;
    if (us > frameUsPeak) frameUsPeak = us;
  }

  // Worst render + Show time over the last second, in /api/state as frameUs.
  static uint32_t lastPeak = 0;
  if (millis() - lastPeak >= 1000) {
    lastPeak = millis();
    frameUsMax = frameUsPeak;
    frameUsPeak = 0;
  }
}
