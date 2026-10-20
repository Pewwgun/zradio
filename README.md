# zradio

realistic radio thing for zcity, this is jsut the dll tho

`gmsv_zradio` is the Garry's Mod server module behind the Z-City **Walkie-Talkie Radio Test** addon.
It hooks the engine's voice broadcast and gives every radio listener their own copy of the
talker's voice, run through a real-time two-way NBFM radio simulation. The addon's Lua decides
the signal from the game world: distance, walls, antenna height and movement. The module also
mixes in what the transmitting radio's microphone picks up: nearby voices, gunshots, footsteps.

## Downloads

Every push builds all four DLLs with GitHub Actions and tests them:

- **Latest build:** Actions tab → newest "Build" run → **Artifacts** → `gmsv_zradio` (a zip of the four DLLs).
- **Releases:** push a tag like `v1.1.0` and the DLLs are attached to a GitHub release automatically.

| File | For |
|---|---|
| `gmsv_zradio_win32.dll` | Windows, normal GMod branch (most listen servers) |
| `gmsv_zradio_win64.dll` | Windows, x86-64 beta branch |
| `gmsv_zradio_linux.dll` | Linux dedicated server, 32-bit |
| `gmsv_zradio_linux64.dll` | Linux dedicated server, 64-bit |

Put them in `garrysmod/lua/bin/` on the **server** (for a listen server, the host's PC). Players
don't need anything.

## Building locally

On Debian/Ubuntu, Linux or WSL:

```sh
sudo apt install clang lld llvm g++-multilib mingw-w64
./build.sh            # all four, into bin/
./build.sh win32      # or just one
```

Windows DLLs are cross-compiled with clang targeting the MSVC ABI, so their C++ vtables match the
engine's. They link against the system `msvcrt.dll` only, so no Visual C++ runtime is needed on the
server. The Linux builds need glibc 2.17 or newer.

## Tests

`tests/run_tests.sh` builds a fake engine (exports `SV_BroadcastVoiceData` and `VoiceServer002`)
and a test program that loads the real DLL through GMod's real `ILuaBase` interface. It then runs a
voice scenario:
- strong and weak radio listeners
- a listener the game says can't hear
- people standing nearby
- someone talking next to the radio
- a gunshot WAV
- a held-silent stretch

It checks routing, squelch tails, listening-bit restore and a simulated map change. Windows builds
run under Wine. CI runs it on every push.

## Lua API (global `zradio`, server)

| Function | |
|---|---|
| `Init(maxplayers)` | |
| `SetLink(talker, listener, {cnr=, k=, doppler=, offset=, impulse_rate=, impulse_amp=, beat_db=, beat_hz=, squelch=, mic_gain=, volume=})` | refresh at least once a second |
| `ClearLink(t, l)`, `ClearSender(i)`, `ClearAll()` | |
| `Poll()` | call every tick (squelch tails) |
| `GetLinkStats(t, l)` | returns cnr_db, squelch_open, transmitting |
| `SetPickup(source, transmitter, gain, muffle)` | a voice reaching a transmitting radio's microphone |
| `LoadSound(name, filedata)`, `HasSound(name)` | WAV (PCM / MS-ADPCM / IMA-ADPCM), MP3, OGG |
| `PlaySound(transmitter, name, gain, rate, delay_s, muffle)` | mix a sound into a transmission |
| `IsTransmitting(i)`, `Enable(bool)`, `IsHooked()`, `HookedAfterOther()`, `GetError()`, `SetOption(k, v)`, `Version` | |

Entity indices are used throughout (`ply:EntIndex()`).

See `THIRD_PARTY_NOTICES.md` for bundled libraries.
