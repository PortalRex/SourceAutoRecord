#pragma once

// SAR proof files (.sarp): a speedrun proof format that records the same
// information as a Source demo without making the engine record one.
//
// This header is shared by the in-game recorder and by the converter, which
// is also built into a standalone tool (tools/sarproof), so it must not
// depend on anything engine-specific.
//
// File layout (all integers little-endian):
//   FileHeader                      64 bytes
//   Chunk*                          ChunkHeader + stored bytes
//   Trailer                         written by a clean stop
//
// A chunk holds a run of records, deflated (zlib format) when that is smaller.
// Each record is: u8 type, varint length, payload. Chunks are self-contained,
// so a file cut short by a crash is readable up to its last whole chunk.
//
// Integrity: hash[0] = SHA-512 of the file header; hash[i] = SHA-512 of
// (hash[i-1] || raw bytes of chunk i). The trailer stores the final hash and
// an ed25519 signature of it made with SAR's demo signing key (zero when the
// build has no key).

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace SarProof {
	constexpr char FILE_MAGIC[8] = {'S', 'A', 'R', 'P', 'R', 'O', 'O', 'F'};
	constexpr uint32_t FORMAT_VERSION = 1;
	constexpr uint32_t FILE_HEADER_SIZE = 64;
	constexpr uint32_t CHUNK_MAGIC = 0x4B4E4843;    // "CHNK"
	constexpr uint32_t TRAILER_MAGIC = 0x444E4553;  // "SEND"
	constexpr uint32_t CHUNK_HEADER_SIZE = 24;
	constexpr uint32_t TRAILER_SIZE = 4 + 4 + 64 + 64 + 32;
	constexpr uint32_t CHUNK_FLAG_DEFLATE = 1;
	constexpr uint32_t MAX_CHUNK_RAW = 16 * 1024 * 1024;

	enum RecordType : uint8_t {
		REC_FILE_INFO = 0x01,         // varint n, n * (string key, string value)
		REC_DATATABLES = 0x02,        // dem_datatables blob
		REC_CUSTOMDATA_TABLE = 0x03,  // varint n, n * string (demo custom data callback names)
		REC_SEGMENT_BEGIN = 0x10,
		REC_SEGMENT_END = 0x11,
		REC_SYNC = 0x12,              // i32 clock: demo tick 0 of the segment
		REC_PACKET = 0x20,            // a packet the client processed
		REC_ENTITIES = 0x21,          // synthesized svc_PacketEntities body (backdoor mode)
		REC_STRINGTABLE = 0x22,       // string table entries the client gained outside the network stream
		REC_USERCMD = 0x23,
		REC_CONSOLECMD = 0x24,
		REC_CUSTOMDATA = 0x25,        // demo custom data (SAR's own data uses callback 0)
		REC_SIGNON_STATE = 0x26,
		REC_EVENT = 0x27,             // u8 kind, string text: informational
		REC_END = 0x7F,
	};

	enum PacketFlags : uint8_t {
		PKT_SIGNON = 1,    // part of the segment's signon data
		PKT_BACKDOOR = 2,  // the client ignored this packet's svc_PacketEntities (local backdoor active)
	};

	enum SegmentFlags : uint8_t {
		SEG_MIDSTART = 1,     // recording started in the middle of a map
		SEG_MULTIPLAYER = 2,  // demo clock runs off net_time (GetRecordingTick for maxclients > 1)
	};

	enum EntitiesFlags : uint8_t {
		ENT_DELTA = 1,  // delta from the previous REC_ENTITIES of the segment
	};

	enum StringTableFlags : uint8_t {
		STE_STRING = 1,
		STE_DATA = 2,
	};

	enum ConsoleCmdFlags : uint8_t {
		CMD_DEMOCVAR = 1,  // FCVAR_DEMO value at segment start
	};

	enum EventKind : uint8_t {
		EV_NOTE = 0,
		EV_KEYFRAME = 1,
		EV_FULLUPDATE = 2,
		EV_LOST_DATA = 3,
	};

	// Source demo constants (Portal 2: demo protocol 4, network protocol 2001)
	enum DemoCmd : uint8_t {
		dem_signon = 1,
		dem_packet = 2,
		dem_synctick = 3,
		dem_consolecmd = 4,
		dem_usercmd = 5,
		dem_datatables = 6,
		dem_stop = 7,
		dem_customdata = 8,
		dem_stringtables = 9,
	};
	constexpr int DEMO_HEADER_SIZE = 1072;
	constexpr int DEMO_CMDINFO_SIZE = 76 * 2;  // democmdinfo_t, MAX_SPLITSCREEN_CLIENTS = 2
	constexpr int DEMO_MAX_PACKET = 96000;     // larger packets are dropped by the demo player

	enum NetMsg : int {
		net_NOP = 0,
		net_Disconnect = 1,
		net_File = 2,
		net_SplitScreenUser = 3,
		net_Tick = 4,
		net_StringCmd = 5,
		net_SetConVar = 6,
		net_SignonState = 7,
		svc_ServerInfo = 8,
		svc_SendTable = 9,
		svc_ClassInfo = 10,
		svc_SetPause = 11,
		svc_CreateStringTable = 12,
		svc_UpdateStringTable = 13,
		svc_VoiceInit = 14,
		svc_VoiceData = 15,
		svc_Print = 16,
		svc_Sounds = 17,
		svc_SetView = 18,
		svc_FixAngle = 19,
		svc_CrosshairAngle = 20,
		svc_BSPDecal = 21,
		svc_SplitScreen = 22,
		svc_UserMessage = 23,
		svc_EntityMessage = 24,
		svc_GameEvent = 25,
		svc_PacketEntities = 26,
		svc_TempEntities = 27,
		svc_Prefetch = 28,
		svc_Menu = 29,
		svc_GameEventList = 30,
		svc_GetCvarValue = 31,
		svc_CmdKeyValues = 32,
		svc_PaintmapData = 33,
		NET_MSG_COUNT = 34,
	};
	constexpr int NETMSG_TYPE_BITS = 6;
	constexpr int MAX_EDICT_BITS = 11;
	constexpr int NUM_NETWORKED_EHANDLE_SERIAL_BITS = 10;

	inline int Log2(unsigned v) {
		int r = -1;
		while (v) {
			v >>= 1;
			++r;
		}
		return r;
	}

	// ---- byte buffers ----

	class ByteWriter {
	public:
		std::vector<uint8_t> buf;

		void U8(uint8_t v) { buf.push_back(v); }
		void U16(uint16_t v) {
			U8(v & 0xFF);
			U8(v >> 8);
		}
		void U32(uint32_t v) {
			for (int i = 0; i < 4; ++i) U8((v >> (i * 8)) & 0xFF);
		}
		void I32(int32_t v) { U32((uint32_t)v); }
		void U64(uint64_t v) {
			for (int i = 0; i < 8; ++i) U8((v >> (i * 8)) & 0xFF);
		}
		void F32(float v) {
			uint32_t u;
			memcpy(&u, &v, 4);
			U32(u);
		}
		void Var(uint64_t v) {
			while (v >= 0x80) {
				U8((uint8_t)(v | 0x80));
				v >>= 7;
			}
			U8((uint8_t)v);
		}
		void SVar(int64_t v) { Var(((uint64_t)v << 1) ^ (uint64_t)(v >> 63)); }
		void Bytes(const void *p, size_t n) {
			auto b = (const uint8_t *)p;
			buf.insert(buf.end(), b, b + n);
		}
		void Str(const char *s) {
			size_t n = s ? strlen(s) : 0;
			Var(n);
			Bytes(s, n);
		}
		void Str(const std::string &s) {
			Var(s.size());
			Bytes(s.data(), s.size());
		}
		void Blob(const void *p, size_t n) {
			Var(n);
			Bytes(p, n);
		}
		size_t Size() const { return buf.size(); }
		void Clear() { buf.clear(); }
	};

	class ByteReader {
	public:
		const uint8_t *p;
		size_t n;
		size_t pos = 0;
		bool bad = false;

		ByteReader(const uint8_t *p, size_t n)
			: p(p)
			, n(n) {}

		bool Have(size_t k) {
			if (bad || pos + k > n) {
				bad = true;
				return false;
			}
			return true;
		}
		uint8_t U8() { return Have(1) ? p[pos++] : 0; }
		uint16_t U16() {
			uint16_t v = U8();
			v |= (uint16_t)U8() << 8;
			return v;
		}
		uint32_t U32() {
			if (!Have(4)) return 0;
			uint32_t v = p[pos] | (p[pos + 1] << 8) | (p[pos + 2] << 16) | ((uint32_t)p[pos + 3] << 24);
			pos += 4;
			return v;
		}
		int32_t I32() { return (int32_t)U32(); }
		uint64_t U64() {
			uint64_t lo = U32();
			uint64_t hi = U32();
			return lo | (hi << 32);
		}
		float F32() {
			uint32_t u = U32();
			float f;
			memcpy(&f, &u, 4);
			return f;
		}
		uint64_t Var() {
			uint64_t v = 0;
			for (int shift = 0; shift < 64; shift += 7) {
				uint8_t b = U8();
				if (bad) return 0;
				v |= (uint64_t)(b & 0x7F) << shift;
				if (!(b & 0x80)) return v;
			}
			bad = true;
			return 0;
		}
		int64_t SVar() {
			uint64_t u = Var();
			return (int64_t)(u >> 1) ^ -(int64_t)(u & 1);
		}
		const uint8_t *Bytes(size_t k) {
			if (!Have(k)) return nullptr;
			auto r = p + pos;
			pos += k;
			return r;
		}
		std::string Str() {
			size_t k = (size_t)Var();
			auto b = Bytes(k);
			return b ? std::string((const char *)b, k) : std::string();
		}
		const uint8_t *Blob(size_t &k) {
			k = (size_t)Var();
			return Bytes(k);
		}
		bool AtEnd() const { return pos >= n; }
	};

	// ---- bit buffers (LSB-first, like the engine's bf_write/bf_read) ----

	class BitWriter {
	public:
		std::vector<uint8_t> buf;
		size_t bits = 0;

		void Bit(int v) {
			if ((bits & 7) == 0) buf.push_back(0);
			if (v) buf[bits >> 3] |= (uint8_t)(1 << (bits & 7));
			++bits;
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
		void BitsFrom(const uint8_t *src, size_t startBit, size_t n) {
			// fast path when both sides are byte aligned
			if ((bits & 7) == 0 && (startBit & 7) == 0) {
				size_t whole = n >> 3;
				buf.insert(buf.end(), src + (startBit >> 3), src + (startBit >> 3) + whole);
				bits += whole * 8;
				startBit += whole * 8;
				n -= whole * 8;
			}
			for (size_t i = 0; i < n; ++i) {
				size_t b = startBit + i;
				Bit((src[b >> 3] >> (b & 7)) & 1);
			}
		}
		void Append(const BitWriter &o) { BitsFrom(o.buf.data(), 0, o.bits); }
		void String(const char *s) {
			for (; *s; ++s) UBits((uint8_t)*s, 8);
			UBits(0, 8);
		}
		size_t Bytes() const { return (bits + 7) >> 3; }
	};

	class BitReader {
	public:
		const uint8_t *d;
		size_t pos, end;
		bool overflow = false;

		BitReader(const uint8_t *d, size_t startBit, size_t endBit)
			: d(d)
			, pos(startBit)
			, end(endBit) {}

		uint32_t U(int n) {
			if (n <= 0) return 0;
			if (pos + n > end) {
				overflow = true;
				pos = end;
				return 0;
			}
			uint32_t v = 0;
			for (int i = 0; i < n; ++i, ++pos) v |= (uint32_t)((d[pos >> 3] >> (pos & 7)) & 1) << i;
			return v;
		}
		int32_t S(int n) {
			uint32_t v = U(n);
			if (n < 32 && (v >> (n - 1)) & 1) v |= ~0u << n;
			return (int32_t)v;
		}
		bool B() { return U(1) != 0; }
		void Skip(size_t n) {
			if (pos + n > end) {
				overflow = true;
				pos = end;
			} else {
				pos += n;
			}
		}
		std::string String() {
			std::string s;
			while (true) {
				uint32_t c = U(8);
				if (overflow || c == 0) break;
				s.push_back((char)c);
			}
			return s;
		}
		uint32_t UBitVar() {
			uint32_t r = U(4);
			switch (U(2)) {
			case 1: return r | (U(4) << 4);
			case 2: return r | (U(8) << 4);
			case 3: return r | (U(28) << 4);
			default: return r;
			}
		}
		size_t Left() const { return end - pos; }
	};
}  // namespace SarProof
