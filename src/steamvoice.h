// steamvoice.h - Steam voice packet <-> 24 kHz PCM, STL-free.
//
// Packet layout (as sent by GMod clients and parsed by gm_8bit):
//   [u64 steamid] { [u8 opcode] payload }* [u32 crc32]
//   opcode 11 (SAMPLERATE): u16 rate
//   opcode  6 (OPUS PLC)  : u16 total_len, then frames of [u16 len][u16 seq][len bytes opus]
//                            len == 0xFFFF marks end of stream (resets the decoder)
//   opcode  0 (SILENCE)   : u16 sample count
#pragma once
#include "minirt.h"
#include "opus.h"

namespace steamvoice {

enum { OP_SILENCE = 0, OP_CODEC_OPUSPLC = 6, OP_SAMPLERATE = 11 };
static const int RATE = 24000;
static const int FRAME = 480;      // 20 ms

static inline uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void wr16(unsigned char* p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

static uint32_t crc_table[256];
static bool crc_ready = false;
static void crc_init() {
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
	crc_ready = true;
}
static uint32_t crc32(const unsigned char* d, int n) {
	if (!crc_ready) crc_init();
	uint32_t c = 0xFFFFFFFFu;
	for (int i = 0; i < n; i++) c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

struct Decoder {
	OpusDecoder* dec;
	uint16_t seq;
	bool init() { int err = 0; dec = opus_decoder_create(RATE, 1, &err); seq = 0; return dec != 0; }
	void destroy() { if (dec) opus_decoder_destroy(dec); dec = 0; }

	// returns samples written, or -1 if the packet is malformed / not Opus
	int decode_packet(const unsigned char* data, int n, int16_t* out, int max_samples) {
		if (n < 8 + 4) return -1;
		const unsigned char* p = data + 8;
		const unsigned char* end = data + n - 4;
		int written = 0;
		while (p < end) {
			int op = *p++;
			if (op == OP_SAMPLERATE) {
				if (p + 2 > end) return -1;
				p += 2;
			} else if (op == OP_SILENCE) {
				// the client is keyed but quiet: keep time moving (hiss, background sound continue)
				if (p + 2 > end) return -1;
				int cnt = rd16(p); p += 2;
				if (cnt > max_samples - written) cnt = max_samples - written;
				if (cnt > 0) { rt_memset(out + written, 0, cnt * sizeof(int16_t)); written += cnt; }
			} else if (op == OP_CODEC_OPUSPLC) {
				if (p + 2 > end) return -1;
				int len = rd16(p); p += 2;
				if (p + len > end) return -1;
				int r = decode_frames(p, len, out + written, max_samples - written);
				if (r < 0) return -1;
				written += r;
				p += len;
			} else {
				return -1;
			}
		}
		return written;
	}

	int decode_frames(const unsigned char* p, int n, int16_t* out, int max_samples) {
		const unsigned char* end = p + n;
		int written = 0;
		while (p + 2 <= end) {
			uint16_t len = rd16(p); p += 2;
			if (len == 0xFFFF) { opus_decoder_ctl(dec, OPUS_RESET_STATE); seq = 0; continue; }
			if (p + 2 > end) return -1;
			uint16_t s = rd16(p); p += 2;
			if (s < seq) opus_decoder_ctl(dec, OPUS_RESET_STATE);
			else if (s > seq) {
				int lost = s - seq; if (lost > 10) lost = 10;
				for (int i = 0; i < lost && max_samples - written >= FRAME; i++) {
					int r = opus_decode(dec, 0, 0, out + written, max_samples - written, 0);
					if (r < 0) break;
					written += r;
				}
			}
			seq = (uint16_t)(s + 1);
			if (len == 0 || p + len > end) return -1;
			int r = opus_decode(dec, p, len, out + written, max_samples - written, 0);
			if (r < 0) return -1;
			written += r;
			p += len;
		}
		return written;
	}
};

struct Encoder {
	OpusEncoder* enc;
	uint16_t seq;
	int16_t pend[FRAME];
	int npend;
	bool init() {
		int err = 0;
		enc = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &err);
		if (enc) opus_encoder_ctl(enc, OPUS_SET_BITRATE(32000));   // keep the hiss reasonably intact
		seq = 0; npend = 0;
		return enc != 0;
	}
	void destroy() { if (enc) opus_encoder_destroy(enc); enc = 0; }
	void reset() { if (enc) opus_encoder_ctl(enc, OPUS_RESET_STATE); seq = 0; npend = 0; }

	// Build a complete Steam voice packet from PCM. Partial frames are kept for next time.
	// final = end of transmission: pads the last frame and appends the end-of-stream marker.
	// Returns packet bytes (0 if nothing to send yet), -1 on error.
	int make_packet(uint64_t steamid, const int16_t* pcm, int n, bool final, unsigned char* out, int max) {
		if (max < 8 + 3 + 3 + 4) return -1;
		unsigned char* w = out;
		unsigned char* wend = out + max - 4;
		rt_memcpy(w, &steamid, 8); w += 8;
		*w++ = OP_SAMPLERATE; wr16(w, RATE); w += 2;
		*w++ = OP_CODEC_OPUSPLC;
		unsigned char* lenp = w; w += 2;
		unsigned char* frames0 = w;
		int i = 0;
		int frames = 0;
		while (true) {
			// fill the pending frame
			while (npend < FRAME && i < n) pend[npend++] = pcm[i++];
			if (npend < FRAME) {
				if (!final || npend == 0) break;
				while (npend < FRAME) pend[npend++] = 0;
			}
			if (w + 4 + 1275 > wend) return -1;
			unsigned char* flen = w; w += 2;
			wr16(w, seq++); w += 2;
			int b = opus_encode(enc, pend, FRAME, w, 1275);
			if (b < 0) return -1;
			wr16(flen, (uint16_t)b);
			w += b;
			npend = 0;
			frames++;
		}
		if (final) {
			if (w + 2 > wend) return -1;
			wr16(w, 0xFFFF); w += 2;
			opus_encoder_ctl(enc, OPUS_RESET_STATE);
			seq = 0;
		}
		if (w == frames0) return 0;
		(void)frames;
		wr16(lenp, (uint16_t)(w - frames0));
		uint32_t c = crc32(out, (int)(w - out));
		rt_memcpy(w, &c, 4); w += 4;
		return (int)(w - out);
	}
};

} // namespace steamvoice
