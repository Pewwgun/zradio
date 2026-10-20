// fake_engine.cpp - stands in for engine.dll / engine_srv.so: exports SV_BroadcastVoiceData under the
// Linux symbol name (the module looks that up before signature scanning) and a VoiceServer002
// interface, and records every packet each client would have received.
#include "testcrt.h"
#include "engine.h"
struct Delivery { int sender, receiver, nbytes; unsigned char* data; };
static Delivery g_del[20000]; static int g_ndel = 0;
static bool listening[129][129];
static int g_maxclients = 6;
class VS : public IVoiceServer {
public:
  bool GetClientListening(int r, int s) override { return listening[r][s]; }
  bool SetClientListening(int r, int s, bool b) override { if (r < 1 || r > g_maxclients) return false; listening[r][s] = b; return true; }
  bool SetClientProximity(int, int, bool) override { return true; }
};
static VS* vs;
#define X TEST_EXPORT

X void* CreateInterface(const char* n, int*) { if (!vs) vs = new (malloc(sizeof(VS))) VS(); return strcmp(n, "VoiceServer002") == 0 ? vs : 0; }
X void fake_set_listening(int r, int s, bool b) { listening[r][s] = b; }
X bool fake_get_listening(int r, int s) { return listening[r][s]; }
X int fake_ndel() { return g_ndel; }
X Delivery* fake_del(int i) { return &g_del[i]; }
// export under the Linux symbol name: the module tries that name before signature scanning
X TEST_NOINLINE void _Z21SV_BroadcastVoiceDataP7IClientiPcx(IClient* cl, int nBytes, char* data, long long xuid) {
  volatile int slot = cl->GetPlayerSlot();
  for (int i = 1; i <= g_maxclients; i++) {
    bool self = (i == slot + 1), hears = listening[i][slot + 1];
    if (!hears && !self) continue;
    Delivery& d = g_del[g_ndel++]; d.sender = slot + 1; d.receiver = i; d.nbytes = hears ? nBytes : 0;
    d.data = 0; if (hears) { d.data = (unsigned char*)malloc(nBytes); memcpy(d.data, data, nBytes); }
  }
  (void)xuid;
}

