/* audiofile.c - decode a game sound file (as read from the server's content) to mono float PCM.
 * WAV: PCM 8/16/24/32-bit, IEEE float, MS-ADPCM, IMA-ADPCM (all used by Source games)
 * MP3: minimp3 (CC0)    OGG: stb_vorbis (public domain)
 * Built as plain C (GNU target on Windows, like Opus), called from the C++ module.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#define MAX_SECONDS 12

static int rd16(const unsigned char* p) { return (int16_t)(p[0] | (p[1] << 8)); }
static unsigned ru16(const unsigned char* p) { return p[0] | (p[1] << 8); }
static uint32_t ru32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ---------------------------------------------------------------- WAV */
static const int ms_adapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };
static const int ms_coef_def[7][2] = { {256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232} };
static const int ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int ima_step[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
	97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
	1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
	8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };

static int clamp16(int v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); }

static int decode_wav(const unsigned char* d, int n, float** out, int* rate) {
	if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return -1;
	const unsigned char* fmt = 0; uint32_t fmtlen = 0;
	const unsigned char* data = 0; uint32_t datalen = 0;
	int p = 12;
	while (p + 8 <= n) {
		uint32_t cl = ru32(d + p + 4);
		const unsigned char* c = d + p + 8;
		if (!memcmp(d + p, "fmt ", 4)) { fmt = c; fmtlen = cl; }
		else if (!memcmp(d + p, "data", 4)) { data = c; datalen = cl; if (p + 8 + (int64_t)datalen > n) datalen = n - p - 8; }
		p += 8 + cl + (cl & 1);
	}
	if (!fmt || !data || fmtlen < 16) return -1;
	int tag = ru16(fmt), ch = ru16(fmt + 2), bits = ru16(fmt + 14);
	int sr = (int)ru32(fmt + 4), align = ru16(fmt + 12);
	if (tag == 0xFFFE && fmtlen >= 26) tag = ru16(fmt + 24);
	if (ch < 1 || ch > 8 || sr < 1000 || sr > 192000) return -1;
	*rate = sr;
	int maxs = sr * MAX_SECONDS;
	float* o = 0; int ns = 0;
	if (tag == 1 || tag == 3) {
		int bps = bits / 8;
		if (bps < 1 || bps > 4) return -1;
		ns = (int)(datalen / (bps * ch));
		if (ns > maxs) ns = maxs;
		o = (float*)malloc(sizeof(float) * (ns + 1));
		if (!o) return -1;
		for (int i = 0; i < ns; i++) {
			float acc = 0;
			for (int c = 0; c < ch; c++) {
				const unsigned char* s = data + (i * ch + c) * bps;
				float v;
				if (tag == 3 && bps == 4) { float f; memcpy(&f, s, 4); v = f; }
				else if (bps == 1) v = (s[0] - 128) / 128.0f;
				else if (bps == 2) v = rd16(s) / 32768.0f;
				else if (bps == 3) v = (int32_t)((s[0] << 8) | (s[1] << 16) | ((uint32_t)s[2] << 24)) / 2147483648.0f;
				else v = (int32_t)ru32(s) / 2147483648.0f;
				acc += v;
			}
			o[i] = acc / ch;
		}
	} else if (tag == 2) {   /* MS-ADPCM */
		if (ch > 2 || align < 7 * ch) return -1;
		int coef[32][2]; int ncoef = 7;
		for (int i = 0; i < 7; i++) { coef[i][0] = ms_coef_def[i][0]; coef[i][1] = ms_coef_def[i][1]; }
		if (fmtlen >= 22) {
			int nc = ru16(fmt + 20);
			if (nc > 0 && nc <= 32 && fmtlen >= 22 + (uint32_t)nc * 4) {
				ncoef = nc;
				for (int i = 0; i < nc; i++) { coef[i][0] = rd16(fmt + 22 + i * 4); coef[i][1] = rd16(fmt + 24 + i * 4); }
			}
		}
		int spb = (align - 7 * ch) * 2 / ch + 2;
		int nblocks = (int)(datalen / align);
		int total = nblocks * spb; if (total > maxs) total = maxs;
		o = (float*)malloc(sizeof(float) * (total + spb + 1));
		if (!o) return -1;
		for (int b = 0; b < nblocks && ns < total; b++) {
			const unsigned char* blk = data + b * align;
			int pred[2], delta[2], s1[2], s2[2];
			const unsigned char* q = blk;
			for (int c = 0; c < ch; c++) { pred[c] = q[0] < ncoef ? q[0] : 0; q++; }
			for (int c = 0; c < ch; c++) { delta[c] = rd16(q); q += 2; }
			for (int c = 0; c < ch; c++) { s1[c] = rd16(q); q += 2; }
			for (int c = 0; c < ch; c++) { s2[c] = rd16(q); q += 2; }
			/* first two samples are stored directly (sample2 first) */
			float a = 0, bsm = 0;
			for (int c = 0; c < ch; c++) { a += s2[c]; bsm += s1[c]; }
			o[ns++] = a / ch / 32768.0f; o[ns++] = bsm / ch / 32768.0f;
			int nib = 0, got = 0; float acc = 0;
			const unsigned char* end = blk + align;
			while (q < end && ns < total + spb) {
				for (int h = 0; h < 2; h++) {
					int c = nib % ch;
					int code = h == 0 ? (*q >> 4) : (*q & 15);
					int sc = code >= 8 ? code - 16 : code;
					int predv = (s1[c] * coef[pred[c]][0] + s2[c] * coef[pred[c]][1]) / 256 + sc * delta[c];
					predv = clamp16(predv);
					s2[c] = s1[c]; s1[c] = predv;
					delta[c] = ms_adapt[code] * delta[c] / 256; if (delta[c] < 16) delta[c] = 16;
					acc += predv; got++;
					if (got == ch) { o[ns++] = acc / ch / 32768.0f; acc = 0; got = 0; }
					nib++;
				}
				q++;
			}
		}
		if (ns > total) ns = total;
	} else if (tag == 0x11) {   /* IMA-ADPCM */
		if (ch > 2 || align < 4 * ch) return -1;
		int spb = (align - 4 * ch) * 2 / ch + 1;
		int nblocks = (int)(datalen / align);
		int total = nblocks * spb; if (total > maxs) total = maxs;
		o = (float*)malloc(sizeof(float) * (total + spb + 1));
		if (!o) return -1;
		float* tmp = (float*)malloc(sizeof(float) * (spb + 8) * 2);
		if (!tmp) { free(o); return -1; }
		for (int b = 0; b < nblocks && ns < total; b++) {
			const unsigned char* blk = data + b * align;
			for (int c = 0; c < ch; c++) {
				int pv = rd16(blk + c * 4), idx = blk[c * 4 + 2]; if (idx > 88) idx = 88;
				float* t = tmp + c * (spb + 8);
				int k = 0; t[k++] = pv / 32768.0f;
				/* data: groups of 4 bytes per channel, interleaved */
				const unsigned char* q = blk + 4 * ch + c * 4;
				while (q + 4 <= blk + align && k < spb) {
					for (int j = 0; j < 4 && k < spb; j++) {
						for (int h = 0; h < 2 && k < spb; h++) {
							int code = h == 0 ? (q[j] & 15) : (q[j] >> 4);
							int st = ima_step[idx], diff = st >> 3;
							if (code & 1) diff += st >> 2;
							if (code & 2) diff += st >> 1;
							if (code & 4) diff += st;
							pv = (code & 8) ? pv - diff : pv + diff;
							pv = clamp16(pv);
							idx += ima_index[code]; if (idx < 0) idx = 0; if (idx > 88) idx = 88;
							t[k++] = pv / 32768.0f;
						}
					}
					q += 4 * ch;
				}
			}
			for (int k = 0; k < spb && ns < total; k++) {
				float acc = 0;
				for (int c = 0; c < ch; c++) acc += tmp[c * (spb + 8) + k];
				o[ns++] = acc / ch;
			}
		}
		free(tmp);
	} else {
		return -1;
	}
	*out = o;
	return ns;
}

int zr_decode_mp3(const unsigned char* d, int n, float** out, int* rate);   /* dec_mp3.c */
int zr_decode_ogg(const unsigned char* d, int n, float** out, int* rate);   /* dec_ogg.c */

/* returns samples (mono float, *out malloc'd: free with zr_free_audio), or -1 */
int zr_decode_audio(const unsigned char* d, int n, float** out, int* rate) {
	*out = 0; *rate = 0;
	if (!d || n < 16) return -1;
	if (!memcmp(d, "RIFF", 4)) return decode_wav(d, n, out, rate);
	if (!memcmp(d, "OggS", 4)) return zr_decode_ogg(d, n, out, rate);
	return zr_decode_mp3(d, n, out, rate);    /* ID3 tag or raw MPEG frames */
}

void zr_free_audio(float* p) { free(p); }
