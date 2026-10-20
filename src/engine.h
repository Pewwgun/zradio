// engine.h - the small slice of the Source engine the module touches, plus platform helpers.
#pragma once
#include "minirt.h"

// ---- interfaces (vtable order matches the Source SDK / sourcesdk-minimal, which gm_8bit is built against)
class INetChannelHandler {
public:
	virtual ~INetChannelHandler() {}
	virtual void ConnectionStart(void* chan) = 0;
	virtual void ConnectionClosing(const char* reason) = 0;
	virtual void ConnectionCrashed(const char* reason) = 0;
	virtual void PacketStart(int incoming_sequence, int outgoing_acknowledged) = 0;
	virtual void PacketEnd(void) = 0;
	virtual void FileRequested(const char* fileName, unsigned int transferID) = 0;
	virtual void FileReceived(const char* fileName, unsigned int transferID) = 0;
	virtual void FileDenied(const char* fileName, unsigned int transferID) = 0;
	virtual void FileSent(const char* fileName, unsigned int transferID) = 0;
	virtual bool ShouldAcceptFile(const char* fileName, unsigned int transferID) = 0;
};

class IClient : public INetChannelHandler {
public:
	virtual ~IClient() {}
	virtual void Connect(const char* szName, int nUserID, void* pNetChannel, bool bFakePlayer, int clientChallenge) = 0;
	virtual void Inactivate(void) = 0;
	virtual void Reconnect(void) = 0;
	virtual void Disconnect(const char* reason, ...) = 0;
	virtual int GetPlayerSlot() const = 0;   // entity index - 1
	virtual int GetUserID() const = 0;
};

// Who can hear whom. Receiver/sender are entity indices (1..maxplayers), as CVoiceGameMgr passes them.
class IVoiceServer {
public:
	virtual ~IVoiceServer() {}
	virtual bool GetClientListening(int iReceiver, int iSender) = 0;
	virtual bool SetClientListening(int iReceiver, int iSender, bool bListen) = 0;
	virtual bool SetClientProximity(int iReceiver, int iSender, bool bUseProximity) = 0;
};
#define INTERFACEVERSION_VOICESERVER "VoiceServer002"

typedef void* (*CreateInterfaceFn)(const char* name, int* ret);

// ---- platform
namespace plat {
	struct Module { unsigned char* base; size_t size; void* handle; char path[512]; };

	bool find_engine(Module& m);                               // engine.dll / engine_srv.so / engine_client.so
	void* get_proc(Module& m, const char* name);
	struct Range { unsigned char* start; size_t len; };
	int exec_ranges(Module& m, Range* out, int max);           // executable regions of the module
	double now();                                              // seconds, monotonic
	void pin_self();                                           // keep this library loaded forever
	void log(const char* msg);
}

// Pattern with '*' wildcards (same convention as gm_8bit's signatures).
static inline unsigned char* sig_scan(unsigned char* start, size_t len, const char* sig, size_t siglen) {
	if (len < siglen) return 0;
	for (size_t i = 0; i + siglen <= len; i++) {
		size_t k = 0;
		for (; k < siglen; k++) {
			if (sig[k] != '*' && (unsigned char)sig[k] != start[i + k]) break;
		}
		if (k == siglen) return start + i;
	}
	return 0;
}
