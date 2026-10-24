#include "ProofRecorder.hpp"

#include "Command.hpp"
#include "Event.hpp"
#include "Modules/Client.hpp"
#include "Modules/Console.hpp"
#include "Modules/Engine.hpp"
#include "Modules/EngineDemoRecorder.hpp"
#include "Modules/Server.hpp"
#include "Modules/Tier1.hpp"
#include "Offsets.hpp"
#include "ProofConvert.hpp"
#include "SAR.hpp"
#include "Utils.hpp"
#include "Utils/Memory.hpp"
#include "Variable.hpp"
#include "Version.hpp"

#include "Entity.hpp"

#include <bitset>
#include <map>
#include <chrono>
#include <filesystem>

using namespace SarProof;

ProofRecorder *proofRecorder;

void ProofDebugLoadTimingStart();

Variable sar_proof_replace_demos("sar_proof_replace_demos", "0",
	"Make 'record' and 'stop' record SAR proofs (.sarp) instead of demos.\n");

// Signon states
#define SS_NEW 3
#define SS_SPAWN 5
#define SS_FULL 6

// CLocalNetworkBackdoor layout (Portal 2 engine branch)
#define BD_ENTS_ALIVE 0x0
#define BD_CREATED 0x200
#define BD_NCREATED 0x1200
#define BD_CHANGED 0x1204
#define BD_NCHANGED 0x2204
#define BD_CACHE 0x2224  // CCachedEntState[MAX_EDICTS] {bool dormant; int serial; void *data; IClientNetworkable *net;}
#define BD_CACHE_STRIDE 16

// CFrameSnapshot
#define SNAP_TICK 0x4
#define SNAP_ENTITIES 0x8  // CFrameSnapshotEntry[] {ServerClass *, int serial, handle}
#define SNAP_NUM_ENTITIES 0xC
#define SNAP_ENTRY_STRIDE 12

#define PRECALC_NUM_DT_PROXIES 0xF8
#define CLIENTSTATE_NETCHANNEL 0x10
#define NETCHAN_GETSEQUENCEDATA 54
#define INPUT_ENCODEUSERCMD 6

#define MAX_EDICTS 2048
#define INVALID_STRING_INDEX 65535
#define SENDPROXY_RECIPIENTS_SIZE 8
#define MAX_DELTA_PROPS 4096

// INetworkStringTableContainer / INetworkStringTable vtable slots
#define STC_FINDTABLE 3
#define STC_GETTABLE 4
#define STC_GETNUMTABLES 5
#define ST_GETTABLENAME 1
#define ST_GETNUMSTRINGS 3
#define ST_CHANGEDSINCETICK 7
#define ST_ADDSTRING 8
#define ST_GETSTRING 9
#define ST_GETSTRINGUSERDATA 11
#define ST_FINDSTRINGINDEX 12

// old_bf_write as the engine lays it out
struct EngineBfWrite {
	unsigned char *data;
	int bytes;
	int bits;
	int curBit;
	bool overflow;
	bool assertOnOverflow;
	const char *debugName;

	void Start(void *buf, int nBytes) {
		data = (unsigned char *)buf;
		bytes = nBytes & ~3;
		bits = bytes * 8;
		curBit = 0;
		overflow = false;
		assertOnOverflow = false;
		debugName = "SarProof";
	}
	void Bit(int v) {
		if (curBit + 1 > bits) {
			overflow = true;
			return;
		}
		uint8_t m = (uint8_t)(1 << (curBit & 7));
		if (v) data[curBit >> 3] |= m;
		else data[curBit >> 3] &= ~m;
		++curBit;
	}
	void UBits(uint32_t v, int n) {
		for (int i = 0; i < n; ++i) Bit((v >> i) & 1);
	}
	void UBitVar(uint32_t v) {
		UBits(v & 15, 4);
		if (v < 16) {
			UBits(0, 2);
		} else if (v < 256) {
			UBits(1, 2);
			UBits(v >> 4, 4);
		} else if (v < 4096) {
			UBits(2, 2);
			UBits(v >> 4, 8);
		} else {
			UBits(3, 2);
			UBits(v >> 4, 28);
		}
	}
	int BytesWritten() const { return (curBit + 7) >> 3; }
};

struct EngineUtlMemory {
	void *memory;
	int allocationCount;
	int growSize;
};

using SendTable_Encode_t = bool(__cdecl *)(const SendTable *pTable, const void *pStruct, EngineBfWrite *pOut, int objectID, EngineUtlMemory *pRecipients, bool bNonZeroOnly);
using SendTable_CalcDelta_t = int(__cdecl *)(const SendTable *pTable, const void *pFromState, int nFromBits, const void *pToState, int nToBits, int *pDeltaProps, int nMaxDeltaProps, int objectID);
using CullPropsFromProxies_t = int(__cdecl *)(const SendTable *pTable, const int *pStartProps, int nStartProps, int iClient, const void *pOldStateProxies, int nOldStateProxies, const void *pNewStateProxies, int nNewStateProxies, int *pOutProps, int nMaxOutProps);
using SendTable_WritePropList_t = void(__cdecl *)(const SendTable *pTable, const void *pState, int nBits, EngineBfWrite *pOut, int objectID, const int *pCheckProps, int nCheckProps);
using DataTable_WriteBuffer_t = void(__cdecl *)(ServerClass *pClasses, EngineBfWrite *pBuf);
using GetClientCmdInfo_t = void(__rescall *)(void *thisptr, void *info);
using ForceFullUpdate_t = void(__rescall *)(void *thisptr, const char *reason);
using GetHostVersion_t = int(__cdecl *)();
using PackEntitiesBackdoor_t = void(__cdecl *)(int clientCount, void **clients, void *snapshot);
using EndEntityStateUpdate_t = void(__rescall *)(void *thisptr);
using DirectUpdate_t = void(__rescall *)(void *thisptr, int tick);

static SendTable_Encode_t SendTable_Encode;
static SendTable_CalcDelta_t SendTable_CalcDelta;
static CullPropsFromProxies_t CullPropsFromProxies;
static SendTable_WritePropList_t SendTable_WritePropList;
static DataTable_WriteBuffer_t DataTable_WriteSendTablesBuffer;
static DataTable_WriteBuffer_t DataTable_WriteClassInfosBuffer;
static GetClientCmdInfo_t GetClientCmdInfo;
static ForceFullUpdate_t ForceFullUpdate;
static GetHostVersion_t GetHostVersion;
static void **g_pLocalNetworkBackdoor;
static uint8_t *g_customDataCallbacks;  // CUtlVector<DemoCustomDataCallbackMenuItem_t>
static int g_resetInterpolationOffset;
static void *g_serverStringTables;
static void *g_clientStringTables;

#ifdef _WIN32
static PackEntitiesBackdoor_t PackEntitiesBackdoor_Target, PackEntitiesBackdoor_Orig;
static EndEntityStateUpdate_t EndEntityStateUpdate_Target, EndEntityStateUpdate_Orig;
static DirectUpdate_t DirectUpdate_Target, DirectUpdate_Orig;

static void __cdecl PackEntitiesBackdoor_Hook(int clientCount, void **clients, void *snapshot) {
	if (proofRecorder) proofRecorder->OnPackEntitiesBackdoor(snapshot);
	PackEntitiesBackdoor_Orig(clientCount, clients, snapshot);
}
static void __fastcall EndEntityStateUpdate_Hook(void *thisptr, void *edx) {
	if (proofRecorder) proofRecorder->OnEndEntityStateUpdate(thisptr);
	EndEntityStateUpdate_Orig(thisptr);
}
static void __fastcall DirectUpdate_Hook(void *thisptr, void *edx, int tick) {
	if (proofRecorder) proofRecorder->OnStringTableDirectUpdate(thisptr, tick);
	DirectUpdate_Orig(thisptr, tick);
}

template <typename T>
static bool InstallHook(T target, T hook, T *orig) {
	if (!target) return false;
	MH_Initialize();  // no-op when already initialized
	return MH_CreateHook((LPVOID)target, (LPVOID)hook, (LPVOID *)orig) == MH_OK && MH_EnableHook((LPVOID)target) == MH_OK;
}
template <typename T>
static void RemoveHook(T target) {
	if (!target) return;
	MH_DisableHook((LPVOID)target);
	MH_RemoveHook((LPVOID)target);
}
#endif

template <typename R, typename... Args>
static R VCall(void *obj, int index, Args... args) {
	using Fn = R(__rescall *)(void *, Args...);
	return (*(Fn **)obj)[index](obj, args...);
}

static int CountServerClasses(ServerClass *classes) {
	int n = 0;
	for (auto c = classes; c; c = c->m_pNext) ++n;
	return n;
}

static double Now() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct ProofRecorder::CostTimer {
	CostStat &stat;
	double start;
	CostTimer(CostStat &stat)
		: stat(stat)
		, start(Now()) {}
	~CostTimer() {
		double d = Now() - this->start;
		this->stat.total += d;
		++this->stat.count;
		if (d > this->stat.max) this->stat.max = d;
	}
};

ProofRecorder::ProofRecorder() {
	this->ents.resize(MAX_EDICTS);
	this->entBuf.resize(256 * 1024);  // 1 MiB, dword aligned for the engine's bf_write
	this->encBuf.resize(16 * 1024 + 4);
	this->propsA.resize(MAX_DELTA_PROPS);
	this->propsB.resize(MAX_DELTA_PROPS);
	this->hasLoaded = true;
}

bool ProofRecorder::Init() {
#ifdef _WIN32
	auto mod = MODULE("engine");
	SendTable_Encode = (SendTable_Encode_t)Memory::Scan(mod, Offsets::SendTable_Encode);
	SendTable_CalcDelta = (SendTable_CalcDelta_t)Memory::Scan(mod, Offsets::SendTable_CalcDelta);
	CullPropsFromProxies = (CullPropsFromProxies_t)Memory::Scan(mod, Offsets::CullPropsFromProxies);
	SendTable_WritePropList = (SendTable_WritePropList_t)Memory::Scan(mod, Offsets::SendTable_WritePropList);
	DataTable_WriteSendTablesBuffer = (DataTable_WriteBuffer_t)Memory::Scan(mod, Offsets::DataTable_WriteSendTablesBuffer);
	DataTable_WriteClassInfosBuffer = (DataTable_WriteBuffer_t)Memory::Scan(mod, Offsets::DataTable_WriteClassInfosBuffer);
	GetClientCmdInfo = (GetClientCmdInfo_t)Memory::Scan(mod, Offsets::CDemoRecorder_GetClientCmdInfo);
	ForceFullUpdate = (ForceFullUpdate_t)Memory::Scan(mod, Offsets::CBaseClientState_ForceFullUpdate);
	auto packBackdoor = Memory::Scan(mod, Offsets::PackEntities_NetworkBackDoor);
	auto endUpdate = Memory::Scan(mod, Offsets::CLocalNetworkBackdoor_EndEntityStateUpdate);
	auto directUpdate = Memory::Scan(mod, Offsets::CNetworkStringTableContainer_DirectUpdate);
	auto computePacks = Memory::Scan(mod, Offsets::SV_ComputeClientPacks);
	auto startupDemoFile = Memory::Scan(mod, Offsets::StartupDemoFile);

	// g_pLocalNetworkBackdoor: SV_ComputeClientPacks loads it into ecx right
	// after calling PackEntities_NetworkBackDoor
	if (computePacks && packBackdoor) {
		for (uintptr_t p = computePacks; p < computePacks + 0x400; ++p) {
			if (*(uint8_t *)p == 0xE8 && p + 5 + *(int32_t *)(p + 1) == packBackdoor) {
				if (*(uint16_t *)(p + 5) == 0x0D8B) g_pLocalNetworkBackdoor = *(void ***)(p + 7);
				break;
			}
		}
	}
	// CDemoRecorder::m_bResetInterpolation: "cmp byte ptr [ebx+off], 0" in GetClientCmdInfo
	if (GetClientCmdInfo) {
		for (uintptr_t p = (uintptr_t)GetClientCmdInfo; p < (uintptr_t)GetClientCmdInfo + 0x80; ++p) {
			if (*(uint16_t *)p == 0xBB80 && *(uint8_t *)(p + 6) == 0) {
				g_resetInterpolationOffset = *(int32_t *)(p + 2);
				break;
			}
		}
	}
	if (startupDemoFile) {
		for (uintptr_t p = startupDemoFile; p < startupDemoFile + 0x400 && !(g_customDataCallbacks && GetHostVersion); ++p) {
			// "mov edi, [count]" followed shortly by "mov ecx, [memory]" (count - 0xC)
			if (!g_customDataCallbacks && *(uint16_t *)p == 0x3D8B) {
				uintptr_t count = *(uint32_t *)(p + 2);
				for (uintptr_t q = p + 6; q < p + 40; ++q) {
					if (*(uint16_t *)q == 0x0D8B && *(uint32_t *)(q + 2) == count - 0xC) {
						g_customDataCallbacks = (uint8_t *)(count - 0xC);
						break;
					}
				}
			}
			// "mov dword ptr [esi+8], 4" (demoprotocol) then "call GetHostVersion"
			if (!GetHostVersion && !memcmp((void *)p, "\xC7\x46\x08\x04\x00\x00\x00\xE8", 8)) {
				GetHostVersion = (GetHostVersion_t)(p + 12 + *(int32_t *)(p + 8));
			}
		}
	}

	g_serverStringTables = Interface::GetPtr(mod, "VEngineServerStringTable001");
	g_clientStringTables = Interface::GetPtr(mod, "VEngineClientStringTable001");

	bool ok = SendTable_Encode && SendTable_CalcDelta && CullPropsFromProxies && SendTable_WritePropList && DataTable_WriteSendTablesBuffer && DataTable_WriteClassInfosBuffer && GetClientCmdInfo && g_pLocalNetworkBackdoor && g_resetInterpolationOffset && g_serverStringTables && g_clientStringTables && packBackdoor && endUpdate && directUpdate && engine->demorecorder->s_ClientDemoRecorder;
	if (!ok) {
		console->DevWarning("SAR proofs unavailable: engine functions not found\n");
		return this->available = false;
	}

	PackEntitiesBackdoor_Target = (PackEntitiesBackdoor_t)packBackdoor;
	EndEntityStateUpdate_Target = (EndEntityStateUpdate_t)endUpdate;
	DirectUpdate_Target = (DirectUpdate_t)directUpdate;
	ok = InstallHook(PackEntitiesBackdoor_Target, (PackEntitiesBackdoor_t)&PackEntitiesBackdoor_Hook, &PackEntitiesBackdoor_Orig);
	ok = ok && InstallHook(EndEntityStateUpdate_Target, (EndEntityStateUpdate_t)&EndEntityStateUpdate_Hook, &EndEntityStateUpdate_Orig);
	ok = ok && InstallHook(DirectUpdate_Target, (DirectUpdate_t)&DirectUpdate_Hook, &DirectUpdate_Orig);
	if (!ok) {
		this->Shutdown();
		console->DevWarning("SAR proofs unavailable: hooks failed\n");
		return this->available = false;
	}
	return this->available = true;
#else
	return this->available = false;
#endif
}

void ProofRecorder::Shutdown() {
	if (this->recording) this->Stop("SAR unloaded");
#ifdef _WIN32
	RemoveHook(PackEntitiesBackdoor_Target);
	RemoveHook(EndEntityStateUpdate_Target);
	RemoveHook(DirectUpdate_Target);
	PackEntitiesBackdoor_Target = nullptr;
	EndEntityStateUpdate_Target = nullptr;
	DirectUpdate_Target = nullptr;
#endif
	this->available = false;
}

int ProofRecorder::Clock() {
	if (this->multiplayer) {
		return (int)(0.5 + *engine->net_time / engine->GetIPT());
	}
	int host, server, client;
	engine->GetTicks(host, server, client);
	return client;
}

bool ProofRecorder::BackdoorActive() {
	return g_pLocalNetworkBackdoor && *g_pLocalNetworkBackdoor;
}

void ProofRecorder::Emit(RecordType type, const ByteWriter &payload) {
	this->pending.U8(type);
	this->pending.Var(payload.buf.size());
	this->pending.Bytes(payload.buf.data(), payload.buf.size());
	if (this->pending.Size() > 4 * 1024 * 1024) this->Flush(true);
}

void ProofRecorder::Flush(bool force) {
	if (this->pending.buf.empty()) return;
	double now = Now();
	if (!force && this->pending.Size() < 256 * 1024 && now - this->lastFlush < 0.5) return;
	this->lastFlush = now;
	this->writer.Submit(std::move(this->pending.buf));
	this->pending.buf = {};
}

void ProofRecorder::Frame() {
	if (!this->recording) return;
	this->Flush(false);
	if (this->writer.Failed()) {
		console->Warning("SAR proof: writing %s failed; recording stopped\n", this->writer.Path().c_str());
		this->Stop("write failed");
	}
}

void ProofRecorder::Event(EventKind kind, const std::string &text) {
	ByteWriter w;
	w.U8(kind);
	w.I32(this->Clock());
	w.Str(text);
	this->Emit(REC_EVENT, w);
}

// ---- recording control ----

bool ProofRecorder::Start(const std::string &name, std::string &error) {
	if (!this->available) {
		error = "SAR proofs are not available in this game";
		return false;
	}
	if (this->recording) {
		error = "already recording " + this->writer.Path();
		return false;
	}
	if (engine->demoplayer->IsPlaying()) {
		error = "can't record while a demo is playing";
		return false;
	}
	std::string file = name;
	if (!Utils::EndsWith(file, ".sarp")) file += ".sarp";
	std::string path = std::string(engine->GetGameDirectory()) + "/" + file;
	try {
		auto dir = std::filesystem::path(path).parent_path();
		if (!dir.empty() && !std::filesystem::exists(dir)) std::filesystem::create_directories(dir);
	} catch (...) {
	}
	if (!this->writer.Open(path, error)) return false;

	this->recording = true;
	this->segmentActive = false;
	this->synced = false;
	this->segmentIndex = 0;
	this->wroteCustomDataTable = false;
	this->lastDatatables.clear();
	this->pending.Clear();
	this->lastFlush = Now();
	this->costEntities = this->costPackets = this->costStrings = CostStat();

	time_t t = time(nullptr);
	char date[64];
	strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
	std::vector<std::pair<std::string, std::string>> info = {
		{"sar_version", SAR_VERSION},
		{"sar_built", SAR_BUILT},
		{"game", sar.game->Version()},
		{"game_dir", std::filesystem::path(engine->GetGameDirectory()).filename().string()},
		{"created_utc", date},
		{"name", file},
		{"player", Variable("name").GetString()},
#ifdef _WIN32
		{"platform", "windows"},
#else
		{"platform", "linux"},
#endif
	};
	if (GetHostVersion) info.push_back({"engine_build", std::to_string(GetHostVersion())});
	ByteWriter w;
	w.Var(info.size());
	for (auto &kv : info) {
		w.Str(kv.first);
		w.Str(kv.second);
	}
	this->Emit(REC_FILE_INFO, w);

	if (this->signonValid && this->signonState >= SS_SPAWN) {
		// connected: start a segment right away from the cached signon data
		this->BeginSegment(this->signonState >= SS_FULL);
	} else if (!this->inSignon && engine->isRunning()) {
		console->Print("SAR proof: no signon data for this map (SAR was loaded after it started); recording starts at the next load\n");
	}
	this->Flush(true);
	console->Print("Recording SAR proof to %s\n", path.c_str());
	return true;
}

void ProofRecorder::Stop(const char *why) {
	if (!this->recording) return;
	if (this->segmentActive) this->EndSegment(why);
	this->Flush(true);
	unsigned char pub[32] = SAR_DEMO_SIGN_PUBKEY;
	unsigned char priv[64] = SAR_DEMO_SIGN_PRIVKEY;
	uint8_t key[96];
	memcpy(key, pub, 32);
	memcpy(key + 32, priv, 64);
	this->writer.Close(!this->writer.Failed(), key);
	this->recording = false;
	console->Print("Saved SAR proof %s (%.1f KiB, %.1f KiB uncompressed)\n", this->writer.Path().c_str(), this->writer.BytesWritten() / 1024.0, this->writer.RawBytes() / 1024.0);
	auto cost = [](const CostStat &s) { return Utils::ssprintf("%d (avg %.0f us, max %.0f us)", s.count, s.count ? s.total / s.count * 1e6 : 0.0, s.max * 1e6); };
	console->DevMsg("SAR proof capture cost: snapshots %s, packets %s, string tables %s\n", cost(this->costEntities).c_str(), cost(this->costPackets).c_str(), cost(this->costStrings).c_str());
}

// ---- segments ----

void ProofRecorder::BeginSegment(bool midStart, bool requestFullUpdate) {
	this->segmentActive = true;
	this->synced = false;
	this->initialSarDataPending = false;
	++this->segmentIndex;
	this->multiplayer = engine->GetMaxClients() > 1;

	auto classes = server->GetAllServerClasses ? server->GetAllServerClasses() : nullptr;
	int numClasses = CountServerClasses(classes);
	this->classBits = Log2((unsigned)numClasses) + 1;

	bool localServer = server->gpGlobals && server->gpGlobals->maxEntities > 0;
	ByteWriter w;
	w.Var(this->segmentIndex);
	w.U8((midStart ? SEG_MIDSTART : 0) | (this->multiplayer ? SEG_MULTIPLAYER : 0));
	w.Str(engine->GetCurrentMapName());
	w.Str(std::filesystem::path(engine->GetGameDirectory()).filename().string());
	w.Str(localServer ? "localhost:27015" : "remote");
	w.Str(Variable("name").GetString());
	w.I32(GetHostVersion ? GetHostVersion() : 2001);
	w.F32(engine->GetIPT());
	w.Var(engine->GetMaxClients());
	w.Var(numClasses);
	this->Emit(REC_SEGMENT_BEGIN, w);

	this->WriteDatatables();
	this->WriteCustomDataTable();
	for (auto &p : this->signonPackets) {
		ByteWriter pw;
		pw.buf = p;
		this->Emit(REC_PACKET, pw);
	}

	this->ResetEntities();
	this->needKeyframe = true;
	this->sawNetworkMode = false;
	this->lastEntTick = -1;
	this->synthBaselines.clear();
	this->baselineIndexMap.clear();
	void *baselines = VCall<void *>(g_clientStringTables, STC_FINDTABLE, "instancebaseline");
	this->baselineLiveStart = this->baselineCount = baselines ? VCall<int>(baselines, ST_GETNUMSTRINGS) : 0;

	if (midStart) {
		this->Sync();
		this->WriteFullStringTableSync();
		if (this->BackdoorActive()) {
			this->Event(EV_KEYFRAME, "recording started mid-map; entity keyframe from the local backdoor");
		} else if (requestFullUpdate && ForceFullUpdate && engine->cl) {
			// no backdoor to copy from (multiplayer): ask the server for a full
			// update, like a demo start does
			ForceFullUpdate(engine->cl->ThisPtr(), "SAR proof");
			this->Event(EV_FULLUPDATE, "recording started mid-map; requested a full update");
		}
		if (this->WantsInitialSarData()) {
			RecordSarInitialData();
			this->InitialSarDataWritten();
		}
	}
}

void ProofRecorder::EndSegment(const char *why) {
	ByteWriter w;
	w.I32(this->lastClock);
	w.Str(why ? why : "");
	this->Emit(REC_SEGMENT_END, w);
	this->segmentActive = false;
	this->synced = false;
	this->Flush(true);
}

void ProofRecorder::Sync() {
	this->synced = true;
	ByteWriter w;
	w.I32(this->Clock());
	this->Emit(REC_SYNC, w);
	this->WriteDemoCvars();
	this->initialSarDataPending = true;
}

void ProofRecorder::WriteDatatables() {
	auto classes = server->GetAllServerClasses ? server->GetAllServerClasses() : nullptr;
	if (!classes) return;
	std::vector<uint32_t> buf(0x200000 / 4);
	EngineBfWrite bf;
	bf.Start(buf.data(), (int)buf.size() * 4);
	DataTable_WriteSendTablesBuffer(classes, &bf);
	DataTable_WriteClassInfosBuffer(classes, &bf);
	if (bf.overflow) {
		this->Event(EV_LOST_DATA, "datatables overflowed");
		return;
	}
	auto bytes = (const uint8_t *)buf.data();
	std::vector<uint8_t> blob(bytes, bytes + bf.BytesWritten());
	if (blob == this->lastDatatables) return;
	ByteWriter w;
	w.buf = blob;
	this->Emit(REC_DATATABLES, w);
	this->lastDatatables = std::move(blob);
}

void ProofRecorder::WriteCustomDataTable() {
	if (this->wroteCustomDataTable) return;
	this->wroteCustomDataTable = true;
	std::vector<std::string> names;
	if (g_customDataCallbacks) {
		auto mem = *(uint8_t **)g_customDataCallbacks;
		int count = *(int *)(g_customDataCallbacks + 0xC);
		for (int i = 0; mem && i >= 0 && i < count && i < 64; ++i) {
			auto name = *(const char **)(mem + i * 8 + 4);
			names.push_back(name ? name : "");
		}
	}
	ByteWriter w;
	w.Var(names.size());
	for (auto &n : names) w.Str(n);
	this->Emit(REC_CUSTOMDATA_TABLE, w);
}

void ProofRecorder::WriteDemoCvars() {
	int clock = this->Clock();
	for (ConCommandBase *cmd = tier1->m_pConCommandList; cmd; cmd = cmd->m_pNext) {
		if (!cmd->m_bRegistered || cmd->IsCommand()) continue;
		if (!(cmd->m_nFlags & FCVAR_DEMO)) continue;
		auto var = (ConVar *)cmd;
		ByteWriter w;
		w.I32(clock);
		w.U8(CMD_DEMOCVAR);
		w.Str(Utils::ssprintf("%s \"%s\"", var->m_pszName, var->m_pszString));
		this->Emit(REC_CONSOLECMD, w);
	}
}

// Every entry of every client string table. Brings the string tables of a
// demo that starts from this map's signon data up to the client's current
// state (string tables are append-only, so the signon's strings are a prefix).
void ProofRecorder::WriteFullStringTableSync() {
	int n = VCall<int>(g_clientStringTables, STC_GETNUMTABLES);
	for (int t = 0; t < n; ++t) {
		void *table = VCall<void *>(g_clientStringTables, STC_GETTABLE, t);
		if (!table) continue;
		int count = VCall<int>(table, ST_GETNUMSTRINGS);
		ByteWriter w;
		w.I32(-1);
		w.Str(VCall<const char *>(table, ST_GETTABLENAME));
		w.Var(count);
		for (int i = 0; i < count; ++i) {
			int len = 0;
			auto data = VCall<const void *>(table, ST_GETSTRINGUSERDATA, i, &len);
			auto str = VCall<const char *>(table, ST_GETSTRING, i);
			bool hasData = data && len > 0;
			w.Var(i);
			w.U8(STE_STRING | (hasData ? STE_DATA : 0));
			w.Str(str ? str : "");
			if (hasData) w.Blob(data, len);
		}
		this->Emit(REC_STRINGTABLE, w);
	}
}

// ---- engine callbacks ----

void ProofRecorder::OnSignonState(int state) {
	// demo playback goes through signon too; the net channel records nothing then
	if (engine->demoplayer->IsPlaying()) return;
	this->signonState = state;
	if (state == SS_NEW) {
		if (this->segmentActive) this->EndSegment("level change");
		this->signonPackets.clear();
		this->inSignon = true;
		this->closeSignonAfterPacket = false;
		this->signonValid = false;
	} else if (state == SS_SPAWN) {
		if (this->inSignon) this->closeSignonAfterPacket = true;
	} else if (state == SS_FULL) {
		if (this->recording && this->segmentActive && !this->synced) this->Sync();
		ProofDebugLoadTimingStart();
	}
}

void ProofRecorder::OnDisconnect() {
	if (this->segmentActive) this->EndSegment("disconnect");
	this->inSignon = false;
	this->signonValid = false;
	this->signonState = 0;
}

void ProofRecorder::OnRecordMessages(bf_read &data, int bits) {
	if (bits <= 0 || !data.m_pData) return;
	// CBitRead::GetNumBitsRead - the net channel hands over a copy of the
	// reader positioned where its messages start
	int start = (int)(data.m_nDataBytes & 3) * 8 + (int)(((uintptr_t)data.m_pDataIn - (uintptr_t)data.m_pData) >> 2) * 32 - data.m_nBitsAvail;
	if (start > data.m_nDataBits) start = data.m_nDataBits;
	if (start < 0 || start + bits > data.m_nDataBits) return;
	this->msgBits.BitsFrom((const uint8_t *)data.m_pData, (size_t)start, (size_t)bits);
}

std::vector<uint8_t> ProofRecorder::BuildPacketPayload(uint8_t flags) {
	ByteWriter w;
	w.U8(flags);
	int clock = this->Clock();
	this->lastClock = clock;
	w.I32(clock);

	uint8_t info[DEMO_CMDINFO_SIZE] = {};
	void *rec = engine->demorecorder->s_ClientDemoRecorder->ThisPtr();
	auto flag = (bool *)((uintptr_t)rec + g_resetInterpolationOffset);
	bool engineFlag = *flag;
	*flag = this->resetInterp;
	GetClientCmdInfo(rec, info);
	*flag = engineFlag;
	w.Bytes(info, sizeof info);

	int outSeq = 0, inSeq = 0, outAck = 0;
	void *chan = engine->cl ? *(void **)((uintptr_t)engine->cl->ThisPtr() + CLIENTSTATE_NETCHANNEL) : nullptr;
	if (chan) VCall<void>(chan, NETCHAN_GETSEQUENCEDATA, &outSeq, &inSeq, &outAck);
	w.I32(inSeq);
	w.I32(outAck);

	w.Var(this->msgBits.bits);
	w.Bytes(this->msgBits.buf.data(), this->msgBits.Bytes());
	return std::move(w.buf);
}

void ProofRecorder::OnRecordPacket() {
	bool want = this->inSignon || (this->recording && this->segmentActive);
	if (want) {
		CostTimer timer(this->costPackets);
		uint8_t flags = 0;
		if (this->inSignon) flags |= PKT_SIGNON;
		bool backdoor = this->BackdoorActive();
		if (backdoor) flags |= PKT_BACKDOOR;
		else if (!this->inSignon) this->sawNetworkMode = true;
		if (!backdoor && this->lastPacketBackdoor && !this->inSignon && this->segmentActive && !this->synthBaselines.empty()) {
			// networking switched on (e.g. a demo started recording). Its string
			// table updates would collide with the baselines synthesized for
			// this segment, so continue in a new segment; the engine has
			// already asked for the full update a mid-map start needs.
			this->EndSegment("networking switched on");
			this->BeginSegment(true, false);
		}
		this->lastPacketBackdoor = backdoor;
		auto payload = this->BuildPacketPayload(flags);
		if (this->inSignon) {
			this->signonPackets.push_back(std::move(payload));
			if (this->closeSignonAfterPacket) {
				this->inSignon = false;
				this->closeSignonAfterPacket = false;
				this->signonValid = true;
				if (this->recording) this->BeginSegment(false);
			}
		} else {
			ByteWriter w;
			w.buf = std::move(payload);
			this->Emit(REC_PACKET, w);
		}
	}
	this->msgBits = BitWriter();
	this->resetInterp = false;
}

void ProofRecorder::OnRecordCommand(const char *cmd) {
	if (!this->recording || !this->segmentActive || !this->synced || !cmd) return;
	ByteWriter w;
	w.I32(this->Clock());
	w.U8(0);
	w.Str(cmd);
	this->Emit(REC_CONSOLECMD, w);
}

void ProofRecorder::OnUserCmd(int sequenceNumber) {
	if (!this->recording || !this->segmentActive || !this->synced || !client->Input()) return;
	uint32_t buf[64] = {};
	EngineBfWrite bf;
	bf.Start(buf, sizeof buf);
	int slot = GET_ACTIVE_SPLITSCREEN_SLOT();
	VCall<void>(client->Input()->ThisPtr(), INPUT_ENCODEUSERCMD, slot, &bf, sequenceNumber);
	if (bf.overflow) return;
	ByteWriter w;
	w.I32(this->Clock());
	w.I32(sequenceNumber);
	w.U8((uint8_t)slot);
	w.Blob(buf, bf.BytesWritten());
	this->Emit(REC_USERCMD, w);
}

void ProofRecorder::OnCustomData(int callback, const void *data, size_t len) {
	if (!this->WantsSarData()) return;
	ByteWriter w;
	w.I32(this->Clock());
	w.I32(callback);
	w.Blob(data, len);
	this->Emit(REC_CUSTOMDATA, w);
}

// ---- string tables (backdoor mode) ----

// The backdoor copies server string table changes straight into the client's
// tables (no network message). Record what is about to be copied: the
// difference between each changed server table and its client mirror.
void ProofRecorder::OnStringTableDirectUpdate(void *container, int tick) {
	if (!this->recording || !this->segmentActive) return;
	CostTimer timer(this->costStrings);
	int n = VCall<int>(container, STC_GETNUMTABLES);
	for (int t = 0; t < n; ++t) {
		void *st = VCall<void *>(container, STC_GETTABLE, t);
		if (!st || !VCall<bool>(st, ST_CHANGEDSINCETICK, tick)) continue;
		const char *name = VCall<const char *>(st, ST_GETTABLENAME);
		void *ct = VCall<void *>(g_clientStringTables, STC_FINDTABLE, name);
		if (!ct) continue;
		bool baselines = !strcmp(name, "instancebaseline");
		int sn = VCall<int>(st, ST_GETNUMSTRINGS);
		int cn = VCall<int>(ct, ST_GETNUMSTRINGS);
		ByteWriter entries;
		int count = 0;
		for (int i = 0; i < sn; ++i) {
			int slen = 0, clen = 0;
			auto sdata = VCall<const uint8_t *>(st, ST_GETSTRINGUSERDATA, i, &slen);
			if (!sdata) slen = 0;
			bool isNew = i >= cn;
			if (!isNew) {
				auto cdata = VCall<const uint8_t *>(ct, ST_GETSTRINGUSERDATA, i, &clen);
				if (!cdata) clen = 0;
				if (slen == clen && (slen == 0 || !memcmp(sdata, cdata, slen))) continue;
			}
			int index = i;
			bool addToDemo = isNew;
			if (baselines && i >= this->baselineLiveStart) {
				// the demo's table may already have synthesized entries here
				auto it = this->baselineIndexMap.find(i);
				if (it == this->baselineIndexMap.end()) {
					index = this->baselineIndexMap[i] = this->baselineCount++;
					addToDemo = true;
				} else {
					index = it->second;
					addToDemo = false;
				}
			}
			entries.Var(index);
			entries.U8((addToDemo ? STE_STRING : 0) | (slen > 0 ? STE_DATA : 0));
			if (addToDemo) {
				auto str = VCall<const char *>(st, ST_GETSTRING, i);
				entries.Str(str ? str : "");
			}
			if (slen > 0) entries.Blob(sdata, slen);
			++count;
		}
		if (!count) continue;
		ByteWriter w;
		w.I32(-1);  // attaches to the next REC_ENTITIES
		w.Str(name);
		w.Var(count);
		w.Bytes(entries.buf.data(), entries.Size());
		this->Emit(REC_STRINGTABLE, w);
	}
}

// ---- entities (backdoor mode) ----

void ProofRecorder::ResetEntities() {
	for (auto &e : this->ents) e = EntState();
}

bool ProofRecorder::EncodeEntity(int index, ServerClass *cls, void *edict, EntState &out) {
	if (!cls || !cls->m_pTable || !cls->m_pTable->m_pPrecalc || !edict) return false;
	void *ent = ((CBaseEdict *)edict)->m_pUnk;
	if (!ent) return false;
	int nProxies = *(int *)((uintptr_t)cls->m_pTable->m_pPrecalc + PRECALC_NUM_DT_PROXIES);
	if (nProxies < 0 || nProxies > 64) return false;
	out.recips.assign((size_t)(nProxies > 0 ? nProxies : 1) * SENDPROXY_RECIPIENTS_SIZE, 0);
	out.nRecips = nProxies;
	EngineUtlMemory recips = {out.recips.data(), nProxies, -1};
	EngineBfWrite bf;
	bf.Start(this->encBuf.data() + ((4 - ((uintptr_t)this->encBuf.data() & 3)) & 3), 16 * 1024);
	if (!SendTable_Encode(cls->m_pTable, ent, &bf, index, &recips, false) || bf.overflow) return false;
	out.bits = bf.curBit;
	out.state.assign(bf.data, bf.data + bf.BytesWritten());
	out.cls = cls;
	return true;
}

// The class baseline an entering entity is encoded against: the client's
// instance baseline when it has one, otherwise one synthesized for the demo
// from this entity's state (what SV_EnsureInstanceBaseline does when the
// server networks an entity). The game's own tables are never changed:
// adding a baseline the backdoor would never mirror leaves server and client
// disagreeing, which breaks the game if networking switches on later.
const std::vector<uint8_t> *ProofRecorder::Baseline(ServerClass *cls, const EntState &st) {
	char id[16];
	snprintf(id, sizeof id, "%d", cls->m_ClassID);
	void *ct = VCall<void *>(g_clientStringTables, STC_FINDTABLE, "instancebaseline");
	if (ct) {
		int idx = VCall<int>(ct, ST_FINDSTRINGINDEX, (const char *)id);
		if (idx >= 0 && idx != INVALID_STRING_INDEX) {
			int len = 0;
			auto data = VCall<const uint8_t *>(ct, ST_GETSTRINGUSERDATA, idx, &len);
			if (data && len > 0) {
				this->baselineScratch.assign(data, data + len);
				return &this->baselineScratch;
			}
		}
	}
	auto it = this->synthBaselines.find(cls->m_ClassID);
	if (it != this->synthBaselines.end()) return &it->second;

	ByteWriter w;
	w.I32(-1);
	w.Str("instancebaseline");
	w.Var(1);
	w.Var(this->baselineCount++);
	w.U8(STE_STRING | STE_DATA);
	w.Str(id);
	w.Blob(st.state.data(), st.state.size());
	this->Emit(REC_STRINGTABLE, w);
	return &(this->synthBaselines[cls->m_ClassID] = st.state);
}

// Called inside SV_ComputeClientPacks right after the backdoor has copied this
// snapshot's entities to the client. Read back what it did and write the
// equivalent svc_PacketEntities data.
void ProofRecorder::OnEndEntityStateUpdate(void *backdoor) {
	auto snap = (uint8_t *)this->curSnapshot;
	this->curSnapshot = nullptr;
	if (!this->recording || !this->segmentActive || !snap || !backdoor) return;
	CostTimer timer(this->costEntities);

	int tick = *(int *)(snap + SNAP_TICK);
	auto snapEnts = *(uint8_t **)(snap + SNAP_ENTITIES);
	int numEnts = *(int *)(snap + SNAP_NUM_ENTITIES);
	auto bd = (uint8_t *)backdoor;
	auto aliveBits = (const uint32_t *)(bd + BD_ENTS_ALIVE);
	auto created = (const uint16_t *)(bd + BD_CREATED);
	auto changed = (const uint16_t *)(bd + BD_CHANGED);
	int nCreated = *(int *)(bd + BD_NCREATED);
	int nChanged = *(int *)(bd + BD_NCHANGED);
	if (numEnts < 0 || numEnts > MAX_EDICTS || nCreated < 0 || nCreated > MAX_EDICTS || nChanged < 0 || nChanged > MAX_EDICTS) return;
	auto edicts = (uint8_t *)server->gpGlobals->pEdicts;
	if (!edicts) return;

	std::bitset<MAX_EDICTS> isCreated, isChanged;
	for (int i = 0; i < nCreated; ++i) isCreated.set(created[i] & (MAX_EDICTS - 1));
	for (int i = 0; i < nChanged; ++i) isChanged.set(changed[i] & (MAX_EDICTS - 1));

	bool full = this->needKeyframe || this->sawNetworkMode;
	if (full) this->ResetEntities();

	EngineBfWrite bf;
	bf.Start(this->entBuf.data(), (int)this->entBuf.size() * 4);
	int updated = 0, last = -1;
	std::vector<int> explicitDeletes;
	bool failed = false;

	for (int i = 0; i < MAX_EDICTS && !failed; ++i) {
		EntState &e = this->ents[i];
		bool aliveNow = (aliveBits[i >> 5] >> (i & 31)) & 1;
		if (!aliveNow && !e.alive) continue;
		const uint8_t *cache = bd + BD_CACHE + i * BD_CACHE_STRIDE;
		bool dormantNow = cache[0] != 0 && !isCreated[i];
		int serialNow = *(const int *)(cache + 4);
		bool inPrev = e.alive && !e.dormant;
		bool inNow = aliveNow && !dormantNow;
		ServerClass *cls = i < numEnts ? *(ServerClass **)(snapEnts + i * SNAP_ENTRY_STRIDE) : nullptr;
		void *edict = edicts + i * sizeof(edict_t);

		if (inNow && (isCreated[i] || !inPrev || serialNow != e.serial)) {
			// enter PVS: delta from the class baseline, every prop (SV_WriteEnterPVS)
			EntState ns;
			const std::vector<uint8_t> *base = nullptr;
			if (!this->EncodeEntity(i, cls, edict, ns) || !(base = this->Baseline(cls, ns))) {
				failed = true;
				break;
			}
			int n = SendTable_CalcDelta(cls->m_pTable, base->data(), (int)base->size() * 8, ns.state.data(), ns.bits, this->propsA.data(), MAX_DELTA_PROPS, i);
			bf.UBitVar(i - last - 1);
			bf.Bit(0);
			bf.Bit(1);
			bf.UBits(cls->m_ClassID, this->classBits);
			bf.UBits(serialNow, NUM_NETWORKED_EHANDLE_SERIAL_BITS);
			SendTable_WritePropList(cls->m_pTable, ns.state.data(), ns.bits, &bf, i, this->propsA.data(), n);
			ns.alive = true;
			ns.dormant = false;
			ns.serial = serialNow;
			e = std::move(ns);
			++updated;
			last = i;
		} else if (inPrev && inNow) {
			// same entity, still transmitted
			if (!isChanged[i]) continue;
			EntState ns;
			if (!this->EncodeEntity(i, cls ? cls : e.cls, edict, ns)) {
				failed = true;
				break;
			}
			int n = SendTable_CalcDelta(ns.cls->m_pTable, e.state.data(), e.bits, ns.state.data(), ns.bits, this->propsA.data(), MAX_DELTA_PROPS, i);
			int culled = n > 0 ? CullPropsFromProxies(ns.cls->m_pTable, this->propsA.data(), n, 0, e.recips.data(), e.nRecips, ns.recips.data(), ns.nRecips, this->propsB.data(), MAX_DELTA_PROPS) : 0;
			if (culled > 0) {
				bf.UBitVar(i - last - 1);
				bf.Bit(0);
				bf.Bit(0);
				SendTable_WritePropList(ns.cls->m_pTable, ns.state.data(), ns.bits, &bf, i, this->propsB.data(), culled);
				++updated;
				last = i;
			}
			ns.alive = true;
			ns.dormant = false;
			ns.serial = e.serial;
			e = std::move(ns);
		} else if (inPrev) {
			// was transmitted, isn't now: dormant (leave PVS) or gone (delete)
			bool dormant = aliveNow && serialNow == e.serial;
			bf.UBitVar(i - last - 1);
			bf.Bit(1);
			bf.Bit(dormant ? 0 : 1);
			++updated;
			last = i;
			if (dormant) {
				e.dormant = true;
			} else {
				e = EntState();
			}
		} else if (e.alive && !(aliveNow && serialNow == e.serial)) {
			// dormant on the client, now gone
			explicitDeletes.push_back(i);
			e = EntState();
		}
	}

	if (!full) {
		for (int idx : explicitDeletes) {
			bf.Bit(1);
			bf.UBits(idx, MAX_EDICT_BITS);
		}
		bf.Bit(0);
	}

	if (failed || bf.overflow || updated >= (1 << 11)) {
		// the client and the proof no longer agree on this snapshot; start
		// over from a keyframe on the next one
		this->needKeyframe = true;
		this->Event(EV_LOST_DATA, Utils::ssprintf("entity snapshot %d could not be encoded", tick));
		return;
	}

	ByteWriter w;
	w.I32(tick);
	w.U8(full ? 0 : ENT_DELTA);
	w.Var(numEnts);
	w.Var(updated);
	w.Var(bf.curBit);
	w.Bytes(bf.data, bf.BytesWritten());
	this->Emit(REC_ENTITIES, w);
	this->needKeyframe = false;
	this->sawNetworkMode = false;
	this->lastEntTick = tick;
}

// ---- debug: client entity dump ----

// Writes the networked origin/angles/model of every client entity after each
// entity update, keyed by server tick. Dump a live recording and then the
// playback of its converted demo, and diff the two (tools/sarproof/entdiff.py).
Variable sar_proof_debug_dump("sar_proof_debug_dump", "", "Debug: file to write client entity states to after every entity update (empty = off).\n", FCVAR_DONTRECORD);

struct DumpOffsets {
	int origin = -1, angles = -1, model = -1;
};

static void FindRecvOffsets(RecvTable *table, int base, DumpOffsets &out, int depth = 0) {
	if (!table || depth > 16) return;
	for (int i = 0; i < table->m_nProps; ++i) {
		RecvProp *p = &table->m_pProps[i];
		if (!p->m_pVarName) continue;
		if (p->m_RecvType == DPT_DataTable && p->m_pDataTable) {
			FindRecvOffsets(p->m_pDataTable, base + p->m_Offset, out, depth + 1);
		} else if (out.origin < 0 && !strcmp(p->m_pVarName, "m_vecOrigin")) {
			out.origin = base + p->m_Offset;
		} else if (out.angles < 0 && !strcmp(p->m_pVarName, "m_angRotation")) {
			out.angles = base + p->m_Offset;
		} else if (out.model < 0 && !strcmp(p->m_pVarName, "m_nModelIndex")) {
			out.model = base + p->m_Offset;
		}
	}
}

void ProofRecorder::OnClientFrameStage(int stage) {
	static FILE *fp = nullptr;
	static std::string path;
	static std::map<ClientClass *, DumpOffsets> offsets;
	const int FRAME_NET_UPDATE_END = 4;
	if (stage != FRAME_NET_UPDATE_END) return;
	const char *want = sar_proof_debug_dump.GetString();
	if (!want[0] || path != want) {
		if (fp) fclose(fp);
		fp = nullptr;
		path = want;
		if (!want[0]) return;
		fp = fopen((std::string(engine->GetGameDirectory()) + "/" + path).c_str(), "w");
		if (!fp) return;
	}
	if (!fp || !engine->cl) return;
	int serverTick = *(int *)((uintptr_t)engine->cl->ThisPtr() + 0xC4);  // CBaseClientState::GetServerTickCount
	fprintf(fp, "T %d %s\n", serverTick, engine->demoplayer->IsPlaying() ? "play" : "live");
	for (int i = 0; i < MAX_EDICTS; ++i) {
		ClientEnt *ent = client->GetEntity(i);
		if (!ent) continue;
		ClientClass *cc = ent->GetClientClass();
		if (!cc || !cc->m_pRecvTable) continue;
#ifdef _WIN32
		// IClientNetworkable is the third base of C_BaseEntity; skip dormant ents
		if (VCall<bool>((uint8_t *)ent + 8, 9)) continue;
#endif
		auto it = offsets.find(cc);
		if (it == offsets.end()) {
			DumpOffsets o;
			FindRecvOffsets(cc->m_pRecvTable, 0, o);
			it = offsets.emplace(cc, o).first;
		}
		auto &o = it->second;
		auto base = (const uint8_t *)ent;
		fprintf(fp, "%d %s", i, cc->m_pNetworkName ? cc->m_pNetworkName : "?");
		if (o.origin >= 0) {
			auto v = (const float *)(base + o.origin);
			fprintf(fp, " o %.3f %.3f %.3f", v[0], v[1], v[2]);
		}
		if (o.angles >= 0) {
			auto v = (const float *)(base + o.angles);
			fprintf(fp, " a %.3f %.3f %.3f", v[0], v[1], v[2]);
		}
		if (o.model >= 0) fprintf(fp, " m %d", *(const int *)(base + o.model));
		fputc('\n', fp);
	}
}

// ---- debug: load timing ----

// Measures the frames right after a load reaches SIGNONSTATE_FULL, to compare
// the hitch of starting a demo with starting a proof segment.
Variable sar_proof_debug_loadtiming("sar_proof_debug_loadtiming", "0", "Debug: print frame times for 3 seconds after each load.\n", FCVAR_DONTRECORD);

static struct {
	bool active = false;
	double start = 0, last = 0, maxDt = 0, over50 = 0;
	int frames = 0, slowFrames = 0, startServerTick = 0;
} g_loadTiming;

void ProofDebugLoadTimingStart() {
	if (!sar_proof_debug_loadtiming.GetBool() || g_loadTiming.active) return;
	int host, server, client;
	engine->GetTicks(host, server, client);
	g_loadTiming = {};
	g_loadTiming.active = true;
	g_loadTiming.start = g_loadTiming.last = Now();
	g_loadTiming.startServerTick = server;
}

CON_COMMAND_F(sar_proof_debug_timing_start, "sar_proof_debug_timing_start - debug: measure frame times for the next 3 seconds.\n", FCVAR_DONTRECORD) {
	g_loadTiming.active = false;
	sar_proof_debug_loadtiming.SetValue(1);
	ProofDebugLoadTimingStart();
}

static void ProofDebugLoadTimingFrame() {
	if (!g_loadTiming.active) return;
	double now = Now();
	double dt = now - g_loadTiming.last;
	g_loadTiming.last = now;
	++g_loadTiming.frames;
	if (dt > g_loadTiming.maxDt) g_loadTiming.maxDt = dt;
	if (dt > 0.05) {
		++g_loadTiming.slowFrames;
		g_loadTiming.over50 += dt - 0.05;
	}
	if (now - g_loadTiming.start >= 3.0) {
		int host, server, client;
		engine->GetTicks(host, server, client);
		console->Print("[loadtiming] 3s after load: %d frames, %d server ticks, longest frame %.1f ms, %d frames over 50 ms (%.1f ms over)\n",
			g_loadTiming.frames, server - g_loadTiming.startServerTick, g_loadTiming.maxDt * 1000, g_loadTiming.slowFrames, g_loadTiming.over50 * 1000);
		g_loadTiming.active = false;
	}
}

// ---- events ----

ON_EVENT(FRAME) {
	if (proofRecorder) proofRecorder->Frame();
	ProofDebugLoadTimingFrame();
}

ON_EVENT(SAR_UNLOAD) {
	// stops a recording and removes the engine function hooks
	if (proofRecorder) proofRecorder->Shutdown();
}

// ---- commands ----

CON_COMMAND(sar_proof_record, "sar_proof_record <name> - record a SAR proof (.sarp) instead of a demo. It keeps recording across loads until sar_proof_stop.\n") {
	if (!proofRecorder) return;
	if (args.ArgC() != 2) return console->Print(sar_proof_record.ThisPtr()->m_pszHelpString);
	std::string error;
	if (!proofRecorder->Start(args[1], error)) console->Print("sar_proof_record: %s\n", error.c_str());
}

CON_COMMAND(sar_proof_stop, "sar_proof_stop - stop recording the SAR proof.\n") {
	if (!proofRecorder || !proofRecorder->IsRecording()) return console->Print("Not recording a SAR proof\n");
	proofRecorder->Stop("stopped");
}

static std::string ResolveProofPath(const char *arg) {
	std::string p = arg;
	if (!Utils::EndsWith(p, ".sarp")) p += ".sarp";
	if (std::filesystem::exists(p)) return p;
	return std::string(engine->GetGameDirectory()) + "/" + p;
}

CON_COMMAND(sar_proof_info, "sar_proof_info <file> - verify a SAR proof and print what it contains.\n") {
	if (args.ArgC() != 2) return console->Print(sar_proof_info.ThisPtr()->m_pszHelpString);
	unsigned char pub[32] = SAR_DEMO_SIGN_PUBKEY;
	std::string text;
	SarProof::Describe(ResolveProofPath(args[1]), pub, text);
	console->Print("%s", text.c_str());
}

CON_COMMAND(sar_proof_export, "sar_proof_export <file> [output base name] - convert a SAR proof to demo files (<base>.dem, <base>_2.dem, ...).\n") {
	if (args.ArgC() < 2 || args.ArgC() > 3) return console->Print(sar_proof_export.ThisPtr()->m_pszHelpString);
	std::string in = ResolveProofPath(args[1]);
	std::string base = args.ArgC() == 3 ? std::string(engine->GetGameDirectory()) + "/" + args[2] : in.substr(0, in.size() - 5);
	SarProof::ConvertResult res;
	if (!SarProof::ConvertToDemos(in, base, res)) {
		return console->Print("sar_proof_export: %s\n", res.error.c_str());
	}
	for (auto &f : res.files) console->Print("Wrote %s\n", f.c_str());
	for (auto &w : res.warnings) console->Print("warning: %s\n", w.c_str());
}

CON_COMMAND(sar_proof_play, "sar_proof_play <file> [segment] - play a SAR proof (converted to temporary demos in <game>/sarproof_play).\n") {
	if (args.ArgC() < 2 || args.ArgC() > 3) return console->Print(sar_proof_play.ThisPtr()->m_pszHelpString);
	if (proofRecorder && proofRecorder->IsRecording()) return console->Print("Stop recording first (sar_proof_stop)\n");
	std::string in = ResolveProofPath(args[1]);
	std::string dir = std::string(engine->GetGameDirectory()) + "/sarproof_play";
	try {
		std::filesystem::create_directories(dir);
		for (auto &f : std::filesystem::directory_iterator(dir)) {
			if (f.path().extension() == ".dem") std::filesystem::remove(f.path());
		}
	} catch (...) {
	}
	SarProof::ConvertResult res;
	if (!SarProof::ConvertToDemos(in, dir + "/proof", res)) {
		return console->Print("sar_proof_play: %s\n", res.error.c_str());
	}
	for (auto &w : res.warnings) console->Print("warning: %s\n", w.c_str());
	if (res.files.empty()) return console->Print("sar_proof_play: the proof has no segments\n");
	int first = args.ArgC() == 3 ? std::atoi(args[2]) : 1;
	if (first < 1 || first > (int)res.files.size()) return console->Print("sar_proof_play: segment must be 1-%d\n", (int)res.files.size());
	// sar_startdemos plays proof_N.dem and every later segment. Leave the
	// current game through the command buffer first: starting playback in the
	// middle of a frame leaves the server running for the rest of it.
	auto name = std::filesystem::path(res.files[first - 1]).stem().string();
	engine->ExecuteCommand(("disconnect; sar_startdemos sarproof_play/" + name).c_str());
}
