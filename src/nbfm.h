// nbfm.h - real-time two-way NBFM radio link simulation (transmitter -> channel -> receiver)
//
// Everything runs on the actual signal: the voice frequency-modulates a carrier at complex
// baseband, the carrier is faded, offset and buried in receiver noise according to the link
// budget computed by the game, and a superhet receiver recovers it with an IF filter, a
// limiter/discriminator, de-emphasis and a noise-operated squelch. Hiss, FM threshold clicks,
// picket-fence fading, capture effect and squelch tails all come out of that chain.
//
// No STL, no exceptions, no allocation after construction: safe to build freestanding.
#pragma once
#include "minirt.h"

namespace nbfm {

static const int FS_IN = 24000;          // Steam voice (Opus) sample rate
static const int FS = 48000;             // internal simulation rate (complex baseband)
static const float CHAN_BW = 12500.0f;   // 12.5 kHz channel (PMR446 / modern land mobile)
static const float DEVIATION = 2500.0f;  // +-2.5 kHz peak deviation
static const double PI = 3.14159265358979323846;

struct cpx { float re, im; };
static inline cpx cmul(cpx a, cpx b) { cpx r = { a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re }; return r; }

// ------------------------------------------------------------------ random numbers
struct Rng {
	uint64_t s;
	void seed(uint64_t v) { s = v ? v : 0x9E3779B97F4A7C15ull; for (int i = 0; i < 4; i++) next(); }
	uint64_t next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 2685821657736338717ull; }
	float uni() { return (float)((next() >> 40) * (1.0 / 16777216.0)); }              // [0,1)
	float gauss() {
		if (have) { have = false; return spare; }
		float u = uni(), v = uni();
		if (u < 1e-7f) u = 1e-7f;
		float r = (float)rt_sqrt(-2.0 * rt_log(u));
		double a = 2.0 * PI * v;
		spare = r * (float)rt_sin(a); have = true;
		return r * (float)rt_cos(a);
	}
	bool have; float spare;
};

// ------------------------------------------------------------------ filters
struct Biquad {
	float b0, b1, b2, a1, a2, z1, z2;
	void reset() { z1 = z2 = 0; }
	inline float run(float x) {
		float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return y;
	}
	void set(double b0_, double b1_, double b2_, double a0_, double a1_, double a2_) {
		b0 = (float)(b0_ / a0_); b1 = (float)(b1_ / a0_); b2 = (float)(b2_ / a0_);
		a1 = (float)(a1_ / a0_); a2 = (float)(a2_ / a0_);
		reset();
	}
	// RBJ cookbook designs
	void lowpass(double fs, double f, double q) {
		double w = 2 * PI * f / fs, c = rt_cos(w), al = rt_sin(w) / (2 * q);
		set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
	}
	void highpass(double fs, double f, double q) {
		double w = 2 * PI * f / fs, c = rt_cos(w), al = rt_sin(w) / (2 * q);
		set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
	}
	void bandpass(double fs, double f, double q) {
		double w = 2 * PI * f / fs, c = rt_cos(w), al = rt_sin(w) / (2 * q);
		set(al, 0, -al, 1 + al, -2 * c, 1 - al);
	}
	// first-order shelf from an analog prototype (s*t1 + 1)/(s*t2 + 1), bilinear transform
	void shelf1(double fs, double t1, double t2) {
		double k = 2 * fs;
		double nb0 = k * t1 + 1, nb1 = 1 - k * t1, na0 = k * t2 + 1, na1 = 1 - k * t2;
		set(nb0, nb1, 0, na0, na1, 0);
	}
};

// symmetric FIR with a circular history, real coefficients (used on I and Q)
template <int N>
struct FirC {
	float h[N];
	cpx hist[2 * N];
	int pos;
	void reset() { rt_memset(hist, 0, sizeof(hist)); pos = 0; }
	void design_lowpass(double fs, double fc) {
		double sum = 0;
		for (int i = 0; i < N; i++) {
			double m = i - (N - 1) / 2.0;
			double x = 2 * fc / fs * m;
			double sinc = (m == 0) ? 1.0 : rt_sin(PI * x) / (PI * x);
			double w = 0.42 - 0.5 * rt_cos(2 * PI * i / (N - 1)) + 0.08 * rt_cos(4 * PI * i / (N - 1));  // Blackman
			h[i] = (float)(sinc * w); sum += h[i];
		}
		for (int i = 0; i < N; i++) h[i] = (float)(h[i] / sum);
		reset();
	}
	inline cpx run(cpx x) {
		hist[pos] = x; hist[pos + N] = x;
		pos = (pos + 1) % N;
		const cpx* p = hist + pos;   // p[0] oldest ... p[N-1] newest
		float re = 0, im = 0;
		for (int i = 0; i < N; i++) { re += h[i] * p[i].re; im += h[i] * p[i].im; }
		cpx r = { re, im };
		return r;
	}
};

// 2x half-band interpolator / decimator (real)
struct HalfBand {
	static const int N = 31;
	float h[N];
	float hist[2 * N];
	int pos;
	void init() {
		double sum = 0;
		for (int i = 0; i < N; i++) {
			double m = i - (N - 1) / 2.0;
			double x = 0.5 * m;
			double sinc = (m == 0) ? 1.0 : rt_sin(PI * x) / (PI * x);
			double w = 0.42 - 0.5 * rt_cos(2 * PI * i / (N - 1)) + 0.08 * rt_cos(4 * PI * i / (N - 1));
			h[i] = (float)(sinc * w); sum += h[i];
		}
		for (int i = 0; i < N; i++) h[i] = (float)(h[i] / sum);
		rt_memset(hist, 0, sizeof(hist)); pos = 0;
	}
	inline void push(float x) { hist[pos] = x; hist[pos + N] = x; pos = (pos + 1) % N; }
	inline float out() const {
		const float* p = hist + pos; float s = 0;
		for (int i = 0; i < N; i++) s += h[i] * p[i];
		return s;
	}
};

// ------------------------------------------------------------------ link parameters
struct LinkParams {
	float cnr_db;       // carrier-to-noise ratio in the 12.5 kHz channel (from the propagation model)
	float k_factor;     // Rician K (linear). large = line of sight, 0 = pure Rayleigh (behind walls)
	float doppler_hz;   // max Doppler shift from movement (sets the fading speed)
	float lo_offset_hz; // transmitter/receiver crystal mismatch
	float impulse_rate; // ignition / electrical clicks per second
	float impulse_amp;  // impulse strength relative to receiver noise (linear)
	float beat_db;      // unmodulated interfering carrier, dB relative to receiver noise (-100 = none)
	float beat_hz;      // its offset from our channel centre
	float squelch;      // 0 = open, 1 = tight
	float tx_gain;      // microphone gain multiplier
	float volume;       // receiver speaker volume
};

static inline void default_params(LinkParams& p) {
	p.cnr_db = 60; p.k_factor = 20; p.doppler_hz = 0; p.lo_offset_hz = 0;
	p.impulse_rate = 0; p.impulse_amp = 0; p.beat_db = -100; p.beat_hz = 1700;
	p.squelch = 0.45f; p.tx_gain = 1.0f; p.volume = 1.0f;
}

// ------------------------------------------------------------------ the link
struct Link {
	LinkParams p;
	Rng rng;
	// transmitter
	HalfBand up;
	Biquad tx_hp1, tx_hp2, tx_lp1, tx_lp2, preemph, splat1, splat2;
	float agc_env, agc_gain;
	double tx_phase;
	// channel
	static const int NOSC = 12;
	cpx osc[NOSC], osc_rot[NOSC];
	float osc_norm_cnt;
	cpx los;
	float amp_db_s;     // smoothed carrier level (dB)
	float doppler_cur;
	double lo_phase, beat_phase;
	float beat_db_c, beat_amp;
	float key;          // transmitter keyed envelope (0..1), ramps for key-up / key-down
	// receiver
	FirC<73> ifilt;
	cpx prev;
	Biquad deemph, rx_hp1, rx_hp2, rx_lp1, rx_lp2, rx_lp3, rx_lp4, sq_bp1, sq_bp2;
	float sq_env, sq_gate;
	int sq_open, sq_close_cnt, sq_open_cnt;
	HalfBand down;
	int phase2;
	float noise_sigma;
	// stats for the HUD
	float stat_cnr, stat_open;

	void init(uint64_t seed) {
		rng.seed(seed); rng.have = false;
		default_params(p);
		up.init(); down.init();
		tx_hp1.highpass(FS, 300, 0.707); tx_hp2.highpass(FS, 300, 0.707);
		tx_lp1.lowpass(FS, 3000, 0.707); tx_lp2.lowpass(FS, 3000, 0.707);
		// 750 us pre-emphasis (6 dB/octave from ~212 Hz), flattened above ~3.4 kHz
		preemph.shelf1(FS, 750e-6, 1.0 / (2 * PI * 3400));
		splat1.lowpass(FS, 3000, 0.6); splat2.lowpass(FS, 3000, 0.9);
		agc_env = 0.05f; agc_gain = 1.0f; tx_phase = 0;
		for (int i = 0; i < NOSC; i++) { osc[i].re = 1; osc[i].im = 0; osc_rot[i].re = 1; osc_rot[i].im = 0; }
		randomize_scatter();
		osc_norm_cnt = 0;
		double a = 2 * PI * rng.uni(); los.re = (float)rt_cos(a); los.im = (float)rt_sin(a);
		amp_db_s = -200; doppler_cur = -1;
		lo_phase = 0; beat_phase = 0; key = 0; beat_db_c = -999; beat_amp = 0;
		ifilt.design_lowpass(FS, 6250);
		prev.re = 1; prev.im = 0;
		deemph.shelf1(FS, 1.0 / (2 * PI * 3400), 750e-6);
		rx_hp1.highpass(FS, 300, 0.707); rx_hp2.highpass(FS, 300, 0.707);   // CTCSS / sub-audio filter
		// 8th-order Butterworth voice filter: FM noise rises with frequency, real sets cut hard above 3 kHz
		rx_lp1.lowpass(FS, 3100, 0.5098); rx_lp2.lowpass(FS, 3100, 0.6013);
		rx_lp3.lowpass(FS, 3100, 0.9000); rx_lp4.lowpass(FS, 3100, 2.5629);
		sq_bp1.bandpass(FS, 7000, 1.5); sq_bp2.bandpass(FS, 7000, 1.5);
		sq_env = 1.0f; sq_gate = 0; sq_open = 0; sq_close_cnt = 0; sq_open_cnt = 0;
		phase2 = 0;
		// receiver noise: unit power inside the channel bandwidth
		noise_sigma = (float)rt_sqrt((double)FS / CHAN_BW / 2.0);
		stat_cnr = -99; stat_open = 0;
	}

	void randomize_scatter() {
		for (int i = 0; i < NOSC; i++) {
			double ph = 2 * PI * rng.uni();
			osc[i].re = (float)rt_cos(ph); osc[i].im = (float)rt_sin(ph);
		}
		doppler_cur = -1;
	}

	void update_doppler() {
		float fd = p.doppler_hz < 0.05f ? 0.05f : p.doppler_hz;   // even standing still, the world moves a little
		if (fd == doppler_cur) return;
		doppler_cur = fd;
		double th = rng.uni();
		for (int i = 0; i < NOSC; i++) {
			double al = 2 * PI * (i + th) / NOSC;          // arrival angles (Jakes / Clarke model)
			double f = fd * rt_cos(al);
			double w = 2 * PI * f / FS;
			osc_rot[i].re = (float)rt_cos(w); osc_rot[i].im = (float)rt_sin(w);
		}
	}

	// noise-squelch thresholds. Calibrated so that knob 0.45 opens around 6 dB CNR.
	// (noise power in the 7 kHz band of the discriminator output, measured offline)
	float sq_thr_open() const { return (float)rt_pow(10.0, -0.25 - 1.9 * p.squelch); }

	// One input sample at 24 kHz in (keyed: carrier on/off), one sample at 24 kHz out.
	// Internally runs two samples at 48 kHz.
	float process(float in, bool keyed) {
		up.push(in * 2.0f);
		float y0 = step48(up.out(), keyed);
		up.push(0.0f);
		float y1 = step48(up.out(), keyed);
		down.push(y0);
		down.push(y1);
		return down.out();
	}

	float step48(float a, bool keyed) {
		// ---------------- transmitter audio chain
		a *= p.tx_gain;
		a = tx_hp2.run(tx_hp1.run(a));
		a = tx_lp2.run(tx_lp1.run(a));
		// speech AGC / compressor like a handheld's mic amp
		float ab = a < 0 ? -a : a;
		agc_env += (ab > agc_env ? 0.004f : 0.00005f) * (ab - agc_env);
		float target = 0.22f / (agc_env + 1e-4f);
		if (target > 12.0f) target = 12.0f;
		if (target < 0.3f) target = 0.3f;
		agc_gain += 0.0005f * (target - agc_gain);
		a *= agc_gain;
		a = preemph.run(a);
		// deviation limiter (clipper) + post-limiter splatter filter
		if (a > 1.0f) a = 1.0f; else if (a < -1.0f) a = -1.0f;
		a = splat2.run(splat1.run(a));
		// FM modulator
		tx_phase += 2 * PI * DEVIATION * a / FS;
		if (tx_phase > PI) tx_phase -= 2 * PI; else if (tx_phase < -PI) tx_phase += 2 * PI;
		cpx tx = { (float)rt_cos(tx_phase), (float)rt_sin(tx_phase) };
		// key-up / key-down: the carrier ramps over ~2 ms (PA ramp)
		key += ((keyed ? 1.0f : 0.0f) - key) * 0.01f;

		// ---------------- propagation
		// smoothed carrier amplitude (link budget is updated a few times a second)
		amp_db_s += (p.cnr_db - amp_db_s) * (amp_db_s < -150 ? 1.0f : 0.00015f);
		float amp = (float)rt_pow(10.0, amp_db_s / 20.0) * key;
		// Rician fading: steady line-of-sight part + scattered part from a Jakes oscillator bank
		update_doppler();
		cpx sc = { 0, 0 };
		for (int i = 0; i < NOSC; i++) { osc[i] = cmul(osc[i], osc_rot[i]); sc.re += osc[i].re; sc.im += osc[i].im; }
		if (++osc_norm_cnt > 4800) {
			osc_norm_cnt = 0;
			for (int i = 0; i < NOSC; i++) {
				float m = (float)rt_sqrt(osc[i].re * osc[i].re + osc[i].im * osc[i].im);
				osc[i].re /= m; osc[i].im /= m;
			}
		}
		float K = p.k_factor < 0 ? 0 : p.k_factor;
		float gl = (float)rt_sqrt(K / (K + 1.0f)), gs = (float)rt_sqrt(1.0f / (K + 1.0f)) / (float)rt_sqrt((double)NOSC);
		cpx g = { gl * los.re + gs * sc.re, gl * los.im + gs * sc.im };
		cpx z = cmul(tx, g);
		z.re *= amp; z.im *= amp;
		// crystal offset between the two radios
		if (p.lo_offset_hz != 0) {
			lo_phase += 2 * PI * p.lo_offset_hz / FS;
			if (lo_phase > PI) lo_phase -= 2 * PI;
			cpx r = { (float)rt_cos(lo_phase), (float)rt_sin(lo_phase) };
			z = cmul(z, r);
		}
		// receiver front-end noise
		z.re += noise_sigma * rng.gauss();
		z.im += noise_sigma * rng.gauss();
		// interfering unmodulated carrier
		if (p.beat_db > -60) {
			if (p.beat_db != beat_db_c) { beat_db_c = p.beat_db; beat_amp = (float)rt_pow(10.0, p.beat_db / 20.0); }
			float ba = beat_amp;
			beat_phase += 2 * PI * p.beat_hz / FS;
			if (beat_phase > PI) beat_phase -= 2 * PI;
			z.re += ba * (float)rt_cos(beat_phase); z.im += ba * (float)rt_sin(beat_phase);
		}
		// impulse noise (sparks, switching transients)
		if (p.impulse_rate > 0 && rng.uni() < p.impulse_rate / FS) {
			float ia = p.impulse_amp * noise_sigma * 6.0f * (0.3f - (float)rt_log(rng.uni() + 1e-6f));
			double ph = 2 * PI * rng.uni();
			z.re += ia * (float)rt_cos(ph); z.im += ia * (float)rt_sin(ph);
		}

		// ---------------- receiver
		cpx f = ifilt.run(z);
		// limiter-discriminator
		cpx d = { f.re * prev.re + f.im * prev.im, f.im * prev.re - f.re * prev.im };
		prev = f;
		float disc = (float)(rt_atan2(d.im, d.re) * FS / (2 * PI * DEVIATION));
		// noise squelch: measure noise above the voice band
		float nb = sq_bp2.run(sq_bp1.run(disc));
		sq_env += 0.002f * (nb * nb - sq_env);
		float thr = sq_thr_open();
		if (p.squelch <= 0.001f) sq_open = 1;
		else if (sq_open) {
			if (sq_env > thr * 2.5f) { if (++sq_close_cnt > FS * 0.11) { sq_open = 0; sq_close_cnt = 0; } }
			else sq_close_cnt = 0;
		} else {
			if (sq_env < thr) { if (++sq_open_cnt > FS * 0.004) { sq_open = 1; sq_open_cnt = 0; } }
			else sq_open_cnt = 0;
		}
		sq_gate += ((sq_open ? 1.0f : 0.0f) - sq_gate) * (sq_open ? 0.01f : 0.004f);
		// audio
		float au = deemph.run(disc);
		au = rx_hp2.run(rx_hp1.run(au));
		au = rx_lp4.run(rx_lp3.run(rx_lp2.run(rx_lp1.run(au))));
		au *= sq_gate * p.volume * 1.35f;
		// little speaker amplifier: soft clip
		if (au > 0.8f) au = 0.8f + 0.2f * (float)rt_tanh((au - 0.8f) / 0.2f);
		else if (au < -0.8f) au = -0.8f + 0.2f * (float)rt_tanh((au + 0.8f) / 0.2f);
		stat_open = sq_gate;
		stat_cnr = amp_db_s + 20.0f * (float)rt_log10(rt_sqrt(g.re * g.re + g.im * g.im) * key + 1e-6);
		return au;
	}

	bool squelch_closed() const { return !sq_open && sq_gate < 0.01f; }
};

} // namespace nbfm
