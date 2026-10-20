# Third-party code

| Component | Where | License |
|---|---|---|
| Opus audio codec (Xiph.Org) | `third_party/opus` | BSD 3-clause, see `third_party/opus/COPYING` |
| MinHook (danielga's multi-platform fork of TsudaKageyu/minhook) | `third_party/minhook` | BSD 2-clause, see `third_party/minhook/LICENSE.txt` |
| minimp3 (lieff) | `third_party/decoders/minimp3.h` | CC0, see `third_party/decoders/minimp3_LICENSE` |
| stb_vorbis (Sean Barrett) | `third_party/decoders/stb_vorbis.c` | Public domain / MIT (dual), see the end of the file |
| Garry's Mod module headers (Facepunch, development branch of gmod-module-base) | `include/GarrysMod` | Facepunch's gmod-module-base |

The Steam voice packet format handling is a clean reimplementation based on the documented
format used by gm_8bit / 8bit_zcity (LGPL-2.1); no code from those projects is included.
