#pragma once

// Converts SAR proofs to ordinary Source demos (one .dem per segment), so they
// play back with the engine's demo player and work with every existing demo
// tool. No engine dependencies: also built into tools/sarproof.

#include "ProofFile.hpp"

#include <string>
#include <vector>

namespace SarProof {
	struct ConvertResult {
		std::string error;
		std::vector<std::string> files;
		std::vector<std::string> warnings;
	};

	struct DemoOut {
		std::string mapName;
		int ticks = 0;
		std::vector<uint8_t> bytes;
	};

	bool ConvertRecords(const ReadResult &proof, std::vector<DemoOut> &demos, std::vector<std::string> &warnings, std::string &error);
	// Writes <outBase>.dem, <outBase>_2.dem, ...
	bool ConvertToDemos(const std::string &proofPath, const std::string &outBase, ConvertResult &result);
	// Human-readable verification report
	bool Describe(const std::string &proofPath, const uint8_t *expectedPubKey, std::string &text);
}  // namespace SarProof
