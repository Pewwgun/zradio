/* dec_ogg.c - Ogg Vorbis via stb_vorbis (public domain) */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define MAX_SECONDS 12
#ifndef NDEBUG
#define NDEBUG
#endif
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define NDEBUG
#include "stb_vorbis.c"

/* ---------------------------------------------------------------- OGG */
int zr_decode_ogg(const unsigned char* d, int n, float** out, int* rate) {
	int ch = 0, sr = 0;
	short* s16 = 0;
	int ns = stb_vorbis_decode_memory(d, n, &ch, &sr, &s16);
	if (ns <= 0 || ch < 1 || !s16) { if (s16) free(s16); return -1; }
	if (ns > sr * MAX_SECONDS) ns = sr * MAX_SECONDS;
	float* o = (float*)malloc(sizeof(float) * (ns + 1));
	if (!o) { free(s16); return -1; }
	for (int i = 0; i < ns; i++) {
		float v = 0;
		for (int c = 0; c < ch; c++) v += s16[i * ch + c];
		o[i] = v / ch / 32768.0f;
	}
	free(s16);
	*out = o; *rate = sr;
	return ns;
}

