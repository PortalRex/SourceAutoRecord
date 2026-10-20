#pragma once

// Shared between the renderer core (Renderer.cpp) and the GPU capture path
// (RendererGpu.cpp). Not part of the public Renderer interface.

#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace RendererCore {
	// Takes a free encoder input frame from the pool, blocking while the
	// encoder is behind. Returns nullptr if the render was aborted.
	AVFrame *AcquireVideoFrame();
	// Queues a filled frame for encoding. Frames are encoded in submit order.
	void SubmitVideoFrame(AVFrame *frame);
	// Gives an unused frame back to the pool.
	void ReturnVideoFrame(AVFrame *frame);
	// The video producer is done; the encoder finishes the file after this.
	void SubmitEnd();
	// Hardware encoder path (NVENC): the video is already compressed.
	// The header (codec extradata) must come before the first packet; the
	// output file header is written when it arrives. Packet timestamps are
	// in 1/fps units.
	void SubmitVideoHeader(const uint8_t *data, int size);
	void SubmitVideoPacket(AVPacket *pkt);

	// Something went wrong on a producer thread; the main thread stops the
	// render on its next frame.
	void ReportError(const char *msg);

	// Producer-side timing (microseconds), shown by the end-of-render stats
	void AddCaptureTime(int64_t us);
	void AddStallTime(int64_t us);

	int64_t NowMicros();
}  // namespace RendererCore
