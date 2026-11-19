// Bring-up entry point for the external-power sense (WP9 gate, §13, §20).
//   pio run -e test_power_sense -t upload -t monitor
// Ext. 5 V + rail -> 220 Ohm -> GPIO8 (ADC1_CH7) -> 2 x 220 Ohm -> GND.
// Nominal 3.33 V at 5.00 V, 0 V on battery. No hysteresis on the divider.
// Replaces src/main.c in this env only. No production driver is involved: this
// is the commissioning tool that decides BOARD_POWER_SENSE_VERIFIED.
//
// GPIO8 is read two ways at the same time:
//  - digital: CHANGE interrupt, every edge timestamped in us (esp_timer);
//  - analog: ADC1_CH7 sampled every 1 ms into an 8 s ring buffer.
// The ADC is configured once through the IDF driver and read with
// adc1_get_raw(). Arduino's analogRead*() is NOT used: each call does
// pinMode(pin, ANALOG), which disables the digital input buffer and leaves the
// interrupt deaf. The status line checks that both paths still agree
// ("isr" and "mismatch" counters); if they do not, press 'a' for
// digital-only mode and repeat the steps.
//
// The ADC clips at ~3050 mV (11 dB), so it cannot measure the 3.33 V high
// level. It only shows ">= VIH (clipped)". The exact high level needs a
// multimeter at GPIO8.
//
// Output: a MODE banner (NORMAL / LOW-POWER) each time the level has been
// stable for BOARD_POWER_SENSE_QUALIFY_MS, as the WP9 power task will decide,
// then indented result / timing / bounce lines once the pin has settled.
// A heartbeat repeats the mode every 30 s; problems print as "WARNING".
//
// Procedure:
//  1. External 5 V plugged in: banner "MODE: NORMAL". With a multimeter,
//     GPIO8 must read >= 2.475 V and <= 3.6 V (VDD + 0.3 V).
//  2. Unplug the external 5 V: banner "MODE: LOW-POWER", then result PASS.
//  3. Plug it back in: banner "MODE: NORMAL", then result PASS.
//  4. Repeat 2-3 at least 5 times, once with the sensors working (pH, TDS,
//     HC-SR04 loaded) and once by pulling the cable slowly / wiggling it.
//  5. Press 'd' for the table of all transitions and paste it back.
// Keys: v = edge details on/off, d = transition table, a = ADC on/off,
// r = reset counters, h = help.
// The monitor opens with DTR/RTS released (platformio.ini), so reconnecting
// after an unplug does not reset the board and the RAM history survives.
#include <Arduino.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>
#include <esp_timer.h>
#include <fcntl.h>
#include <soc/gpio_reg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "board_profile.h"
#include "hal/arduino_entry.h"

#define PS_PIN BOARD_PIN_EXTERNAL_POWER_SENSE
#define PS_ADC_CHANNEL ADC1_CHANNEL_7

// ESP32-S3 input thresholds at VDD = 3.3 V (architecture §5, [L14]).
#define PS_VIH_MV 2475u
#define PS_VIL_MV 825u
#define PS_VMAX_MV 3600u      // VDD + 0.3 V
#define PS_CLIP_MV 3050u      // same ceiling as hal/adc_channel.c
#define PS_ZERO_MV 50u        // "fully discharged"

#define PS_SAMPLE_PERIOD_MS 1
#define PS_SAMPLE_RING 8192u      // 8.2 s at 1 kHz, power of two
#define PS_EDGE_RING 1024u        // power of two
#define PS_PRE_TRIGGER_MS 300u    // ADC history kept before the trigger
#define PS_EDGE_QUIET_MS 300u     // no edge for this long = digital settled
#define PS_STABLE_WINDOW_MS 200u  // ADC span below PS_STABLE_SPAN_MV = settled
#define PS_STABLE_SPAN_MV 30u
#define PS_TRIGGER_DELTA_MV 200u  // ADC move that opens a transition
#define PS_TRANSITION_TIMEOUT_MS 7000u
#define PS_STATUS_PERIOD_MS 30000u  // heartbeat
#define PS_EDGES_LISTED 12u
#define PS_HISTORY 48u
#define PS_QUALIFY_US (BOARD_POWER_SENSE_QUALIFY_MS * 1000u)

typedef struct Sample {
  uint32_t tUs;    // esp_timer low 32 bits (wrap-safe for intervals < 71 min)
  uint16_t mv;     // 0 when the ADC is off
  uint8_t before;  // GPIO level just before the conversion
  uint8_t after;   // GPIO level just after the conversion
} Sample;

typedef struct Edge {
  uint32_t tUs;
  uint8_t level;  // level read at ISR entry
} Edge;

typedef struct Transition {
  bool rising;
  uint32_t tStartUs;      // first edge or ADC departure
  uint32_t edges;         // ISR edges in the transition
  uint32_t bounceUs;      // first edge -> last edge
  uint32_t minGapUs;      // shortest interval between two edges
  uint8_t endLevel;
  bool adcValid;
  uint16_t mvBefore, mvAfter;
  int32_t tVihMs, tVilMs, tEndMs;  // from ADC departure; -1 = not reached
  uint16_t flipMinMv, flipMaxMv;   // ADC value where the sampler saw GPIO flip
  bool timedOut;
  bool dip;     // ADC excursion that recovered with no GPIO edge
  bool outage;  // GPIO left its level and came back before the pin settled
} Transition;

static Sample s_samples[PS_SAMPLE_RING];
static volatile uint32_t s_sampleCount;
static volatile bool s_adcOn = true;

static Edge s_edges[PS_EDGE_RING];
static volatile uint32_t s_edgeHead;   // written by the ISR only
static volatile uint32_t s_edgeTotal;  // never wraps the ring
static volatile uint32_t s_lastEdgeUs;
static uint32_t s_edgeTail;
static bool s_verbose;

static esp_adc_cal_characteristics_t s_cal;

// Health of the shared-pin setup.
static volatile uint32_t s_samplerFlips;  // level changes seen by the sampler
static volatile uint32_t s_adcDisturbed;  // level differs before/after a conversion
static uint32_t s_mismatch;               // steady GPIO level disagrees with mV

static Transition s_history[PS_HISTORY];
static uint32_t s_historyCount;

static inline uint8_t gpioLevel(void) {
  return (uint8_t)((REG_READ(GPIO_IN_REG) >> PS_PIN) & 1u);
}

static void IRAM_ATTR onEdge(void) {
  const uint32_t head = s_edgeHead;
  s_edges[head & (PS_EDGE_RING - 1u)].tUs = (uint32_t)esp_timer_get_time();
  s_edges[head & (PS_EDGE_RING - 1u)].level = gpioLevel();
  s_lastEdgeUs = s_edges[head & (PS_EDGE_RING - 1u)].tUs;
  s_edgeHead = head + 1u;
  s_edgeTotal++;
}

static uint16_t adcReadMv(void) {
  const int raw = adc1_get_raw(PS_ADC_CHANNEL);
  if (raw < 0) return 0;
  return (uint16_t)esp_adc_cal_raw_to_voltage((uint32_t)raw, &s_cal);
}

// ---- sampler (core 0): 1 kHz ADC + GPIO level --------------------------------
static void samplerTask(void* arg) {
  (void)arg;
  TickType_t lastWake = xTaskGetTickCount();
  uint8_t prev = gpioLevel();
  for (;;) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(PS_SAMPLE_PERIOD_MS));
    Sample s;
    s.tUs = (uint32_t)esp_timer_get_time();
    s.before = gpioLevel();
    s.mv = s_adcOn ? adcReadMv() : 0;
    s.after = gpioLevel();
    if (s.before != s.after) s_adcDisturbed++;
    if (s.after != prev) s_samplerFlips++;
    prev = s.after;
    s_samples[s_sampleCount & (PS_SAMPLE_RING - 1u)] = s;
    s_sampleCount = s_sampleCount + 1u;
  }
}

// ---- analysis helpers (analyzer task only) ------------------------------------
static const Sample* sampleAt(uint32_t index) {
  return &s_samples[index & (PS_SAMPLE_RING - 1u)];
}

// Index of the first sample taken at or after tUs, searching back from 'last'.
static uint32_t sampleIndexAt(uint32_t tUs, uint32_t last) {
  uint32_t i = last;
  const uint32_t oldest = (last >= PS_SAMPLE_RING - 16u) ? last - (PS_SAMPLE_RING - 16u) : 0u;
  while (i > oldest && (int32_t)(sampleAt(i - 1u)->tUs - tUs) >= 0) --i;
  return i;
}

static void spanMv(uint32_t from, uint32_t to, uint16_t* lo, uint16_t* hi, uint32_t* mean) {
  uint16_t mn = 0xFFFF, mx = 0;
  uint64_t sum = 0;
  for (uint32_t i = from; i < to; ++i) {
    const uint16_t v = sampleAt(i)->mv;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
    sum += v;
  }
  const uint32_t n = to > from ? to - from : 1u;
  if (lo) *lo = (to > from) ? mn : 0;
  if (hi) *hi = mx;
  if (mean) *mean = (uint32_t)(sum / n);
}

static const char* bandName(uint32_t mv) {
  if (mv >= PS_VIH_MV) return "HIGH";
  if (mv <= PS_VIL_MV) return "LOW";
  return "UNDEFINED";
}

static int32_t msSince(uint32_t fromUs, uint32_t toUs) {
  return (int32_t)((toUs - fromUs) / 1000u);
}

// "mm:ss.s" since boot, for the banners (the monitor may not add timestamps).
static void printUptime(void) {
  const uint32_t ds = (uint32_t)(esp_timer_get_time() / 100000);
  printf("%02lu:%02lu.%lu", (unsigned long)(ds / 600u), (unsigned long)(ds / 10u % 60u),
         (unsigned long)(ds % 10u));
}

static void printModeBanner(uint8_t level) {
  printf("\n+--------------------------------------------------------+\n");
  printf("|  MODE: %-10s %-27s ", level ? "NORMAL" : "LOW-POWER",
         level ? "external 5 V present" : "external 5 V LOST");
  printUptime();
  printf("  |\n+--------------------------------------------------------+\n");
}

// "after N ms", or "not reached" for -1.
static void printAfter(int32_t ms) {
  if (ms < 0) {
    printf("not reached");
  } else {
    printf("after %ld ms", (long)ms);
  }
}

static void printSummary(const Transition* t) {
  bool pass = t->endLevel == (t->rising ? 1 : 0);
  if (t->adcValid) pass = pass && (t->rising ? t->mvAfter >= PS_VIH_MV : t->mvAfter <= PS_VIL_MV);

  printf("  result  ");
  if (!t->adcValid) {
    printf("ADC off");
  } else if (t->mvAfter >= PS_CLIP_MV) {
    printf("pin %u -> >=%u mV (clipped)", t->mvBefore, PS_CLIP_MV);
  } else if (t->mvBefore >= PS_CLIP_MV) {
    printf("pin >=%u -> %u mV", PS_CLIP_MV, t->mvAfter);
  } else {
    printf("pin %u -> %u mV", t->mvBefore, t->mvAfter);
  }
  printf(", GPIO %u   %s\n", t->endLevel, pass ? "PASS" : "FAIL");
  if (!pass) {
    printf("  WARNING: expected GPIO %u and %s\n", t->rising ? 1 : 0,
           t->rising ? "pin >= 2475 mV" : "pin <= 825 mV (rail back-fed or not switched off?)");
  }

  if (t->adcValid && t->rising) {
    printf("  timing  > %u mV ", PS_VIH_MV);
    printAfter(t->tVihMs);
    printf("\n");
  } else if (t->adcValid) {
    printf("  timing  < %u mV ", PS_VIH_MV);
    printAfter(t->tVihMs);
    printf(", < %u mV ", PS_VIL_MV);
    printAfter(t->tVilMs);
    if (t->tEndMs >= 0) {
      printf(", ~0 V ");
      printAfter(t->tEndMs);
    } else {
      printf(", settled at %u mV", t->mvAfter);
    }
    printf("\n");
  }

  printf("  bounce  %lu edge(s) in ", (unsigned long)t->edges);
  if (t->bounceUs < 1000u) {
    printf("%lu us", (unsigned long)t->bounceUs);
  } else {
    printf("%lu.%lu ms", (unsigned long)(t->bounceUs / 1000u),
           (unsigned long)(t->bounceUs % 1000u / 100u));
  }
  if (t->bounceUs >= PS_QUALIFY_US) {
    printf("\n  WARNING: longer than the %lu ms qualify window\n",
           (unsigned long)BOARD_POWER_SENSE_QUALIFY_MS);
  } else {
    printf(" -> filtered by the %lu ms qualify window\n",
           (unsigned long)BOARD_POWER_SENSE_QUALIFY_MS);
  }
  if (t->timedOut) printf("  WARNING: pin did not settle within 7 s\n");
}

// Analyse samples [first, last) and the edges collected for this transition.
static void reportTransition(Transition* t, const Edge* edges, uint32_t nListed, uint32_t first,
                             uint32_t last) {
  t->adcValid = s_adcOn;
  t->tVihMs = t->tVilMs = t->tEndMs = -1;
  t->flipMinMv = 0xFFFF;
  t->flipMaxMv = 0;

  uint32_t departUs = t->tStartUs;
  if (t->adcValid && last > first + 2u * PS_STABLE_WINDOW_MS) {
    uint32_t meanBefore, meanAfter;
    spanMv(first, first + PS_PRE_TRIGGER_MS / 2u, NULL, NULL, &meanBefore);
    spanMv(last - PS_STABLE_WINDOW_MS, last, NULL, NULL, &meanAfter);
    t->mvBefore = (uint16_t)meanBefore;
    t->mvAfter = (uint16_t)meanAfter;
    t->rising = meanAfter > meanBefore;

    // Departure: first sample more than 50 mV away from the level before.
    uint32_t depart = first;
    for (uint32_t i = first; i < last; ++i) {
      const int32_t d = (int32_t)sampleAt(i)->mv - (int32_t)meanBefore;
      if ((t->rising && d > 50) || (!t->rising && d < -50)) {
        depart = i;
        break;
      }
    }
    departUs = sampleAt(depart)->tUs;
    for (uint32_t i = depart; i < last; ++i) {
      const Sample* s = sampleAt(i);
      const int32_t ms = msSince(departUs, s->tUs);
      if (t->rising) {
        if (t->tVilMs < 0 && s->mv > PS_VIL_MV) t->tVilMs = ms;
        if (t->tVihMs < 0 && s->mv >= PS_VIH_MV) t->tVihMs = ms;
        if (t->tEndMs < 0 && s->mv >= PS_CLIP_MV) t->tEndMs = ms;
      } else {
        if (t->tVihMs < 0 && s->mv < PS_VIH_MV) t->tVihMs = ms;
        if (t->tVilMs < 0 && s->mv <= PS_VIL_MV) t->tVilMs = ms;
        if (t->tEndMs < 0 && s->mv <= PS_ZERO_MV) t->tEndMs = ms;
      }
      if (i > first && s->before != sampleAt(i - 1u)->after) {
        if (s->mv < t->flipMinMv) t->flipMinMv = s->mv;
        if (s->mv > t->flipMaxMv) t->flipMaxMv = s->mv;
      }
    }
  } else if (t->edges > 0) {
    t->rising = t->endLevel == 1;
  }

  // ADC moved >= PS_TRIGGER_DELTA_MV and came back without a GPIO edge, e.g.
  // contact bounce of the plug. Not a power transition: no verdict.
  if (t->adcValid && t->edges == 0 && !t->timedOut &&
      strcmp(bandName(t->mvBefore), bandName(t->mvAfter)) == 0) {
    uint16_t dipLo, dipHi;
    spanMv(first, last, &dipLo, &dipHi, NULL);
    printf("  note    pin dipped to %u mV and recovered, GPIO stayed %u -> ignored\n",
           t->endLevel ? dipLo : dipHi, t->endLevel);
    t->tVihMs = t->tVilMs = t->tEndMs = -1;
    t->dip = true;
    s_history[s_historyCount % PS_HISTORY] = *t;
    s_historyCount++;
    return;
  }

  // GPIO edges but the pin ended in the band it started in: power went away
  // and came back (or the reverse) before the pin settled. The mode banners
  // already showed both changes; report it as one short outage, no verdict.
  if (t->adcValid && t->edges > 0 && !t->timedOut &&
      strcmp(bandName(t->mvBefore), bandName(t->mvAfter)) == 0) {
    const bool wasHigh = t->mvBefore >= PS_VIH_MV;
    uint32_t extreme = wasHigh ? 0xFFFFu : 0u;
    int32_t leftMs = -1, backMs = -1;
    for (uint32_t i = sampleIndexAt(departUs, last); i < last; ++i) {
      const Sample* s = sampleAt(i);
      const bool inBand = wasHigh ? s->mv >= PS_VIH_MV : s->mv <= PS_VIL_MV;
      if (wasHigh ? s->mv < extreme : s->mv > extreme) extreme = s->mv;
      if (leftMs < 0 && !inBand) leftMs = msSince(departUs, s->tUs);
      if (leftMs >= 0 && backMs < 0 && inBand) backMs = msSince(departUs, s->tUs);
    }
    printf("  result  short %s: pin %s for %ld ms (down to %lu mV), GPIO back to %u   OK\n",
           wasHigh ? "outage" : "power blip", wasHigh ? "left HIGH" : "left LOW",
           (leftMs >= 0 && backMs >= 0) ? (long)(backMs - leftMs) : -1L,
           (unsigned long)extreme, t->endLevel);
    printf("  bounce  %lu edge(s) over both changes\n", (unsigned long)t->edges);
    t->outage = true;
    t->tEndMs = backMs;
    s_history[s_historyCount % PS_HISTORY] = *t;
    s_historyCount++;
    return;
  }

  printSummary(t);
  if (s_verbose) {
    if (t->adcValid && t->flipMaxMv > 0) {
      printf("  detail  GPIO flipped at %u-%u mV (1 ms sampler)\n", t->flipMinMv, t->flipMaxMv);
    }
    for (uint32_t i = 0; i < nListed; ++i) {
      const int32_t dt = (int32_t)(edges[i].tUs - departUs);
      const uint32_t si = sampleIndexAt(edges[i].tUs, last);
      printf("  detail  edge %2lu  %+9.3f ms  level %u", (unsigned long)(i + 1u),
             (double)dt / 1000.0, edges[i].level);
      if (t->adcValid && si < last) printf("  ~%u mV", sampleAt(si)->mv);
      printf("\n");
    }
    if (t->edges > nListed) {
      printf("  detail  ... %lu more edge(s), shortest gap %lu us\n",
             (unsigned long)(t->edges - nListed), (unsigned long)t->minGapUs);
    }
  }
  s_history[s_historyCount % PS_HISTORY] = *t;
  s_historyCount++;
}

static void printTable(void) {
  printf("\n%lu transition(s); last %u shown\n", (unsigned long)s_historyCount,
         (unsigned)(s_historyCount < PS_HISTORY ? s_historyCount : PS_HISTORY));
  printf("  #  dir   edges  bounce_ms  mV_before  mV_after  t_VIH  t_VIL  t_end  gpio  flip_mV\n");
  const uint32_t from = s_historyCount > PS_HISTORY ? s_historyCount - PS_HISTORY : 0u;
  for (uint32_t k = from; k < s_historyCount; ++k) {
    const Transition* t = &s_history[k % PS_HISTORY];
    printf("%3lu  %s  %5lu  %9.3f  %9u  %8u  %5ld  %5ld  %5ld  %4u  %u-%u%s\n",
           (unsigned long)(k + 1u), t->dip ? "DIP " : t->outage ? "OUT " : t->rising ? "RISE" : "FALL",
           (unsigned long)t->edges,
           (double)t->bounceUs / 1000.0, t->mvBefore, t->mvAfter, (long)t->tVihMs,
           (long)t->tVilMs, (long)t->tEndMs, t->endLevel,
           t->flipMaxMv ? t->flipMinMv : 0, t->flipMaxMv, t->timedOut ? "  TIMEOUT" : "");
  }
  printf("  (t_* in ms from ADC departure; -1 = not reached; mV = 0 when the ADC was off)\n\n");
}

static void printHelp(void) {
  printf("keys: v = edge details (%s), d = transition table, a = ADC (%s), r = reset, "
         "h = help\n", s_verbose ? "on" : "off", s_adcOn ? "on" : "off");
}

static void handleKeys(void) {
  char c;
  while (read(STDIN_FILENO, &c, 1) == 1) {
    switch (c) {
      case 'd': printTable(); break;
      case 'v':
        s_verbose = !s_verbose;
        printf("edge details %s\n", s_verbose ? "on" : "off");
        break;
      case 'a':
        s_adcOn = !s_adcOn;
        printf("ADC %s\n", s_adcOn ? "on (combined mode)" : "off (digital-only mode)");
        break;
      case 'r':
        s_historyCount = 0;
        s_mismatch = 0;
        s_adcDisturbed = 0;
        s_samplerFlips = 0;
        s_edgeTotal = 0;
        printf("counters reset\n");
        break;
      case 'h': printHelp(); break;
      default: break;
    }
  }
}

// ---- analyzer (core 1): groups edges + ADC into transitions -------------------
static void analyzerTask(void* arg) {
  (void)arg;
  bool active = false;
  Transition cur;
  Edge listed[PS_EDGES_LISTED];
  uint32_t nListed = 0;
  uint32_t firstSample = 0;
  uint32_t lastEdgeUs = 0;
  uint32_t settledMv = 0;
  bool settledKnown = false;
  uint32_t lastStatusUs = (uint32_t)esp_timer_get_time();
  uint32_t edgesAtStatus = 0, flipsAtStatus = 0;
  int mode = -1;  // qualified GPIO level: 1 = NORMAL, 0 = LOW-POWER, -1 = not yet

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10));
    handleKeys();
    const uint32_t nowUs = (uint32_t)esp_timer_get_time();
    const uint32_t last = s_sampleCount;
    if (last < PS_PRE_TRIGGER_MS + PS_STABLE_WINDOW_MS) continue;

    // Mode decision as WP9 will make it: act on the level only once no edge
    // has arrived for BOARD_POWER_SENSE_QUALIFY_MS.
    const uint8_t pinLevel = gpioLevel();
    if ((s_edgeTotal == 0 || nowUs - s_lastEdgeUs >= PS_QUALIFY_US) && (int)pinLevel != mode) {
      mode = pinLevel;
      printModeBanner(pinLevel);
      lastStatusUs = nowUs;
    }

    // Drain new edges.
    const uint32_t head = s_edgeHead;
    if (head - s_edgeTail > PS_EDGE_RING) s_edgeTail = head - PS_EDGE_RING;  // overrun
    while (s_edgeTail != head) {
      const Edge e = s_edges[s_edgeTail & (PS_EDGE_RING - 1u)];
      s_edgeTail++;
      if (!active) {
        memset(&cur, 0, sizeof(cur));
        cur.tStartUs = e.tUs;
        cur.minGapUs = UINT32_MAX;
        nListed = 0;
        firstSample = sampleIndexAt(e.tUs - PS_PRE_TRIGGER_MS * 1000u, last);
        active = true;
      } else if (cur.edges > 0 && e.tUs - lastEdgeUs < cur.minGapUs) {
        cur.minGapUs = e.tUs - lastEdgeUs;
      }
      if (cur.edges == 0) cur.tStartUs = e.tUs;
      if (nListed < PS_EDGES_LISTED) listed[nListed++] = e;
      cur.edges++;
      cur.bounceUs = e.tUs - cur.tStartUs;
      lastEdgeUs = e.tUs;
    }

    uint16_t lo, hi;
    uint32_t mean;
    spanMv(last - PS_STABLE_WINDOW_MS, last, &lo, &hi, &mean);
    const bool adcStable = !s_adcOn || (uint32_t)(hi - lo) < PS_STABLE_SPAN_MV;

    // ADC departure without any edge (e.g. rail drops but stays above VIH).
    if (!active && s_adcOn && settledKnown) {
      const uint32_t now = sampleAt(last - 1u)->mv;
      const uint32_t delta = now > settledMv ? now - settledMv : settledMv - now;
      if (delta >= PS_TRIGGER_DELTA_MV) {
        memset(&cur, 0, sizeof(cur));
        cur.tStartUs = sampleAt(last - 1u)->tUs;
        cur.minGapUs = UINT32_MAX;
        nListed = 0;
        firstSample = sampleIndexAt(cur.tStartUs - PS_PRE_TRIGGER_MS * 1000u, last);
        active = true;
      }
    }

    if (active) {
      const bool edgesQuiet = cur.edges == 0 || nowUs - lastEdgeUs >= PS_EDGE_QUIET_MS * 1000u;
      const bool longEnough = nowUs - cur.tStartUs >= PS_EDGE_QUIET_MS * 1000u;
      const bool timedOut = nowUs - cur.tStartUs >= PS_TRANSITION_TIMEOUT_MS * 1000u;
      if ((edgesQuiet && adcStable && longEnough) || timedOut) {
        cur.timedOut = timedOut && !(edgesQuiet && adcStable);
        cur.endLevel = gpioLevel();
        if (cur.minGapUs == UINT32_MAX) cur.minGapUs = 0;
        reportTransition(&cur, listed, nListed, firstSample, last);
        active = false;
        settledMv = mean;
        settledKnown = true;
        lastStatusUs = nowUs;
      }
      continue;
    }

    if (!settledKnown && adcStable) {
      settledMv = mean;
      settledKnown = true;
    }

    // Steady state: GPIO level must agree with the ADC band.
    const uint8_t level = gpioLevel();
    if (s_adcOn && ((level == 1 && mean <= PS_VIL_MV) || (level == 0 && mean >= PS_VIH_MV))) {
      s_mismatch++;
    }

    if (nowUs - lastStatusUs >= PS_STATUS_PERIOD_MS * 1000u) {
      lastStatusUs = nowUs;
      uint16_t sLo, sHi;
      uint32_t sMean;
      spanMv(last - 1000u, last, &sLo, &sHi, &sMean);
      const uint32_t edges = s_edgeTotal, flips = s_samplerFlips;
      // The sampler saw the level change but the ISR did not: interrupt is deaf.
      const bool isrOk = !(flips != flipsAtStatus && edges == edgesAtStatus);
      edgesAtStatus = edges;
      flipsAtStatus = flips;
      printf("  ");
      printUptime();
      printf("  still %s | pin ", mode == 1 ? "NORMAL" : mode == 0 ? "LOW-POWER" : "?");
      if (!s_adcOn) {
        printf("ADC off");
      } else if (sHi >= PS_CLIP_MV) {
        printf(">=%u mV", PS_CLIP_MV);
      } else {
        printf("%lu mV", (unsigned long)sMean);
      }
      printf(" | %lu transition(s)\n", (unsigned long)s_historyCount);
      if (!isrOk) printf("  WARNING: GPIO changed but the interrupt did not fire (ISR deaf)\n");
      if (s_mismatch > 0) {
        printf("  WARNING: GPIO level disagreed with the ADC for %lu check(s)\n",
               (unsigned long)s_mismatch);
        s_mismatch = 0;
      }
    }
  }
}

void setup(void) {
  delay(200);
  printf("\n=== power-sense test: GPIO%d (ADC1_CH7), qualify %lu ms ===\n", (int)PS_PIN,
         (unsigned long)BOARD_POWER_SENSE_QUALIFY_MS);

  gpio_num_t io = GPIO_NUM_NC;
  if (adc1_pad_get_io_num(PS_ADC_CHANNEL, &io) != ESP_OK || (int)io != PS_PIN) {
    printf("ADC1_CH7 is GPIO%d, not GPIO%d: fix PS_ADC_CHANNEL\n", (int)io, (int)PS_PIN);
    return;
  }

  // ADC first (this routes the pad to the RTC mux), then the digital input:
  // gpio_config() hands the pad back to the digital mux. The ADC still samples
  // the pad; the counters below confirm it on this board.
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(PS_ADC_CHANNEL, ADC_ATTEN_DB_12);
  const esp_adc_cal_value_t calType =
      esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, &s_cal);
  pinMode(PS_PIN, INPUT);  // no pull: the 440 Ohm bottom leg defines LOW

  // Self-check: one conversion must not change the digital level.
  const uint8_t l0 = gpioLevel();
  const uint16_t mv = adcReadMv();
  const uint8_t l1 = gpioLevel();
  const bool agree = (l1 == 1 && mv >= PS_VIH_MV) || (l1 == 0 && mv <= PS_VIL_MV);
  if (calType != ESP_ADC_CAL_VAL_EFUSE_TP_FIT && calType != ESP_ADC_CAL_VAL_EFUSE_TP) {
    printf("WARNING: no eFuse ADC calibration, mV values are approximate\n");
  }
  if (l0 == l1 && agree) {
    printf("self-check OK: ADC and GPIO agree (%u mV, GPIO %u)\n", mv, l1);
  } else if (l0 != l1) {
    printf("WARNING: self-check failed, an ADC conversion changes the GPIO level; press 'a'\n");
  } else {
    printf("WARNING: self-check: GPIO %u but ADC %u mV (pin in the undefined band?)\n", l1, mv);
  }

  attachInterrupt(digitalPinToInterrupt(PS_PIN), onEdge, CHANGE);
  fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);
  printHelp();

  xTaskCreatePinnedToCore(samplerTask, "ps_sample", 3072, NULL, 5, NULL, 0);
  xTaskCreatePinnedToCore(analyzerTask, "ps_analyze", 6144, NULL, 2, NULL, 1);
}

void loop(void) { vTaskDelay(portMAX_DELAY); }
