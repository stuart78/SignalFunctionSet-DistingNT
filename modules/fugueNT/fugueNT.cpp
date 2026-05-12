// FugueNT — port of Signal Function Set's Fugue (+ X subset) for the Disting NT.
// See docs/fugue-nt.md for design notes.

#include <math.h>
#include <new>
#include <string.h>
#include <distingnt/api.h>
#include <distingnt/serialisation.h>

// ─── Constants ───────────────────────────────────────────────────────────────

static const int NUM_STEPS = 8;
static const int NUM_VOICES = 3;
static const int CHROMATIC_SCALE_INDEX = 0;

static const int SLEEP_VALUES[10] = {0, 1, 2, 4, 5, 8, 16, 32, 48, 64};
static const float RANGE_VALUES[3] = {1.f, 2.f, 5.f};

// ─── Scale Tables ────────────────────────────────────────────────────────────

struct ScaleInfo {
	const float* intervals;
	int size;
};

static const float SCALE_CHROMATIC[]   = {0,1,2,3,4,5,6,7,8,9,10,11};
static const float SCALE_MAJOR[]       = {0,2,4,5,7,9,11};
static const float SCALE_NAT_MINOR[]   = {0,2,3,5,7,8,10};
static const float SCALE_PENTA_MAJ[]   = {0,2,4,7,9};
static const float SCALE_PENTA_MIN[]   = {0,3,5,7,10};
static const float SCALE_BLUES[]       = {0,3,5,6,7,10};
static const float SCALE_WHOLE[]       = {0,2,4,6,8,10};
static const float SCALE_HARMONIC[]    = {
	0.f, 12.f, 19.0196f, 24.f, 27.8631f, 31.0196f,
	33.6883f, 36.f, 38.0392f, 39.8632f, 41.5126f, 43.0196f
};
static const float SCALE_DORIAN[]      = {0,2,3,5,7,9,10};
static const float SCALE_PHRYGIAN[]    = {0,1,3,5,7,8,10};
static const float SCALE_LYDIAN[]      = {0,2,4,6,7,9,11};
static const float SCALE_MIXOLYDIAN[]  = {0,2,4,5,7,9,10};
static const float SCALE_HARM_MINOR[]  = {0,2,3,5,7,8,11};
static const float SCALE_HIJAZ[]       = {0,1,4,5,7,8,10};
static const float SCALE_HIRAJOSHI[]   = {0,2,3,7,8};
static const float SCALE_PELOG[]       = {0.f, 1.2f, 2.7f, 5.4f, 7.0f, 8.0f, 10.4f};
static const float SCALE_SLENDRO[]     = {0.f, 2.4f, 4.8f, 7.2f, 9.6f};
static const float SCALE_MELO_MINOR[]  = {0,2,3,5,7,9,11};
static const float SCALE_LOCRIAN[]     = {0,1,3,5,6,8,10};

static const ScaleInfo SCALES[] = {
	{SCALE_CHROMATIC, 12},
	{SCALE_MAJOR,      7},
	{SCALE_NAT_MINOR,  7},
	{SCALE_PENTA_MAJ,  5},
	{SCALE_PENTA_MIN,  5},
	{SCALE_BLUES,      6},
	{SCALE_WHOLE,      6},
	{SCALE_HARMONIC,  12},
	{SCALE_DORIAN,     7},
	{SCALE_PHRYGIAN,   7},
	{SCALE_LYDIAN,     7},
	{SCALE_MIXOLYDIAN, 7},
	{SCALE_HARM_MINOR, 7},
	{SCALE_HIJAZ,      7},
	{SCALE_HIRAJOSHI,  5},
	{SCALE_PELOG,      7},
	{SCALE_SLENDRO,    5},
	{SCALE_MELO_MINOR, 7},
	{SCALE_LOCRIAN,    7},
};
static const int NUM_SCALES = ARRAY_SIZE(SCALES);

// ─── Deviation Tier Tables ───────────────────────────────────────────────────

struct DeviationTier {
	int offsets[6];
	int count;
};

static const DeviationTier DIATONIC_TIERS[] = {
	{{0},          1},
	{{2, 4},       2},
	{{6, 1, 3},    3},
	{{5},          1},
};
static const int NUM_DIATONIC_TIERS = 4;

static const DeviationTier PENTATONIC_TIERS[] = {
	{{0},          1},
	{{1, 2},       2},
	{{3, 4},       2},
};
static const int NUM_PENTATONIC_TIERS = 3;

static const int CHROM_TIER_0[] = {0};
static const int CHROM_TIER_1[] = {7, 5};
static const int CHROM_TIER_2[] = {4, 3, 9, 8};
static const int CHROM_TIER_3[] = {2, 10};
static const int CHROM_TIER_4[] = {1, 11, 6};

struct ChromTierInfo {
	const int* intervals;
	int count;
};

static const ChromTierInfo CHROM_TIERS[] = {
	{CHROM_TIER_0, 1},
	{CHROM_TIER_1, 2},
	{CHROM_TIER_2, 4},
	{CHROM_TIER_3, 2},
	{CHROM_TIER_4, 3},
};

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

static float intervalConsonance(int semitones) {
	semitones = ((semitones % 12) + 12) % 12;
	static const float scores[] = {
		1.0f, 0.1f, 0.3f, 0.65f, 0.7f, 0.85f,
		0.15f, 0.9f, 0.55f, 0.6f, 0.25f, 0.1f,
	};
	return scores[semitones];
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

// ─── Algorithm state ─────────────────────────────────────────────────────────

struct VoiceState {
	int currentStep;
	float clockPeriod;       // estimated time between clocks (s)
	float clockTimer;        // time since last clock (s)
	bool clockHigh;          // raw clock-high flag (gates depend on this)
	uint32_t stepCounter;
	Schmitt clockTrigger;
	float currentVoltage;
	float targetVoltage;
	float slewRate;          // V/s (0 = jump)
	bool firstClockPending;

	int sleepCounter;
	bool sleeping;
	bool sampleHoldHolding;
	bool probGateSuppress;
};

// Tracks the last-seen value of kParamRandomizeNow so we only fire the
// randomize action on the 0→non-zero edge. Set to -1 in construct so the
// param's initial value of 0 doesn't itself look like an edge.

struct _fugueNT : public _NT_algorithm {
	_fugueNT() {}
	~_fugueNT() {}

	VoiceState voices[NUM_VOICES];
	Schmitt resetTrigger;
	Schmitt randomizeTrigger;
	uint32_t probRng;

	// UI state
	int selectedStep;        // 0..NUM_STEPS-1
	int focusVoice;          // 0..NUM_VOICES-1
	int currentPage;         // 0=main, 1=per-voice

	int lastRandomizeNow;    // edge-detect state for the Randomize Now param
};

// ─── Parameter enum ──────────────────────────────────────────────────────────
//
// Layout matters: macros that expand to >1 entry must align with the enum.
//   NT_PARAMETER_CV_INPUT and NT_PARAMETER_CV_OUTPUT emit 1 entry each.
//   NT_PARAMETER_CV_OUTPUT_WITH_MODE emits 2 entries (bus + output-mode).

enum {
	// 21 input bus params
	kParamClockA = 0,
	kParamClockB,
	kParamClockC,
	kParamResetIn,
	kParamRootCV,
	kParamScaleCV,
	kParamStepsCV,
	kParamSlewCV,
	kParamWanderACV,
	kParamWanderBCV,
	kParamWanderCCV,
	kParamStepsACV,
	kParamStepsBCV,
	kParamStepsCCV,
	kParamSleepACV,
	kParamSleepBCV,
	kParamSleepCCV,
	kParamProbACV,
	kParamProbBCV,
	kParamProbCCV,
	kParamRandomizeIn,

	// 9 output bus params, each with mode (= 2 entries each, 18 total)
	kParamGateAOut,    kParamGateAMode,
	kParamGateBOut,    kParamGateBMode,
	kParamGateCOut,    kParamGateCMode,
	kParamCVAOut,      kParamCVAMode,
	kParamCVBOut,      kParamCVBMode,
	kParamCVCOut,      kParamCVCMode,
	kParamMinOut,      kParamMinMode,
	kParamMidOut,      kParamMidMode,
	kParamMaxOut,      kParamMaxMode,

	// Global / tonality
	kParamRoot,
	kParamScale,
	kParamSteps,
	kParamSlew,
	kParamFaderRange,
	kParamHarmonicLock,
	kParamSampleHold,

	// Per-step (×8). 4 params per step: pitch + 3 gate toggles.
	kParamStep1Pitch,  kParamStep1GateA, kParamStep1GateB, kParamStep1GateC,
	kParamStep2Pitch,  kParamStep2GateA, kParamStep2GateB, kParamStep2GateC,
	kParamStep3Pitch,  kParamStep3GateA, kParamStep3GateB, kParamStep3GateC,
	kParamStep4Pitch,  kParamStep4GateA, kParamStep4GateB, kParamStep4GateC,
	kParamStep5Pitch,  kParamStep5GateA, kParamStep5GateB, kParamStep5GateC,
	kParamStep6Pitch,  kParamStep6GateA, kParamStep6GateB, kParamStep6GateC,
	kParamStep7Pitch,  kParamStep7GateA, kParamStep7GateB, kParamStep7GateC,
	kParamStep8Pitch,  kParamStep8GateA, kParamStep8GateB, kParamStep8GateC,

	// Per-playhead (×3). 4 params per voice.
	kParamWanderA, kParamStepsA, kParamSleepA, kParamProbA,
	kParamWanderB, kParamStepsB, kParamSleepB, kParamProbB,
	kParamWanderC, kParamStepsC, kParamSleepC, kParamProbC,

	// UI-triggered actions (auto-reset after firing).
	kParamRandomizeNow,

	NUM_PARAMS
};

// Helper macros: per-step + per-voice param index from base + offset
#define STEP_PITCH(s)  (kParamStep1Pitch + (s) * 4)
#define STEP_GATE(s,v) (kParamStep1Pitch + (s) * 4 + 1 + (v))
#define VOICE_WANDER(v) (kParamWanderA + (v) * 4)
#define VOICE_STEPS(v)  (kParamStepsA  + (v) * 4)
#define VOICE_SLEEP(v)  (kParamSleepA  + (v) * 4)
#define VOICE_PROB(v)   (kParamProbA   + (v) * 4)

// ─── Enum strings ────────────────────────────────────────────────────────────

static char const * const enumRoot[] = {
	"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};
static char const * const enumScale[] = {
	"Chromatic", "Major", "Minor",
	"Pentatonic Major", "Pentatonic Minor",
	"Blues", "Whole tone", "Harmonic series",
	"Dorian", "Phrygian", "Lydian", "Mixolydian",
	"Harmonic Minor", "Hijaz", "Hirajoshi",
	"Pelog", "Slendro",
	"Melodic Minor", "Locrian"
};
static char const * const enumRange[] = { "1V", "2V", "5V" };
static char const * const enumOnOff[] = { "Off", "On" };
static char const * const enumSleep[] = {
	"0", "1", "2", "4", "5", "8", "16", "32", "48", "64"
};
static char const * const enumRunIdle[] = { "Idle", "Run" };

// ─── Parameter table ─────────────────────────────────────────────────────────

static const _NT_parameter parameters[] = {
	NT_PARAMETER_CV_INPUT( "Clock A",         0, 1 )
	NT_PARAMETER_CV_INPUT( "Clock B",         0, 0 )
	NT_PARAMETER_CV_INPUT( "Clock C",         0, 0 )
	NT_PARAMETER_CV_INPUT( "Reset",           0, 0 )
	NT_PARAMETER_CV_INPUT( "Root CV",         0, 0 )
	NT_PARAMETER_CV_INPUT( "Scale CV",        0, 0 )
	NT_PARAMETER_CV_INPUT( "Steps CV",        0, 0 )
	NT_PARAMETER_CV_INPUT( "Slew CV",         0, 0 )
	NT_PARAMETER_CV_INPUT( "Wander A CV",     0, 0 )
	NT_PARAMETER_CV_INPUT( "Wander B CV",     0, 0 )
	NT_PARAMETER_CV_INPUT( "Wander C CV",     0, 0 )
	NT_PARAMETER_CV_INPUT( "Steps A CV",      0, 0 )
	NT_PARAMETER_CV_INPUT( "Steps B CV",      0, 0 )
	NT_PARAMETER_CV_INPUT( "Steps C CV",      0, 0 )
	NT_PARAMETER_CV_INPUT( "Sleep A CV",      0, 0 )
	NT_PARAMETER_CV_INPUT( "Sleep B CV",      0, 0 )
	NT_PARAMETER_CV_INPUT( "Sleep C CV",      0, 0 )
	NT_PARAMETER_CV_INPUT( "Prob A CV",       0, 0 )
	NT_PARAMETER_CV_INPUT( "Prob B CV",       0, 0 )
	NT_PARAMETER_CV_INPUT( "Prob C CV",       0, 0 )
	NT_PARAMETER_CV_INPUT( "Randomize",       0, 0 )

	// Default routing pairs gate→CV adjacent (gate=N, CV=N+1), Disting convention.
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Gate A", 0, 13 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Gate B", 0, 15 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Gate C", 0, 17 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "CV A",   0, 14 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "CV B",   0, 16 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "CV C",   0, 18 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Min",    0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Mid",    0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Max",    0, 0 )

	{ .name = "Root",          .min = 0,  .max = 11, .def = 0,  .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumRoot },
	{ .name = "Scale",         .min = 0,  .max = 18, .def = 1,  .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumScale },
	{ .name = "Steps",         .min = 1,  .max = 8,  .def = 8,  .unit = kNT_unitNone,     .scaling = 0,                .enumStrings = NULL },
	{ .name = "Slew",          .min = 0,  .max = 100,.def = 0,  .unit = kNT_unitPercent,  .scaling = 0,                .enumStrings = NULL },
	{ .name = "Fader Range",   .min = 0,  .max = 2,  .def = 0,  .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumRange },
	{ .name = "Harmonic Lock", .min = 0,  .max = 1,  .def = 1,  .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "S&H Mode",      .min = 0,  .max = 1,  .def = 0,  .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },

	// Step 1
	{ .name = "Step 1 Pitch",  .min = 0, .max = 1000, .def = 0, .unit = kNT_unitNone,     .scaling = kNT_scaling1000,  .enumStrings = NULL },
	{ .name = "Step 1 Gate A", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 1 Gate B", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 1 Gate C", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	// Step 2
	{ .name = "Step 2 Pitch",  .min = 0, .max = 1000, .def = 0, .unit = kNT_unitNone,     .scaling = kNT_scaling1000,  .enumStrings = NULL },
	{ .name = "Step 2 Gate A", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 2 Gate B", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 2 Gate C", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	// Step 3
	{ .name = "Step 3 Pitch",  .min = 0, .max = 1000, .def = 0, .unit = kNT_unitNone,     .scaling = kNT_scaling1000,  .enumStrings = NULL },
	{ .name = "Step 3 Gate A", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 3 Gate B", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 3 Gate C", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	// Step 4
	{ .name = "Step 4 Pitch",  .min = 0, .max = 1000, .def = 0, .unit = kNT_unitNone,     .scaling = kNT_scaling1000,  .enumStrings = NULL },
	{ .name = "Step 4 Gate A", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 4 Gate B", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 4 Gate C", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	// Step 5
	{ .name = "Step 5 Pitch",  .min = 0, .max = 1000, .def = 0, .unit = kNT_unitNone,     .scaling = kNT_scaling1000,  .enumStrings = NULL },
	{ .name = "Step 5 Gate A", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 5 Gate B", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 5 Gate C", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	// Step 6
	{ .name = "Step 6 Pitch",  .min = 0, .max = 1000, .def = 0, .unit = kNT_unitNone,     .scaling = kNT_scaling1000,  .enumStrings = NULL },
	{ .name = "Step 6 Gate A", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 6 Gate B", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 6 Gate C", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	// Step 7
	{ .name = "Step 7 Pitch",  .min = 0, .max = 1000, .def = 0, .unit = kNT_unitNone,     .scaling = kNT_scaling1000,  .enumStrings = NULL },
	{ .name = "Step 7 Gate A", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 7 Gate B", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 7 Gate C", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	// Step 8
	{ .name = "Step 8 Pitch",  .min = 0, .max = 1000, .def = 0, .unit = kNT_unitNone,     .scaling = kNT_scaling1000,  .enumStrings = NULL },
	{ .name = "Step 8 Gate A", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 8 Gate B", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },
	{ .name = "Step 8 Gate C", .min = 0, .max = 1,    .def = 1, .unit = kNT_unitEnum,     .scaling = 0,                .enumStrings = enumOnOff },

	// Voice A
	{ .name = "Wander A",      .min = 0,  .max = 100, .def = 0,  .unit = kNT_unitPercent, .scaling = 0,                .enumStrings = NULL },
	{ .name = "Steps A",       .min = 1,  .max = 8,   .def = 8,  .unit = kNT_unitNone,    .scaling = 0,                .enumStrings = NULL },
	{ .name = "Sleep A",       .min = 0,  .max = 9,   .def = 0,  .unit = kNT_unitEnum,    .scaling = 0,                .enumStrings = enumSleep },
	{ .name = "Prob A",        .min = 0,  .max = 100, .def = 100,.unit = kNT_unitPercent, .scaling = 0,                .enumStrings = NULL },
	// Voice B
	{ .name = "Wander B",      .min = 0,  .max = 100, .def = 0,  .unit = kNT_unitPercent, .scaling = 0,                .enumStrings = NULL },
	{ .name = "Steps B",       .min = 1,  .max = 8,   .def = 8,  .unit = kNT_unitNone,    .scaling = 0,                .enumStrings = NULL },
	{ .name = "Sleep B",       .min = 0,  .max = 9,   .def = 0,  .unit = kNT_unitEnum,    .scaling = 0,                .enumStrings = enumSleep },
	{ .name = "Prob B",        .min = 0,  .max = 100, .def = 100,.unit = kNT_unitPercent, .scaling = 0,                .enumStrings = NULL },
	// Voice C
	{ .name = "Wander C",      .min = 0,  .max = 100, .def = 0,  .unit = kNT_unitPercent, .scaling = 0,                .enumStrings = NULL },
	{ .name = "Steps C",       .min = 1,  .max = 8,   .def = 8,  .unit = kNT_unitNone,    .scaling = 0,                .enumStrings = NULL },
	{ .name = "Sleep C",       .min = 0,  .max = 9,   .def = 0,  .unit = kNT_unitEnum,    .scaling = 0,                .enumStrings = enumSleep },
	{ .name = "Prob C",        .min = 0,  .max = 100, .def = 100,.unit = kNT_unitPercent, .scaling = 0,                .enumStrings = NULL },

	// UI action: set to "Run" to scramble all 8 step pitches; auto-resets to "Idle".
	{ .name = "Randomize Now", .min = 0,  .max = 1,   .def = 0,  .unit = kNT_unitEnum,    .scaling = 0,                .enumStrings = enumRunIdle },
};

// ─── Parameter pages ─────────────────────────────────────────────────────────

static const uint8_t pageGlobal[] = {
	kParamRoot, kParamScale, kParamSteps, kParamSlew,
	kParamFaderRange, kParamHarmonicLock, kParamSampleHold,
	kParamRandomizeNow,
};
static const uint8_t pageSteps[] = {
	kParamStep1Pitch, kParamStep2Pitch, kParamStep3Pitch, kParamStep4Pitch,
	kParamStep5Pitch, kParamStep6Pitch, kParamStep7Pitch, kParamStep8Pitch,
};
static const uint8_t pageGates[] = {
	// Voice A across all 8 steps
	kParamStep1GateA, kParamStep2GateA, kParamStep3GateA, kParamStep4GateA,
	kParamStep5GateA, kParamStep6GateA, kParamStep7GateA, kParamStep8GateA,
	// Voice B
	kParamStep1GateB, kParamStep2GateB, kParamStep3GateB, kParamStep4GateB,
	kParamStep5GateB, kParamStep6GateB, kParamStep7GateB, kParamStep8GateB,
	// Voice C
	kParamStep1GateC, kParamStep2GateC, kParamStep3GateC, kParamStep4GateC,
	kParamStep5GateC, kParamStep6GateC, kParamStep7GateC, kParamStep8GateC,
};
static const uint8_t pageVoiceA[] = { kParamWanderA, kParamStepsA, kParamSleepA, kParamProbA };
static const uint8_t pageVoiceB[] = { kParamWanderB, kParamStepsB, kParamSleepB, kParamProbB };
static const uint8_t pageVoiceC[] = { kParamWanderC, kParamStepsC, kParamSleepC, kParamProbC };
static const uint8_t pageRouting[] = {
	// Voice outputs first, paired gate → CV per voice
	kParamGateAOut, kParamCVAOut,
	kParamGateBOut, kParamCVBOut,
	kParamGateCOut, kParamCVCOut,
	// Aggregate outputs
	kParamMinOut, kParamMidOut, kParamMaxOut,
	// Inputs
	kParamClockA, kParamClockB, kParamClockC, kParamResetIn,
	kParamRootCV, kParamScaleCV, kParamStepsCV, kParamSlewCV,
	kParamWanderACV, kParamWanderBCV, kParamWanderCCV,
	kParamStepsACV, kParamStepsBCV, kParamStepsCCV,
	kParamSleepACV, kParamSleepBCV, kParamSleepCCV,
	kParamProbACV, kParamProbBCV, kParamProbCCV,
	kParamRandomizeIn,
};

static const _NT_parameterPage pages[] = {
	{ .name = "Global",    .numParams = ARRAY_SIZE(pageGlobal),  .group = 1, .params = pageGlobal  },
	{ .name = "Pitches",   .numParams = ARRAY_SIZE(pageSteps),   .group = 1, .params = pageSteps   },
	{ .name = "Gates",     .numParams = ARRAY_SIZE(pageGates),   .group = 1, .params = pageGates   },
	{ .name = "Voice A",   .numParams = ARRAY_SIZE(pageVoiceA),  .group = 2, .params = pageVoiceA  },
	{ .name = "Voice B",   .numParams = ARRAY_SIZE(pageVoiceB),  .group = 2, .params = pageVoiceB  },
	{ .name = "Voice C",   .numParams = ARRAY_SIZE(pageVoiceC),  .group = 2, .params = pageVoiceC  },
	{ .name = "Routing",   .numParams = ARRAY_SIZE(pageRouting), .group = 3, .params = pageRouting },
};

static const _NT_parameterPages parameterPages = {
	.numPages = ARRAY_SIZE(pages),
	.pages = pages,
};

// ─── DSP: quantization ───────────────────────────────────────────────────────

static float faderToVoltage(float faderValue, int rootNote, int scaleIndex, float faderRange) {
	float rawVoltage = faderValue * faderRange;
	const ScaleInfo& scale = SCALES[scaleIndex];

	float bestVoltage = 0.f;
	float bestDist = 999.f;

	int maxSemitones = (int)(faderRange * 12.f) + 12;
	int maxOctaves = maxSemitones / 12 + 2;

	for (int oct = 0; oct <= maxOctaves; oct++) {
		for (int d = 0; d < scale.size; d++) {
			float semitone = (float)(oct * 12) + scale.intervals[d];
			float noteVoltage = semitone / 12.f;
			if (noteVoltage > faderRange + 0.05f) break;
			if (noteVoltage < -0.05f) continue;
			float dist = fabsf(noteVoltage - rawVoltage);
			if (dist < bestDist) {
				bestDist = dist;
				bestVoltage = noteVoltage;
			}
		}
	}
	return bestVoltage + (float)rootNote / 12.f;
}

// ─── DSP: deviation ──────────────────────────────────────────────────────────

static float selectDeviationNote(float baseVoltage, float stability, int rootNote,
                                 int scaleIndex, float faderRange, uint32_t seed) {
	uint32_t rng = seed;
	if (stability >= 0.999f) return baseVoltage;
	float tierRoll = randFloat(rng);

	if (scaleIndex == CHROMATIC_SCALE_INDEX) {
		float p0 = stability + (1.f - stability) * 0.05f;
		float p1 = p0 + (1.f - stability) * 0.15f;
		float p2 = p1 + (1.f - stability) * 0.35f;
		float p3 = p2 + (1.f - stability) * 0.30f;

		int selectedTier;
		if (tierRoll < p0) selectedTier = 0;
		else if (tierRoll < p1) selectedTier = 1;
		else if (tierRoll < p2) selectedTier = 2;
		else if (tierRoll < p3) selectedTier = 3;
		else selectedTier = 4;

		const ChromTierInfo& tier = CHROM_TIERS[selectedTier];
		int idx = (int)(randFloat(rng) * tier.count) % tier.count;
		int semiOffset = tier.intervals[idx];
		if (randFloat(rng) < 0.5f) semiOffset = -semiOffset;
		float dev = baseVoltage + (float)semiOffset / 12.f;
		return clampf(dev, baseVoltage - faderRange, baseVoltage + faderRange);
	}

	const ScaleInfo& scale = SCALES[scaleIndex];
	bool isPenta = (scale.size == 5);
	const DeviationTier* tiers = isPenta ? PENTATONIC_TIERS : DIATONIC_TIERS;
	int numTiers = isPenta ? NUM_PENTATONIC_TIERS : NUM_DIATONIC_TIERS;

	float p[5];
	if (isPenta) {
		p[0] = stability + (1.f - stability) * 0.10f;
		p[1] = p[0] + (1.f - stability) * 0.45f;
		p[2] = 1.0f;
	} else {
		p[0] = stability + (1.f - stability) * 0.05f;
		p[1] = p[0] + (1.f - stability) * 0.30f;
		p[2] = p[1] + (1.f - stability) * 0.30f;
		p[3] = p[2] + (1.f - stability) * 0.20f;
		p[4] = 1.0f;
	}
	int maxTier = isPenta ? 2 : 4;
	int selectedTier = maxTier;
	for (int t = 0; t <= maxTier; t++) {
		if (tierRoll < p[t]) { selectedTier = t; break; }
	}

	if (selectedTier < numTiers) {
		const DeviationTier& tier = tiers[selectedTier];
		int idx = (int)(randFloat(rng) * tier.count) % tier.count;
		int scaleDegreeOffset = tier.offsets[idx];

		float baseSemiFromRoot = (baseVoltage - (float)rootNote / 12.f) * 12.f;
		int baseSemiNorm = ((int)roundf(baseSemiFromRoot)) % 12;
		if (baseSemiNorm < 0) baseSemiNorm += 12;
		int baseOctave = (int)floorf(baseSemiFromRoot / 12.f);

		int baseDegree = 0;
		float bestDiff = 999.f;
		for (int d = 0; d < scale.size; d++) {
			float diff = fabsf(scale.intervals[d] - (float)baseSemiNorm);
			if (diff < bestDiff) { bestDiff = diff; baseDegree = d; }
		}

		bool goDown = (randFloat(rng) < 0.4f);
		int targetDegree, targetOctave;
		if (goDown) {
			int raw = baseDegree - scaleDegreeOffset;
			if (raw < 0) {
				targetOctave = baseOctave - 1;
				targetDegree = ((raw % scale.size) + scale.size) % scale.size;
			} else {
				targetOctave = baseOctave;
				targetDegree = raw;
			}
		} else {
			int raw = baseDegree + scaleDegreeOffset;
			targetOctave = baseOctave + raw / scale.size;
			targetDegree = raw % scale.size;
		}
		float targetSemi = (float)(targetOctave * 12) + scale.intervals[targetDegree];
		float dev = (float)rootNote / 12.f + targetSemi / 12.f;
		return clampf(dev, baseVoltage - faderRange, baseVoltage + faderRange);
	}

	// chromatic neighbor (diatonic only)
	int semiOffset = (randFloat(rng) < 0.5f) ? 1 : -1;
	float dev = baseVoltage + (float)semiOffset / 12.f;
	return clampf(dev, baseVoltage - faderRange, baseVoltage + faderRange);
}

static float scoreConsonance(float candidateVolt, int voiceIdx, const VoiceState* voices) {
	float score = 0.f;
	int count = 0;
	for (int v = 0; v < NUM_VOICES; v++) {
		if (v == voiceIdx) continue;
		int interval = (int)roundf((candidateVolt - voices[v].targetVoltage) * 12.f);
		score += intervalConsonance(interval);
		count++;
	}
	return count > 0 ? score / (float)count : 1.f;
}

// ─── Step advance: pick the next target voltage for a voice ─────────────────

static void advanceVoice(_fugueNT* pThis, int voiceIdx,
                         int rootNote, int scaleIndex, float faderRange,
                         float slewPercent, int voiceSteps,
                         float wanderAmount, bool harmonicLock) {
	VoiceState& voice = pThis->voices[voiceIdx];

	float faderValue = (float)pThis->v[STEP_PITCH(voice.currentStep)] / 1000.f;
	float baseVolt = faderToVoltage(faderValue, rootNote, scaleIndex, faderRange);
	float stability = 1.f - wanderAmount;

	uint32_t seed = voice.stepCounter * 2654435761u
	              + voiceIdx * 340573321u
	              + voice.currentStep * 1234577u;
	if (seed == 0) seed = 1;

	if (harmonicLock) {
		float bestVolt = baseVolt;
		float bestScore = -1.f;
		for (int c = 0; c < 3; c++) {
			uint32_t candidateSeed = seed + c * 7919u;
			if (candidateSeed == 0) candidateSeed = 1;
			float candidate = selectDeviationNote(baseVolt, stability, rootNote,
				scaleIndex, faderRange, candidateSeed);
			float score = scoreConsonance(candidate, voiceIdx, pThis->voices);
			if (score > bestScore) { bestScore = score; bestVolt = candidate; }
		}
		voice.targetVoltage = bestVolt;
	} else {
		voice.targetVoltage = selectDeviationNote(baseVolt, stability,
			rootNote, scaleIndex, faderRange, seed);
	}

	// Adaptive slew: look ahead to next active gate for this voice
	if (slewPercent < 0.001f) {
		voice.slewRate = 0.f;
		return;
	}
	int stepsToNext = 0;
	for (int i = 1; i <= voiceSteps; i++) {
		int checkStep = (voice.currentStep + i) % voiceSteps;
		if (pThis->v[STEP_GATE(checkStep, voiceIdx)] > 0) {
			stepsToNext = i;
			break;
		}
	}
	if (stepsToNext == 0) stepsToNext = voiceSteps;

	float timeAvailable = stepsToNext * voice.clockPeriod;
	float slewTime = slewPercent * timeAvailable;
	if (slewTime < 0.001f) slewTime = 0.001f;
	float voltageDiff = fabsf(voice.targetVoltage - voice.currentVoltage);
	if (voltageDiff < 0.0001f) {
		voice.slewRate = 0.f;
	} else {
		voice.slewRate = voltageDiff / slewTime;
	}
}

// ─── Randomize helper ────────────────────────────────────────────────────────

static void randomizePitches(_fugueNT* pThis, uint32_t algIdx, uint32_t off) {
	for (int s = 0; s < NUM_STEPS; s++) {
		uint32_t r = xorshift32(pThis->probRng);
		int16_t val = (int16_t)(r % 1001);
		NT_setParameterFromUi(algIdx, STEP_PITCH(s) + off, val);
	}
}

// ─── Construct / parameter-changed ───────────────────────────────────────────

void calculateRequirements(_NT_algorithmRequirements& req, const int32_t* /*specifications*/) {
	req.numParameters = NUM_PARAMS;
	req.sram = sizeof(_fugueNT);
	req.dram = 0;
	req.dtc = 0;
	req.itc = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs,
                         const _NT_algorithmRequirements& /*req*/,
                         const int32_t* /*specifications*/) {
	_fugueNT* alg = new (ptrs.sram) _fugueNT();
	alg->parameters = parameters;
	alg->parameterPages = &parameterPages;
	alg->probRng = 12345u;
	alg->selectedStep = 0;
	alg->focusVoice = 0;
	alg->currentPage = 0;
	alg->lastRandomizeNow = -1;  // sentinel so first step() doesn't treat default 0 as an edge
	for (int v = 0; v < NUM_VOICES; v++) {
		VoiceState& vs = alg->voices[v];
		vs.currentStep = 0;
		vs.clockPeriod = 0.5f;
		vs.clockTimer = 0.f;
		vs.clockHigh = false;
		vs.stepCounter = 0;
		vs.clockTrigger.reset();
		vs.currentVoltage = 0.f;
		vs.targetVoltage = 0.f;
		vs.slewRate = 0.f;
		vs.firstClockPending = true;
		vs.sleepCounter = 0;
		vs.sleeping = false;
		vs.sampleHoldHolding = false;
		vs.probGateSuppress = false;
	}
	alg->resetTrigger.reset();
	alg->randomizeTrigger.reset();
	return alg;
}

// ─── Bus access helpers ──────────────────────────────────────────────────────

static inline const float* inputBus(const _fugueNT* pThis, int p, float* busFrames, int numFrames) {
	int bus = pThis->v[p];
	return bus > 0 ? busFrames + (bus - 1) * numFrames : NULL;
}
static inline float* outputBus(const _fugueNT* pThis, int p, float* busFrames, int numFrames) {
	int bus = pThis->v[p];
	return bus > 0 ? busFrames + (bus - 1) * numFrames : NULL;
}

static inline void writeOut(float* out, int frame, bool replace, float v) {
	if (replace) out[frame] = v;
	else         out[frame] += v;
}

// Clock normalling: voice 0 uses Clock A; voice 1 falls back to A; voice 2 falls back to B then A.
static inline const float* normalledClock(int voiceIdx, const float* a, const float* b, const float* c) {
	if (voiceIdx == 0) return a;
	if (voiceIdx == 1) return b ? b : a;
	return c ? c : (b ? b : a);
}

// ─── step ────────────────────────────────────────────────────────────────────

void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
	_fugueNT* pThis = (_fugueNT*)self;
	int numFrames = numFramesBy4 * 4;
	const float sampleTime = 1.f / (float)NT_globals.sampleRate;

	// Input bus pointers
	const float* clkA  = inputBus(pThis, kParamClockA,    busFrames, numFrames);
	const float* clkB  = inputBus(pThis, kParamClockB,    busFrames, numFrames);
	const float* clkC  = inputBus(pThis, kParamClockC,    busFrames, numFrames);
	const float* rstIn = inputBus(pThis, kParamResetIn,   busFrames, numFrames);
	const float* rootCV  = inputBus(pThis, kParamRootCV,  busFrames, numFrames);
	const float* scaleCV = inputBus(pThis, kParamScaleCV, busFrames, numFrames);
	const float* stepsCV = inputBus(pThis, kParamStepsCV, busFrames, numFrames);
	const float* slewCV  = inputBus(pThis, kParamSlewCV,  busFrames, numFrames);
	const float* wandCV[NUM_VOICES] = {
		inputBus(pThis, kParamWanderACV, busFrames, numFrames),
		inputBus(pThis, kParamWanderBCV, busFrames, numFrames),
		inputBus(pThis, kParamWanderCCV, busFrames, numFrames),
	};
	const float* vStepsCV[NUM_VOICES] = {
		inputBus(pThis, kParamStepsACV, busFrames, numFrames),
		inputBus(pThis, kParamStepsBCV, busFrames, numFrames),
		inputBus(pThis, kParamStepsCCV, busFrames, numFrames),
	};
	const float* vSleepCV[NUM_VOICES] = {
		inputBus(pThis, kParamSleepACV, busFrames, numFrames),
		inputBus(pThis, kParamSleepBCV, busFrames, numFrames),
		inputBus(pThis, kParamSleepCCV, busFrames, numFrames),
	};
	const float* vProbCV[NUM_VOICES] = {
		inputBus(pThis, kParamProbACV, busFrames, numFrames),
		inputBus(pThis, kParamProbBCV, busFrames, numFrames),
		inputBus(pThis, kParamProbCCV, busFrames, numFrames),
	};
	const float* randIn = inputBus(pThis, kParamRandomizeIn, busFrames, numFrames);

	// Output bus pointers (and per-output replace mode)
	float* gateOut[NUM_VOICES] = {
		outputBus(pThis, kParamGateAOut, busFrames, numFrames),
		outputBus(pThis, kParamGateBOut, busFrames, numFrames),
		outputBus(pThis, kParamGateCOut, busFrames, numFrames),
	};
	bool gateMode[NUM_VOICES] = {
		(bool)pThis->v[kParamGateAMode],
		(bool)pThis->v[kParamGateBMode],
		(bool)pThis->v[kParamGateCMode],
	};
	float* cvOut[NUM_VOICES] = {
		outputBus(pThis, kParamCVAOut, busFrames, numFrames),
		outputBus(pThis, kParamCVBOut, busFrames, numFrames),
		outputBus(pThis, kParamCVCOut, busFrames, numFrames),
	};
	bool cvMode[NUM_VOICES] = {
		(bool)pThis->v[kParamCVAMode],
		(bool)pThis->v[kParamCVBMode],
		(bool)pThis->v[kParamCVCMode],
	};
	float* minOut = outputBus(pThis, kParamMinOut, busFrames, numFrames);
	float* midOut = outputBus(pThis, kParamMidOut, busFrames, numFrames);
	float* maxOut = outputBus(pThis, kParamMaxOut, busFrames, numFrames);
	bool minMode = pThis->v[kParamMinMode];
	bool midMode = pThis->v[kParamMidMode];
	bool maxMode = pThis->v[kParamMaxMode];

	// UI-driven randomize: fire once on the 0→1 edge of kParamRandomizeNow,
	// then reset the param back to 0. Doing this here in step() rather than
	// in parameterChanged avoids re-entrancy crashes from calling
	// NT_setParameterFromUi inside a parameter-change notification.
	int rNow = pThis->v[kParamRandomizeNow];
	if (rNow != 0 && pThis->lastRandomizeNow == 0) {
		uint32_t algIdx = NT_algorithmIndex(self);
		uint32_t off = NT_parameterOffset();
		randomizePitches(pThis, algIdx, off);
		NT_setParameterFromUi(algIdx, kParamRandomizeNow + off, 0);
	}
	pThis->lastRandomizeNow = rNow;

	// Static params for this block
	int rootBase  = pThis->v[kParamRoot];
	int scaleBase = pThis->v[kParamScale];
	int stepsBase = pThis->v[kParamSteps];
	float slewBase = (float)pThis->v[kParamSlew] / 100.f;
	float faderRange = RANGE_VALUES[pThis->v[kParamFaderRange]];
	bool harmonicLock = pThis->v[kParamHarmonicLock];
	bool sampleHold   = pThis->v[kParamSampleHold];

	for (int f = 0; f < numFrames; f++) {
		// Reset
		float rstV = rstIn ? rstIn[f] : 0.f;
		if (pThis->resetTrigger.process(rstV)) {
			for (int v = 0; v < NUM_VOICES; v++) {
				VoiceState& vs = pThis->voices[v];
				vs.currentStep = 0;
				vs.stepCounter = 0;
				vs.sleeping = false;
				vs.sleepCounter = 0;
				vs.sampleHoldHolding = false;
				vs.firstClockPending = true;
			}
		}

		// Randomize trigger: scramble all 8 pitches.
		float rndV = randIn ? randIn[f] : 0.f;
		if (pThis->randomizeTrigger.process(rndV)) {
			randomizePitches(pThis, NT_algorithmIndex(self), NT_parameterOffset());
		}

		// Per-voice processing
		float voiceVoltages[NUM_VOICES];
		for (int v = 0; v < NUM_VOICES; v++) {
			VoiceState& vs = pThis->voices[v];
			const float* clk = normalledClock(v, clkA, clkB, clkC);
			float clkV = clk ? clk[f] : 0.f;

			vs.clockHigh = (clkV >= 1.f);
			vs.clockTimer += sampleTime;

			bool clockRose = vs.clockTrigger.process(clkV);
			if (clockRose) {
				if (vs.clockTimer > 0.001f) vs.clockPeriod = vs.clockTimer;
				vs.clockTimer = 0.f;

				// CV-modulated per-voice values (sampled at clock event)
				int rootNote = rootBase + (rootCV ? (int)roundf(rootCV[f]) : 0);
				rootNote = ((rootNote % 12) + 12) % 12;

				int scaleIdx = scaleBase + (scaleCV ? (int)roundf(scaleCV[f]) : 0);
				scaleIdx = clampi(scaleIdx, 0, NUM_SCALES - 1);

				int globalSteps = stepsBase + (stepsCV ? (int)roundf(stepsCV[f]) : 0);
				globalSteps = clampi(globalSteps, 1, NUM_STEPS);

				float slewPercent = slewBase + (slewCV ? slewCV[f] / 5.f : 0.f);
				slewPercent = clampf(slewPercent, 0.f, 1.f);

				int voiceSteps = pThis->v[VOICE_STEPS(v)]
					+ (vStepsCV[v] ? (int)roundf(vStepsCV[v][f]) : 0);
				voiceSteps = clampi(voiceSteps, 1, globalSteps);

				int sleepIdx = pThis->v[VOICE_SLEEP(v)]
					+ (vSleepCV[v] ? (int)roundf(vSleepCV[v][f]) : 0);
				sleepIdx = clampi(sleepIdx, 0, 9);
				int sleepDiv = SLEEP_VALUES[sleepIdx];

				float prob = (float)pThis->v[VOICE_PROB(v)] / 100.f
					+ (vProbCV[v] ? vProbCV[v][f] / 5.f : 0.f);
				prob = clampf(prob, 0.f, 1.f);

				float wander = (float)pThis->v[VOICE_WANDER(v)] / 100.f
					+ (wandCV[v] ? wandCV[v][f] / 5.f : 0.f);
				wander = clampf(wander, 0.f, 1.f);

				if (vs.sleeping) {
					vs.sleepCounter--;
					if (vs.sleepCounter <= 0) vs.sleeping = false;
					// don't advance while sleeping
				} else {
					if (vs.firstClockPending) {
						vs.firstClockPending = false;
					} else {
						vs.stepCounter++;
						vs.currentStep++;
						if (vs.currentStep >= voiceSteps) {
							vs.currentStep = 0;
							if (sleepDiv > 0) {
								vs.sleeping = true;
								vs.sleepCounter = sleepDiv;
							}
						}
					}

					// Probability roll
					if (prob < 1.f) {
						float roll = (float)(xorshift32(pThis->probRng) & 0x7FFFFFFF) / (float)0x7FFFFFFF;
						vs.probGateSuppress = (roll >= prob);
					} else {
						vs.probGateSuppress = false;
					}

					advanceVoice(pThis, v, rootNote, scaleIdx, faderRange,
						slewPercent, voiceSteps, wander, harmonicLock);

					// S&H: in sample-hold mode, voltage only commits on an
					// accepted clock with an active gate toggle.
					if (sampleHold) {
						bool toggleOn = pThis->v[STEP_GATE(vs.currentStep, v)] > 0;
						if (toggleOn && !vs.probGateSuppress) {
							vs.currentVoltage = vs.targetVoltage;
							vs.sampleHoldHolding = true;
						}
					}
				}
			}

			// Slew (skipped in S&H mode — voltage only changes on accepted clocks)
			if (!sampleHold && !vs.sleeping) {
				if (vs.slewRate <= 0.f) {
					vs.currentVoltage = vs.targetVoltage;
				} else {
					float diff = vs.targetVoltage - vs.currentVoltage;
					float maxStep = vs.slewRate * sampleTime;
					if (fabsf(diff) <= maxStep) {
						vs.currentVoltage = vs.targetVoltage;
					} else {
						vs.currentVoltage += (diff > 0.f ? maxStep : -maxStep);
					}
				}
			}

			voiceVoltages[v] = vs.currentVoltage;

			// Gate output
			bool toggleOn = pThis->v[STEP_GATE(vs.currentStep, v)] > 0;
			bool gateActive = vs.clockHigh && toggleOn && !vs.sleeping && !vs.probGateSuppress;
			float gateV = gateActive ? 10.f : 0.f;
			if (gateOut[v]) writeOut(gateOut[v], f, gateMode[v], gateV);

			// CV output
			if (cvOut[v]) writeOut(cvOut[v], f, cvMode[v], vs.currentVoltage);
		}

		// Min / Mid / Max
		float v0 = voiceVoltages[0], v1 = voiceVoltages[1], v2 = voiceVoltages[2];
		float mn = v0 < v1 ? (v0 < v2 ? v0 : v2) : (v1 < v2 ? v1 : v2);
		float mx = v0 > v1 ? (v0 > v2 ? v0 : v2) : (v1 > v2 ? v1 : v2);
		float md = v0 + v1 + v2 - mn - mx;
		if (minOut) writeOut(minOut, f, minMode, mn);
		if (midOut) writeOut(midOut, f, midMode, md);
		if (maxOut) writeOut(maxOut, f, maxMode, mx);
	}
}

// ─── draw ────────────────────────────────────────────────────────────────────

static void noteName(float voltage, char* out) {
	static const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
	int totalSemis = (int)roundf(voltage * 12.f);
	int oct = 4 + (totalSemis / 12);
	int idx = ((totalSemis % 12) + 12) % 12;
	if (totalSemis < 0 && idx != 0) oct--;
	const char* nm = names[idx];
	out[0] = nm[0];
	int pos = 1;
	if (nm[1]) out[pos++] = nm[1];
	if (oct < 0) { out[pos++] = '-'; oct = -oct; }
	if (oct >= 10) out[pos++] = '0' + (oct/10);
	out[pos++] = '0' + (oct % 10);
	out[pos] = 0;
}

bool draw(_NT_algorithm* self) {
	_fugueNT* pThis = (_fugueNT*)self;
	int scaleIdx = pThis->v[kParamScale];
	int root = pThis->v[kParamRoot];
	float faderRange = RANGE_VALUES[pThis->v[kParamFaderRange]];
	bool hLock = pThis->v[kParamHarmonicLock];
	bool sh    = pThis->v[kParamSampleHold];

	// Top status line
	static const char* rootNames[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
	static const char* rangeNames[] = {"1V","2V","5V"};
	char buf[48];
	int rngIdx = pThis->v[kParamFaderRange];
	int len = 0;
	const char* rn = rootNames[root];
	while (rn[len]) { buf[len] = rn[len]; len++; }
	buf[len++] = ' ';
	const char* sn = enumScale[scaleIdx];
	int snLen = 0;
	while (sn[snLen] && len < 30) { buf[len++] = sn[snLen++]; }
	buf[len++] = ' '; buf[len++] = ' ';
	buf[len++] = rangeNames[rngIdx][0];
	buf[len++] = rangeNames[rngIdx][1];
	if (hLock) { buf[len++] = ' '; buf[len++] = 'H'; }
	if (sh)    { buf[len++] = ' '; buf[len++] = 'S'; }
	buf[len] = 0;
	NT_drawText(2, 8, buf, 15, kNT_textLeft, kNT_textNormal);

	// 8-column step grid
	const int gridY0 = 14;
	const int gridY1 = 46;
	const int colW = 28;
	const int colGap = 4;
	const int gridH = gridY1 - gridY0;
	int x0 = 4;

	int globalSteps = pThis->v[kParamSteps];
	for (int s = 0; s < NUM_STEPS; s++) {
		int x = x0 + s * (colW + colGap);
		bool inRange = (s < globalSteps);
		// Outline
		int outlineColor = (s == pThis->selectedStep) ? 15 : (inRange ? 6 : 3);
		NT_drawShapeI(kNT_box, x, gridY0, x + colW, gridY1, outlineColor);

		// Pitch fill bar
		float pitch = (float)pThis->v[STEP_PITCH(s)] / 1000.f;
		int barH = (int)(pitch * (gridH - 2));
		if (barH > 0) {
			NT_drawShapeI(kNT_rectangle, x + 1, gridY1 - 1 - barH, x + colW - 1, gridY1 - 1,
				inRange ? 10 : 4);
		}

		// Quantized note label, top-right inside the column.
		// Drawn after the bar so it overlays at high pitches.
		float quantVolt = faderToVoltage(pitch, root, scaleIdx, faderRange);
		char noteBuf[8];
		noteName(quantVolt, noteBuf);
		NT_drawText(x + colW - 2, gridY0 + 6, noteBuf,
			inRange ? 15 : 6, kNT_textRight, kNT_textTiny);

		// Per-voice playhead markers above the column
		for (int v = 0; v < NUM_VOICES; v++) {
			if (pThis->voices[v].currentStep == s && s < globalSteps) {
				int ty = gridY0 - 4 + v;  // stacked A,B,C
				NT_drawShapeI(kNT_rectangle, x + 2, ty, x + colW - 2, ty, 15);
			}
		}

		// Gate A/B/C boxes below the bar.
		// Filled rect when voice's gate is enabled on this step, outline only when off.
		const int boxY0 = gridY1 + 1;    // 47
		const int boxY1 = boxY0 + 7;     // 54  (8-pixel-tall box)
		const int boxW  = 7;
		const int boxGap = 1;
		const int boxesTotal = NUM_VOICES * boxW + (NUM_VOICES - 1) * boxGap;
		const int boxesX0 = x + (colW - boxesTotal) / 2;
		for (int v = 0; v < NUM_VOICES; v++) {
			bool on = pThis->v[STEP_GATE(s, v)] > 0;
			int bx0 = boxesX0 + v * (boxW + boxGap);
			int bx1 = bx0 + boxW - 1;
			char letter[2] = { (char)('A' + v), 0 };
			int cx = bx0 + boxW / 2 + 1;  // tiny-text centre needs +1 nudge
			int by = boxY1 - 1;  // baseline for tiny font inside box
			if (on) {
				NT_drawShapeI(kNT_rectangle, bx0, boxY0, bx1, boxY1, 12);
				NT_drawText(cx, by, letter, 0, kNT_textCentre, kNT_textTiny);
			} else {
				NT_drawShapeI(kNT_box, bx0, boxY0, bx1, boxY1, 6);
				NT_drawText(cx, by, letter, 10, kNT_textCentre, kNT_textTiny);
			}
		}
	}

	// Bottom per-voice status
	for (int v = 0; v < NUM_VOICES; v++) {
		char nb[8];
		noteName(pThis->voices[v].currentVoltage, nb);
		char line[24];
		int p = 0;
		line[p++] = 'A' + v; line[p++] = ':'; line[p++] = ' ';
		int nl = 0; while (nb[nl]) { line[p++] = nb[nl++]; }
		if (pThis->voices[v].sleeping) { line[p++] = ' '; line[p++] = 'z'; }
		else if (pThis->voices[v].probGateSuppress) { line[p++] = ' '; line[p++] = '?'; }
		line[p] = 0;
		NT_drawText(2 + v * 86, 63, line, 12, kNT_textLeft, kNT_textTiny);
	}

	return true;  // suppress standard parameter line
}

// ─── Custom UI ───────────────────────────────────────────────────────────────

uint32_t hasCustomUi(_NT_algorithm* /*self*/) {
	return kNT_potL | kNT_potC | kNT_encoderL | kNT_encoderR
	     | kNT_button1 | kNT_button2;
}

void customUi(_NT_algorithm* self, const _NT_uiData& data) {
	_fugueNT* pThis = (_fugueNT*)self;
	uint32_t algIdx = NT_algorithmIndex(self);
	uint32_t off = NT_parameterOffset();

	// Encoder L: step selector
	if (data.encoders[0]) {
		int s = pThis->selectedStep + data.encoders[0];
		if (s < 0) s = 0;
		if (s >= NUM_STEPS) s = NUM_STEPS - 1;
		pThis->selectedStep = s;
	}
	// Encoder R: focus voice
	if (data.encoders[1]) {
		int v = pThis->focusVoice + data.encoders[1];
		if (v < 0) v = 0;
		if (v >= NUM_VOICES) v = NUM_VOICES - 1;
		pThis->focusVoice = v;
	}

	// Pot L: pitch of selected step
	if (data.controls & kNT_potL) {
		int16_t val = (int16_t)(roundf(data.pots[0] * 1000.f));
		NT_setParameterFromUi(algIdx, STEP_PITCH(pThis->selectedStep) + off, val);
	}
	// Pot C: wander of focused voice
	if (data.controls & kNT_potC) {
		int16_t val = (int16_t)(roundf(data.pots[1] * 100.f));
		NT_setParameterFromUi(algIdx, VOICE_WANDER(pThis->focusVoice) + off, val);
	}

	// Button 1 (press, on rising edge): toggle gate of focused voice on selected step
	bool b1Now = (data.controls & kNT_button1);
	bool b1Was = (data.lastButtons & kNT_button1);
	if (b1Now && !b1Was) {
		int p = STEP_GATE(pThis->selectedStep, pThis->focusVoice);
		NT_setParameterFromUi(algIdx, p + off, pThis->v[p] ? 0 : 1);
	}
	// Button 2 (press): cycle page
	bool b2Now = (data.controls & kNT_button2);
	bool b2Was = (data.lastButtons & kNT_button2);
	if (b2Now && !b2Was) {
		pThis->currentPage = (pThis->currentPage + 1) % 2;
	}
}

void setupUi(_NT_algorithm* self, _NT_float3& pots) {
	_fugueNT* pThis = (_fugueNT*)self;
	pots[0] = (float)pThis->v[STEP_PITCH(pThis->selectedStep)] / 1000.f;
	pots[1] = (float)pThis->v[VOICE_WANDER(pThis->focusVoice)] / 100.f;
	pots[2] = 0.5f;
}

// ─── Serialisation ───────────────────────────────────────────────────────────
// All algorithm params auto-persist; we record schema version + UI state.

static const int kSchemaVersion = 1;

void serialise(_NT_algorithm* self, _NT_jsonStream& stream) {
	_fugueNT* pThis = (_fugueNT*)self;
	stream.addMemberName("schemaVersion");
	stream.addNumber(kSchemaVersion);
	stream.addMemberName("selectedStep");
	stream.addNumber(pThis->selectedStep);
	stream.addMemberName("focusVoice");
	stream.addNumber(pThis->focusVoice);
	stream.addMemberName("currentPage");
	stream.addNumber(pThis->currentPage);
}

bool deserialise(_NT_algorithm* self, _NT_jsonParse& parse) {
	_fugueNT* pThis = (_fugueNT*)self;
	int num;
	if (!parse.numberOfObjectMembers(num)) return false;
	for (int i = 0; i < num; i++) {
		if (parse.matchName("schemaVersion")) {
			int v = 0;
			if (!parse.number(v)) return false;
		} else if (parse.matchName("selectedStep")) {
			int v = 0;
			if (!parse.number(v)) return false;
			pThis->selectedStep = clampi(v, 0, NUM_STEPS - 1);
		} else if (parse.matchName("focusVoice")) {
			int v = 0;
			if (!parse.number(v)) return false;
			pThis->focusVoice = clampi(v, 0, NUM_VOICES - 1);
		} else if (parse.matchName("currentPage")) {
			int v = 0;
			if (!parse.number(v)) return false;
			pThis->currentPage = clampi(v, 0, 1);
		} else {
			if (!parse.skipMember()) return false;
		}
	}
	return true;
}

// ─── Factory ─────────────────────────────────────────────────────────────────

static const _NT_factory factory = {
	.guid = NT_MULTICHAR('S','F','f','N'),  // "SFfN" - Signal Function Set: Fugue NT
	.name = "FugueNT",
	.description = "3-voice harmonic deviation sequencer (port of SFS Fugue + X)",
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
	.tags = kNT_tagUtility,
	.hasCustomUi = hasCustomUi,
	.customUi = customUi,
	.setupUi = setupUi,
	.serialise = serialise,
	.deserialise = deserialise,
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
