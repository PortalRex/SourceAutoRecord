#pragma once

// Plugin load timing, printed to the console when the environment variable
// SAR_PROFILE_LOAD is set. Used to find what makes plugin_load slow.

#include <chrono>
#include <cstdlib>

namespace LoadProfile {
	inline bool Enabled() {
		static int enabled = -1;
		if (enabled < 0) enabled = std::getenv("SAR_PROFILE_LOAD") ? 1 : 0;
		return enabled == 1;
	}

	inline double NowMs() {
		using namespace std::chrono;
		return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
	}

	// Prints how long it has been since the previous Step() and returns now
	void Step(const char *what);
	void Begin();
}  // namespace LoadProfile
