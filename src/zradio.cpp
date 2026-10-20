// zradio.cpp - gmsv_zradio: per-listener two-way radio simulation for Garry's Mod voice chat.
//
// How it works
//   The engine's SV_BroadcastVoiceData(sender, bytes, data, xuid) sends one compressed voice
//   packet to every client whose "listening" bit for that sender is set (the bits come from
//   PlayerCanHearPlayersVoice via IVoiceServer::SetClientListening).
//
//   We detour that function. When the sender has active radio links (set from Lua), we:
//     1. decode the packet once to 24 kHz PCM,
//     2. clear the listening bit of every radio listener and let the engine send the original
//        packet to everyone else (people standing next to the talker hear them normally),
//     3. for each radio listener: run the voice through that listener's own NBFM link
//        simulation (path loss, walls, fading, noise, squelch... from Lua), re-encode it, and
//        call the engine again with only that listener's bit set,
//     4. restore every bit exactly as it was.
//   When a transmission ends, Poll() lets each receiver's squelch tail play out.
//
// Lua API (global table `zradio`), server side:
//   zradio.Init(maxplayers)
//   zradio.SetLink(sender_entindex, receiver_entindex, params_table)   -- refresh every <1 s
//   zradio.ClearLink(sender, receiver) / zradio.ClearSender(sender) / zradio.ClearAll()
//   zradio.Poll()                    -- call every tick (squelch tails, expiry)
//   zradio.GetLinkStats(s, r)        -- returns cnr_db, squelch_open (0..1), transmitting
//   zradio.Enable(bool), zradio.IsHooked(), zradio.HookedAfterOther(), zradio.Version
#include "minirt.h"
#include "engine.h"
#include "nbfm.h"
#include "steamvoice.h"
#include "MinHook.h"

#include "GarrysMod/Lua/Interface.h"

using GarrysMod::Lua::ILuaBase;

static const int MAXP = 129;                 // entity indices 1..128
static const int MAX_SAMPLES = 24000;         // up to 1 s of audio in one packet
static const char* VERSION = "1.1.0";

typedef void (*BroadcastVoiceFn)(IClient* cl, int nBytes, char* data, int64_t xuid);

struct PairLink {
	nbfm::Link dsp;
	steamvoice::Encoder enc;
	double t_update;          // last SetLink from Lua
	double t_used;            // last time audio was sent through it
	bool dsp_ready;
	bool in_tx;               // carried audio in the current transmission
};

// Microphone bus: what the transmitting radio's microphone picks up besides its owner's voice
// (other people talking nearby, gunshots, footsteps...). Sources are mixed in ahead of the read
// position; each voice packet from the owner consumes as many samples as it carries.
static const int BUS = 1 << 16;               // 2.7 s at 24 kHz
static const int BUS_MASK = BUS - 1;
static const int BUS_LAT = 2400;              // write 100 ms ahead of the read position (jitter buffer)
static const int BUS_MIN = 480, BUS_MAX = 9600;

struct Pickup {                               // another player's voice reaching a transmitter's mic
	double t_update;
	float gain, muffle, lp;
	uint64_t wp;
	bool wp_valid;
};

struct Sender {
	steamvoice::Decoder dec;
	bool dec_ok;
	float* bus;
	uint64_t rp;
	IClient* cl;
	uint64_t steamid;
	int64_t xuid;
	double last_packet;
	bool transmitting;
};

// ---- global state (POD only: the module is built without a C runtime initialiser)
static bool g_hooked, g_enabled, g_after_other;
static BroadcastVoiceFn g_orig;
static void* g_target;
static IVoiceServer* g_vs;
static int g_maxp = 32;
static PairLink* g_links[MAXP][MAXP];
static Sender* g_senders[MAXP];
static bool g_saved[MAXP];
static int16_t g_pcm[MAX_SAMPLES + 512];
static int16_t g_out[MAX_SAMPLES + 512];
static unsigned char g_pkt[64 * 1024];
static int g_rx_list[MAXP];
static Pickup* g_pick[MAXP][MAXP];            // [source][transmitter]
static float g_mix[MAX_SAMPLES + 512];

// decoded game sounds, resampled to 24 kHz mono
struct SoundEntry { uint32_t hash; char* name; float* data; int n; };
static const int MAX_SOUNDS = 768;
static SoundEntry g_sounds[MAX_SOUNDS];
static int g_nsounds;
static long long g_sound_samples;
static const long long MAX_SOUND_SAMPLES = 24000LL * 60 * 30;   // 30 minutes of audio (~170 MB cap)

extern "C" int zr_decode_audio(const unsigned char* d, int n, float** out, int* rate);
extern "C" void zr_free_audio(float* p);
static bool g_in_hook;
static uint64_t g_seed = 0x5EED1234ABCDull;
static double g_tail_after = 0.22;            // seconds of no packets = transmission ended
static double g_link_ttl = 1.5;
static bool g_tails = true;
static char g_err[256];

static bool streq(const char* a, const char* b) {
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

static Sender* sender(int s) {
	if (s < 1 || s >= MAXP) return 0;
	if (!g_senders[s]) {
		Sender* p = (Sender*)rt_calloc(1, sizeof(Sender));
		if (!p) return 0;
		p->dec_ok = p->dec.init();
		g_senders[s] = p;
	}
	return g_senders[s];
}

static PairLink* link(int s, int r, bool create) {
	if (s < 1 || s >= MAXP || r < 1 || r >= MAXP) return 0;
	PairLink* L = g_links[s][r];
	if (!L && create) {
		L = (PairLink*)rt_calloc(1, sizeof(PairLink));
		if (!L) return 0;
		g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull + (uint64_t)(s * 131 + r);
		L->dsp.init(g_seed);
		L->dsp_ready = true;
		L->enc.init();
		g_links[s][r] = L;
	}
	return L;
}

static inline bool link_active(PairLink* L, double now) { return L && (now - L->t_update) < g_link_ttl; }

static inline bool keyed(Sender* S, double now) { return S && S->bus && S->transmitting && now - S->last_packet < 0.35; }

static uint32_t fnv(const char* s) { uint32_t h = 2166136261u; while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; } return h; }
static SoundEntry* find_sound(const char* name) {
	uint32_t h = fnv(name);
	for (int i = 0; i < g_nsounds; i++) if (g_sounds[i].hash == h && streq(g_sounds[i].name, name)) return &g_sounds[i];
	return 0;
}

// a pickup writes a stream into a transmitter's bus, keeping its own write cursor (jitter buffer)
static void pickup_write(Sender* T, Pickup* P, const int16_t* pcm, int n) {
	uint64_t lo = T->rp + BUS_MIN, hi = T->rp + BUS_MAX;
	if (!P->wp_valid || P->wp < lo || P->wp > hi) { P->wp = T->rp + BUS_LAT; P->wp_valid = true; }
	if (n > BUS - BUS_MAX) n = BUS - BUS_MAX;
	float a = 1.0f - P->muffle * 0.93f;           // one-pole low-pass: walls take the highs first
	for (int k = 0; k < n; k++) {
		P->lp += a * (pcm[k] / 32768.0f - P->lp);
		T->bus[(P->wp + k) & BUS_MASK] += P->lp * P->gain;
	}
	P->wp += n;
}

static inline int clampi(float v) {
	int x = (int)(v * 32767.0f);
	return x > 32767 ? 32767 : (x < -32768 ? -32768 : x);
}

// send `data` from `cl` to exactly one receiver (or nobody else), restoring the bits afterwards
static void send_to_only(IClient* cl, int s, int r, unsigned char* data, int n, int64_t xuid) {
	for (int q = 1; q <= g_maxp; q++) g_saved[q] = g_vs->GetClientListening(q, s);
	for (int q = 1; q <= g_maxp; q++) if (g_saved[q] != (q == r)) g_vs->SetClientListening(q, s, q == r);
	g_orig(cl, n, (char*)data, xuid);
	for (int q = 1; q <= g_maxp; q++) if (g_saved[q] != (q == r)) g_vs->SetClientListening(q, s, g_saved[q]);
}

// ---------------------------------------------------------------- the detour
static void hook_BroadcastVoiceData(IClient* cl, int nBytes, char* data, int64_t xuid) {
	if (!g_enabled || !g_vs || g_in_hook || !cl || nBytes < 8 + 4 + 3) return g_orig(cl, nBytes, data, xuid);
	int s = cl->GetPlayerSlot() + 1;
	if (s < 1 || s > g_maxp) return g_orig(cl, nBytes, data, xuid);
	double now = plat::now();

	// any radio listeners for this talker who are currently allowed to hear them?
	int nrx = 0;
	for (int r = 1; r <= g_maxp; r++) {
		PairLink* L = g_links[s][r];
		if (r != s && link_active(L, now) && g_vs->GetClientListening(r, s)) g_rx_list[nrx++] = r;
	}
	Sender* S = sender(s);
	if (!S || !S->dec_ok) return g_orig(cl, nBytes, data, xuid);

	// decode (always, so the decoder stays in step with the stream)
	int ns = S->dec.decode_packet((const unsigned char*)data, nBytes, g_pcm, MAX_SAMPLES);
	if (ns <= 0) return g_orig(cl, nBytes, data, xuid);

	// this talker's voice reaching other people's transmitting radios
	for (int t = 1; t <= g_maxp; t++) {
		Pickup* P = g_pick[s][t];
		if (!P || t == s || now - P->t_update > 1.0) continue;
		Sender* T = g_senders[t];
		if (keyed(T, now)) pickup_write(T, P, g_pcm, ns);
		else P->wp_valid = false;
	}
	if (nrx == 0) return g_orig(cl, nBytes, data, xuid);

	g_in_hook = true;
	S->cl = cl;
	rt_memcpy(&S->steamid, data, 8);
	S->xuid = xuid;
	if (!S->bus) { S->bus = (float*)rt_calloc(BUS, sizeof(float)); S->rp = 0; }
	if (!S->transmitting || now - S->last_packet > 0.35) {
		// new transmission: nothing stale left in the microphone bus
		if (S->bus) rt_memset(S->bus, 0, BUS * sizeof(float));
	}
	S->last_packet = now;
	S->transmitting = true;

	// the microphone: the owner's voice plus everything else it picks up
	for (int k = 0; k < ns; k++) {
		float x = g_pcm[k] / 32768.0f;
		if (S->bus) { uint32_t i = (uint32_t)((S->rp + k) & BUS_MASK); x += S->bus[i]; S->bus[i] = 0; }
		g_mix[k] = x;
	}
	if (S->bus) S->rp += ns;

	// 1) everyone who is not a radio listener gets the untouched packet
	for (int i = 0; i < nrx; i++) g_vs->SetClientListening(g_rx_list[i], s, false);
	g_orig(cl, nBytes, data, xuid);
	for (int i = 0; i < nrx; i++) g_vs->SetClientListening(g_rx_list[i], s, true);

	// 2) each radio listener gets their own received signal
	for (int i = 0; i < nrx; i++) {
		int r = g_rx_list[i];
		PairLink* L = g_links[s][r];
		for (int k = 0; k < ns; k++) g_out[k] = (int16_t)clampi(L->dsp.process(g_mix[k], true));
		L->t_used = now;
		L->in_tx = true;
		int n = L->enc.make_packet(S->steamid, g_out, ns, false, g_pkt, sizeof(g_pkt));
		if (n > 0) send_to_only(cl, s, r, g_pkt, n, xuid);
	}
	g_in_hook = false;
}

// ---------------------------------------------------------------- end of transmission: squelch tails
static void finish_transmission(int s, Sender* S) {
	S->transmitting = false;
	if (S->bus) rt_memset(S->bus, 0, BUS * sizeof(float));
	for (int q = 1; q < MAXP; q++) if (g_pick[q][s]) g_pick[q][s]->wp_valid = false;
	if (!g_vs || !S->cl) return;
	double now = plat::now();
	g_in_hook = true;
	for (int r = 1; r <= g_maxp; r++) {
		PairLink* L = g_links[s][r];
		if (!L || !L->in_tx) continue;
		L->in_tx = false;
		if (!g_tails || !link_active(L, now) || !g_vs->GetClientListening(r, s)) { L->enc.reset(); continue; }
		// carrier drops: run the receiver until its squelch shuts (or 0.45 s)
		int n = 0, closed_for = 0;
		while (n < steamvoice::RATE * 45 / 100) {
			float y = L->dsp.process(0.0f, false);
			g_out[n++] = (int16_t)clampi(y);
			closed_for = L->dsp.squelch_closed() ? closed_for + 1 : 0;
			if (closed_for > 480) break;
		}
		int b = L->enc.make_packet(S->steamid, g_out, n, true, g_pkt, sizeof(g_pkt));
		if (b > 0) send_to_only(S->cl, s, r, g_pkt, b, S->xuid);
	}
	g_in_hook = false;
}

// ---------------------------------------------------------------- Lua API
static float opt_number(ILuaBase* LUA, int tbl, const char* key, float def) {
	LUA->GetField(tbl, key);
	float v = LUA->IsType(-1, GarrysMod::Lua::Type::Number) ? (float)LUA->GetNumber(-1) : def;
	LUA->Pop();
	return v;
}

LUA_FUNCTION(l_init) {
	int m = (int)LUA->CheckNumber(1);
	g_maxp = m < 1 ? 1 : (m > MAXP - 1 ? MAXP - 1 : m);
	return 0;
}

LUA_FUNCTION(l_setlink) {
	int s = (int)LUA->CheckNumber(1), r = (int)LUA->CheckNumber(2);
	LUA->CheckType(3, GarrysMod::Lua::Type::Table);
	if (s == r || s < 1 || r < 1 || s > g_maxp || r > g_maxp) return 0;
	PairLink* L = link(s, r, true);
	if (!L) return 0;
	nbfm::LinkParams& p = L->dsp.p;
	p.cnr_db = opt_number(LUA, 3, "cnr", p.cnr_db);
	p.k_factor = opt_number(LUA, 3, "k", p.k_factor);
	p.doppler_hz = opt_number(LUA, 3, "doppler", p.doppler_hz);
	p.lo_offset_hz = opt_number(LUA, 3, "offset", p.lo_offset_hz);
	p.impulse_rate = opt_number(LUA, 3, "impulse_rate", p.impulse_rate);
	p.impulse_amp = opt_number(LUA, 3, "impulse_amp", p.impulse_amp);
	p.beat_db = opt_number(LUA, 3, "beat_db", p.beat_db);
	p.beat_hz = opt_number(LUA, 3, "beat_hz", p.beat_hz);
	p.squelch = opt_number(LUA, 3, "squelch", p.squelch);
	p.tx_gain = opt_number(LUA, 3, "mic_gain", p.tx_gain);
	p.volume = opt_number(LUA, 3, "volume", p.volume);
	L->t_update = plat::now();
	return 0;
}

LUA_FUNCTION(l_clearlink) {
	int s = (int)LUA->CheckNumber(1), r = (int)LUA->CheckNumber(2);
	PairLink* L = link(s, r, false);
	if (L) L->t_update = -1e9;
	return 0;
}

LUA_FUNCTION(l_clearsender) {
	int s = (int)LUA->CheckNumber(1);
	if (s < 1 || s >= MAXP) return 0;
	for (int r = 1; r < MAXP; r++) if (g_links[s][r]) { g_links[s][r]->t_update = -1e9; g_links[s][r]->in_tx = false; }
	for (int q = 1; q < MAXP; q++) if (g_links[q][s]) { g_links[q][s]->t_update = -1e9; g_links[q][s]->in_tx = false; }
	if (g_senders[s]) { g_senders[s]->cl = 0; g_senders[s]->transmitting = false; }
	for (int q = 1; q < MAXP; q++) {
		if (g_pick[s][q]) g_pick[s][q]->t_update = -1e9;
		if (g_pick[q][s]) g_pick[q][s]->t_update = -1e9;
	}
	return 0;
}

LUA_FUNCTION(l_clearall) {
	for (int s = 1; s < MAXP; s++) {
		for (int r = 1; r < MAXP; r++) if (g_links[s][r]) { g_links[s][r]->t_update = -1e9; g_links[s][r]->in_tx = false; }
		if (g_senders[s]) { g_senders[s]->cl = 0; g_senders[s]->transmitting = false; }
	}
	return 0;
}

LUA_FUNCTION(l_poll) {
	if (!g_hooked) return 0;
	double now = plat::now();
	for (int s = 1; s <= g_maxp; s++) {
		Sender* S = g_senders[s];
		if (S && S->transmitting && now - S->last_packet > g_tail_after) finish_transmission(s, S);
	}
	return 0;
}

LUA_FUNCTION(l_stats) {
	int s = (int)LUA->CheckNumber(1), r = (int)LUA->CheckNumber(2);
	PairLink* L = link(s, r, false);
	if (!L) { LUA->PushNumber(-99); LUA->PushNumber(0); LUA->PushBool(false); return 3; }
	LUA->PushNumber(L->dsp.stat_cnr);
	LUA->PushNumber(L->dsp.stat_open);
	LUA->PushBool(L->in_tx && plat::now() - L->t_used < 0.5);
	return 3;
}

// zradio.SetPickup(source, transmitter, gain, muffle): source's voice reaches transmitter's microphone
LUA_FUNCTION(l_setpickup) {
	int c = (int)LUA->CheckNumber(1), t = (int)LUA->CheckNumber(2);
	float gain = (float)LUA->CheckNumber(3);
	float muffle = LUA->IsType(4, GarrysMod::Lua::Type::Number) ? (float)LUA->GetNumber(4) : 0.0f;
	if (c == t || c < 1 || t < 1 || c > g_maxp || t > g_maxp) return 0;
	Pickup* P = g_pick[c][t];
	if (!P) { P = (Pickup*)rt_calloc(1, sizeof(Pickup)); if (!P) return 0; g_pick[c][t] = P; }
	P->gain = gain < 0 ? 0 : (gain > 8 ? 8 : gain);
	P->muffle = muffle < 0 ? 0 : (muffle > 1 ? 1 : muffle);
	P->t_update = plat::now();
	return 0;
}

LUA_FUNCTION(l_clearpickup) {
	int c = (int)LUA->CheckNumber(1), t = (int)LUA->CheckNumber(2);
	if (c >= 1 && t >= 1 && c < MAXP && t < MAXP && g_pick[c][t]) g_pick[c][t]->t_update = -1e9;
	return 0;
}

// zradio.HasSound(name) -> true if decoded and cached
LUA_FUNCTION(l_hassound) { LUA->PushBool(find_sound(LUA->CheckString(1)) != 0); return 1; }

// zradio.LoadSound(name, filedata) -> true on success. filedata = file.Read("sound/"..name, "GAME")
LUA_FUNCTION(l_loadsound) {
	const char* name = LUA->CheckString(1);
	if (find_sound(name)) { LUA->PushBool(true); return 1; }
	unsigned int len = 0;
	const char* d = LUA->GetString(2, &len);
	if (!d || len < 16 || g_nsounds >= MAX_SOUNDS || g_sound_samples > MAX_SOUND_SAMPLES) { LUA->PushBool(false); return 1; }
	float* src = 0; int rate = 0;
	int n = zr_decode_audio((const unsigned char*)d, (int)len, &src, &rate);
	if (n <= 0 || !src || rate <= 0) { if (src) zr_free_audio(src); LUA->PushBool(false); return 1; }
	// anti-alias low-pass at the source rate, then resample to 24 kHz
	nbfm::Biquad f1, f2;
	double fc = rate > 24000 ? 10000.0 : rate * 0.42;
	f1.lowpass(rate, fc, 0.54); f2.lowpass(rate, fc, 1.31);
	for (int i = 0; i < n; i++) src[i] = f2.run(f1.run(src[i]));
	double step = (double)rate / steamvoice::RATE;
	int m = (int)(n / step);
	float* dst = (float*)rt_calloc(m + 1, sizeof(float));
	if (!dst) { zr_free_audio(src); LUA->PushBool(false); return 1; }
	for (int i = 0; i < m; i++) {
		double x = i * step; int i0 = (int)x; double fr = x - i0;
		float a = src[i0], b = i0 + 1 < n ? src[i0 + 1] : a;
		dst[i] = (float)(a + (b - a) * fr);
	}
	zr_free_audio(src);
	size_t nl = 0; while (name[nl]) nl++;
	char* nm = (char*)rt_calloc(nl + 1, 1);
	rt_memcpy(nm, name, nl);
	SoundEntry& e = g_sounds[g_nsounds++];
	e.hash = fnv(name); e.name = nm; e.data = dst; e.n = m;
	g_sound_samples += m;
	LUA->PushBool(true);
	return 1;
}

// zradio.PlaySound(transmitter, name, gain, rate, delay_s, muffle) -> true if mixed into a keyed radio
LUA_FUNCTION(l_playsound) {
	int t = (int)LUA->CheckNumber(1);
	const char* name = LUA->CheckString(2);
	float gain = (float)LUA->CheckNumber(3);
	double rate = LUA->IsType(4, GarrysMod::Lua::Type::Number) ? LUA->GetNumber(4) : 1.0;
	double delay = LUA->IsType(5, GarrysMod::Lua::Type::Number) ? LUA->GetNumber(5) : 0.0;
	float muffle = LUA->IsType(6, GarrysMod::Lua::Type::Number) ? (float)LUA->GetNumber(6) : 0.0f;
	double now = plat::now();
	Sender* T = (t >= 1 && t <= g_maxp) ? g_senders[t] : 0;
	SoundEntry* e = find_sound(name);
	if (!keyed(T, now) || !e) { LUA->PushBool(false); return 1; }
	if (rate < 0.25) rate = 0.25; if (rate > 4) rate = 4;
	if (delay < 0) delay = 0; if (delay > 1.0) delay = 1.0;
	if (gain > 60) gain = 60;
	muffle = muffle < 0 ? 0 : (muffle > 1 ? 1 : muffle);
	uint64_t pos = T->rp + BUS_LAT + (uint64_t)(delay * steamvoice::RATE);
	int room = BUS - (int)(pos - T->rp) - 64;
	int m = (int)(e->n / rate);
	if (m > room) m = room;
	float a = 1.0f - muffle * 0.93f, lp = 0;
	for (int i = 0; i < m; i++) {
		double x = i * rate; int i0 = (int)x; double fr = x - i0;
		float v0 = e->data[i0], v1 = i0 + 1 < e->n ? e->data[i0 + 1] : v0;
		lp += a * ((float)(v0 + (v1 - v0) * fr) - lp);
		T->bus[(pos + i) & BUS_MASK] += lp * gain;
	}
	LUA->PushBool(true);
	return 1;
}

LUA_FUNCTION(l_istx) {
	int t = (int)LUA->CheckNumber(1);
	LUA->PushBool(t >= 1 && t <= g_maxp && keyed(g_senders[t], plat::now()));
	return 1;
}

LUA_FUNCTION(l_enable) { g_enabled = LUA->GetBool(1); return 0; }
LUA_FUNCTION(l_ishooked) { LUA->PushBool(g_hooked); return 1; }
LUA_FUNCTION(l_after) { LUA->PushBool(g_after_other); return 1; }
LUA_FUNCTION(l_error) { LUA->PushString(g_err); return 1; }
LUA_FUNCTION(l_setoption) {
	const char* k = LUA->CheckString(1);
	double v = LUA->CheckNumber(2);
	if (streq(k, "tails")) g_tails = v != 0;
	else if (streq(k, "tail_after")) g_tail_after = v;
	else if (streq(k, "link_ttl")) g_link_ttl = v;
	return 0;
}

// ---------------------------------------------------------------- hooking
static const char* const SIG_NAMES_LINUX = "_Z21SV_BroadcastVoiceDataP7IClientiPcx";
struct Sig { const char* s; size_t n; };
#define SIG(x) { x, sizeof(x) - 1 }
#if defined(_WIN32) && !defined(_WIN64)
static const Sig SIGS[] = {
	SIG("\x55\x8B\xEC\xA1****\x83\xEC\x50\x83\x78\x30\x00\x0F\x84****\x53\x8D\x4D\xD8\xC6\x45\xB4\x01\xC7\x45*****"),
	SIG("\x55\x8B\xEC\x8B\x0D****\x83\xEC\x58\x81\xF9****"),
	SIG("\x55\x8B\xEC\xA1****\x83\xEC\x50"),
};
#elif defined(_WIN64)
static const Sig SIGS[] = {
	SIG("\x48\x89\x5C\x24*\x56\x57\x41\x56\x48\x81\xEC****\x8B\xF2\x4C\x8B\xF1"),
};
#elif defined(__x86_64__)
static const Sig SIGS[] = {
	SIG("\x55\x48\x8D\x05****\x48\x89\xE5\x41\x57\x41\x56\x41\x89\xF6\x41\x55\x49\x89\xFD\x41\x54\x49\x89\xD4\x53\x48\x89\xCB\x48\x81\xEC****\x48\x8B\x3D****\x48\x39\xC7\x74\x25"),
};
#else
static const Sig SIGS[] = { { 0, 0 } };   // 32-bit Linux engine exports the symbol by name
#endif

static bool looks_hooked(const unsigned char* p) {
	return p[0] == 0xE9 || (p[0] == 0xFF && p[1] == 0x25) || (p[0] == 0x68 && p[5] == 0xC3);
}

static bool install_hook() {
	plat::Module m;
	rt_memset(&m, 0, sizeof(m));
	if (!plat::find_engine(m)) { rt_snprintf(g_err, sizeof(g_err), "engine module not found"); return false; }
	CreateInterfaceFn ci = (CreateInterfaceFn)plat::get_proc(m, "CreateInterface");
	if (!ci) { rt_snprintf(g_err, sizeof(g_err), "engine CreateInterface not found"); return false; }
	g_vs = (IVoiceServer*)ci(INTERFACEVERSION_VOICESERVER, 0);
	if (!g_vs) { rt_snprintf(g_err, sizeof(g_err), "VoiceServer002 interface not found"); return false; }

	void* target = plat::get_proc(m, SIG_NAMES_LINUX);
	if (!target) {
		plat::Range rg[16];
		int nr = plat::exec_ranges(m, rg, 16);
		for (size_t i = 0; i < sizeof(SIGS) / sizeof(SIGS[0]) && !target; i++) if (SIGS[i].s)
			for (int k = 0; k < nr && !target; k++) target = sig_scan(rg[k].start, rg[k].len, SIGS[i].s, SIGS[i].n);
	}
	if (!target) { rt_snprintf(g_err, sizeof(g_err), "SV_BroadcastVoiceData not found (engine updated?)"); return false; }
	g_after_other = looks_hooked((const unsigned char*)target);
	if (MH_Initialize() != MH_OK) { rt_snprintf(g_err, sizeof(g_err), "MinHook init failed"); return false; }
	if (MH_CreateHook(target, (void*)&hook_BroadcastVoiceData, (void**)&g_orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
		rt_snprintf(g_err, sizeof(g_err), "could not hook SV_BroadcastVoiceData");
		return false;
	}
	g_target = target;
	return true;
}

static void push_fn(ILuaBase* LUA, const char* name, GarrysMod::Lua::CFunc f) {
	LUA->PushCFunction(f);
	LUA->SetField(-2, name);
}

GMOD_MODULE_OPEN() {
	// The detour must outlive any Lua state: never unhook, never unload. On map change the
	// library stays loaded and the hook simply resumes (other modules may sit on top of it).
	plat::pin_self();
	if (!g_hooked) {
		g_err[0] = 0;
		g_hooked = install_hook();
	}
	g_enabled = g_hooked;
	// forget per-map state (entity indices change meaning on a new map)
	for (int s = 0; s < MAXP; s++) for (int t = 0; t < MAXP; t++) if (g_pick[s][t]) { g_pick[s][t]->t_update = -1e9; g_pick[s][t]->wp_valid = false; }
	for (int s = 0; s < MAXP; s++) {
		for (int r = 0; r < MAXP; r++) if (g_links[s][r]) { g_links[s][r]->t_update = -1e9; g_links[s][r]->in_tx = false; }
		if (g_senders[s]) { g_senders[s]->cl = 0; g_senders[s]->transmitting = false; }
	}

	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->CreateTable();
	push_fn(LUA, "Init", l_init);
	push_fn(LUA, "SetLink", l_setlink);
	push_fn(LUA, "ClearLink", l_clearlink);
	push_fn(LUA, "ClearSender", l_clearsender);
	push_fn(LUA, "ClearAll", l_clearall);
	push_fn(LUA, "Poll", l_poll);
	push_fn(LUA, "GetLinkStats", l_stats);
	push_fn(LUA, "Enable", l_enable);
	push_fn(LUA, "IsHooked", l_ishooked);
	push_fn(LUA, "HookedAfterOther", l_after);
	push_fn(LUA, "GetError", l_error);
	push_fn(LUA, "SetOption", l_setoption);
	push_fn(LUA, "SetPickup", l_setpickup);
	push_fn(LUA, "ClearPickup", l_clearpickup);
	push_fn(LUA, "HasSound", l_hassound);
	push_fn(LUA, "LoadSound", l_loadsound);
	push_fn(LUA, "PlaySound", l_playsound);
	push_fn(LUA, "IsTransmitting", l_istx);
	LUA->PushString(VERSION);
	LUA->SetField(-2, "Version");
	LUA->SetField(-2, "zradio");
	LUA->Pop();
	return 0;
}

GMOD_MODULE_CLOSE() {
	// stop processing; the hook stays in place and just passes packets through
	g_enabled = false;
	for (int s = 0; s < MAXP; s++) if (g_senders[s]) { g_senders[s]->cl = 0; g_senders[s]->transmitting = false; }
	return 0;
}
