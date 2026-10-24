// sarproof - standalone tool for SAR proof files (.sarp)
//
//   sarproof info <file.sarp>                 verify a proof and describe it
//   sarproof convert <file.sarp> [outbase]    write <outbase>.dem, <outbase>_2.dem, ...
//   sarproof roundtrip <file.dem>...          self-test: wrap demos as proofs, convert
//                                             back, and compare the packet streams

#include "Features/Demo/ProofConvert.hpp"
#include "Features/Demo/ProofFile.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace SarProof;

static bool ReadAll(const char *path, std::vector<uint8_t> &out) {
	FILE *fp = fopen(path, "rb");
	if (!fp) return false;
	fseek(fp, 0, SEEK_END);
	long n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	out.resize(n > 0 ? (size_t)n : 0);
	bool ok = fread(out.data(), 1, out.size(), fp) == out.size();
	fclose(fp);
	return ok;
}

struct Frame {
	uint8_t cmd;
	int tick;
	uint8_t slot;
	const uint8_t *cmdinfo = nullptr;
	int inSeq = 0, outAck = 0;
	int extra = 0;  // usercmd number / customdata callback
	const uint8_t *data = nullptr;
	size_t size = 0;
};

static bool ParseDemo(const std::vector<uint8_t> &d, std::vector<Frame> &frames) {
	if (d.size() < DEMO_HEADER_SIZE || memcmp(d.data(), "HL2DEMO", 8)) return false;
	ByteReader r(d.data(), d.size());
	r.pos = DEMO_HEADER_SIZE;
	while (!r.AtEnd()) {
		Frame f;
		f.cmd = r.U8();
		f.tick = r.I32();
		f.slot = r.U8();
		if (r.bad) break;
		switch (f.cmd) {
		case dem_signon:
		case dem_packet:
			f.cmdinfo = r.Bytes(DEMO_CMDINFO_SIZE);
			f.inSeq = r.I32();
			f.outAck = r.I32();
			f.size = r.U32();
			f.data = r.Bytes(f.size);
			break;
		case dem_synctick: break;
		case dem_consolecmd:
		case dem_datatables:
		case dem_stringtables:
			f.size = r.U32();
			f.data = r.Bytes(f.size);
			break;
		case dem_usercmd:
		case dem_customdata:
			f.extra = r.I32();
			f.size = r.U32();
			f.data = r.Bytes(f.size);
			break;
		case dem_stop:
			frames.push_back(f);
			return true;
		default:
			fprintf(stderr, "bad demo command %d\n", f.cmd);
			return false;
		}
		if (r.bad) return false;
		frames.push_back(f);
	}
	return true;
}

// The records a recording of this demo would have produced (network mode).
static void DemoToProof(const std::vector<uint8_t> &d, const std::vector<Frame> &frames, ReadResult &proof) {
	ByteWriter all;
	auto emit = [&](RecordType t, const ByteWriter &w) {
		all.U8(t);
		all.Var(w.buf.size());
		all.Bytes(w.buf.data(), w.buf.size());
	};
	auto str = [&](size_t off) { return std::string((const char *)d.data() + off); };
	{
		ByteWriter w;
		w.Var(1);
		w.U8(0);
		w.Str(str(16 + 520));
		w.Str(str(16 + 780));
		w.Str(str(16));
		w.Str(str(16 + 260));
		w.I32(*(const int32_t *)(d.data() + 12));
		w.F32(1.0f / 60);
		w.Var(1);
		w.Var(0);
		emit(REC_SEGMENT_BEGIN, w);
	}
	bool synced = false;
	for (auto &f : frames) {
		ByteWriter w;
		switch (f.cmd) {
		case dem_signon:
		case dem_packet: {
			w.U8(f.cmd == dem_signon ? PKT_SIGNON : 0);
			w.I32(f.tick);
			w.Bytes(f.cmdinfo, DEMO_CMDINFO_SIZE);
			w.I32(f.inSeq);
			w.I32(f.outAck);
			w.Var(f.size * 8);
			w.Bytes(f.data, f.size);
			emit(REC_PACKET, w);
			break;
		}
		case dem_datatables:
			w.Bytes(f.data, f.size);
			emit(REC_DATATABLES, w);
			break;
		case dem_synctick:
			if (!synced) {
				w.I32(0);
				emit(REC_SYNC, w);
				synced = true;
			}
			break;
		case dem_consolecmd:
			w.I32(f.tick);
			w.U8(0);
			w.Str(std::string((const char *)f.data, strnlen((const char *)f.data, f.size)));
			emit(REC_CONSOLECMD, w);
			break;
		case dem_usercmd:
			w.I32(f.tick);
			w.I32(f.extra);
			w.U8(f.slot);
			w.Blob(f.data, f.size);
			emit(REC_USERCMD, w);
			break;
		case dem_customdata:
			if (f.extra == -1) {
				// callback table: int count, then strings
				ByteReader cr(f.data, f.size);
				int n = cr.I32();
				w.Var(n);
				for (int i = 0; i < n && !cr.bad; ++i) {
					std::string s;
					while (!cr.AtEnd()) {
						char c = (char)cr.U8();
						if (!c) break;
						s.push_back(c);
					}
					w.Str(s);
				}
				emit(REC_CUSTOMDATA_TABLE, w);
			} else {
				w.I32(f.tick);
				w.I32(f.extra);
				w.Blob(f.data, f.size);
				emit(REC_CUSTOMDATA, w);
			}
			break;
		default: break;
		}
	}
	ByteWriter end;
	end.I32(0);
	end.Str("end");
	emit(REC_SEGMENT_END, end);
	proof.ok = proof.complete = true;
	proof.raw = std::move(all.buf);
	ByteReader r(proof.raw.data(), proof.raw.size());
	while (!r.AtEnd()) {
		uint8_t t = r.U8();
		size_t n = (size_t)r.Var();
		proof.records.push_back({t, r.Bytes(n), n});
	}
}

static int Roundtrip(const char *path) {
	std::vector<uint8_t> d;
	std::vector<Frame> orig;
	if (!ReadAll(path, d) || !ParseDemo(d, orig)) {
		printf("%s: can't parse\n", path);
		return 1;
	}
	ReadResult proof;
	DemoToProof(d, orig, proof);
	std::vector<DemoOut> demos;
	std::vector<std::string> warnings;
	std::string error;
	ConvertRecords(proof, demos, warnings, error);
	if (demos.size() != 1) {
		printf("%s: expected 1 demo, got %d\n", path, (int)demos.size());
		return 1;
	}
	std::vector<Frame> conv;
	if (!ParseDemo(demos[0].bytes, conv)) {
		printf("%s: converted demo doesn't parse\n", path);
		return 1;
	}
	// compare signon/packet frames: data must match up to the converter's NOP padding
	std::vector<const Frame *> a, b;
	for (auto &f : orig)
		if (f.cmd == dem_signon || f.cmd == dem_packet) a.push_back(&f);
	for (auto &f : conv)
		if (f.cmd == dem_signon || f.cmd == dem_packet) b.push_back(&f);
	int mismatches = 0;
	size_t n = std::min(a.size(), b.size());
	for (size_t i = 0; i < n; ++i) {
		auto x = a[i], y = b[i];
		size_t common = std::min(x->size, y->size);
		bool same = x->cmd == y->cmd && !memcmp(x->data, y->data, common) && !memcmp(x->cmdinfo, y->cmdinfo, DEMO_CMDINFO_SIZE);
		// trailing bytes may differ only by padding
		for (size_t k = common; k < x->size && same; ++k) same = x->data[k] == 0;
		for (size_t k = common; k < y->size && same; ++k) same = y->data[k] == 0;
		bool tickOk = x->cmd == dem_signon || x->tick == y->tick;
		if (!same || !tickOk) {
			if (mismatches < 5) {
				size_t k = 0;
				while (k < common && x->data[k] == y->data[k]) ++k;
				bool infoSame = !memcmp(x->cmdinfo, y->cmdinfo, DEMO_CMDINFO_SIZE);
				printf("  frame %d: cmd %d/%d tick %d/%d size %d/%d differs at byte %d (%02x/%02x) cmdinfo %s\n", (int)i, x->cmd, y->cmd, x->tick, y->tick, (int)x->size, (int)y->size, (int)k, k < x->size ? x->data[k] : 0, k < y->size ? y->data[k] : 0, infoSame ? "same" : "differs");
			}
			++mismatches;
		}
	}
	int counts[2][10] = {};
	for (auto &f : orig) counts[0][f.cmd < 10 ? f.cmd : 0]++;
	for (auto &f : conv) counts[1][f.cmd < 10 ? f.cmd : 0]++;
	printf("%s: %d/%d packet frames, %d mismatches; usercmd %d/%d consolecmd %d/%d customdata %d/%d; %d warnings\n", path, (int)a.size(), (int)b.size(), mismatches, counts[0][dem_usercmd], counts[1][dem_usercmd], counts[0][dem_consolecmd], counts[1][dem_consolecmd], counts[0][dem_customdata], counts[1][dem_customdata], (int)warnings.size());
	for (size_t i = 0; i < warnings.size() && i < 5; ++i) printf("  warning: %s\n", warnings[i].c_str());
	return (mismatches || a.size() != b.size()) ? 1 : 0;
}

int main(int argc, char **argv) {
	if (argc >= 3 && !strcmp(argv[1], "info")) {
		std::string text;
		bool ok = Describe(argv[2], nullptr, text);
		fputs(text.c_str(), stdout);
		return ok ? 0 : 1;
	}
	if (argc >= 3 && !strcmp(argv[1], "convert")) {
		std::string in = argv[2];
		std::string base = argc >= 4 ? argv[3] : in.substr(0, in.size() >= 5 ? in.size() - 5 : in.size());
		ConvertResult res;
		if (!ConvertToDemos(in, base, res)) {
			fprintf(stderr, "error: %s\n", res.error.c_str());
			return 1;
		}
		for (auto &f : res.files) printf("wrote %s\n", f.c_str());
		for (auto &w : res.warnings) printf("warning: %s\n", w.c_str());
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "roundtrip")) {
		int fails = 0;
		for (int i = 2; i < argc; ++i) fails += Roundtrip(argv[i]);
		return fails ? 1 : 0;
	}
	fprintf(stderr,
	        "usage:\n"
	        "  sarproof info <file.sarp>\n"
	        "  sarproof convert <file.sarp> [outbase]\n"
	        "  sarproof roundtrip <file.dem>...\n");
	return 2;
}
