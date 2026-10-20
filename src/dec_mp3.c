/* dec_mp3.c - MP3 via minimp3 (CC0) */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define MAX_SECONDS 12
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3.h"

/* ---------------------------------------------------------------- MP3 */
int zr_decode_mp3(const unsigned char* d, int n, float** out, int* rate) {
	static mp3dec_t dec;     /* ~6 KB: keep it off the stack */
	static float pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	mp3dec_frame_info_t info;
	mp3dec_init(&dec);
	int cap = 0, ns = 0, sr = 0;
	float* o = 0;
	int pos = 0;
	while (pos < n) {
		int s = mp3dec_decode_frame(&dec, d + pos, n - pos, pcm, &info);
		if (info.frame_bytes <= 0) break;
		pos += info.frame_bytes;
		if (s <= 0) continue;
		if (!sr) sr = info.hz;
		if (sr && ns > sr * MAX_SECONDS) break;
		if (ns + s > cap) {
			cap = (ns + s) * 2;
			float* no = (float*)realloc(o, sizeof(float) * cap);
			if (!no) { free(o); return -1; }
			o = no;
		}
		for (int i = 0; i < s; i++) {
			float v = 0;
			for (int c = 0; c < info.channels; c++) v += pcm[i * info.channels + c];
			o[ns++] = v / info.channels;
		}
	}
	if (!ns || !sr) { free(o); return -1; }
	*out = o; *rate = sr;
	return ns;
}

