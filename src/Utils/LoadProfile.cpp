#include "LoadProfile.hpp"

#include "Modules/Console.hpp"

#include <string>
#include <vector>

namespace {
	double g_start, g_last;
	std::vector<std::string> g_lines;  // printed once the console is up
}

void LoadProfile::Begin() {
	if (!Enabled()) return;
	g_start = g_last = NowMs();
}

void LoadProfile::Step(const char *what) {
	if (!Enabled()) return;
	double now = NowMs();
	char buf[256];
	snprintf(buf, sizeof buf, "[load] %8.1f ms  +%7.1f ms  %s\n", now - g_start, now - g_last, what);
	g_last = now;
	if (console) {
		for (auto &l : g_lines) console->Print("%s", l.c_str());
		g_lines.clear();
		console->Print("%s", buf);
	} else {
		g_lines.push_back(buf);
	}
}
