// ShiftNT — port of Signal Function Set's Shift (4-output CV shift register
// with per-output controls) for the Disting NT. See docs/shift-nt.md.

#include <math.h>
#include <new>
#include <string.h>
#include <distingnt/api.h>
#include <distingnt/serialisation.h>

// ─── Constants ───────────────────────────────────────────────────────────────

static const int NUM_OUTS  = 4;
static const int MAX_N     = 16;   // delay-line / history ring size
static const int MAX_STEPS = 15;   // max selectable delay in steps; 0 = passthrough

// Per-lane clock divider values, selected by an enum parameter.
static const int DIV_VALUES[6] = { 1, 2, 3, 4, 5, 8 };
static const int NUM_DIV_VALUES = 6;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static inline uint32_t xorshift32(uint32_t& state) {
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
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

// ─── Algorithm state ─────────────────────────────────────────────────────────

struct _shiftNT : public _NT_algorithm {
	_shiftNT() {}
	~_shiftNT() {}

	Schmitt clockTrigger;
	Schmitt resetTrigger;

	// Per-lane state
	int   divCounter[NUM_OUTS];      // counts input clocks toward the lane's divider
	float held[NUM_OUTS];            // current output CV (S&H)
	int   gateSamples[NUM_OUTS];     // samples remaining in the 1ms gate pulse

	// Active delay-line buffer (parallel mode) / tape-loop buffer (cascade mode).
	// Live slots are 0..targetN[i]-1 of the row.
	float delayLine[NUM_OUTS][MAX_N];
	int   readIdx[NUM_OUTS];
	int   writeIdx[NUM_OUTS];

	// Full-depth history ring (always MAX_N slots), used to keep the output
	// playing when the CV input is disconnected.
	float historyLine[NUM_OUTS][MAX_N];
	int   historyReadIdx[NUM_OUTS];
	int   historyWriteIdx[NUM_OUTS];

	// Jumble
	float jumbleHeld;
	int   jumbleGateSamples;
	uint32_t rng;

	// Edge-detect state for the Reset Now UI param. -1 = sentinel.
	int lastResetNow;
};

// ─── Parameter enum ──────────────────────────────────────────────────────────
//
// Inputs first, then output bus + mode pairs, then algorithm params.

enum {
	// 8 input bus params (1 entry each)
	kParamCVIn = 0,
	kParamClockIn,
	kParamNCV,
	kParamResetIn,
	kParamStepCVA,
	kParamStepCVB,
	kParamStepCVC,
	kParamStepCVD,

	// 10 output bus params (WITH_MODE = 2 entries each, 20 total).
	// Per-lane order is gate-then-CV (adjacent, gate=N / CV=N+1).
	kParamGateAOut, kParamGateAMode,
	kParamCVAOut,   kParamCVAMode,
	kParamGateBOut, kParamGateBMode,
	kParamCVBOut,   kParamCVBMode,
	kParamGateCOut, kParamGateCMode,
	kParamCVCOut,   kParamCVCMode,
	kParamGateDOut, kParamGateDMode,
	kParamCVDOut,   kParamCVDMode,
	kParamJumbleGateOut, kParamJumbleGateMode,
	kParamJumbleCVOut,   kParamJumbleCVMode,

	// Per-lane controls
	kParamNA, kParamModeA, kParamDivA,
	kParamNB, kParamModeB, kParamDivB,
	kParamNC, kParamModeC, kParamDivC,
	kParamND, kParamModeD, kParamDivD,

	// UI action
	kParamResetNow,

	NUM_PARAMS
};

#define LANE_N(i)    (kParamNA    + (i) * 3)
#define LANE_MODE(i) (kParamModeA + (i) * 3)
#define LANE_DIV(i)  (kParamDivA  + (i) * 3)

// ─── Enum strings ────────────────────────────────────────────────────────────

static char const * const enumMode[]  = { "Parallel", "Cascade" };
static char const * const enumDiv[]   = { "/1", "/2", "/3", "/4", "/5", "/8" };
static char const * const enumOffOn[] = { "Off", "On" };

// ─── Parameter table ─────────────────────────────────────────────────────────

static const _NT_parameter parameters[] = {
	NT_PARAMETER_CV_INPUT( "CV",        0, 1 )
	NT_PARAMETER_CV_INPUT( "Clock",     0, 2 )
	NT_PARAMETER_CV_INPUT( "N CV",      0, 0 )
	NT_PARAMETER_CV_INPUT( "Reset",     0, 0 )
	NT_PARAMETER_CV_INPUT( "Step CV A", 0, 0 )
	NT_PARAMETER_CV_INPUT( "Step CV B", 0, 0 )
	NT_PARAMETER_CV_INPUT( "Step CV C", 0, 0 )
	NT_PARAMETER_CV_INPUT( "Step CV D", 0, 0 )

	// Outputs: gate=N, CV=N+1 per lane. Jumble defaults to aux busses 21/22.
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Gate A",       0, 13 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "CV A",         0, 14 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Gate B",       0, 15 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "CV B",         0, 16 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Gate C",       0, 17 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "CV C",         0, 18 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Gate D",       0, 19 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "CV D",         0, 20 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Jumble Gate",  0, 0 )
	NT_PARAMETER_CV_OUTPUT_WITH_MODE( "Jumble CV",    0, 0 )

	// Per-lane: N (0 = passthrough), Mode, Div
	{ .name = "N A",    .min = 0, .max = MAX_STEPS,        .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = NULL },
	{ .name = "Mode A", .min = 0, .max = 1,                .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumMode },
	{ .name = "Div A",  .min = 0, .max = NUM_DIV_VALUES-1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumDiv },

	{ .name = "N B",    .min = 0, .max = MAX_STEPS,        .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = NULL },
	{ .name = "Mode B", .min = 0, .max = 1,                .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumMode },
	{ .name = "Div B",  .min = 0, .max = NUM_DIV_VALUES-1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumDiv },

	{ .name = "N C",    .min = 0, .max = MAX_STEPS,        .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = NULL },
	{ .name = "Mode C", .min = 0, .max = 1,                .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumMode },
	{ .name = "Div C",  .min = 0, .max = NUM_DIV_VALUES-1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumDiv },

	{ .name = "N D",    .min = 0, .max = MAX_STEPS,        .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = NULL },
	{ .name = "Mode D", .min = 0, .max = 1,                .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumMode },
	{ .name = "Div D",  .min = 0, .max = NUM_DIV_VALUES-1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumDiv },

	// UI action: Off→On clears state (same effect as Reset trigger).
	// No auto-reset (writing same param crashes the host).
	{ .name = "Reset Now", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOffOn },
};

// ─── Parameter pages ─────────────────────────────────────────────────────────

static const uint8_t pageLaneA[] = { kParamNA, kParamModeA, kParamDivA };
static const uint8_t pageLaneB[] = { kParamNB, kParamModeB, kParamDivB };
static const uint8_t pageLaneC[] = { kParamNC, kParamModeC, kParamDivC };
static const uint8_t pageLaneD[] = { kParamND, kParamModeD, kParamDivD };
static const uint8_t pageAction[] = { kParamResetNow };
static const uint8_t pageRouting[] = {
	// Outputs paired gate→CV per lane, then jumble
	kParamGateAOut, kParamCVAOut,
	kParamGateBOut, kParamCVBOut,
	kParamGateCOut, kParamCVCOut,
	kParamGateDOut, kParamCVDOut,
	kParamJumbleGateOut, kParamJumbleCVOut,
	// Inputs
	kParamCVIn, kParamClockIn, kParamNCV, kParamResetIn,
	kParamStepCVA, kParamStepCVB, kParamStepCVC, kParamStepCVD,
};

static const _NT_parameterPage pages[] = {
	{ .name = "Lane A",  .numParams = ARRAY_SIZE(pageLaneA),  .group = 1, .params = pageLaneA },
	{ .name = "Lane B",  .numParams = ARRAY_SIZE(pageLaneB),  .group = 1, .params = pageLaneB },
	{ .name = "Lane C",  .numParams = ARRAY_SIZE(pageLaneC),  .group = 1, .params = pageLaneC },
	{ .name = "Lane D",  .numParams = ARRAY_SIZE(pageLaneD),  .group = 1, .params = pageLaneD },
	{ .name = "Action",  .numParams = ARRAY_SIZE(pageAction), .group = 2, .params = pageAction },
	{ .name = "Routing", .numParams = ARRAY_SIZE(pageRouting),.group = 3, .params = pageRouting },
};

static const _NT_parameterPages parameterPages = {
	.numPages = ARRAY_SIZE(pages),
	.pages = pages,
};

// ─── Clear all internal state ────────────────────────────────────────────────

static void shiftClearAll(_shiftNT* p) {
	for (int i = 0; i < NUM_OUTS; i++) {
		p->divCounter[i] = 0;
		p->held[i] = 0.f;
		p->gateSamples[i] = 0;
		p->readIdx[i] = 0;
		p->writeIdx[i] = 0;
		p->historyReadIdx[i] = 0;
		p->historyWriteIdx[i] = 0;
		for (int k = 0; k < MAX_N; k++) {
			p->delayLine[i][k] = 0.f;
			p->historyLine[i][k] = 0.f;
		}
	}
	p->jumbleHeld = 0.f;
	p->jumbleGateSamples = 0;
}

// ─── Construct ───────────────────────────────────────────────────────────────

void calculateRequirements(_NT_algorithmRequirements& req, const int32_t* /*spec*/) {
	req.numParameters = NUM_PARAMS;
	req.sram = sizeof(_shiftNT);
	req.dram = 0;
	req.dtc = 0;
	req.itc = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs,
                         const _NT_algorithmRequirements& /*req*/,
                         const int32_t* /*spec*/) {
	_shiftNT* alg = new (ptrs.sram) _shiftNT();
	alg->parameters = parameters;
	alg->parameterPages = &parameterPages;
	alg->clockTrigger.reset();
	alg->resetTrigger.reset();
	alg->rng = 0xCAFEBABEu;
	alg->lastResetNow = -1;
	shiftClearAll(alg);
	return alg;
}

// ─── Bus helpers ─────────────────────────────────────────────────────────────

static inline const float* inputBus(const _shiftNT* p, int param,
                                    float* busFrames, int numFrames) {
	int bus = p->v[param];
	return bus > 0 ? busFrames + (bus - 1) * numFrames : NULL;
}
static inline float* outputBus(const _shiftNT* p, int param,
                               float* busFrames, int numFrames) {
	int bus = p->v[param];
	return bus > 0 ? busFrames + (bus - 1) * numFrames : NULL;
}
static inline void writeOut(float* out, int frame, bool replace, float v) {
	if (replace) out[frame] = v;
	else         out[frame] += v;
}

// ─── step ────────────────────────────────────────────────────────────────────

void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
	_shiftNT* p = (_shiftNT*)self;
	int numFrames = numFramesBy4 * 4;

	// Edge-detect Reset Now param (Off→On). No state write — just clear.
	int rstNow = p->v[kParamResetNow];
	if (rstNow != 0 && p->lastResetNow == 0) {
		shiftClearAll(p);
	}
	p->lastResetNow = rstNow;

	// Hoist all routing pointers and per-block static params.
	const float* cvIn    = inputBus(p, kParamCVIn,    busFrames, numFrames);
	const float* clockIn = inputBus(p, kParamClockIn, busFrames, numFrames);
	const float* nCV     = inputBus(p, kParamNCV,     busFrames, numFrames);
	const float* rstIn   = inputBus(p, kParamResetIn, busFrames, numFrames);
	const float* stepCV[NUM_OUTS] = {
		inputBus(p, kParamStepCVA, busFrames, numFrames),
		inputBus(p, kParamStepCVB, busFrames, numFrames),
		inputBus(p, kParamStepCVC, busFrames, numFrames),
		inputBus(p, kParamStepCVD, busFrames, numFrames),
	};

	float* gateOut[NUM_OUTS] = {
		outputBus(p, kParamGateAOut, busFrames, numFrames),
		outputBus(p, kParamGateBOut, busFrames, numFrames),
		outputBus(p, kParamGateCOut, busFrames, numFrames),
		outputBus(p, kParamGateDOut, busFrames, numFrames),
	};
	float* cvOut[NUM_OUTS] = {
		outputBus(p, kParamCVAOut, busFrames, numFrames),
		outputBus(p, kParamCVBOut, busFrames, numFrames),
		outputBus(p, kParamCVCOut, busFrames, numFrames),
		outputBus(p, kParamCVDOut, busFrames, numFrames),
	};
	bool gateMode[NUM_OUTS] = {
		(bool)p->v[kParamGateAMode], (bool)p->v[kParamGateBMode],
		(bool)p->v[kParamGateCMode], (bool)p->v[kParamGateDMode],
	};
	bool cvMode[NUM_OUTS] = {
		(bool)p->v[kParamCVAMode], (bool)p->v[kParamCVBMode],
		(bool)p->v[kParamCVCMode], (bool)p->v[kParamCVDMode],
	};
	float* jumbleGate = outputBus(p, kParamJumbleGateOut, busFrames, numFrames);
	float* jumbleCV   = outputBus(p, kParamJumbleCVOut,   busFrames, numFrames);
	bool jumbleGateMode = p->v[kParamJumbleGateMode];
	bool jumbleCVMode   = p->v[kParamJumbleCVMode];

	int   nPot[NUM_OUTS]   = { p->v[kParamNA], p->v[kParamNB], p->v[kParamNC], p->v[kParamND] };
	bool  cascade[NUM_OUTS] = {
		(bool)p->v[kParamModeA], (bool)p->v[kParamModeB],
		(bool)p->v[kParamModeC], (bool)p->v[kParamModeD],
	};
	int   laneDiv[NUM_OUTS] = {
		DIV_VALUES[clampi(p->v[kParamDivA], 0, NUM_DIV_VALUES - 1)],
		DIV_VALUES[clampi(p->v[kParamDivB], 0, NUM_DIV_VALUES - 1)],
		DIV_VALUES[clampi(p->v[kParamDivC], 0, NUM_DIV_VALUES - 1)],
		DIV_VALUES[clampi(p->v[kParamDivD], 0, NUM_DIV_VALUES - 1)],
	};

	const int gatePulseSamples = (int)(0.001f * (float)NT_globals.sampleRate);
	bool cvConnected = (cvIn != NULL);

	for (int f = 0; f < numFrames; f++) {
		// Reset (CV trigger). The clearAll wipes counters too — process this
		// before the clock so a coincident clock/reset still starts fresh.
		float rstV = rstIn ? rstIn[f] : 0.f;
		if (p->resetTrigger.process(rstV)) {
			shiftClearAll(p);
		}

		// Clock rising edge: do all the per-lane shift work.
		float clkV = clockIn ? clockIn[f] : 0.f;
		bool clockRose = p->clockTrigger.process(clkV);

		if (clockRose) {
			// Compute effective N per lane (knob + global N CV + per-lane Step CV).
			// CV scaling is ±5V → ±MAX_STEPS so a full ±5V sweep covers the whole
			// 0..MAX_STEPS range. Clamped to [0, MAX_STEPS]; 0 = passthrough.
			float nCvOffset = nCV ? nCV[f] * (float)MAX_STEPS / 5.f : 0.f;
			int targetN[NUM_OUTS];
			for (int i = 0; i < NUM_OUTS; i++) {
				float n = (float)nPot[i] + nCvOffset;
				if (stepCV[i]) n += stepCV[i][f] * (float)MAX_STEPS / 5.f;
				targetN[i] = clampi((int)roundf(n), 0, MAX_STEPS);
			}

			float inCV = cvIn ? cvIn[f] : 0.f;
			bool tickFired[NUM_OUTS] = { false, false, false, false };
			bool laneTick[NUM_OUTS]  = { false, false, false, false };

			for (int i = 0; i < NUM_OUTS; i++) {
				// Per-lane divider: only proceed when this lane's counter wraps.
				p->divCounter[i]++;
				if (p->divCounter[i] < laneDiv[i]) continue;
				p->divCounter[i] = 0;
				laneTick[i] = true;

				int N = targetN[i];

				if (cvConnected) {
					if (cascade[i] && i > 0) {
						// Cascade tape loop. Read continuously at lane-clock
						// rate; write only when parent fired this sample.
						// N==0 = zero-length loop: pass the parent straight through.
						if (N == 0) {
							p->held[i] = p->held[i - 1];
							if (tickFired[i - 1]) {
								p->historyLine[i][p->historyWriteIdx[i]] = p->held[i - 1];
								p->historyWriteIdx[i] = (p->historyWriteIdx[i] + 1) % MAX_N;
								tickFired[i] = true;
							}
						} else {
							int rIdx = p->readIdx[i] % N;
							p->held[i] = p->delayLine[i][rIdx];
							p->readIdx[i] = (rIdx + 1) % N;

							if (tickFired[i - 1]) {
								int wIdx = p->writeIdx[i] % N;
								p->delayLine[i][wIdx] = p->held[i - 1];
								p->writeIdx[i] = (wIdx + 1) % N;
								p->historyLine[i][p->historyWriteIdx[i]] = p->held[i - 1];
								p->historyWriteIdx[i] = (p->historyWriteIdx[i] + 1) % MAX_N;
								tickFired[i] = true;
							}
						}
					} else {
						// Parallel (or cascade-on-A): read from the always-
						// written full-depth history ring at lookback = N,
						// then write the current input. Reading from the
						// continuously-written ring (rather than an N-sized
						// buffer) keeps the delay correct even when N is
						// modulated on the fly — an N-sized buffer's slots
						// go stale as N changes and the output freezes.
						// N==0 = no delay: input passes straight through.
						if (N == 0) {
							p->held[i] = inCV;
						} else {
							int rIdx = (p->historyWriteIdx[i] - N + MAX_N) % MAX_N;
							p->held[i] = p->historyLine[i][rIdx];
						}
						p->historyLine[i][p->historyWriteIdx[i]] = inCV;
						p->historyWriteIdx[i] = (p->historyWriteIdx[i] + 1) % MAX_N;
						tickFired[i] = true;
					}
				} else {
					// Disconnected: cycle through the full-depth history ring.
					int hIdx = p->historyReadIdx[i] % MAX_N;
					p->held[i] = p->historyLine[i][hIdx];
					p->historyReadIdx[i] = (hIdx + 1) % MAX_N;
				}

				// Gate fires on the lane-clock tick regardless of CV connection.
				p->gateSamples[i] = gatePulseSamples;
			}

			// Jumble: pick a random lane, sample its held value, fire jumble gate.
			int pick = (int)(((uint64_t)xorshift32(p->rng) * NUM_OUTS) >> 32);
			if (pick < 0)         pick = 0;
			if (pick >= NUM_OUTS) pick = NUM_OUTS - 1;
			p->jumbleHeld = p->held[pick];
			p->jumbleGateSamples = gatePulseSamples;
			// Silence the unused laneTick array to avoid "set but not used"
			// warnings — the values are part of the original semantics but
			// gate output is keyed on p->gateSamples now.
			(void)laneTick;
		}

		// Write outputs (and decay gate pulse counters).
		for (int i = 0; i < NUM_OUTS; i++) {
			float gateV = p->gateSamples[i] > 0 ? 10.f : 0.f;
			if (gateOut[i]) writeOut(gateOut[i], f, gateMode[i], gateV);
			if (cvOut[i])   writeOut(cvOut[i],   f, cvMode[i],   p->held[i]);
			if (p->gateSamples[i] > 0) p->gateSamples[i]--;
		}
		float jGateV = p->jumbleGateSamples > 0 ? 10.f : 0.f;
		if (jumbleGate) writeOut(jumbleGate, f, jumbleGateMode, jGateV);
		if (jumbleCV)   writeOut(jumbleCV,   f, jumbleCVMode,   p->jumbleHeld);
		if (p->jumbleGateSamples > 0) p->jumbleGateSamples--;
	}
}

// ─── Draw ────────────────────────────────────────────────────────────────────

// Small helper: format a signed voltage as e.g. "+1.23" / "-0.50" into a buf.
static void fmtVolt(float v, char* buf) {
	if (v < 0) {
		buf[0] = '-';
		v = -v;
	} else {
		buf[0] = '+';
	}
	int whole = (int)v;
	int frac = (int)((v - (float)whole) * 100.f + 0.5f);
	if (frac >= 100) { frac -= 100; whole++; }
	if (whole >= 10) {
		buf[1] = '0' + (whole / 10);
		buf[2] = '0' + (whole % 10);
		buf[3] = '.';
		buf[4] = '0' + (frac / 10);
		buf[5] = '0' + (frac % 10);
		buf[6] = 0;
	} else {
		buf[1] = '0' + whole;
		buf[2] = '.';
		buf[3] = '0' + (frac / 10);
		buf[4] = '0' + (frac % 10);
		buf[5] = 0;
	}
}

bool draw(_NT_algorithm* self) {
	_shiftNT* p = (_shiftNT*)self;

	// Header: title + jumble held value
	NT_drawText(2, 8, "SHIFT", 15, kNT_textLeft, kNT_textNormal);

	char jbuf[12] = { 'J', ':', ' ', 0 };
	char vbuf[8];
	fmtVolt(p->jumbleHeld, vbuf);
	int jl = 3;
	for (int i = 0; vbuf[i] && jl < 11; i++) jbuf[jl++] = vbuf[i];
	jbuf[jl] = 0;
	NT_drawText(254, 8, jbuf, 12, kNT_textRight, kNT_textNormal);

	// 4 lane rows at baseline y = 22, 33, 44, 55 (11px per row)
	for (int i = 0; i < NUM_OUTS; i++) {
		int y = 22 + i * 11;
		bool cascade = (p->v[LANE_MODE(i)] > 0) && (i > 0);
		int nVal = p->v[LANE_N(i)];
		int divIdx = clampi(p->v[LANE_DIV(i)], 0, NUM_DIV_VALUES - 1);
		int divVal = DIV_VALUES[divIdx];

		// Letter (normal font, 8pt)
		char letter[2] = { (char)('A' + i), 0 };
		NT_drawText(2, y, letter, 15, kNT_textLeft, kNT_textNormal);

		// Mode + Div + N — tiny font, packed
		char info[16];
		int pos = 0;
		info[pos++] = cascade ? 'C' : 'P';
		info[pos++] = ' ';
		info[pos++] = '/';
		info[pos++] = '0' + divVal;       // works for 1,2,3,4
		if (divVal == 8) info[pos - 1] = '8';
		info[pos++] = ' ';
		info[pos++] = 'N';
		if (nVal >= 10) {
			info[pos++] = '0' + (nVal / 10);
			info[pos++] = '0' + (nVal % 10);
		} else {
			info[pos++] = '0' + nVal;
		}
		info[pos] = 0;
		NT_drawText(14, y, info, 12, kNT_textLeft, kNT_textTiny);

		// Held-value bar: centred at x=140, half-width 50 maps to ±10V
		int cx = 140;
		int barY0 = y - 5;
		int barY1 = y - 1;
		// Background track
		NT_drawShapeI(kNT_box, cx - 50, barY0, cx + 50, barY1, 4);
		// Centre tick
		NT_drawShapeI(kNT_line, cx, barY0, cx, barY1, 8);
		// Filled segment from centre out to held voltage
		float clipped = p->held[i];
		if (clipped > 10.f) clipped = 10.f;
		if (clipped < -10.f) clipped = -10.f;
		int barLen = (int)(clipped * 5.f);  // 5 px per volt
		if (barLen > 0) {
			NT_drawShapeI(kNT_rectangle, cx, barY0 + 1, cx + barLen, barY1 - 1, 15);
		} else if (barLen < 0) {
			NT_drawShapeI(kNT_rectangle, cx + barLen, barY0 + 1, cx, barY1 - 1, 15);
		}

		// Numeric voltage on the right
		fmtVolt(p->held[i], vbuf);
		NT_drawText(254, y, vbuf, 15, kNT_textRight, kNT_textTiny);

		// Activity dot — bright while gate pulse is high
		bool gateHigh = p->gateSamples[i] > 0;
		NT_drawShapeI(kNT_rectangle, 198, y - 4, 200, y - 2, gateHigh ? 15 : 3);
	}

	return true;  // suppress the standard parameter line
}

// ─── Factory ─────────────────────────────────────────────────────────────────

static const _NT_factory factory = {
	.guid = NT_MULTICHAR('S','F','s','N'),  // "SFsN" — SFS Shift NT
	.name = "ShiftNT",
	.description = "4-output CV shift register, per-output controls (port of SFS Shift)",
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
