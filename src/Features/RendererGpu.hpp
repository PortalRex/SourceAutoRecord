#pragma once

// GPU capture path for the renderer (Windows, D3D9 - native or DXVK).
//
// Instead of a synchronous IVideoMode::ReadScreenPixels on the main thread,
// every capture is queued into the material system's call queue, so it runs
// on the render thread at exactly the point the read-back used to happen.
// There the back buffer is copied (and MSAA-resolved) on the GPU, frames are
// blended on the GPU when needed, converted to 8-bit YUV planes by pixel
// shaders, and read back through a ring of staging surfaces a few frames
// later, so neither the CPU nor the GPU waits for the other.

#include <vector>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace RendererGpu {
	struct Params {
		int width;
		int height;
		AVPixelFormat pixFmt;  // AV_PIX_FMT_YUV420P or AV_PIX_FMT_NV12
		int toBlend;
		std::vector<float> weights;  // normalized blend weights per subframe
		int slots;                   // read-back ring depth

		// Encode on the GPU with NVENC straight from D3D9 surfaces instead
		// of reading frames back (pixFmt is then unused)
		struct {
			bool enabled;
			bool hevc;
			int preset;  // 1-7 (P1 fastest ... P7 best)
			int cq;      // constant quality target, 1-51
			int gop;
			int fps;
			int bframes;
			int lookahead;  // frames, 0 = off
			int multipass;  // -1 = preset default, 0 = off, 1 = quarter res, 2 = full res
			int bitrate;    // bit/s: average bitrate instead of the cq target, 0 = constant quality
		} nvenc;
	};

	// Whether NVENC can encode directly from the game's D3D9 device
	bool NvencAvailable(const char **why);

	// Whether the GPU path can be used for this output (game/engine support,
	// D3D9 device located, format and size supported). Main thread.
	bool CanUse(int width, int height, AVPixelFormat pixFmt, const char **why);

	// Main thread. Queue session setup / one capture / the final flush.
	bool Begin(const Params &params);
	void Capture(int subframe, bool accumulate, bool first, bool emit);
	void Finish();

	// True once the render thread hit an error (resources, device reset...)
	bool Failed();
	// True while a session's queued work has not completed
	bool Busy();

	// Skip IDirect3DDevice9::Present (frames still get captured); returns
	// false if Present couldn't be hooked
	bool SetSkipPresent(bool skip);

	void Shutdown();
}  // namespace RendererGpu
