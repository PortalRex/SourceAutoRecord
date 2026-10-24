#include "ProofConvert.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <deque>
#include <map>

using namespace SarProof;

namespace {
	struct StringTableInfo {
		std::string name;
		int maxEntries = 0;
		int entryBits = 0;
		bool fixedSize = false;
		int userDataBits = 0;
	};

	struct MsgSpan {
		int type;
		size_t start, end;
		int tick = -1;         // net_Tick
		bool peDelta = false;  // svc_PacketEntities
	};

	// Walks the net messages of one packet (Portal 2, network protocol 2001).
	bool WalkMessages(const uint8_t *d, size_t nbits, std::vector<StringTableInfo> &tables, std::vector<MsgSpan> &out, std::string &err) {
		out.clear();
		BitReader br(d, 0, nbits);
		while (br.Left() >= NETMSG_TYPE_BITS) {
			MsgSpan m;
			m.start = br.pos;
			m.type = (int)br.U(NETMSG_TYPE_BITS);
			switch (m.type) {
			case net_NOP: break;
			case net_Disconnect: br.String(); break;
			case net_File:
				br.Skip(32);
				br.String();
				br.Skip(2);
				break;
			case net_SplitScreenUser: br.Skip(1); break;
			case net_Tick:
				m.tick = (int)br.U(32);
				br.Skip(32);
				break;
			case net_StringCmd: br.String(); break;
			case net_SetConVar: {
				int n = (int)br.U(8);
				for (int i = 0; i < n && !br.overflow; ++i) {
					br.String();
					br.String();
				}
				break;
			}
			case net_SignonState: {
				br.Skip(8 + 32 + 32);
				uint32_t l = br.U(32);
				br.Skip((size_t)l * 8);
				l = br.U(32);
				br.Skip((size_t)l * 8);
				break;
			}
			case svc_ServerInfo:
				br.Skip(16 + 32 + 1 + 1 + 32 + 32 + 16 + 32 + 8 + 8 + 32 + 8);
				for (int i = 0; i < 4; ++i) br.String();
				break;
			case svc_SendTable: {
				br.Skip(1);
				uint32_t l = br.U(16);
				br.Skip(l);
				break;
			}
			case svc_ClassInfo: {
				int n = (int)br.U(16);
				if (!br.B()) {
					int bits = Log2((unsigned)n) + 1;
					for (int i = 0; i < n && !br.overflow; ++i) {
						br.Skip(bits);
						br.String();
						br.String();
					}
				}
				break;
			}
			case svc_SetPause: br.Skip(1); break;
			case svc_CreateStringTable: {
				StringTableInfo t;
				t.name = br.String();
				t.maxEntries = (int)br.U(16);
				t.entryBits = Log2((unsigned)t.maxEntries);
				br.Skip(t.entryBits + 1);
				uint32_t dlen = br.U(20);
				t.fixedSize = br.B();
				if (t.fixedSize) {
					br.Skip(12);
					t.userDataBits = (int)br.U(4);
				}
				br.Skip(2);
				br.Skip(dlen);
				if (!br.overflow) tables.push_back(t);
				break;
			}
			case svc_UpdateStringTable: {
				br.Skip(5);
				if (br.B()) br.Skip(16);
				uint32_t dlen = br.U(20);
				br.Skip(dlen);
				break;
			}
			case svc_VoiceInit:
				br.String();
				if (br.U(8) == 255) br.Skip(16);
				break;
			case svc_VoiceData: {
				br.Skip(16);
				uint32_t l = br.U(16);
				br.Skip(l);
				break;
			}
			case svc_Print: br.String(); break;
			case svc_Sounds: {
				bool reliable = br.B();
				if (!reliable) br.Skip(8);
				uint32_t l = br.U(reliable ? 8 : 16);
				br.Skip(l);
				break;
			}
			case svc_SetView: br.Skip(MAX_EDICT_BITS); break;
			case svc_FixAngle: br.Skip(1 + 48); break;
			case svc_CrosshairAngle: br.Skip(48); break;
			case svc_BSPDecal: {
				bool has[3] = {br.B(), br.B(), br.B()};
				for (bool h : has) {
					if (!h) continue;
					bool hasInt = br.B(), hasFrac = br.B();
					if (hasInt || hasFrac) {
						br.Skip(1);
						if (hasInt) br.Skip(14);
						if (hasFrac) br.Skip(5);
					}
				}
				br.Skip(9);
				if (br.B()) br.Skip(MAX_EDICT_BITS + 11);
				br.Skip(1);
				break;
			}
			case svc_SplitScreen: br.Skip(1 + 1 + MAX_EDICT_BITS); break;
			case svc_UserMessage: {
				br.Skip(8);
				uint32_t l = br.U(12);
				br.Skip(l);
				break;
			}
			case svc_EntityMessage: {
				br.Skip(MAX_EDICT_BITS + 9);
				uint32_t l = br.U(11);
				br.Skip(l);
				break;
			}
			case svc_GameEvent: {
				uint32_t l = br.U(11);
				br.Skip(l);
				break;
			}
			case svc_PacketEntities: {
				br.Skip(11);
				m.peDelta = br.B();
				if (m.peDelta) br.Skip(32);
				br.Skip(1 + 11);
				uint32_t l = br.U(20);
				br.Skip(1);
				br.Skip(l);
				break;
			}
			case svc_TempEntities: {
				br.Skip(8);
				uint32_t l = br.U(17);
				br.Skip(l);
				break;
			}
			case svc_Prefetch: br.Skip(13); break;
			case svc_Menu: {
				br.Skip(16);
				uint32_t l = br.U(32);
				br.Skip(l);
				break;
			}
			case svc_GameEventList: {
				br.Skip(9);
				uint32_t l = br.U(20);
				br.Skip(l);
				break;
			}
			case svc_GetCvarValue:
				br.Skip(32);
				br.String();
				break;
			case svc_CmdKeyValues: {
				uint32_t l = br.U(32);
				br.Skip((size_t)l * 8);
				break;
			}
			case svc_PaintmapData: {
				uint32_t l = br.U(32);
				br.Skip(l);
				break;
			}
			default:
				err = "unknown net message " + std::to_string(m.type);
				return false;
			}
			if (br.overflow) {
				err = "truncated net message " + std::to_string(m.type);
				return false;
			}
			m.end = br.pos;
			out.push_back(m);
		}
		return true;
	}

	struct PacketRec {
		uint8_t flags;
		int clock;
		const uint8_t *cmdinfo;
		int inSeq, outAck;
		size_t nbits;
		const uint8_t *data;
	};

	bool ParsePacket(const Record &r, PacketRec &p) {
		ByteReader br(r.data, r.size);
		p.flags = br.U8();
		p.clock = br.I32();
		p.cmdinfo = br.Bytes(DEMO_CMDINFO_SIZE);
		p.inSeq = br.I32();
		p.outAck = br.I32();
		p.nbits = (size_t)br.Var();
		p.data = br.Bytes((p.nbits + 7) / 8);
		return !br.bad && p.data && p.cmdinfo;
	}

	struct StringEntry {
		int index;
		bool hasString, hasData;
		std::string str;
		const uint8_t *data;
		size_t len;
	};

	struct StringTableRec {
		std::string table;
		std::vector<StringEntry> entries;
	};

	bool ParseStringTable(const Record &r, StringTableRec &s) {
		ByteReader br(r.data, r.size);
		br.I32();
		s.table = br.Str();
		size_t n = (size_t)br.Var();
		for (size_t i = 0; i < n && !br.bad; ++i) {
			StringEntry e;
			e.index = (int)br.Var();
			uint8_t f = br.U8();
			e.hasString = f & STE_STRING;
			e.hasData = f & STE_DATA;
			if (e.hasString) e.str = br.Str();
			e.data = nullptr;
			e.len = 0;
			if (e.hasData) e.data = br.Blob(e.len);
			s.entries.push_back(e);
		}
		return !br.bad;
	}

	struct EntitiesRec {
		int tick;
		bool delta;
		int maxEntries, updated;
		size_t nbits;
		const uint8_t *data;
	};

	bool ParseEntities(const Record &r, EntitiesRec &e) {
		ByteReader br(r.data, r.size);
		e.tick = br.I32();
		e.delta = br.U8() & ENT_DELTA;
		e.maxEntries = (int)br.Var();
		e.updated = (int)br.Var();
		e.nbits = (size_t)br.Var();
		e.data = br.Bytes((e.nbits + 7) / 8);
		return !br.bad && e.data;
	}

	class DemoBuilder {
	public:
		ByteWriter out;
		int lastTick = 0;
		int frames = 0;

		DemoBuilder() { out.buf.resize(DEMO_HEADER_SIZE, 0); }

		void Header(uint8_t cmd, int tick) {
			out.U8(cmd);
			out.I32(tick);
			out.U8(0);  // player slot
			if (tick > lastTick) lastTick = tick;
		}
		void Packet(uint8_t cmd, int tick, const uint8_t *cmdinfo, int inSeq, int outAck, const BitWriter &msgs) {
			Header(cmd, tick);
			out.Bytes(cmdinfo, DEMO_CMDINFO_SIZE);
			out.I32(inSeq);
			out.I32(outAck);
			BitWriter padded = msgs;
			// the engine pads short tails with a NOP so readers never see a partial type
			int rem = (int)(padded.bits % 8);
			if (rem > 0 && rem <= 8 - NETMSG_TYPE_BITS) padded.UBits(net_NOP, NETMSG_TYPE_BITS);
			out.I32((int32_t)padded.Bytes());
			out.Bytes(padded.buf.data(), padded.Bytes());
			if (cmd == dem_packet) ++frames;
		}
		void Blob(uint8_t cmd, int tick, const void *p, size_t n) {
			Header(cmd, tick);
			out.I32((int32_t)n);
			out.Bytes(p, n);
		}
		void FinishHeader(const char *server, const char *client, const char *map, const char *gamedir, int netProtocol, float ipt, int signonLength) {
			auto h = out.buf.data();
			memset(h, 0, DEMO_HEADER_SIZE);
			memcpy(h, "HL2DEMO", 8);
			auto put32 = [&](int off, uint32_t v) {
				for (int i = 0; i < 4; ++i) h[off + i] = (v >> (i * 8)) & 0xFF;
			};
			put32(8, 4);
			put32(12, (uint32_t)netProtocol);
			auto putStr = [&](int off, const char *s) {
				strncpy((char *)h + off, s ? s : "", 259);
			};
			putStr(16, server);
			putStr(16 + 260, client);
			putStr(16 + 520, map);
			putStr(16 + 780, gamedir);
			float t = lastTick * ipt;
			uint32_t tu;
			memcpy(&tu, &t, 4);
			put32(1056, tu);
			put32(1060, (uint32_t)lastTick);
			put32(1064, (uint32_t)frames);
			put32(1068, (uint32_t)signonLength);
		}
	};

	void WriteStringTableMsg(BitWriter &m, int tableId, const StringTableInfo &t, const std::vector<StringEntry> &entries, size_t begin, size_t end) {
		BitWriter data;
		data.Bit(0);  // not dictionary encoded
		int last = -1;
		for (size_t i = begin; i < end; ++i) {
			auto &e = entries[i];
			if (e.index == last + 1) {
				data.Bit(1);
			} else {
				data.Bit(0);
				data.UBits((uint32_t)e.index, t.entryBits);
			}
			if (e.hasString) {
				data.Bit(1);
				data.Bit(0);  // no substring reference
				data.String(e.str.c_str());
			} else {
				data.Bit(0);
			}
			if (e.hasData && e.len > 0) {
				data.Bit(1);
				if (t.fixedSize) {
					data.BitsFrom(e.data, 0, std::min((size_t)t.userDataBits, e.len * 8));
					for (size_t k = e.len * 8; k < (size_t)t.userDataBits; ++k) data.Bit(0);
				} else {
					data.UBits((uint32_t)e.len, 14);
					data.BitsFrom(e.data, 0, e.len * 8);
				}
			} else {
				data.Bit(0);
			}
			last = e.index;
		}
		size_t count = end - begin;
		m.UBits(svc_UpdateStringTable, NETMSG_TYPE_BITS);
		m.UBits((uint32_t)tableId, 5);
		if (count != 1) {
			m.Bit(1);
			m.UBits((uint32_t)count, 16);
		} else {
			m.Bit(0);
		}
		m.UBits((uint32_t)data.bits, 20);
		m.Append(data);
	}

	void WriteEntitiesMsg(BitWriter &m, const EntitiesRec &e, int deltaFrom) {
		m.UBits(svc_PacketEntities, NETMSG_TYPE_BITS);
		m.UBits((uint32_t)e.maxEntries, 11);
		m.Bit(e.delta);
		if (e.delta) m.UBits((uint32_t)deltaFrom, 32);
		m.Bit(0);  // baseline
		m.UBits((uint32_t)e.updated, 11);
		m.UBits((uint32_t)e.nbits, 20);
		m.Bit(0);  // update baseline
		m.BitsFrom(e.data, 0, e.nbits);
	}

	struct SegmentInfo {
		int index = 0;
		uint8_t flags = 0;
		std::string map, gamedir, server, client;
		int netProtocol = 2001;
		float ipt = 1.0f / 60;
		int maxClients = 1;
		int numClasses = 0;
	};

	class SegmentConverter {
	public:
		SegmentInfo info;
		const std::vector<uint8_t> *datatables = nullptr;
		const std::vector<std::string> *callbacks = nullptr;
		std::vector<std::string> *warnings = nullptr;
		DemoBuilder demo;
		std::vector<StringTableInfo> tables;
		bool synced = false;
		int syncClock = 0;
		int signonLength = 0;
		bool wroteDatatables = false;
		bool dropNetEntities = true;  // until the first full network update
		int lastSynthTick = -1;
		std::deque<StringTableRec> pendingStrings;
		std::deque<EntitiesRec> pendingEntities;
		std::vector<std::pair<const uint8_t *, int>> pendingSignon;  // (packet record, size)
		const uint8_t *lastCmdinfo = nullptr;
		int lastInSeq = 0, lastOutAck = 0;
		int packets = 0;

		void Warn(const std::string &w) {
			if (warnings && warnings->size() < 100) warnings->push_back("segment " + std::to_string(info.index) + ": " + w);
		}

		int TableId(const std::string &name) {
			for (size_t i = 0; i < tables.size(); ++i)
				if (tables[i].name == name) return (int)i;
			return -1;
		}

		// string table updates as one or more messages, each comfortably below
		// the packet size limit
		void StringTableMsgs(const StringTableRec &s, std::vector<BitWriter> &msgs) {
			int id = TableId(s.table);
			if (id < 0) {
				Warn("string table '" + s.table + "' was never created; update dropped");
				return;
			}
			auto &t = tables[id];
			size_t begin = 0;
			while (begin < s.entries.size()) {
				size_t end = begin, bytes = 0;
				while (end < s.entries.size() && end - begin < 65535) {
					size_t sz = 8 + s.entries[end].str.size() + s.entries[end].len;
					if (end > begin && bytes + sz > 48000) break;
					bytes += sz;
					++end;
				}
				BitWriter m;
				WriteStringTableMsg(m, id, t, s.entries, begin, end);
				msgs.push_back(std::move(m));
				begin = end;
			}
		}

		void EmitPacket(uint8_t cmd, int tick, const uint8_t *cmdinfo, int inSeq, int outAck, const BitWriter &msgs) {
			if (msgs.Bytes() > (size_t)DEMO_MAX_PACKET) Warn("packet of " + std::to_string(msgs.Bytes()) + " bytes exceeds the demo player's limit");
			demo.Packet(cmd, tick, cmdinfo, inSeq, outAck, msgs);
		}

		// Flush queued string table updates as packets of their own.
		void FlushStrings(uint8_t cmd, int tick, const uint8_t *cmdinfo, int inSeq, int outAck) {
			while (!pendingStrings.empty()) {
				std::vector<BitWriter> msgs;
				StringTableMsgs(pendingStrings.front(), msgs);
				pendingStrings.pop_front();
				BitWriter pkt;
				for (auto &m : msgs) {
					if (pkt.Bytes() + m.Bytes() > 60000 && pkt.bits) {
						EmitPacket(cmd, tick, cmdinfo, inSeq, outAck, pkt);
						pkt = BitWriter();
					}
					pkt.Append(m);
				}
				if (pkt.bits) EmitPacket(cmd, tick, cmdinfo, inSeq, outAck, pkt);
			}
		}

		bool EntitiesMsg(const EntitiesRec &e, int netTick, BitWriter &m) {
			if (e.delta && lastSynthTick < 0) {
				Warn("entity delta at tick " + std::to_string(e.tick) + " has nothing to delta from; dropped");
				return false;
			}
			WriteEntitiesMsg(m, e, lastSynthTick);
			lastSynthTick = netTick;
			return true;
		}

		// Snapshots no packet carried (shouldn't happen): give each its own
		// packet so later deltas still have a frame to refer to.
		void FlushOrphanEntities(int beforeTick, uint8_t cmd, int demoTick, const uint8_t *cmdinfo, int inSeq, int outAck) {
			while (!pendingEntities.empty() && pendingEntities.front().tick < beforeTick) {
				auto e = pendingEntities.front();
				pendingEntities.pop_front();
				FlushStrings(cmd, demoTick, cmdinfo, inSeq, outAck);
				BitWriter pkt;
				pkt.UBits(net_Tick, NETMSG_TYPE_BITS);
				pkt.UBits((uint32_t)e.tick, 32);
				pkt.UBits(0, 32);
				if (EntitiesMsg(e, e.tick, pkt)) EmitPacket(cmd, demoTick, cmdinfo, inSeq, outAck, pkt);
				Warn("entity snapshot " + std::to_string(e.tick) + " had no packet");
			}
		}

		void Packet(const PacketRec &p) {
			++packets;
			bool signon = (p.flags & PKT_SIGNON) || !synced;
			uint8_t cmd = signon ? dem_signon : dem_packet;
			// like GetRecordingTick: packets the client processes after its tick
			// count resets for a level change (before the next signon) get
			// negative ticks
			int tick = signon ? 0 : p.clock - syncClock;
			lastCmdinfo = p.cmdinfo;
			lastInSeq = p.inSeq;
			lastOutAck = p.outAck;

			std::vector<MsgSpan> msgs;
			std::string err;
			if (!WalkMessages(p.data, p.nbits, tables, msgs, err)) {
				Warn("packet at clock " + std::to_string(p.clock) + ": " + err + "; passed through unchanged");
				BitWriter raw;
				raw.BitsFrom(p.data, 0, p.nbits);
				if (raw.bits) EmitPacket(cmd, tick, p.cmdinfo, p.inSeq, p.outAck, raw);
				return;
			}

			// the datatables must be loaded before svc_ClassInfo is processed
			if (!wroteDatatables && datatables && std::any_of(msgs.begin(), msgs.end(), [](auto &m) { return m.type == svc_ClassInfo; })) {
				demo.Blob(dem_datatables, 0, datatables->data(), datatables->size());
				wroteDatatables = true;
			}

			bool backdoor = p.flags & PKT_BACKDOOR;
			int netTick = -1;
			for (auto &m : msgs) {
				if (m.type == svc_PacketEntities) break;
				if (m.type == net_Tick) netTick = m.tick;
			}
			if (backdoor && netTick >= 0) FlushOrphanEntities(netTick, cmd, tick, p.cmdinfo, p.inSeq, p.outAck);

			// string table updates the client received out of band go before
			// any entities that may refer to them
			if (!pendingStrings.empty() && (!backdoor || (!pendingEntities.empty() && pendingEntities.front().tick == netTick))) {
				FlushStrings(cmd, tick, p.cmdinfo, p.inSeq, p.outAck);
			}

			BitWriter outMsgs;
			int curTick = -1;
			for (auto &m : msgs) {
				if (m.type == net_Tick) curTick = m.tick;
				if (m.type == svc_PacketEntities) {
					if (backdoor) {
						// the client ignored this one; the backdoor copy replaces it
						if (!pendingEntities.empty() && pendingEntities.front().tick == curTick) {
							auto e = pendingEntities.front();
							pendingEntities.pop_front();
							EntitiesMsg(e, curTick, outMsgs);
						}
						continue;
					}
					if (dropNetEntities) {
						if (m.peDelta) continue;  // refers to frames this demo doesn't have
						dropNetEntities = false;
					}
					lastSynthTick = -1;
				}
				outMsgs.BitsFrom(p.data, m.start, m.end - m.start);
			}
			// whatever trails the last whole message (fewer bits than a message type)
			size_t tail = msgs.empty() ? 0 : msgs.back().end;
			if (tail < p.nbits) outMsgs.BitsFrom(p.data, tail, p.nbits - tail);
			if (backdoor) dropNetEntities = true;
			// the engine never writes packets without messages
			if (outMsgs.bits) EmitPacket(cmd, tick, p.cmdinfo, p.inSeq, p.outAck, outMsgs);
		}

		void Sync(int clock) {
			if (synced) return;
			synced = true;
			syncClock = clock;
			if (!wroteDatatables && datatables) {
				demo.Blob(dem_datatables, 0, datatables->data(), datatables->size());
				wroteDatatables = true;
				Warn("no svc_ClassInfo in the signon data");
			}
			signonLength = (int)demo.out.Size() - DEMO_HEADER_SIZE;
			demo.Header(dem_synctick, 0);
			WriteCallbacks();
		}

		bool wroteCallbacks = false;
		void WriteCallbacks() {
			if (!wroteCallbacks && callbacks && !callbacks->empty()) {
				wroteCallbacks = true;
				ByteWriter w;
				w.I32((int32_t)callbacks->size());
				for (auto &c : *callbacks) w.Bytes(c.c_str(), c.size() + 1);
				demo.Header(dem_customdata, 0);
				demo.out.I32(-1);
				demo.out.I32((int32_t)w.Size());
				demo.out.Bytes(w.buf.data(), w.Size());
			}
		}

		int DemoTick(int clock) { return clock - syncClock; }

		void Finish(DemoOut &out) {
			if (!pendingEntities.empty() && lastCmdinfo) FlushOrphanEntities(INT32_MAX, dem_packet, demo.lastTick, lastCmdinfo, lastInSeq, lastOutAck);
			if (!synced) Sync(0);
			demo.Header(dem_stop, demo.lastTick);
			demo.FinishHeader(info.server.c_str(), info.client.c_str(), info.map.c_str(), info.gamedir.c_str(), info.netProtocol, info.ipt, signonLength);
			out.mapName = info.map;
			out.ticks = demo.lastTick;
			out.bytes = std::move(demo.out.buf);
		}
	};
}  // namespace

bool SarProof::ConvertRecords(const ReadResult &proof, std::vector<DemoOut> &demos, std::vector<std::string> &warnings, std::string &error) {
	std::vector<uint8_t> datatables;
	std::vector<std::string> callbacks;
	std::unique_ptr<SegmentConverter> seg;

	auto finishSegment = [&]() {
		if (!seg) return;
		DemoOut out;
		seg->Finish(out);
		if (seg->packets > 0) demos.push_back(std::move(out));
		seg.reset();
	};

	for (auto &r : proof.records) {
		ByteReader br(r.data, r.size);
		switch (r.type) {
		case REC_DATATABLES:
			datatables.assign(r.data, r.data + r.size);
			break;
		case REC_CUSTOMDATA_TABLE: {
			callbacks.clear();
			size_t n = (size_t)br.Var();
			for (size_t i = 0; i < n && !br.bad; ++i) callbacks.push_back(br.Str());
			if (seg && seg->synced) seg->WriteCallbacks();
			break;
		}
		case REC_SEGMENT_BEGIN: {
			finishSegment();
			seg.reset(new SegmentConverter());
			auto &i = seg->info;
			i.index = (int)br.Var();
			i.flags = br.U8();
			i.map = br.Str();
			i.gamedir = br.Str();
			i.server = br.Str();
			i.client = br.Str();
			i.netProtocol = br.I32();
			i.ipt = br.F32();
			i.maxClients = (int)br.Var();
			i.numClasses = (int)br.Var();
			seg->datatables = &datatables;
			seg->callbacks = &callbacks;
			seg->warnings = &warnings;
			break;
		}
		case REC_SEGMENT_END:
			finishSegment();
			break;
		case REC_SYNC:
			if (seg) seg->Sync(br.I32());
			break;
		case REC_PACKET: {
			if (!seg) break;
			PacketRec p;
			if (!ParsePacket(r, p)) {
				warnings.push_back("bad packet record");
				break;
			}
			seg->Packet(p);
			break;
		}
		case REC_ENTITIES: {
			if (!seg) break;
			EntitiesRec e;
			if (ParseEntities(r, e)) seg->pendingEntities.push_back(e);
			break;
		}
		case REC_STRINGTABLE: {
			if (!seg) break;
			StringTableRec s;
			if (ParseStringTable(r, s)) seg->pendingStrings.push_back(std::move(s));
			break;
		}
		case REC_USERCMD: {
			if (!seg || !seg->synced) break;
			int clock = br.I32();
			int cmdNumber = br.I32();
			uint8_t slot = br.U8();
			size_t len;
			auto data = br.Blob(len);
			if (br.bad) break;
			seg->demo.out.U8(dem_usercmd);
			int tick = seg->DemoTick(clock);
			seg->demo.out.I32(tick);
			seg->demo.out.U8(slot);
			if (tick > seg->demo.lastTick) seg->demo.lastTick = tick;
			seg->demo.out.I32(cmdNumber);
			seg->demo.out.I32((int32_t)len);
			seg->demo.out.Bytes(data, len);
			break;
		}
		case REC_CONSOLECMD: {
			if (!seg || !seg->synced) break;
			int clock = br.I32();
			br.U8();
			std::string cmd = br.Str();
			if (br.bad) break;
			seg->demo.Blob(dem_consolecmd, seg->DemoTick(clock), cmd.c_str(), cmd.size() + 1);
			break;
		}
		case REC_CUSTOMDATA: {
			if (!seg || !seg->synced) break;
			int clock = br.I32();
			int callback = br.I32();
			size_t len;
			auto data = br.Blob(len);
			if (br.bad) break;
			seg->demo.Header(dem_customdata, seg->DemoTick(clock));
			seg->demo.out.I32(callback);
			seg->demo.out.I32((int32_t)len);
			seg->demo.out.Bytes(data, len);
			break;
		}
		default:
			break;
		}
	}
	finishSegment();
	if (!proof.complete) warnings.push_back("proof incomplete: " + proof.error);
	return true;
}

bool SarProof::ConvertToDemos(const std::string &proofPath, const std::string &outBase, ConvertResult &result) {
	ReadResult proof;
	if (!ReadFile(proofPath, proof) || !proof.ok) {
		result.error = proof.error.empty() ? "couldn't read " + proofPath : proof.error;
		return false;
	}
	std::vector<DemoOut> demos;
	if (!ConvertRecords(proof, demos, result.warnings, result.error)) return false;
	for (size_t i = 0; i < demos.size(); ++i) {
		std::string path = outBase + (i == 0 ? "" : "_" + std::to_string(i + 1)) + ".dem";
		FILE *fp = fopen(path.c_str(), "wb");
		if (!fp) {
			result.error = "couldn't write " + path;
			return false;
		}
		bool ok = fwrite(demos[i].bytes.data(), 1, demos[i].bytes.size(), fp) == demos[i].bytes.size();
		fclose(fp);
		if (!ok) {
			result.error = "couldn't write " + path;
			return false;
		}
		result.files.push_back(path);
	}
	return true;
}

bool SarProof::Describe(const std::string &proofPath, const uint8_t *expectedPubKey, std::string &text) {
	ReadResult proof;
	char line[512];
	if (!ReadFile(proofPath, proof, expectedPubKey) || !proof.ok) {
		text = "Not a readable SAR proof: " + proof.error + "\n";
		return false;
	}
	time_t created = (time_t)proof.createdTime;
	char date[64] = "?";
	if (auto tm = gmtime(&created)) strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S UTC", tm);
	snprintf(line, sizeof line, "SAR proof %s\n  format v%u, created %s, %u chunks, %.1f KiB raw\n", proofPath.c_str(), proof.version, date, proof.chunks, proof.raw.size() / 1024.0);
	text += line;
	text += "  integrity: ";
	text += proof.complete ? "complete, hash chain matches" : "NOT COMPLETE (" + proof.error + ")";
	text += "\n  signature: ";
	text += proof.keyMatches ? "valid (SAR release key)" : proof.signedByKey ? "valid, but not this build's key" : "none";
	text += "\n  hash: " + HashHex(proof.hash, 32) + "...\n";

	struct SegStat {
		std::string map;
		uint8_t flags = 0;
		int packets = 0, entities = 0, usercmds = 0, cmds = 0, custom = 0, strings = 0, events = 0;
		int syncClock = 0, lastClock = 0;
		bool synced = false;
		std::string end;
	};
	std::vector<SegStat> segs;
	std::map<int, size_t> bytesByType;
	for (auto &r : proof.records) {
		bytesByType[r.type] += r.size;
		ByteReader br(r.data, r.size);
		switch (r.type) {
		case REC_FILE_INFO: {
			size_t n = (size_t)br.Var();
			for (size_t i = 0; i < n && !br.bad; ++i) {
				auto k = br.Str();
				auto v = br.Str();
				text += "  " + k + ": " + v + "\n";
			}
			break;
		}
		case REC_SEGMENT_BEGIN: {
			SegStat s;
			br.Var();
			s.flags = br.U8();
			s.map = br.Str();
			segs.push_back(s);
			break;
		}
		case REC_SEGMENT_END:
			if (!segs.empty()) {
				br.I32();
				segs.back().end = br.Str();
			}
			break;
		case REC_SYNC:
			if (!segs.empty()) {
				segs.back().syncClock = br.I32();
				segs.back().synced = true;
			}
			break;
		case REC_PACKET:
			if (!segs.empty()) {
				++segs.back().packets;
				br.U8();
				int clock = br.I32();
				if (segs.back().synced) segs.back().lastClock = std::max(segs.back().lastClock, clock);
			}
			break;
		case REC_ENTITIES:
			if (!segs.empty()) ++segs.back().entities;
			break;
		case REC_STRINGTABLE:
			if (!segs.empty()) ++segs.back().strings;
			break;
		case REC_USERCMD:
			if (!segs.empty()) ++segs.back().usercmds;
			break;
		case REC_CONSOLECMD:
			if (!segs.empty()) ++segs.back().cmds;
			break;
		case REC_CUSTOMDATA:
			if (!segs.empty()) ++segs.back().custom;
			break;
		case REC_EVENT:
			if (!segs.empty()) {
				++segs.back().events;
				uint8_t kind = br.U8();
				br.I32();
				auto msg = br.Str();
				if (kind == EV_LOST_DATA) text += "  WARNING (" + segs.back().map + "): " + msg + "\n";
			}
			break;
		}
	}
	int total = 0;
	for (size_t i = 0; i < segs.size(); ++i) {
		auto &s = segs[i];
		int ticks = s.synced ? std::max(0, s.lastClock - s.syncClock) : 0;
		total += ticks;
		snprintf(line, sizeof line, "  segment %d: %-28s %6d ticks  %5d packets %5d snapshots %5d usercmds %4d cmds %4d sar%s%s\n", (int)i + 1, s.map.c_str(), ticks, s.packets, s.entities, s.usercmds, s.cmds, s.custom, (s.flags & SEG_MIDSTART) ? "  (started mid-map)" : "", s.end.empty() ? "" : ("  [" + s.end + "]").c_str());
		text += line;
	}
	snprintf(line, sizeof line, "  %d segments, %d ticks total\n", (int)segs.size(), total);
	text += line;
	return true;
}
