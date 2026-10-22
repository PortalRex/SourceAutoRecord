#pragma once

#include "Utils/SDK/ImageFormat.hpp"

namespace Renderer {
	void Frame();
	void Init(void **videomode);
	void Cleanup();
	bool IsRunning();
	// a co-op transition video started fading out (stop_transition_videos_fadeout)
	void OnTransitionVideoFadeout();
	// quit/exit while a render is still being written: stop it, finish the
	// file, then run `cmd`. Returns false if nothing is being rendered.
	bool DeferQuit(const char *cmd);
	void ReadScreenPixels(int x, int y, int w, int h, void *buf, ImageFormat fmt);
	extern int segmentEndTick;
	extern bool isDemoLoading;
};  // namespace Renderer
