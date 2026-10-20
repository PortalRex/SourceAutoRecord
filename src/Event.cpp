#include "Event.hpp"

#include "Utils/LoadProfile.hpp"

#ifdef _WIN32
extern "C" char __ImageBase;
#else
static char __ImageBase;
#endif

#include <map>

SarInitHandler::SarInitHandler(std::function<void()> cb)
	: cb(cb) {
		SarInitHandler::GetHandlers().push_back(this);
}

void SarInitHandler::RunAll() {
	for (auto h : SarInitHandler::GetHandlers()) {
		double t0 = LoadProfile::Enabled() ? LoadProfile::NowMs() : 0;
		h->cb();
		if (LoadProfile::Enabled() && LoadProfile::NowMs() - t0 > 5.0) {
			auto fn = h->cb.target<void (*)()>();
			char buf[96];
			snprintf(buf, sizeof buf, "init handler rva 0x%x", fn ? (unsigned)((uintptr_t)*fn - (uintptr_t)&__ImageBase) : 0);
			LoadProfile::Step(buf);
		}
	}
	SarInitHandler::GetHandlers().clear();
}

std::vector<SarInitHandler *> &SarInitHandler::GetHandlers() {
	static std::vector<SarInitHandler *> handlers;
	return handlers;
}
