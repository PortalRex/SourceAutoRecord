#include "ProofFile.hpp"

#include "Utils/ed25519/ed25519.h"
// <cstdint> already has the fixed-width types fixedint.h would typedef
#define FIXEDINT_H_INCLUDED
#include "Utils/ed25519/sha512.h"
#include "Utils/miniz/miniz.h"

#include <ctime>

using namespace SarProof;

static uint32_t Crc32(const uint8_t *p, size_t n) {
	return (uint32_t)mz_crc32(MZ_CRC32_INIT, p, n);
}

static void ChainHash(uint8_t *hash, const uint8_t *data, size_t n) {
	sha512_context ctx;
	sha512_init(&ctx);
	sha512_update(&ctx, hash, 64);
	sha512_update(&ctx, data, n);
	sha512_final(&ctx, hash);
}

static void PutU32(uint8_t *p, uint32_t v) {
	for (int i = 0; i < 4; ++i) p[i] = (v >> (i * 8)) & 0xFF;
}
static uint32_t GetU32(const uint8_t *p) {
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

Writer::~Writer() {
	if (this->fp) this->Close(false, nullptr);
}

bool Writer::Open(const std::string &path, std::string &error) {
	this->fp = fopen(path.c_str(), "wb");
	if (!this->fp) {
		error = "couldn't open " + path + " for writing";
		return false;
	}
	this->path = path;

	ByteWriter hdr;
	hdr.Bytes(FILE_MAGIC, 8);
	hdr.U32(FORMAT_VERSION);
	hdr.U32(FILE_HEADER_SIZE);
	hdr.U64((uint64_t)time(nullptr));
	while (hdr.Size() < FILE_HEADER_SIZE) hdr.U8(0);

	if (fwrite(hdr.buf.data(), 1, hdr.Size(), this->fp) != hdr.Size()) {
		error = "write failed";
		fclose(this->fp);
		this->fp = nullptr;
		return false;
	}
	fflush(this->fp);
	this->bytesWritten = hdr.Size();
	sha512(hdr.buf.data(), hdr.Size(), this->hash);

	this->stopping = false;
	this->thread = std::thread(&Writer::ThreadMain, this);
	return true;
}

void Writer::Submit(std::vector<uint8_t> &&records) {
	if (records.empty() || !this->fp) return;
	this->rawBytes += records.size();
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->queue.push_back(std::move(records));
	}
	this->cv.notify_one();
}

void Writer::ThreadMain() {
	while (true) {
		std::vector<uint8_t> raw;
		{
			std::unique_lock<std::mutex> lock(this->mutex);
			this->cv.wait(lock, [&] { return this->stopping || !this->queue.empty(); });
			if (this->queue.empty()) return;  // stopping and drained
			raw = std::move(this->queue.front());
			this->queue.pop_front();
		}
		this->WriteChunk(raw);
	}
}

void Writer::WriteChunk(const std::vector<uint8_t> &raw) {
	if (this->failed) return;

	std::vector<uint8_t> stored;
	uint32_t flags = 0;
	mz_ulong bound = mz_compressBound((mz_ulong)raw.size());
	stored.resize(bound);
	mz_ulong outLen = bound;
	if (mz_compress2(stored.data(), &outLen, raw.data(), (mz_ulong)raw.size(), 6) == MZ_OK && outLen < raw.size()) {
		stored.resize(outLen);
		flags |= CHUNK_FLAG_DEFLATE;
	} else {
		stored = raw;
	}

	uint8_t hdr[CHUNK_HEADER_SIZE];
	PutU32(hdr + 0, CHUNK_MAGIC);
	PutU32(hdr + 4, this->chunkIndex);
	PutU32(hdr + 8, (uint32_t)raw.size());
	PutU32(hdr + 12, (uint32_t)stored.size());
	PutU32(hdr + 16, flags);
	PutU32(hdr + 20, Crc32(stored.data(), stored.size()));

	if (fwrite(hdr, 1, sizeof hdr, this->fp) != sizeof hdr || fwrite(stored.data(), 1, stored.size(), this->fp) != stored.size()) {
		this->failed = true;
		return;
	}
	fflush(this->fp);
	ChainHash(this->hash, raw.data(), raw.size());
	++this->chunkIndex;
	this->bytesWritten += sizeof hdr + stored.size();
}

void Writer::Close(bool clean, const uint8_t *key) {
	if (!this->fp) return;
	if (clean) {
		ByteWriter end;
		end.U8(REC_END);
		end.Var(0);
		this->Submit(std::move(end.buf));
	}
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->stopping = true;
	}
	this->cv.notify_one();
	if (this->thread.joinable()) this->thread.join();

	if (clean && !this->failed) {
		uint8_t trailer[TRAILER_SIZE] = {};
		PutU32(trailer + 0, TRAILER_MAGIC);
		PutU32(trailer + 4, this->chunkIndex);
		memcpy(trailer + 8, this->hash, 64);
		bool haveKey = false;
		if (key) {
			for (int i = 0; i < 96; ++i) haveKey |= key[i] != 0;
		}
		if (haveKey) {
			ed25519_sign(trailer + 72, this->hash, 64, key, key + 32);
			memcpy(trailer + 136, key, 32);
		}
		if (fwrite(trailer, 1, sizeof trailer, this->fp) != sizeof trailer) this->failed = true;
		this->bytesWritten += sizeof trailer;
	}
	fclose(this->fp);
	this->fp = nullptr;
}

std::string SarProof::HashHex(const uint8_t *hash, int n) {
	static const char *hex = "0123456789abcdef";
	std::string s;
	for (int i = 0; i < n; ++i) {
		s.push_back(hex[hash[i] >> 4]);
		s.push_back(hex[hash[i] & 15]);
	}
	return s;
}

bool SarProof::ReadFile(const std::string &path, ReadResult &out, const uint8_t *expectedPubKey) {
	out = ReadResult();
	FILE *fp = fopen(path.c_str(), "rb");
	if (!fp) {
		out.error = "couldn't open " + path;
		return false;
	}
	std::vector<uint8_t> file;
	{
		fseek(fp, 0, SEEK_END);
		long size = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		if (size > 0) {
			file.resize((size_t)size);
			if (fread(file.data(), 1, file.size(), fp) != file.size()) file.clear();
		}
		fclose(fp);
	}
	if (file.size() < FILE_HEADER_SIZE || memcmp(file.data(), FILE_MAGIC, 8)) {
		out.error = "not a SAR proof file";
		return false;
	}
	out.version = GetU32(file.data() + 8);
	uint32_t headerSize = GetU32(file.data() + 12);
	if (out.version != FORMAT_VERSION || headerSize < FILE_HEADER_SIZE || headerSize > file.size()) {
		out.error = "unsupported proof file version " + std::to_string(out.version);
		return false;
	}
	out.createdTime = (uint64_t)GetU32(file.data() + 16) | ((uint64_t)GetU32(file.data() + 20) << 32);
	sha512(file.data(), headerSize, out.hash);
	out.ok = true;

	size_t pos = headerSize;
	bool trailerOk = false;
	while (pos + 4 <= file.size()) {
		uint32_t magic = GetU32(file.data() + pos);
		if (magic == TRAILER_MAGIC) {
			if (pos + TRAILER_SIZE > file.size()) {
				out.error = "truncated trailer";
				break;
			}
			const uint8_t *t = file.data() + pos;
			if (GetU32(t + 4) != out.chunks || memcmp(t + 8, out.hash, 64)) {
				out.error = "hash chain does not match the trailer (file was modified)";
				break;
			}
			trailerOk = true;
			bool anySig = false;
			for (int i = 0; i < 96; ++i) anySig |= t[72 + i] != 0;
			if (anySig) {
				out.signedByKey = ed25519_verify(t + 72, out.hash, 64, t + 136) == 1;
				out.keyMatches = out.signedByKey && expectedPubKey && !memcmp(expectedPubKey, t + 136, 32);
				if (!out.signedByKey) out.error = "bad signature";
			}
			pos += TRAILER_SIZE;
			break;
		}
		if (magic != CHUNK_MAGIC || pos + CHUNK_HEADER_SIZE > file.size()) {
			out.error = "bad chunk header at offset " + std::to_string(pos);
			break;
		}
		const uint8_t *h = file.data() + pos;
		uint32_t index = GetU32(h + 4), rawSize = GetU32(h + 8), storedSize = GetU32(h + 12), flags = GetU32(h + 16), crc = GetU32(h + 20);
		if (index != out.chunks || rawSize > MAX_CHUNK_RAW || pos + CHUNK_HEADER_SIZE + storedSize > file.size()) {
			out.error = "truncated or out-of-order chunk " + std::to_string(out.chunks);
			break;
		}
		const uint8_t *stored = h + CHUNK_HEADER_SIZE;
		if (Crc32(stored, storedSize) != crc) {
			out.error = "chunk " + std::to_string(index) + " is corrupt (CRC mismatch)";
			break;
		}
		size_t base = out.raw.size();
		out.raw.resize(base + rawSize);
		if (flags & CHUNK_FLAG_DEFLATE) {
			mz_ulong len = rawSize;
			if (mz_uncompress(out.raw.data() + base, &len, stored, storedSize) != MZ_OK || len != rawSize) {
				out.raw.resize(base);
				out.error = "chunk " + std::to_string(index) + " failed to decompress";
				break;
			}
		} else {
			if (storedSize != rawSize) {
				out.raw.resize(base);
				out.error = "chunk " + std::to_string(index) + " has a bad size";
				break;
			}
			memcpy(out.raw.data() + base, stored, rawSize);
		}
		ChainHash(out.hash, out.raw.data() + base, rawSize);
		++out.chunks;
		pos += CHUNK_HEADER_SIZE + storedSize;
	}

	ByteReader r(out.raw.data(), out.raw.size());
	bool truncated = false;
	while (!r.AtEnd()) {
		uint8_t type = r.U8();
		size_t len = (size_t)r.Var();
		const uint8_t *data = r.Bytes(len);
		if (r.bad) {
			truncated = true;
			break;
		}
		out.records.push_back({type, data, len});
	}

	bool sawEnd = !out.records.empty() && out.records.back().type == REC_END;
	out.complete = trailerOk && sawEnd && !truncated && out.error.empty();
	if (out.error.empty()) {
		if (truncated) out.error = "truncated record";
		else if (!trailerOk) out.error = "recording was not stopped cleanly (no trailer)";
		else if (!sawEnd) out.error = "trailer without end record";
	}
	return true;
}
