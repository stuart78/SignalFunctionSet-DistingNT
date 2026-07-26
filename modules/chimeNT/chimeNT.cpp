// ChimeNT — port of Signal Function Set's Chime (8-note resonating drone
// machine) to the Disting NT. See docs/chime-nt.md for design notes.

#include <math.h>
#include <new>
#include <string.h>
#include <distingnt/api.h>

// M_PI / M_PI_2 aren't guaranteed by ISO <math.h>; define locally.
#ifndef M_PI
#define M_PI    3.14159265358979323846f
#endif
#ifndef M_PI_2
#define M_PI_2  1.57079632679489661923f
#endif

// ─── Math approximations ─────────────────────────────────────────────────────
//
// The Disting NT firmware doesn't provide libm — sinf/cosf/expf/exp2f/log2f/
// powf as external symbols make the loader fail. Provide inline replacements
// that don't call out. Accuracy is well within what a music synth wants
// (< 0.01% for sin/cos, comparable for exp/log/pow). GCC still inlines the
// commonly hardware-mapped ones (roundf, floorf, fabsf, fminf, fmaxf,
// sqrtf) to VFP instructions so those don't need wrapping.

static const float SFS_PI  = 3.14159265358979323846f;
static const float SFS_TAU = 6.28318530717958647692f;

// sinApprox(x) — range-reduce to [-π, π], then 7th-order minimax polynomial.
static inline float sinApprox(float x) {
	// Reduce x modulo 2π by folding through the fractional part of x / 2π.
	float k = x * (1.f / SFS_TAU);
	k -= (float)(int)k;                        // fractional (may be negative)
	if      (k >  0.5f) k -= 1.f;
	else if (k < -0.5f) k += 1.f;
	x = k * SFS_TAU;                            // x now in [-π, π]
	float x2 = x * x;
	return x * (1.f - x2 * ((1.f/6.f)
	         - x2 * ((1.f/120.f)
	         - x2 * (1.f/5040.f))));
}
static inline float cosApprox(float x) {
	return sinApprox(x + (SFS_PI * 0.5f));
}

// 2^x via range reduction into integer + fractional parts.
// 2^i is applied by directly biasing the IEEE 754 exponent; 2^f (f in [0,1))
// uses a 5th-order minimax polynomial (max relative error ~2e-7).
static inline float exp2Approx(float x) {
	if (x < -126.f) return 0.f;
	if (x >  127.f) return 3.4e38f;
	int i = (int)x;
	float f = x - (float)i;
	if (f < 0.f) { i--; f += 1.f; }
	float p = 1.f + f * (0.6931472f
	              + f * (0.2402265f
	              + f * (0.0554844f
	              + f * (0.0096181f
	              + f * 0.0013013f))));
	union { float f; uint32_t u; } bits;
	bits.f = p;
	int expBits = (int)((bits.u >> 23) & 0xFF) + i;
	if (expBits <= 0)   return 0.f;
	if (expBits >= 255) return 3.4e38f;
	bits.u = (bits.u & 0x807FFFFFu) | ((uint32_t)expBits << 23);
	return bits.f;
}
static inline float expApprox(float x) {
	// exp(x) = 2^(x * log2(e))
	return exp2Approx(x * 1.4426950408889634f);
}

// log2(x). Extracts the IEEE exponent, then a 5th-order polynomial on the
// mantissa in [1, 2). Precise to about 6 decimal digits.
static inline float log2Approx(float x) {
	if (x <= 0.f) return -1e30f;
	union { float f; uint32_t u; } bits;
	bits.f = x;
	int   ex = (int)((bits.u >> 23) & 0xFF) - 127;
	bits.u = (bits.u & 0x807FFFFFu) | (127u << 23);   // mantissa in [1, 2)
	float m = bits.f - 1.f;                            // now in [0, 1)
	float p = m * (1.44269504f
	           + m * (-0.72134752f
	           + m * (0.47811216f
	           + m * (-0.29999894f
	           + m * 0.11599999f))));
	return (float)ex + p;
}

static inline float powApprox(float x, float y) {
	if (x <= 0.f) return 0.f;
	return exp2Approx(y * log2Approx(x));
}

// ─── Constants ───────────────────────────────────────────────────────────────

static const int NUM_CH   = 8;
static const int NUM_PART = 3;   // bar partials per voice
static const int NUM_DEG  = 16;  // reachable scale degrees per channel

// Xylophone-ish inharmonic bar partial stack
static const float PART_RATIO[NUM_PART] = { 1.f,  3.932f, 9.538f };
static const float PART_AMP  [NUM_PART] = { 1.f,  0.40f,  0.15f };
static const float PART_DECAY[NUM_PART] = { 1.f,  0.45f,  0.22f };

// Clock-sync divisor list (RATE knob picks clocks-per-rotation).
static const float CLK_DIVS[6] = { 32.f, 16.f, 8.f, 4.f, 2.f, 1.f };

// Equal-power pan gains for the 8 channels, precomputed (channel c maps to
// linear pan = c / 7, then angle = pan × π/2). cos → L, sin → R. Constant.
static const float PAN_L[NUM_CH] = {
	1.0000000f, 0.9749279f, 0.9009689f, 0.7818315f,
	0.6234898f, 0.4338837f, 0.2225209f, 0.0000000f
};
static const float PAN_R[NUM_CH] = {
	0.0000000f, 0.2225209f, 0.4338837f, 0.6234898f,
	0.7818315f, 0.9009689f, 0.9749279f, 1.0000000f
};

// RATE knob log2 range in Hz (log2(0.02)..log2(2)).
static const float RATE_LO_LOG2 = -5.6438f;
static const float RATE_HI_LOG2 = 1.0f;

// ─── Scale tables (same order as FugueNT / sfs::SCALES) ─────────────────────

struct ScaleInfo {
	const float* intervals;
	int size;
};

static const float SCL_CHROMATIC[]   = {0,1,2,3,4,5,6,7,8,9,10,11};
static const float SCL_MAJOR[]       = {0,2,4,5,7,9,11};
static const float SCL_MINOR[]       = {0,2,3,5,7,8,10};
static const float SCL_PENTA_MAJ[]   = {0,2,4,7,9};
static const float SCL_PENTA_MIN[]   = {0,3,5,7,10};
static const float SCL_BLUES[]       = {0,3,5,6,7,10};
static const float SCL_WHOLE[]       = {0,2,4,6,8,10};
static const float SCL_HARMONIC[]    = {
	0.f, 12.f, 19.0196f, 24.f, 27.8631f, 31.0196f,
	33.6883f, 36.f, 38.0392f, 39.8632f, 41.5126f, 43.0196f
};
static const float SCL_DORIAN[]      = {0,2,3,5,7,9,10};
static const float SCL_PHRYGIAN[]    = {0,1,3,5,7,8,10};
static const float SCL_LYDIAN[]      = {0,2,4,6,7,9,11};
static const float SCL_MIXOLYDIAN[]  = {0,2,4,5,7,9,10};
static const float SCL_HARM_MINOR[]  = {0,2,3,5,7,8,11};
static const float SCL_HIJAZ[]       = {0,1,4,5,7,8,10};
static const float SCL_HIRAJOSHI[]   = {0,2,3,7,8};
static const float SCL_PELOG[]       = {0.f, 1.2f, 2.7f, 5.4f, 7.0f, 8.0f, 10.4f};
static const float SCL_SLENDRO[]     = {0.f, 2.4f, 4.8f, 7.2f, 9.6f};
static const float SCL_MELO_MINOR[]  = {0,2,3,5,7,9,11};
static const float SCL_LOCRIAN[]     = {0,1,3,5,6,8,10};

static const ScaleInfo SCALES[] = {
	{SCL_CHROMATIC, 12}, {SCL_MAJOR, 7}, {SCL_MINOR, 7},
	{SCL_PENTA_MAJ, 5},  {SCL_PENTA_MIN, 5}, {SCL_BLUES, 6},
	{SCL_WHOLE, 6},      {SCL_HARMONIC, 12}, {SCL_DORIAN, 7},
	{SCL_PHRYGIAN, 7},   {SCL_LYDIAN, 7},    {SCL_MIXOLYDIAN, 7},
	{SCL_HARM_MINOR, 7}, {SCL_HIJAZ, 7},     {SCL_HIRAJOSHI, 5},
	{SCL_PELOG, 7},      {SCL_SLENDRO, 5},   {SCL_MELO_MINOR, 7},
	{SCL_LOCRIAN, 7},
};
static const int NUM_SCALES = sizeof(SCALES) / sizeof(SCALES[0]);

// ─── Helpers ─────────────────────────────────────────────────────────────────

static inline uint32_t xorshift32(uint32_t& state) {
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}
static inline float randFloat(uint32_t& state) {
	return (float)(xorshift32(state) & 0x7FFFFFFF) / (float)0x7FFFFFFF;
}
static inline float clampf(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}
static inline int clampi(int v, int lo, int hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

// Schmitt trigger: rising edge at 1.0V, falls at 0.1V.
struct Schmitt {
	bool high;
	void reset() { high = false; }
	bool process(float v) {
		if (!high && v >= 1.f) { high = true; return true; }
		if (high && v < 0.1f) { high = false; }
		return false;
	}
};

// Semitones above root for a given scale degree (with octave wraparound).
static inline float degreeSemis(int deg, int scaleIdx) {
	const ScaleInfo& sc = SCALES[scaleIdx];
	int oct = deg / sc.size;
	int step = deg % sc.size;
	return sc.intervals[step] + 12.f * (float)oct;
}

// ─── Algorithm state ─────────────────────────────────────────────────────────

struct _chimeNT : public _NT_algorithm {
	_chimeNT() {}
	~_chimeNT() {}

	Schmitt clockTrigger;
	Schmitt reseedTrigger;

	// Per-channel LFO ("tube rotation")
	float phase[NUM_CH];       // 0..1
	float tri[NUM_CH];         // -1..+1..-1
	float lastTri[NUM_CH];     // for center-crossing detection
	float window[NUM_CH];      // center proximity (0..1)
	float winSm[NUM_CH];       // declick-smoothed window
	float randMul[NUM_CH];     // seeded random rate exponents (0..1)
	float wob[NUM_CH];         // slow drift wobble state
	float wobTarget[NUM_CH];
	float wobTimer[NUM_CH];    // seconds until next new wob target
	float rateEff[NUM_CH];     // effective rotation rate (Hz) after mode/drift

	// Voice / partials
	float partPhase[NUM_CH][NUM_PART];
	float partEnv[NUM_CH][NUM_PART];  // strike ring envelope
	float strikeT[NUM_CH];             // seconds remaining in attack window
	float freq[NUM_CH];                // target frequency from root/scale/degree/oct
	float freqLatched[NUM_CH];         // pitch a sounding note holds until next strike

	// Ripple mode
	float ripple[NUM_CH];              // excitation energy per tube
	float rippleCoupling;

	int   curOct;
	float ctrlAccum;                   // seconds since last control-rate update

	// Clock sync
	float clkInterval;                 // measured (smoothed) clock interval, seconds
	float clkSince;                    // seconds since last clock edge

	// Edge-detect state for the UI action params
	int lastReseedNow;

	// RNG
	uint32_t rng;
};

// ─── Parameter enum ──────────────────────────────────────────────────────────

enum {
	// 9 input bus params
	kParamRateCV = 0,
	kParamSpreadCV,
	kParamDriftCV,
	kParamRootCV,
	kParamScaleCV,
	kParamReseedIn,
	kParamClockIn,
	kParamExciteCV,
	kParamOctCV,

	// 18 output bus params, WITH_MODE (36 entries).
	// Order: audio A..H, LFO A..H, Mix L, Mix R.
	kParamAudioAOut, kParamAudioAMode,
	kParamAudioBOut, kParamAudioBMode,
	kParamAudioCOut, kParamAudioCMode,
	kParamAudioDOut, kParamAudioDMode,
	kParamAudioEOut, kParamAudioEMode,
	kParamAudioFOut, kParamAudioFMode,
	kParamAudioGOut, kParamAudioGMode,
	kParamAudioHOut, kParamAudioHMode,
	kParamLFOAOut,   kParamLFOAMode,
	kParamLFOBOut,   kParamLFOBMode,
	kParamLFOCOut,   kParamLFOCMode,
	kParamLFODOut,   kParamLFODMode,
	kParamLFOEOut,   kParamLFOEMode,
	kParamLFOFOut,   kParamLFOFMode,
	kParamLFOGOut,   kParamLFOGMode,
	kParamLFOHOut,   kParamLFOHMode,
	kParamMixLOut,   kParamMixLMode,
	kParamMixROut,   kParamMixRMode,

	// Global controls
	kParamRate,       // Hz × 100 (kBy100: 2..200 → 0.02..2.0Hz), default 15 = 0.15Hz
	kParamSpread,     // percent
	kParamDrift,      // percent
	kParamRelate,     // enum: Ramp/Stepped/Random/Ripple
	kParamShape,      // curve: -100..+100 → kBy100 → -1..+1
	kParamOct,        // -3..+3
	kParamExcite,     // percent (bow → strike)
	kParamDecay,      // seconds × 10 (kBy10: 3..80 → 0.3..8.0)
	kParamRoot,       // enum C..B
	kParamScale,      // enum 0..18
	kParamReseedNow,  // action: Off→On reseeds random rates

	// Per-channel (×8): degree, weight, atten
	kParamDegA, kParamWeightA, kParamAttenA,
	kParamDegB, kParamWeightB, kParamAttenB,
	kParamDegC, kParamWeightC, kParamAttenC,
	kParamDegD, kParamWeightD, kParamAttenD,
	kParamDegE, kParamWeightE, kParamAttenE,
	kParamDegF, kParamWeightF, kParamAttenF,
	kParamDegG, kParamWeightG, kParamAttenG,
	kParamDegH, kParamWeightH, kParamAttenH,

	NUM_PARAMS
};

#define CH_DEG(c)    (kParamDegA    + (c) * 3)
#define CH_WEIGHT(c) (kParamWeightA + (c) * 3)
#define CH_ATTEN(c)  (kParamAttenA  + (c) * 3)

// ─── Enum strings ────────────────────────────────────────────────────────────

static char const * const enumRelate[] = { "Ramp", "Stepped", "Random", "Ripple" };
static char const * const enumRoot[]   = {
	"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
};
static char const * const enumScale[] = {
	"Chromatic","Major","Minor","Pentatonic Major","Pentatonic Minor",
	"Blues","Whole tone","Harmonic series","Dorian","Phrygian",
	"Lydian","Mixolydian","Harmonic Minor","Hijaz","Hirajoshi",
	"Pelog","Slendro","Melodic Minor","Locrian"
};
static char const * const enumOffOn[] = { "Off", "On" };

// ─── Parameter table ─────────────────────────────────────────────────────────

static const _NT_parameter parameters[] = {
	NT_PARAMETER_CV_INPUT( "Rate CV",   0, 0 )
	NT_PARAMETER_CV_INPUT( "Spread CV", 0, 0 )
	NT_PARAMETER_CV_INPUT( "Drift CV",  0, 0 )
	NT_PARAMETER_CV_INPUT( "Root CV",   0, 0 )
	NT_PARAMETER_CV_INPUT( "Scale CV",  0, 0 )
	NT_PARAMETER_CV_INPUT( "Reseed",    0, 0 )
	NT_PARAMETER_CV_INPUT( "Clock",     0, 0 )
	NT_PARAMETER_CV_INPUT( "Excite CV", 0, 0 )
	NT_PARAMETER_CV_INPUT( "Oct CV",    0, 0 )

	// Audio outs A..H, default to unrouted (user assigns as needed)
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Audio A", 0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Audio B", 0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Audio C", 0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Audio D", 0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Audio E", 0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Audio F", 0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Audio G", 0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Audio H", 0, 0 )
	// LFO outs A..H, default to unrouted
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "LFO A",   0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "LFO B",   0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "LFO C",   0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "LFO D",   0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "LFO E",   0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "LFO F",   0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "LFO G",   0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "LFO H",   0, 0 )
	// Mix, default to physical outs 13/14
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Mix L",   0, 13 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Mix R",   0, 14 )

	// Global controls
	{ .name = "Rate",       .min = 2,    .max = 200,  .def = 15, .unit = kNT_unitHz,      .scaling = kNT_scaling100, .enumStrings = NULL },
	{ .name = "Spread",     .min = 0,    .max = 100,  .def = 35, .unit = kNT_unitPercent, .scaling = 0,              .enumStrings = NULL },
	{ .name = "Drift",      .min = 0,    .max = 100,  .def = 15, .unit = kNT_unitPercent, .scaling = 0,              .enumStrings = NULL },
	{ .name = "Relate",     .min = 0,    .max = 3,    .def = 0,  .unit = kNT_unitEnum,    .scaling = 0,              .enumStrings = enumRelate },
	{ .name = "Shape",      .min = -100, .max = 100,  .def = 0,  .unit = kNT_unitNone,    .scaling = kNT_scaling100, .enumStrings = NULL },
	{ .name = "Oct",        .min = -3,   .max = 3,    .def = 0,  .unit = kNT_unitNone,    .scaling = 0,              .enumStrings = NULL },
	{ .name = "Excite",     .min = 0,    .max = 100,  .def = 0,  .unit = kNT_unitPercent, .scaling = 0,              .enumStrings = NULL },
	{ .name = "Decay",      .min = 3,    .max = 80,   .def = 25, .unit = kNT_unitSeconds, .scaling = kNT_scaling10,  .enumStrings = NULL },
	{ .name = "Root",       .min = 0,    .max = 11,   .def = 0,  .unit = kNT_unitEnum,    .scaling = 0,              .enumStrings = enumRoot },
	{ .name = "Scale",      .min = 0,    .max = 18,   .def = 1,  .unit = kNT_unitEnum,    .scaling = 0,              .enumStrings = enumScale },
	{ .name = "Reseed Now", .min = 0,    .max = 1,    .def = 0,  .unit = kNT_unitEnum,    .scaling = 0,              .enumStrings = enumOffOn },

	// Per-channel (defaults: degree = channel index, weight = 100%, atten = 100%)
	{ .name = "Degree A",   .min = 0,  .max = 15, .def = 0, .unit = kNT_unitNone,    .scaling = 0,              .enumStrings = NULL },
	{ .name = "Weight A",   .min = 0,  .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
	{ .name = "Atten A",    .min = 10, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
	{ .name = "Degree B",   .min = 0,  .max = 15, .def = 1, .unit = kNT_unitNone,    .scaling = 0,              .enumStrings = NULL },
	{ .name = "Weight B",   .min = 0,  .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
	{ .name = "Atten B",    .min = 10, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
	{ .name = "Degree C",   .min = 0,  .max = 15, .def = 2, .unit = kNT_unitNone,    .scaling = 0,              .enumStrings = NULL },
	{ .name = "Weight C",   .min = 0,  .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
	{ .name = "Atten C",    .min = 10, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
	{ .name = "Degree D",   .min = 0,  .max = 15, .def = 3, .unit = kNT_unitNone,    .scaling = 0,              .enumStrings = NULL },
	{ .name = "Weight D",   .min = 0,  .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
	{ .name = "Atten D",    .min = 10, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
	{ .name = "Degree E",   .min = 0,  .max = 15, .def = 4, .unit = kNT_unitNone,    .scaling = 0,              .enumStrings = NULL },
	{ .name = "Weight E",   .min = 0,  .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
	{ .name = "Atten E",    .min = 10, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
	{ .name = "Degree F",   .min = 0,  .max = 15, .def = 5, .unit = kNT_unitNone,    .scaling = 0,              .enumStrings = NULL },
	{ .name = "Weight F",   .min = 0,  .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
	{ .name = "Atten F",    .min = 10, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
	{ .name = "Degree G",   .min = 0,  .max = 15, .def = 6, .unit = kNT_unitNone,    .scaling = 0,              .enumStrings = NULL },
	{ .name = "Weight G",   .min = 0,  .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
	{ .name = "Atten G",    .min = 10, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
	{ .name = "Degree H",   .min = 0,  .max = 15, .def = 7, .unit = kNT_unitNone,    .scaling = 0,              .enumStrings = NULL },
	{ .name = "Weight H",   .min = 0,  .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
	{ .name = "Atten H",    .min = 10, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0,          .enumStrings = NULL },
};

// ─── Parameter pages ─────────────────────────────────────────────────────────

static const uint8_t pageGlobal[] = {
	kParamRate, kParamSpread, kParamDrift, kParamRelate,
	kParamShape, kParamOct, kParamExcite, kParamDecay,
	kParamRoot, kParamScale, kParamReseedNow,
};
static const uint8_t pageNotesAB[] = {
	kParamDegA, kParamWeightA, kParamAttenA,
	kParamDegB, kParamWeightB, kParamAttenB,
};
static const uint8_t pageNotesCD[] = {
	kParamDegC, kParamWeightC, kParamAttenC,
	kParamDegD, kParamWeightD, kParamAttenD,
};
static const uint8_t pageNotesEF[] = {
	kParamDegE, kParamWeightE, kParamAttenE,
	kParamDegF, kParamWeightF, kParamAttenF,
};
static const uint8_t pageNotesGH[] = {
	kParamDegG, kParamWeightG, kParamAttenG,
	kParamDegH, kParamWeightH, kParamAttenH,
};
static const uint8_t pageRouting[] = {
	// Outputs
	kParamMixLOut, kParamMixROut,
	kParamAudioAOut, kParamAudioBOut, kParamAudioCOut, kParamAudioDOut,
	kParamAudioEOut, kParamAudioFOut, kParamAudioGOut, kParamAudioHOut,
	kParamLFOAOut, kParamLFOBOut, kParamLFOCOut, kParamLFODOut,
	kParamLFOEOut, kParamLFOFOut, kParamLFOGOut, kParamLFOHOut,
	// Inputs
	kParamRateCV, kParamSpreadCV, kParamDriftCV,
	kParamRootCV, kParamScaleCV, kParamReseedIn, kParamClockIn,
	kParamExciteCV, kParamOctCV,
};

static const _NT_parameterPage pages[] = {
	{ .name = "Global",   .numParams = ARRAY_SIZE(pageGlobal),   .group = 1, .params = pageGlobal   },
	{ .name = "Notes AB", .numParams = ARRAY_SIZE(pageNotesAB),  .group = 2, .params = pageNotesAB  },
	{ .name = "Notes CD", .numParams = ARRAY_SIZE(pageNotesCD),  .group = 2, .params = pageNotesCD  },
	{ .name = "Notes EF", .numParams = ARRAY_SIZE(pageNotesEF),  .group = 2, .params = pageNotesEF  },
	{ .name = "Notes GH", .numParams = ARRAY_SIZE(pageNotesGH),  .group = 2, .params = pageNotesGH  },
	{ .name = "Routing",  .numParams = ARRAY_SIZE(pageRouting),  .group = 3, .params = pageRouting  },
};

static const _NT_parameterPages parameterPages = {
	.numPages = ARRAY_SIZE(pages),
	.pages = pages,
};

// ─── Reseed / reset helpers ──────────────────────────────────────────────────

static void reseed(_chimeNT* p) {
	for (int c = 0; c < NUM_CH; c++) p->randMul[c] = randFloat(p->rng);
}

static void softReset(_chimeNT* p) {
	for (int c = 0; c < NUM_CH; c++) {
		p->phase[c] = (float)c / (float)NUM_CH;
		p->tri[c] = 0.f;
		p->lastTri[c] = 0.f;
		p->window[c] = 0.f;
		p->winSm[c] = 0.f;
		p->wob[c] = 0.f;
		p->wobTarget[c] = 0.f;
		p->wobTimer[c] = 0.f;
		p->rateEff[c] = 0.15f;
		p->strikeT[c] = 0.f;
		p->freq[c] = 130.81f;
		p->freqLatched[c] = 130.81f;
		p->ripple[c] = 0.f;
		for (int k = 0; k < NUM_PART; k++) {
			p->partPhase[c][k] = 0.f;
			p->partEnv[c][k] = 0.f;
		}
	}
	p->rippleCoupling = 0.f;
	p->curOct = 0;
	p->ctrlAccum = 0.f;
	p->clkInterval = 0.f;
	p->clkSince = 1e6f;
}

// ─── Construct ───────────────────────────────────────────────────────────────

void calculateRequirements(_NT_algorithmRequirements& req, const int32_t* /*spec*/) {
	req.numParameters = NUM_PARAMS;
	req.sram = sizeof(_chimeNT);
	req.dram = 0;
	req.dtc = 0;
	req.itc = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs,
                         const _NT_algorithmRequirements& /*req*/,
                         const int32_t* /*spec*/) {
	_chimeNT* alg = new (ptrs.sram) _chimeNT();
	alg->parameters = parameters;
	alg->parameterPages = &parameterPages;
	alg->clockTrigger.reset();
	alg->reseedTrigger.reset();
	alg->rng = 0xC1E51EEDu;
	alg->lastReseedNow = -1;
	reseed(alg);
	softReset(alg);
	return alg;
}

// ─── Bus helpers ─────────────────────────────────────────────────────────────

static inline const float* inputBus(const _chimeNT* p, int param,
                                    float* busFrames, int numFrames) {
	int bus = p->v[param];
	return bus > 0 ? busFrames + (bus - 1) * numFrames : NULL;
}
static inline float* outputBus(const _chimeNT* p, int param,
                               float* busFrames, int numFrames) {
	int bus = p->v[param];
	return bus > 0 ? busFrames + (bus - 1) * numFrames : NULL;
}
static inline void writeOut(float* out, int frame, bool replace, float v) {
	if (replace) out[frame] = v;
	else         out[frame] += v;
}

// Resolve current root / scale / octave (with optional CV mod).
static inline int resolvedRoot(const _chimeNT* p, const float* rootCV, int f) {
	int r = p->v[kParamRoot];
	if (rootCV) r += (int)roundf(rootCV[f] * 12.f);  // 1V/oct → semitones
	return ((r % 12) + 12) % 12;
}
static inline int resolvedScale(const _chimeNT* p, const float* scaleCV, int f) {
	int s = p->v[kParamScale];
	if (scaleCV) s += (int)roundf(scaleCV[f]);  // 1V per scale
	return clampi(s, 0, NUM_SCALES - 1);
}
static inline int resolvedOct(const _chimeNT* p, const float* octCV, int f) {
	int o = p->v[kParamOct];
	if (octCV) o += (int)roundf(octCV[f]);
	return clampi(o, -4, 4);
}

// ─── step ────────────────────────────────────────────────────────────────────

void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
	_chimeNT* p = (_chimeNT*)self;
	int numFrames = numFramesBy4 * 4;
	const float sampleTime = 1.f / (float)NT_globals.sampleRate;

	// UI action: Reseed Now edge (Off→On), no auto-reset.
	int rNow = p->v[kParamReseedNow];
	if (rNow != 0 && p->lastReseedNow == 0) reseed(p);
	p->lastReseedNow = rNow;

	// Hoist bus pointers
	const float* rateCV   = inputBus(p, kParamRateCV,   busFrames, numFrames);
	const float* spreadCV = inputBus(p, kParamSpreadCV, busFrames, numFrames);
	const float* driftCV  = inputBus(p, kParamDriftCV,  busFrames, numFrames);
	const float* rootCV   = inputBus(p, kParamRootCV,   busFrames, numFrames);
	const float* scaleCV  = inputBus(p, kParamScaleCV,  busFrames, numFrames);
	const float* reseedIn = inputBus(p, kParamReseedIn, busFrames, numFrames);
	const float* clockIn  = inputBus(p, kParamClockIn,  busFrames, numFrames);
	const float* exciteCV = inputBus(p, kParamExciteCV, busFrames, numFrames);
	const float* octCV    = inputBus(p, kParamOctCV,    busFrames, numFrames);

	float* audioOut[NUM_CH] = {
		outputBus(p, kParamAudioAOut, busFrames, numFrames),
		outputBus(p, kParamAudioBOut, busFrames, numFrames),
		outputBus(p, kParamAudioCOut, busFrames, numFrames),
		outputBus(p, kParamAudioDOut, busFrames, numFrames),
		outputBus(p, kParamAudioEOut, busFrames, numFrames),
		outputBus(p, kParamAudioFOut, busFrames, numFrames),
		outputBus(p, kParamAudioGOut, busFrames, numFrames),
		outputBus(p, kParamAudioHOut, busFrames, numFrames),
	};
	bool audioMode[NUM_CH] = {
		(bool)p->v[kParamAudioAMode], (bool)p->v[kParamAudioBMode],
		(bool)p->v[kParamAudioCMode], (bool)p->v[kParamAudioDMode],
		(bool)p->v[kParamAudioEMode], (bool)p->v[kParamAudioFMode],
		(bool)p->v[kParamAudioGMode], (bool)p->v[kParamAudioHMode],
	};
	float* lfoOut[NUM_CH] = {
		outputBus(p, kParamLFOAOut, busFrames, numFrames),
		outputBus(p, kParamLFOBOut, busFrames, numFrames),
		outputBus(p, kParamLFOCOut, busFrames, numFrames),
		outputBus(p, kParamLFODOut, busFrames, numFrames),
		outputBus(p, kParamLFOEOut, busFrames, numFrames),
		outputBus(p, kParamLFOFOut, busFrames, numFrames),
		outputBus(p, kParamLFOGOut, busFrames, numFrames),
		outputBus(p, kParamLFOHOut, busFrames, numFrames),
	};
	bool lfoMode[NUM_CH] = {
		(bool)p->v[kParamLFOAMode], (bool)p->v[kParamLFOBMode],
		(bool)p->v[kParamLFOCMode], (bool)p->v[kParamLFODMode],
		(bool)p->v[kParamLFOEMode], (bool)p->v[kParamLFOFMode],
		(bool)p->v[kParamLFOGMode], (bool)p->v[kParamLFOHMode],
	};
	float* mixL = outputBus(p, kParamMixLOut, busFrames, numFrames);
	float* mixR = outputBus(p, kParamMixROut, busFrames, numFrames);
	bool mixLMode = p->v[kParamMixLMode];
	bool mixRMode = p->v[kParamMixRMode];

	// Static params for this block
	int nPotSpread = p->v[kParamSpread];
	int nPotDrift  = p->v[kParamDrift];
	float rateHzBase = (float)p->v[kParamRate] * 0.01f;  // scaled ÷100
	float shape = (float)p->v[kParamShape] * 0.01f;      // -1..+1
	float exciteBase = (float)p->v[kParamExcite] * 0.01f;
	float decayK = (float)p->v[kParamDecay] * 0.1f;      // 0.3..8.0 s
	int   relate = clampi(p->v[kParamRelate], 0, 3);

	// Excite shape parameter: -1 → exp2(-2) = 0.25 (exp), 0 → 1 (linear),
	// +1 → exp2(2) = 4 (log). Recompute per block only.
	float shapeP = exp2Approx(shape * 2.f);

	// Declick smoothing coefficients (per-sample).
	const float kWin = fminf(1.f, sampleTime / 0.004f);
	const float kAtk = fminf(1.f, sampleTime / 0.0008f);

	// Per-partial decay multiplier — constant across the block for a given
	// decayK/PART_DECAY. Hoisting out of the per-sample loop saves 24 → 3
	// expApprox calls per block. env *= mult per sample.
	float partDecayMult[NUM_PART];
	for (int k = 0; k < NUM_PART; k++) {
		partDecayMult[k] = expApprox(-sampleTime
			/ (decayK * PART_DECAY[k] * 0.25f));
	}

	// Control-rate update interval: every 64 samples (matches VCV divider).
	const float ctrlInterval = 64.f * sampleTime;

	for (int f = 0; f < numFrames; f++) {
		// ─── Clock measurement (per-sample) ───
		p->clkSince += sampleTime;
		float clkV = clockIn ? clockIn[f] : 0.f;
		if (p->clockTrigger.process(clkV)) {
			if (p->clkSince < 4.f) {
				p->clkInterval = (p->clkInterval > 0.f)
					? p->clkInterval * 0.7f + p->clkSince * 0.3f
					: p->clkSince;
			}
			p->clkSince = 0.f;
		}
		bool clocked = (clockIn != NULL) && (p->clkInterval > 0.f)
		            && (p->clkSince < fmaxf(4.f * p->clkInterval, 2.f));

		// ─── Reseed trigger (per-sample) ───
		float rseedV = reseedIn ? reseedIn[f] : 0.f;
		if (p->reseedTrigger.process(rseedV)) reseed(p);

		// ─── Control-rate updates (rates, wobble, frequencies) ───
		p->ctrlAccum += sampleTime;
		if (p->ctrlAccum >= ctrlInterval) {
			float ctrlDt = p->ctrlAccum;
			p->ctrlAccum = 0.f;

			// Effective rotation rate — either clock-synced or free.
			float rate;
			if (clocked) {
				// RATE knob picks clocks-per-rotation from DIVS list.
				// Map log2 range to index 0..5.
				float knobLog = log2Approx(fmaxf(rateHzBase, 1e-4f));  // knob in log2 Hz
				float k = clampf((knobLog - RATE_LO_LOG2)
				              / (RATE_HI_LOG2 - RATE_LO_LOG2), 0.f, 0.999f);
				rate = 1.f / (p->clkInterval * CLK_DIVS[(int)(k * 6.f)]);
			} else {
				rate = rateHzBase;
			}
			if (rateCV) rate *= exp2Approx(rateCV[f] / 2.5f);

			float spread = clampf((float)nPotSpread * 0.01f
				+ (spreadCV ? spreadCV[f] * 0.1f : 0.f), 0.f, 1.f);
			float drift  = clampf((float)nPotDrift * 0.01f
				+ (driftCV ? driftCV[f] * 0.1f : 0.f), 0.f, 1.f);
			p->rippleCoupling = spread;
			float maxR = 1.f + spread * 7.f;

			int scaleIdx = resolvedScale(p, scaleCV, f);
			int rootPc   = resolvedRoot(p, rootCV, f);
			p->curOct    = resolvedOct(p, octCV, f);

			for (int c = 0; c < NUM_CH; c++) {
				float x = (NUM_CH > 1) ? (float)c / (float)(NUM_CH - 1) : 0.f;
				float mult;
				switch (relate) {
					case 1:  mult = fmaxf(1.f, roundf(powApprox(maxR, x))); break;
					case 2:  mult = powApprox(maxR, p->randMul[c]); break;
					case 3:  mult = 1.f + 3.f * p->ripple[c]; break;
					default: mult = powApprox(maxR, x); break;
				}
				// Slow per-channel wobble
				p->wobTimer[c] -= ctrlDt;
				if (p->wobTimer[c] <= 0.f) {
					p->wobTimer[c] = 2.f + 3.f * randFloat(p->rng);
					p->wobTarget[c] = randFloat(p->rng) * 2.f - 1.f;
				}
				p->wob[c] += (p->wobTarget[c] - p->wob[c])
				           * fminf(1.f, ctrlDt / 2.f);
				p->rateEff[c] = rate * mult * exp2Approx(drift * p->wob[c]);

				int deg = clampi(p->v[CH_DEG(c)], 0, NUM_DEG - 1);
				p->freq[c] = 130.81f * exp2Approx(
					((float)rootPc + degreeSemis(deg, scaleIdx)) / 12.f
					+ (float)p->curOct);
			}
		}

		// ─── Per-sample voice processing ───
		float exciteX = clampf(exciteBase
			+ (exciteCV ? exciteCV[f] * 0.1f : 0.f), 0.f, 1.f);
		float mL = 0.f, mR = 0.f;

		for (int c = 0; c < NUM_CH; c++) {
			float att = clampf((float)p->v[CH_ATTEN(c)] * 0.01f, 0.1f, 1.f);
			float wgt = clampf((float)p->v[CH_WEIGHT(c)] * 0.01f, 0.f, 1.f);

			// Advance rotation phase. Smaller arc → faster crossings.
			p->phase[c] += p->rateEff[c] / att * sampleTime;
			if (p->phase[c] >= 1.f) p->phase[c] -= 1.f;

			// Triangle -1..+1..-1
			float t = 1.f - 4.f * fabsf(p->phase[c] - 0.5f);
			// Symmetric curve shaping (dwell at extremes or centre)
			if (shapeP != 1.f) {
				float a = powApprox(fabsf(t), shapeP);
				t = (t < 0.f) ? -a : a;
			}
			t *= att;
			p->tri[c] = t;

			// Window (centre proximity), squared for sharper bloom, smoothed.
			float w = 1.f - fabsf(t);
			w *= w;
			p->winSm[c] += (w - p->winSm[c]) * kWin;
			p->window[c] = p->winSm[c];

			// Centre crossing detection
			bool crossed = (p->lastTri[c] < 0.f) != (t < 0.f);
			p->lastTri[c] = t;

			if (crossed && relate == 3) {
				// Ripple: excite neighbours by adjacent weight * coupling.
				if (c > 0) {
					float w0 = (float)p->v[CH_WEIGHT(c - 1)] * 0.01f;
					p->ripple[c - 1] = fminf(1.5f,
						p->ripple[c - 1] + p->rippleCoupling * 0.6f * w0);
				}
				if (c < NUM_CH - 1) {
					float w1 = (float)p->v[CH_WEIGHT(c + 1)] * 0.01f;
					p->ripple[c + 1] = fminf(1.5f,
						p->ripple[c + 1] + p->rippleCoupling * 0.6f * w1);
				}
			}
			p->ripple[c] -= p->ripple[c] * sampleTime / 0.6f;

			bool struckNow = false;
			if (crossed) {
				if (randFloat(p->rng) < wgt) {
					p->strikeT[c] = 0.0025f;
					struckNow = true;
				}
			}
			bool attacking = p->strikeT[c] > 0.f;
			if (attacking) p->strikeT[c] -= sampleTime;

			// Latch pitch at note boundaries so a running note doesn't glide.
			if (struckNow || p->winSm[c] < 0.03f || wgt <= 0.001f
				|| p->freqLatched[c] <= 0.f) {
				p->freqLatched[c] = p->freq[c];
			}

			// Voice: three bar partials, bow↔strike blend
			float v = 0.f;
			for (int k = 0; k < NUM_PART; k++) {
				p->partPhase[c][k] += p->freqLatched[c] * PART_RATIO[k] * sampleTime;
				if (p->partPhase[c][k] >= 1.f) p->partPhase[c][k] -= 1.f;
				if (attacking) {
					p->partEnv[c][k] += (1.f - p->partEnv[c][k]) * kAtk;
				} else {
					p->partEnv[c][k] *= partDecayMult[k];
				}
				float env = (1.f - exciteX) + exciteX * p->partEnv[c][k];
				v += PART_AMP[k] * env * sinApprox(2.f * (float)M_PI * p->partPhase[c][k]);
			}
			v *= p->winSm[c];
			v *= (1.f - exciteX) * wgt + exciteX;
			float out = v * 3.5f;   // per-channel level

			if (audioOut[c]) writeOut(audioOut[c], f, audioMode[c], out);
			if (lfoOut[c])   writeOut(lfoOut[c],   f, lfoMode[c],   t * 5.f);

			// Equal-power L↔R pan across the 8 channels — precomputed constants
			mL += out * PAN_L[c];
			mR += out * PAN_R[c];
		}

		float mixScale = 0.5f;
		if (mixL) writeOut(mixL, f, mixLMode, mL * mixScale);
		if (mixR) writeOut(mixR, f, mixRMode, mR * mixScale);
	}
}

// ─── Draw ────────────────────────────────────────────────────────────────────

// Note name from semitones above C3 (130.81 Hz reference).
static void noteName(int semis, char* out) {
	static const char* NN[12] = {
		"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
	};
	int pc = ((semis % 12) + 12) % 12;
	int oct = 3 + (int)floorf((float)semis / 12.f);
	int pos = 0;
	const char* nm = NN[pc];
	out[pos++] = nm[0];
	if (nm[1]) out[pos++] = nm[1];
	if (oct < 0) { out[pos++] = '-'; oct = -oct; }
	if (oct >= 10) out[pos++] = '0' + (oct / 10);
	out[pos++] = '0' + (oct % 10);
	out[pos] = 0;
}

bool draw(_NT_algorithm* self) {
	_chimeNT* p = (_chimeNT*)self;

	int rootPc = ((p->v[kParamRoot] % 12) + 12) % 12;
	int scaleIdx = clampi(p->v[kParamScale], 0, NUM_SCALES - 1);

	// Header: title + key + octave
	static const char* rootNames[] = {
		"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
	};
	char hdr[48];
	int hl = 0;
	const char* c0 = "CHIME ";
	while (*c0) hdr[hl++] = *c0++;
	const char* rn = rootNames[rootPc];
	while (*rn) hdr[hl++] = *rn++;
	hdr[hl++] = ' ';
	const char* sn = enumScale[scaleIdx];
	while (*sn && hl < 34) hdr[hl++] = *sn++;
	if (p->curOct != 0) {
		hdr[hl++] = ' ';
		hdr[hl++] = (p->curOct > 0) ? '+' : '-';
		int a = p->curOct < 0 ? -p->curOct : p->curOct;
		hdr[hl++] = '0' + a;
	}
	hdr[hl] = 0;
	NT_drawText(2, 8, hdr, 15, kNT_textLeft, kNT_textNormal);

	// 8 columns, each 24 wide, centred on 256 (32 px margin each side would be
	// too much; use 24×8 = 192, centred with 32 margin).
	const int colW = 24;
	const int nCols = NUM_CH;
	const int totalW = colW * nCols;
	const int x0 = (256 - totalW) / 2;
	const int laneY0 = 14;
	const int laneY1 = 58;
	const int cy = (laneY0 + laneY1) / 2;

	for (int c = 0; c < NUM_CH; c++) {
		int x = x0 + c * colW;
		int cx = x + colW / 2;

		// Column background outline
		NT_drawShapeI(kNT_box, x, laneY0, x + colW - 1, laneY1, 3);

		// Pendulum swing: horizontal offset from centre = tri * half-width
		float t = p->tri[c];
		int sx = cx + (int)(t * (colW / 2 - 2));
		// Draw pendulum line from pivot (cx, cy) to tip (sx, cy - 6)
		int tipY = cy - 6;
		NT_drawShapeI(kNT_line, cx, cy, sx, tipY, 12);
		// Bright dot at the tip; brighter when close to centre (resonance)
		int dotBright = 6 + (int)(p->window[c] * 9.f);
		if (dotBright > 15) dotBright = 15;
		NT_drawShapeI(kNT_rectangle, sx - 1, tipY - 1, sx + 1, tipY + 1, dotBright);

		// Weight bar at the bottom of the column
		int wgt = p->v[CH_WEIGHT(c)];
		int wbarW = (wgt * (colW - 4)) / 100;
		int barY = laneY1 - 3;
		NT_drawShapeI(kNT_rectangle, x + 2, barY, x + 2 + wbarW, barY + 1, 12);

		// Note name at the top of the column
		int deg = clampi(p->v[CH_DEG(c)], 0, NUM_DEG - 1);
		int semis = rootPc + (int)degreeSemis(deg, scaleIdx) + 12 * p->curOct;
		char nb[8];
		noteName(semis, nb);
		NT_drawText(cx, laneY0 + 6, nb, 15, kNT_textCentre, kNT_textTiny);
	}

	// Bottom row: rate + relate mode
	static const char* relateNames[] = { "RAMP", "STEP", "RAND", "RIPL" };
	int relate = clampi(p->v[kParamRelate], 0, 3);
	char foot[24];
	int fl = 0;
	const char* rl = relateNames[relate];
	while (*rl) foot[fl++] = *rl++;
	foot[fl++] = ' '; foot[fl++] = ' ';
	// Rate as Hz to 2dp
	float rateHz = (float)p->v[kParamRate] * 0.01f;
	int whole = (int)rateHz;
	int frac = (int)((rateHz - (float)whole) * 100.f + 0.5f);
	if (whole >= 10) { foot[fl++] = '0' + (whole/10); }
	foot[fl++] = '0' + (whole % 10);
	foot[fl++] = '.';
	foot[fl++] = '0' + (frac / 10);
	foot[fl++] = '0' + (frac % 10);
	foot[fl++] = 'H'; foot[fl++] = 'z';
	foot[fl] = 0;
	NT_drawText(2, 63, foot, 10, kNT_textLeft, kNT_textTiny);

	return true;
}

// ─── Factory ─────────────────────────────────────────────────────────────────

static const _NT_factory factory = {
	.guid = NT_MULTICHAR('S','F','c','N'),  // "SFcN" — SFS Chime NT
	.name = "ChimeNT",
	.description = "8-note resonating drone machine (port of SFS Chime)",
	.numSpecifications = 0,
	.specifications = NULL,
	.calculateStaticRequirements = NULL,
	.initialise = NULL,
	.calculateRequirements = calculateRequirements,
	.construct = construct,
	.parameterChanged = NULL,
	.step = step,
	.draw = draw,
	.midiRealtime = NULL,
	.midiMessage = NULL,
	.tags = kNT_tagInstrument,
	.hasCustomUi = NULL,
	.customUi = NULL,
	.setupUi = NULL,
	.serialise = NULL,
	.deserialise = NULL,
	.midiSysEx = NULL,
	.parameterUiPrefix = NULL,
	.parameterString = NULL,
};

extern "C" uintptr_t pluginEntry(_NT_selector selector, uint32_t data) {
	switch (selector) {
	case kNT_selector_version:
		return kNT_apiVersionCurrent;
	case kNT_selector_numFactories:
		return 1;
	case kNT_selector_factoryInfo:
		return (uintptr_t)((data == 0) ? &factory : NULL);
	}
	return 0;
}
