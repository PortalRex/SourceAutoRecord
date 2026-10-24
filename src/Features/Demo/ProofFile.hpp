#pragma once

// Reading and writing .sarp containers (see ProofFormat.hpp for the layout).
// No engine dependencies: also built into tools/sarproof.

#include "ProofFormat.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace SarProof {
	// Appends chunks on a background thread so compression and disk writes
	// never run on the game thread.
	class Writer {
	public:
		~Writer();

		bool Open(const std::string &path, std::string &error);
		// Queue raw record bytes as one chunk. Never blocks on I/O.
		void Submit(std::vector<uint8_t> &&records);
		// Writes everything still queued, then the trailer (when clean).
		// key: 32-byte public key followed by the 64-byte private key, or null.
		void Close(bool clean, const uint8_t *key);

		bool IsOpen() const { return this->fp != nullptr; }
		bool Failed() const { return this->failed; }
		uint64_t BytesWritten() const { return this->bytesWritten; }
		uint64_t RawBytes() const { return this->rawBytes; }
		const std::string &Path() const { return this->path; }

	private:
		void ThreadMain();
		void WriteChunk(const std::vector<uint8_t> &raw);

		std::string path;
		FILE *fp = nullptr;
		std::thread thread;
		std::mutex mutex;
		std::condition_variable cv;
		std::deque<std::vector<uint8_t>> queue;
		bool stopping = false;
		std::atomic<bool> failed{false};
		std::atomic<uint64_t> bytesWritten{0};
		std::atomic<uint64_t> rawBytes{0};
		uint32_t chunkIndex = 0;
		uint8_t hash[64] = {};
	};

	struct Record {
		uint8_t type;
		const uint8_t *data;
		size_t size;
	};

	struct ReadResult {
		bool ok = false;           // header valid and at least the chunks read are sound
		bool complete = false;     // clean trailer present and hash chain matches it
		bool signedByKey = false;  // trailer carries a signature that verifies against its public key
		bool keyMatches = false;   // ... and that public key is the one this build expects
		uint32_t version = 0;
		uint64_t createdTime = 0;
		uint32_t chunks = 0;
		std::string error;         // first problem found, if any
		uint8_t hash[64] = {};
		std::vector<uint8_t> raw;  // concatenated raw chunk contents
		std::vector<Record> records;
	};

	// expectedPubKey: 32 bytes or null
	bool ReadFile(const std::string &path, ReadResult &out, const uint8_t *expectedPubKey = nullptr);
	std::string HashHex(const uint8_t *hash, int n = 64);
}  // namespace SarProof
