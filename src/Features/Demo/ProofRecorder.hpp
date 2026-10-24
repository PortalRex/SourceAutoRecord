#pragma once

#include "Features/Feature.hpp"
#include "ProofFile.hpp"
#include "Utils/SDK.hpp"

#include <string>
#include <map>
#include <vector>

struct ServerClass;
struct SendTable;

// Records SAR proof files (.sarp) - see ProofFormat.hpp.
//
// The engine's demo recorder costs time at every demo start: recording turns
// off the single-player local network backdoor (forcing a full update), asks
// for another full update, and sends "demorestart" (which resets the HUD and
// restarts ambient sounds on the server). This recorder never makes the engine
// record. It copies what the client receives as it goes past:
//  - every network packet the client processes (from the demo recorder
//    callbacks the net channel always makes),
//  - in single player, the entity state the backdoor copies to the client,
//    encoded with the engine's own SendTable encoder (so playback sees exactly
//    what a demo would have contained),
//  - string table changes the backdoor copies, user commands, console
//    commands and SAR's own demo data.
// The proof is played back by converting it to ordinary demo files.
class ProofRecorder : public Feature {
public:
	ProofRecorder();

	bool Init();
	void Shutdown();

	bool IsRecording() const { return this->recording; }
	// SAR's own demo data is wanted now (proof recording, segment synced)
	bool WantsSarData() const { return this->recording && this->segmentActive && this->synced; }
	// The SAR initial-data block (timestamp, file sums, cvars...) should run
	// for this segment
	bool WantsInitialSarData() const { return this->WantsSarData() && this->initialSarDataPending; }
	void InitialSarDataWritten() { this->initialSarDataPending = false; }

	bool Start(const std::string &name, std::string &error);
	void Stop(const char *why);
	std::string CurrentPath() const { return this->writer.Path(); }

	// engine hooks
	void OnSignonState(int state);
	void OnRecordMessages(bf_read &data, int bits);
	void OnRecordPacket();
	void OnRecordCommand(const char *cmd);
	void OnResetInterpolation() { this->resetInterp = true; }
	void OnUserCmd(int sequenceNumber);
	void OnCustomData(int callback, const void *data, size_t len);
	void OnDisconnect();
	void OnPackEntitiesBackdoor(void *snapshot) { this->curSnapshot = snapshot; }
	void OnEndEntityStateUpdate(void *backdoor);
	void OnStringTableDirectUpdate(void *serverContainer, int tick);
	void OnClientFrameStage(int stage);
	void Frame();

	bool available = false;

private:
	struct CostStat {
		double total = 0, max = 0;
		int count = 0;
	};
	struct CostTimer;

	struct EntState {
		bool alive = false;
		bool dormant = false;
		int serial = -1;
		ServerClass *cls = nullptr;
		std::vector<uint8_t> state;  // SendTable_Encode output
		int bits = 0;
		std::vector<uint8_t> recips;  // CSendProxyRecipients array
		int nRecips = 0;
	};

	int Clock();
	bool BackdoorActive();
	void Emit(SarProof::RecordType type, const SarProof::ByteWriter &payload);
	void EmitRaw(const std::vector<uint8_t> &record);
	void Flush(bool force);
	void BeginSegment(bool midStart, bool requestFullUpdate = true);
	void EndSegment(const char *why);
	void Sync();
	void WriteDatatables();
	void WriteCustomDataTable();
	void WriteDemoCvars();
	void WriteFullStringTableSync();
	void ResetEntities();
	bool EncodeEntity(int index, ServerClass *cls, void *edict, EntState &out);
	const std::vector<uint8_t> *Baseline(ServerClass *cls, const EntState &st);
	void Event(SarProof::EventKind kind, const std::string &text);
	std::vector<uint8_t> BuildPacketPayload(uint8_t flags);

	SarProof::Writer writer;
	bool recording = false;
	SarProof::ByteWriter pending;
	double lastFlush = 0;

	// signon (captured for every map, recording or not)
	int signonState = 0;
	bool inSignon = false;
	bool closeSignonAfterPacket = false;
	bool signonValid = false;
	std::vector<std::vector<uint8_t>> signonPackets;  // REC_PACKET payloads

	SarProof::BitWriter msgBits;  // messages of the packet being processed
	bool resetInterp = false;

	// segment
	bool segmentActive = false;
	bool synced = false;
	bool multiplayer = false;
	bool initialSarDataPending = false;
	int segmentIndex = 0;
	int lastClock = 0;
	bool needKeyframe = true;
	bool sawNetworkMode = false;
	bool wroteCustomDataTable = false;
	std::vector<uint8_t> lastDatatables;

	// entities (backdoor mode)
	void *curSnapshot = nullptr;
	std::vector<EntState> ents;
	int classBits = 0;
	int lastEntTick = -1;
	// game-thread time spent capturing, reported at stop
	CostStat costEntities, costPackets, costStrings;
	std::vector<uint32_t> entBuf;
	std::vector<uint8_t> encBuf;
	std::vector<int> propsA, propsB;
	// Instance baselines. The backdoor never needs them, so the server only has
	// them for classes that existed at map load; entering entities of other
	// classes get one synthesized in the proof (the game is never touched).
	std::map<int, std::vector<uint8_t>> synthBaselines;  // class id -> data
	int baselineCount = 0;                               // entries in the demo's instancebaseline table
	int baselineLiveStart = 0;                           // client entries when the segment began
	std::map<int, int> baselineIndexMap;                 // later client entries -> demo index
	std::vector<uint8_t> baselineScratch;
	bool lastPacketBackdoor = false;
};

extern ProofRecorder *proofRecorder;
