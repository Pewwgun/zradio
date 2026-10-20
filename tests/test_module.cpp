// test_module.cpp - end-to-end test of a built gmsv_zradio library.
//
// Loads the real module next to a fake engine, opens it through GMod's real ILuaBase interface
// (a minimal Lua stack implemented here), and runs a voice scenario:
//   player 1 transmits on the radio; players 3 and 4 receive it over the air (3 strong, 4 weak);
//   player 5 has a radio link but the game says he can't hear; players 2 and 6 stand nearby.
//   Player 2 also talks next to player 1's radio (microphone pickup), a gunshot WAV is loaded
//   from memory and played into the transmission, and part of the transmission is pure silence.
// Exit code 0 = all checks passed.
#include "testcrt.h"
#include "engine.h"
#include "GarrysMod/Lua/Interface.h"
#include "steamvoice.h"
using namespace GarrysMod::Lua;

// ---------------- a tiny Lua value model
struct Table;
struct Val { int type; double num; bool b; char* str; Table* t; CFunc f; };
struct Table { char* k[64]; Val v[64]; int n; };
static Val mk(int type) { Val v; memset(&v, 0, sizeof(v)); v.type = type; return v; }
static char* dup(const char* s) { size_t n = strlen(s); char* d = (char*)malloc(n + 1); memcpy(d, s, n + 1); return d; }
static Val* tget(Table* t, const char* k) { for (int i = 0; i < t->n; i++) if (!strcmp(t->k[i], k)) return &t->v[i]; return 0; }
static void tset(Table* t, const char* k, Val v) { Val* e = tget(t, k); if (e) { *e = v; return; } t->k[t->n] = dup(k); t->v[t->n++] = v; }
static Table* newtable() { Table* t = (Table*)malloc(sizeof(Table)); memset(t, 0, sizeof(Table)); return t; }

static int g_unexpected = 0;
static int g_binlen = -1;
#define NOPE(name) { printf("  !! unexpected ILuaBase::%s\n", name); g_unexpected++; }

class FakeLua : public ILuaBase {
public:
  Val stk[256]; int top = 0; Table* glob = newtable(); char err[256];
  Val& at(int i) { return i > 0 ? stk[i - 1] : stk[top + i]; }
  void push(Val v) { stk[top++] = v; }
  int Top() override { return top; }
  void Push(int i) override { push(at(i)); }
  void Pop(int n) override { top -= n; }
  void GetTable(int) override NOPE("GetTable")
  void GetField(int i, const char* k) override { Val t = at(i); Val* v = t.type == Type::Table ? tget(t.t, k) : 0; push(v ? *v : mk(Type::Nil)); }
  void SetField(int i, const char* k) override { Val v = stk[top - 1]; Val t = at(i); top--; tset(t.t, k, v); }
  void CreateTable() override { Val v = mk(Type::Table); v.t = newtable(); push(v); }
  void SetTable(int) override NOPE("SetTable")
  void SetMetaTable(int) override NOPE("SetMetaTable")
  bool GetMetaTable(int) override { NOPE("GetMetaTable") return false; }
  void Call(int, int) override NOPE("Call")
  int PCall(int, int, int) override { NOPE("PCall") return 0; }
  int Equal(int, int) override { NOPE("Equal") return 0; }
  int RawEqual(int, int) override { NOPE("RawEqual") return 0; }
  void Insert(int) override NOPE("Insert")
  void Remove(int) override NOPE("Remove")
  int Next(int) override { NOPE("Next") return 0; }
  void* NewUserdata(unsigned int) override { NOPE("NewUserdata") return 0; }
  void ThrowError(const char* e) override { printf("  !! ThrowError: %s\n", e); g_unexpected++; }
  void CheckType(int i, int t) override { if (at(i).type != t) { printf("  !! CheckType failed at %d\n", i); g_unexpected++; } }
  void ArgError(int, const char* m) override { printf("  !! ArgError %s\n", m); g_unexpected++; }
  void RawGet(int) override NOPE("RawGet")
  void RawSet(int) override NOPE("RawSet")
  const char* GetString(int i, unsigned int* len) override { Val& v = at(i); if (len) *len = (g_binlen >= 0 && i == 2) ? (unsigned)g_binlen : (v.str ? (unsigned)strlen(v.str) : 0); return v.str; }
  double GetNumber(int i) override { return at(i).num; }
  bool GetBool(int i) override { return at(i).b; }
  CFunc GetCFunction(int i) override { return at(i).f; }
  void* GetUserdata(int) override { NOPE("GetUserdata") return 0; }
  void PushNil() override { push(mk(Type::Nil)); }
  void PushString(const char* s, unsigned int) override { Val v = mk(Type::String); v.str = dup(s); push(v); }
  void PushNumber(double d) override { Val v = mk(Type::Number); v.num = d; push(v); }
  void PushBool(bool b) override { Val v = mk(Type::Bool); v.b = b; push(v); }
  void PushCFunction(CFunc f) override { Val v = mk(Type::Function); v.f = f; push(v); }
  void PushCClosure(CFunc, int) override NOPE("PushCClosure")
  void PushUserdata(void*) override NOPE("PushUserdata")
  int ReferenceCreate() override { NOPE("ReferenceCreate") return 0; }
  void ReferenceFree(int) override NOPE("ReferenceFree")
  void ReferencePush(int) override NOPE("ReferencePush")
  void PushSpecial(int t) override { if (t == SPECIAL_GLOB) { Val v = mk(Type::Table); v.t = glob; push(v); } else NOPE("PushSpecial(other)") }
  bool IsType(int i, int t) override { return at(i).type == t; }
  int GetType(int i) override { return at(i).type; }
  const char* GetTypeName(int) override { return "?"; }
  void CreateMetaTableType(const char*, int) override NOPE("CreateMetaTableType")
  const char* CheckString(int i) override { if (at(i).type != Type::String) { printf("  !! CheckString\n"); g_unexpected++; } return at(i).str; }
  double CheckNumber(int i) override { if (at(i).type != Type::Number) { printf("  !! CheckNumber arg %d\n", i); g_unexpected++; } return at(i).num; }
  int ObjLen(int) override { NOPE("ObjLen") return 0; }
  const QAngle& GetAngle(int) override { NOPE("GetAngle") static QAngle a; return a; }
  const Vector& GetVector(int) override { NOPE("GetVector") static Vector a; return a; }
  void PushAngle(const QAngle&) override NOPE("PushAngle")
  void PushVector(const Vector&) override NOPE("PushVector")
  void SetState(lua_State*) override {}
  int CreateMetaTable(const char*) override { NOPE("CreateMetaTable") return 0; }
  bool PushMetaTable(int) override { NOPE("PushMetaTable") return false; }
  void PushUserType(void*, int) override NOPE("PushUserType")
  void SetUserType(int, void*) override NOPE("SetUserType")
};

static FakeLua* LUA_;
static lua_State g_L;

// call zradio.<name>(args...) with a fresh frame; results left on the stack
static int call(const char* name, int nargs, Val* args) {
  Val* z = tget(LUA_->glob, "zradio"); if (!z) { printf("no zradio table\n"); exit(1); }
  Val* f = tget(z->t, name); if (!f || f->type != Type::Function) { printf("no zradio.%s\n", name); exit(1); }
  LUA_->top = 0; for (int i = 0; i < nargs; i++) LUA_->push(args[i]);
  int r = f->f(&g_L);
  // move results to the front
  for (int i = 0; i < r; i++) LUA_->stk[i] = LUA_->stk[LUA_->top - r + i];
  LUA_->top = r;
  return r;
}
static Val N(double d) { Val v = mk(Type::Number); v.num = d; return v; }
static Val linkTable(double cnr, double k, double fd) {
  Val v = mk(Type::Table); v.t = newtable();
  tset(v.t, "cnr", N(cnr)); tset(v.t, "k", N(k)); tset(v.t, "doppler", N(fd)); tset(v.t, "squelch", N(0.45));
  return v;
}

class FakeClient : public IClient {
public:
  int slot;
  void ConnectionStart(void*) override {} void ConnectionClosing(const char*) override {} void ConnectionCrashed(const char*) override {}
  void PacketStart(int, int) override {} void PacketEnd() override {} void FileRequested(const char*, unsigned) override {}
  void FileReceived(const char*, unsigned) override {} void FileDenied(const char*, unsigned) override {} void FileSent(const char*, unsigned) override {}
  bool ShouldAcceptFile(const char*, unsigned) override { return false; }
  void Connect(const char*, int, void*, bool, int) override {} void Inactivate() override {} void Reconnect() override {} void Disconnect(const char*, ...) override {}
  int GetPlayerSlot() const override { return slot; } int GetUserID() const override { return slot + 100; }
};
struct Delivery { int sender, receiver, nbytes; unsigned char* data; };

static int g_fail = 0;
#define CHECK(cond, msg) do { bool ok_ = (cond); printf("%s  %s\n", ok_ ? "PASS" : "FAIL", msg); if (!ok_) g_fail++; } while (0)

// synthetic "speech": voiced harmonics with a syllable envelope
static void make_speech(int16_t* out, int n, double f0, unsigned seed) {
  unsigned s = seed;
  for (int i = 0; i < n; i++) {
    double t = i / 24000.0;
    double env = 0.5 + 0.5 * sin(2 * 3.14159265 * 3.1 * t + seed);
    env = env > 0.25 ? env : 0;
    double v = 0;
    for (int h = 1; h <= 6; h++) v += sin(2 * 3.14159265 * f0 * h * t) / h;
    s = s * 1103515245u + 12345u;
    v += ((int)(s >> 16 & 0x7fff) - 16384) / 65536.0;
    out[i] = (int16_t)(v * env * 6000);
  }
}

// a PCM WAV in memory: a gunshot-like burst
static int make_wav(unsigned char* out, int max) {
  int sr = 44100, n = sr / 2; unsigned s = 7;
  if (44 + n * 2 > max) return 0;
  memcpy(out, "RIFF", 4); *(int*)(out + 4) = 36 + n * 2; memcpy(out + 8, "WAVEfmt ", 8); *(int*)(out + 16) = 16;
  *(short*)(out + 20) = 1; *(short*)(out + 22) = 1; *(int*)(out + 24) = sr; *(int*)(out + 28) = sr * 2;
  *(short*)(out + 32) = 2; *(short*)(out + 34) = 16; memcpy(out + 36, "data", 4); *(int*)(out + 40) = n * 2;
  int16_t* p = (int16_t*)(out + 44);
  for (int i = 0; i < n; i++) {
    s = s * 1103515245u + 12345u;
    double nz = ((int)(s >> 16 & 0x7fff) - 16384) / 16384.0;
    double e = 1.0; for (int k = 0; k < i / 400; k++) e *= 0.9;
    p[i] = (int16_t)(nz * e * 30000);
  }
  return 44 + n * 2;
}

static Val S(const char* s) { Val v = mk(Type::String); v.str = dup(s); return v; }

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: test_module <path to gmsv_zradio library>\n"); return 2; }
  void* eng = tc_load(ENGINE_LIB);
  if (!eng) { tc_err("loading fake engine"); return 1; }
  void* mod = tc_load(argv[1]);
  if (!mod) { tc_err("loading module"); return 1; }
  typedef int (*OpenFn)(lua_State*);
  OpenFn open = (OpenFn)tc_sym(mod, "gmod13_open");
  OpenFn close = (OpenFn)tc_sym(mod, "gmod13_close");
  CHECK(open && close, "module exports gmod13_open / gmod13_close");
  if (!open || !close) return 1;
  LUA_ = new (malloc(sizeof(FakeLua))) FakeLua();
  memset(&g_L, 0, sizeof(g_L)); g_L.luabase = LUA_;
  open(&g_L);
  call("IsHooked", 0, 0); CHECK(LUA_->stk[0].b, "hooked SV_BroadcastVoiceData and found VoiceServer002");
  call("GetError", 0, 0); if (LUA_->stk[0].str && LUA_->stk[0].str[0]) printf("      module error: %s\n", LUA_->stk[0].str);
  Val* ver = tget(tget(LUA_->glob, "zradio")->t, "Version");
  printf("      zradio version %s\n", ver ? ver->str : "?");

  typedef void (*SetL)(int, int, bool); typedef bool (*GetL)(int, int);
  SetL set_listen = (SetL)tc_sym(eng, "fake_set_listening");
  GetL get_listen = (GetL)tc_sym(eng, "fake_get_listening");
  typedef int (*NDel)(); typedef Delivery* (*GDel)(int);
  NDel ndel = (NDel)tc_sym(eng, "fake_ndel"); GDel gdel = (GDel)tc_sym(eng, "fake_del");
  typedef void (*Bcast)(IClient*, int, char*, long long);
  Bcast bcast = (Bcast)tc_sym(eng, "_Z21SV_BroadcastVoiceDataP7IClientiPcx");

  Val a[6];
  a[0] = N(6); call("Init", 1, a);
  for (int r = 1; r <= 6; r++) set_listen(r, 1, r == 2 || r == 3 || r == 4 || r == 6);
  for (int r = 1; r <= 6; r++) set_listen(r, 2, r == 1 || r == 4);

  static unsigned char wav[60000];
  int nw = make_wav(wav, sizeof(wav));
  a[0] = S("test/gunshot.wav"); a[1] = mk(Type::String); a[1].str = (char*)wav; g_binlen = nw;
  call("LoadSound", 2, a); g_binlen = -1;
  CHECK(LUA_->stk[0].b, "LoadSound decodes a WAV passed as a Lua string");

  const int SPN = 24000 * 4, SP2 = 24000 * 2;
  static int16_t sp[SPN], sp2[SP2];
  make_speech(sp, SPN, 120, 1); make_speech(sp2, SP2, 210, 2);
  steamvoice::Encoder c1; c1.init(); steamvoice::Encoder c2; c2.init();
  static unsigned char pkt[8192], pk2[8192];
  uint64_t sid1 = 76561198000000001ull, sid2 = 76561198000000002ull;
  FakeClient t1; t1.slot = 0; FakeClient t2; t2.slot = 1;
  const int chunk = 480 * 4, silent_from = 24000 * 2, silent_len = 24000;
  int npk = 0; bool shot = false, played = false;
  for (int i = 0, j = 0, k2 = 0; i < SPN + silent_len; i += chunk) {
    if (npk % 2 == 0) {
      a[0] = N(1); a[1] = N(3); a[2] = linkTable(40, 20, 0.5); call("SetLink", 3, a);
      a[0] = N(1); a[1] = N(4); a[2] = linkTable(9, 0, 2.2); call("SetLink", 3, a);
      a[0] = N(1); a[1] = N(5); a[2] = linkTable(30, 10, 0); call("SetLink", 3, a);
      a[0] = N(2); a[1] = N(1); a[2] = N(0.25); a[3] = N(0); call("SetPickup", 4, a);
    }
    int b;
    if (i >= silent_from && i < silent_from + silent_len) {
      unsigned char* w = pkt; memcpy(w, &sid1, 8); w += 8; *w++ = 11; w[0] = 24000 & 255; w[1] = 24000 >> 8; w += 2;
      *w++ = 0; w[0] = chunk & 255; w[1] = chunk >> 8; w += 2;
      uint32_t c = steamvoice::crc32(pkt, (int)(w - pkt)); memcpy(w, &c, 4); w += 4; b = (int)(w - pkt);
    } else {
      int n = SPN - j < chunk ? SPN - j : chunk; if (n < 0) n = 0;
      b = c1.make_packet(sid1, sp + j, n, j + chunk >= SPN, pkt, sizeof(pkt)); j += chunk;
    }
    if (b > 0) { bcast(&t1, b, (char*)pkt, 0); npk++; }
    if (i >= 24000 && k2 < SP2) {
      int n2 = SP2 - k2 < chunk ? SP2 - k2 : chunk;
      int b2 = c2.make_packet(sid2, sp2 + k2, n2, k2 + chunk >= SP2, pk2, sizeof(pk2)); k2 += chunk;
      if (b2 > 0) bcast(&t2, b2, (char*)pk2, 0);
    }
    if (!shot && i >= silent_from + 9600) {
      a[0] = N(1); a[1] = S("test/gunshot.wav"); a[2] = N(30); a[3] = N(1); a[4] = N(0.015); a[5] = N(0);
      call("PlaySound", 6, a); played = LUA_->stk[0].b; shot = true;
    }
    call("Poll", 0, 0);
    tc_sleep(2);
  }
  CHECK(played, "PlaySound mixes a sound into a keyed transmitter");
  a[0] = N(1); a[1] = N(4); call("GetLinkStats", 2, a);
  CHECK(LUA_->top == 3 && LUA_->stk[2].b, "GetLinkStats returns cnr, squelch, transmitting");
  tc_sleep(300);
  call("Poll", 0, 0);

  // tally what each client received from player 1
  int nd = ndel();
  int cnt[7] = {0}, bytes[7] = {0}, empty[7] = {0}, from2[7] = {0};
  for (int i = 0; i < nd; i++) {
    Delivery* x = gdel(i);
    if (x->sender == 1) { cnt[x->receiver]++; bytes[x->receiver] += x->nbytes; if (!x->nbytes) empty[x->receiver]++; }
    else if (x->sender == 2 && x->nbytes) from2[x->receiver]++;
  }
  printf("      talker packets %d; deliveries per client: ", npk);
  for (int r = 1; r <= 6; r++) printf("%d:%d ", r, cnt[r]);
  printf("\n");
  CHECK(cnt[2] == npk && cnt[6] == npk, "nearby listeners get every original packet");
  CHECK(cnt[3] == npk + 1 && cnt[4] == npk + 1, "radio listeners get their own copy of every packet + a squelch tail");
  CHECK(cnt[5] == 0, "a listener the game says can't hear gets nothing, even with a radio link");
  CHECK(bytes[3] != bytes[2] && bytes[4] != bytes[3], "each radio listener's copy is processed separately");
  CHECK(empty[1] > 0 && bytes[1] == 0, "the talker only gets the engine's empty 'you are talking' packets");
  CHECK(from2[1] > 0 && from2[4] > 0 && from2[3] == 0, "player 2's own voice still goes only to the people near him");

  // decode what receiver 3 heard: continuous through the silent stretch (no tail mid-transmission)
  steamvoice::Decoder d; d.init();
  static int16_t pcm[24000 * 12]; int np = 0;
  for (int i = 0; i < nd; i++) { Delivery* x = gdel(i); if (x->sender != 1 || x->receiver != 3 || !x->nbytes) continue;
    int ns = d.decode_packet(x->data, x->nbytes, pcm + np, 24000 * 12 - np); if (ns > 0) np += ns; }
  double secs = np / 24000.0;
  printf("      receiver 3 decoded %.2f s of audio\n", secs);
  CHECK(secs > 4.9 && secs < 5.6, "receiver 3's audio covers speech + held-silent stretch + tail");
  double e_sil = 0; int ns_sil = 0;
  for (int i = silent_from + 2400; i < silent_from + 7200 && i < np; i++) { e_sil += (double)pcm[i] * pcm[i]; ns_sil++; }
  CHECK(ns_sil > 0 && e_sil / ns_sil > 1.0, "while the key is held silent, the receiver still hears the channel (not squelched)");

  bool ok = true;
  for (int r = 1; r <= 6; r++) {
    if (get_listen(r, 1) != (r == 2 || r == 3 || r == 4 || r == 6)) ok = false;
    if (get_listen(r, 2) != (r == 1 || r == 4)) ok = false;
  }
  CHECK(ok, "every listening bit is restored exactly");
  close(&g_L);
  LUA_->glob = newtable();
  open(&g_L);
  call("IsHooked", 0, 0);
  CHECK(LUA_->stk[0].b, "survives close + reopen (map change) with the hook intact");
  CHECK(g_unexpected == 0, "no unexpected ILuaBase calls");
  printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
  return g_fail ? 1 : 0;
}

#if defined(_WIN32)
extern "C" __declspec(dllimport) int __cdecl __getmainargs(int*, char***, char***, int, int*);
extern "C" int mainCRTStartup() { int argc = 0, si = 0; char** argv = 0; char** env = 0; __getmainargs(&argc, &argv, &env, 0, &si); exit(main(argc, argv)); return 0; }
#endif
