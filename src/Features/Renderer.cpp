#include "Renderer.hpp"

#include "Command.hpp"
#include "Event.hpp"
#include "Hook.hpp"
#include "Modules/Engine.hpp"
#include "Modules/FileSystem.hpp"
#include "Modules/Server.hpp"
#include "Features/RendererGpu.hpp"
#include "Features/RendererInternal.hpp"
#include "Features/Session.hpp"
#include "Utils/SDK.hpp"
#include "Utils/Math.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
};

// Stuff pulled from the engine
static void **g_videomode;
static int *g_snd_linear_count;
static int **g_snd_p;
static int *g_snd_vol;
static MovieInfo_t *g_movieInfo;

// The demoplayer tick this segment ends on (we'll stop recording at
// this tick if sar_render_autostop is set)
int Renderer::segmentEndTick = -1;

// Whether a demo is loading; used to detect whether we should autostart
bool Renderer::isDemoLoading = false;

// We reference this so we can make sure the output is stereo.
// Supporting other output options is quite tricky; because Source
// movies only support stereo output, the functions for outputting
// surround audio don't have the necessary code for writing it to
// buffers like we need. This is *possible* to get around, but would
// require some horrible mid-function detours.
static Variable snd_surround_speakers;

// h264, hevc, vp8, vp9, dnxhd
// aac, ac3, vorbis, opus, flac
static Variable sar_render_vbitrate("sar_render_vbitrate", "40000", 1, "Video bitrate used in renders (kbit/s)\n");
static Variable sar_render_abitrate("sar_render_abitrate", "160", 1, "Audio bitrate used in renders (kbit/s)\n");
static Variable sar_render_vcodec("sar_render_vcodec", "h264", "Video codec used in renders (h264, hevc, vp8, vp9, dnxhd)\n", 0);
static Variable sar_render_acodec("sar_render_acodec", "aac", "Audio codec used in renders (aac, ac3, vorbis, opus, flac)\n", 0);
static Variable sar_render_quality("sar_render_quality", "35", 0, 50, "Render output quality, higher is better (50=lossless)\n");
static Variable sar_render_fps("sar_render_fps", "60", 1, "Render output FPS\n");
static Variable sar_render_sample_rate("sar_render_sample_rate", "44100", 1000, "Audio output sample rate\n");
static Variable sar_render_blend("sar_render_blend", "0", 0, "How many frames to blend for each output frame; 1 = do not blend, 0 = automatically determine based on host_framerate\n");
static Variable sar_render_blend_mode("sar_render_blend_mode", "1", 0, 1, "What type of frameblending to use. 0 = linear, 1 = Gaussian\n");
static Variable sar_render_autostart("sar_render_autostart", "0", "Whether to automatically start when demo playback begins\n");
static Variable sar_render_autostart_extension("sar_render_autostart_extension", "mp4", "The file extension (and hence container format) to use for automatically started renders.\n", 0);
static Variable sar_render_autostop("sar_render_autostop", "1", "Whether to automatically stop when `__END__` is seen in demo playback\n");
static Variable sar_render_shutter_angle("sar_render_shutter_angle", "360", 30, 360, "The shutter angle to use for rendering in degrees.\n");
static Variable sar_render_merge("sar_render_merge", "0", "When set, merge all the renders until sar_render_finish is entered\n");
static Variable sar_render_skip_coop_videos("sar_render_skip_coop_videos", "1", "When set, don't include coop loading time in renders\n");
static Variable sar_render_preset("sar_render_preset", "fast", "Encoder speed preset for x264/x265 (ultrafast, superfast, veryfast, faster, fast, medium, slow, slower, veryslow) and NVENC (mapped to P1-P7, or p1-p7 directly). Faster presets render faster but make larger files at the same quality.\n", 0);
static Variable sar_render_vencoder("sar_render_vencoder", "auto", "Video encoder implementation. auto = NVIDIA hardware encoding straight from the GPU when available (h264/hevc, native D3D9), otherwise the default encoder for sar_render_vcodec; nvenc = same but warn when unavailable; or an FFmpeg encoder name such as libx264. Empty = the default encoder.\n", 0);
static Variable sar_render_gop("sar_render_gop", "0", 0, "Keyframe interval in frames; 0 = 4 seconds worth of frames\n");
static Variable sar_render_gpu("sar_render_gpu", "1", 0, 1, "Capture, blend and color-convert frames on the GPU when possible (Windows, D3D9). 0 = always use the CPU read-back path.\n");
static Variable sar_render_queue("sar_render_queue", "8", 2, 64, "How many finished frames can wait for the encoder before the game waits for it\n");
static Variable sar_render_threads("sar_render_threads", "0", 0, 16, "Threads used for CPU color conversion; 0 = automatic\n");
static Variable sar_render_nvenc_lookahead("sar_render_nvenc_lookahead", "0", 0, 32, "NVENC look-ahead depth in frames (0 = off)\n");
static Variable sar_render_nvenc_bframes("sar_render_nvenc_bframes", "0", 0, 4, "NVENC B-frames. They make files a few percent smaller but slow renders down a lot (the GPU stalls on them)\n");
static Variable sar_render_nvenc_multipass("sar_render_nvenc_multipass", "-1", -1, 2, "NVENC multi-pass: -1 = preset default, 0 = off, 1 = quarter resolution, 2 = full resolution\n");
static Variable sar_render_display("sar_render_display", "0", 0, 1, "Show the frames in the game window while rendering. Off is much faster: the game window freezes until the render finishes.\n");
static Variable sar_render_stats("sar_render_stats", "1", 0, 1, "Print timing statistics when a render finishes\n");
static Variable sar_render_virtual_clock("sar_render_virtual_clock", "1", 0, 1, "While rendering, make the game's wall clock advance one frame per rendered frame, so HUD elements timed on real time (the challenge mode timer) update as in real-time playback\n");

// g_videomode VMT wrappers {{{

static inline int GetScreenWidth() {
	return Memory::VMT<int(__rescall *)(void *)>(*g_videomode, Offsets::GetModeWidth)(*g_videomode);
}

static inline int GetScreenHeight() {
	return Memory::VMT<int(__rescall *)(void *)>(*g_videomode, Offsets::GetModeHeight)(*g_videomode);
}

void Renderer::ReadScreenPixels(int x, int y, int w, int h, void *buf, ImageFormat fmt) {
	return Memory::VMT<void(__rescall *)(void *, int, int, int, int, void *, ImageFormat)>(*g_videomode, Offsets::ReadScreenPixels)(*g_videomode, x, y, w, h, buf, fmt);
}

// }}}

// Small thread-safe containers {{{

int64_t RendererCore::NowMicros() {
	using namespace std::chrono;
	return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// A pool of reusable objects; Acquire blocks until one is free
template <typename T>
class BlockingPool {
public:
	void Reset() {
		std::lock_guard<std::mutex> lock(m);
		items.clear();
		aborted = false;
	}
	void Put(T item) {
		{
			std::lock_guard<std::mutex> lock(m);
			items.push_back(item);
		}
		cv.notify_one();
	}
	// Returns false if aborted
	bool Take(T &out, int64_t *stallUs = nullptr) {
		std::unique_lock<std::mutex> lock(m);
		if (items.empty()) {
			int64_t t0 = RendererCore::NowMicros();
			cv.wait(lock, [&] { return !items.empty() || aborted; });
			if (stallUs) *stallUs += RendererCore::NowMicros() - t0;
		}
		if (items.empty()) return false;
		out = items.back();
		items.pop_back();
		return true;
	}
	void Abort() {
		{
			std::lock_guard<std::mutex> lock(m);
			aborted = true;
		}
		cv.notify_all();
	}
	std::vector<T> Drain() {
		std::lock_guard<std::mutex> lock(m);
		std::vector<T> out;
		out.swap(items);
		return out;
	}

private:
	std::mutex m;
	std::condition_variable cv;
	std::vector<T> items;
	bool aborted = false;
};

// An unbounded FIFO; Pop blocks until there is something
template <typename T>
class BlockingQueue {
public:
	void Push(T item) {
		{
			std::lock_guard<std::mutex> lock(m);
			q.push_back(item);
		}
		cv.notify_one();
	}
	T Pop(int64_t *idleUs = nullptr) {
		std::unique_lock<std::mutex> lock(m);
		if (q.empty()) {
			int64_t t0 = RendererCore::NowMicros();
			cv.wait(lock, [&] { return !q.empty(); });
			if (idleUs) *idleUs += RendererCore::NowMicros() - t0;
		}
		T item = q.front();
		q.pop_front();
		return item;
	}

private:
	std::mutex m;
	std::condition_variable cv;
	std::deque<T> q;
};

// }}}

struct Stream {
	AVStream *stream;
	AVCodecContext *enc;
	AVCodec *codec;
	SwrContext *swrCtx;
	int64_t nextPts;
};

// Work for the encoder thread, in submission order
struct EncItem {
	enum Type { VIDEO, AUDIO, END, PACKET, HEADER } type;
	AVFrame *frame;
	AVPacket *pkt = nullptr;  // PACKET: compressed video; HEADER: extradata
};

// Work for the CPU conversion thread (read-back path only)
struct ConvItem {
	enum Type { FRAME, END } type;
	uint8_t *pixels;  // BGRX, width * height * 4
	bool accumulate;
	bool emit;
	int subframe;
};

// Splits RGB -> YUV conversion of one frame into horizontal bands that are
// converted in parallel, each with its own swscale context.
class BandConverter {
public:
	bool Init(int w, int h, AVPixelFormat src, AVPixelFormat dst, int bands) {
		this->w = w;
		this->h = h;
		const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(dst);
		vshift = desc ? desc->log2_chroma_h : 1;
		nplanes = av_pix_fmt_count_planes(dst);
		int align = 2;
		bands = std::max(1, std::min(bands, h / 16));
		for (int i = 0; i < bands; ++i) {
			int y0 = (h * i / bands) & ~(align - 1);
			int y1 = i == bands - 1 ? h : ((h * (i + 1) / bands) & ~(align - 1));
			bandY.push_back(y0);
			bandH.push_back(y1 - y0);
			SwsContext *ctx = sws_getContext(w, y1 - y0, src, w, y1 - y0, dst, SWS_BILINEAR, NULL, NULL, NULL);
			if (!ctx) return false;
			// RGB is full range; output BT.709 limited range (tagged on the stream)
			const int *coeffs = sws_getCoefficients(SWS_CS_ITU709);
			sws_setColorspaceDetails(ctx, coeffs, 1, coeffs, 0, 0, 1 << 16, 1 << 16);
			ctxs.push_back(ctx);
		}
		for (size_t i = 1; i < ctxs.size(); ++i) {
			threads.emplace_back(&BandConverter::Worker, this, (int)i);
		}
		return true;
	}

	void Destroy() {
		{
			std::lock_guard<std::mutex> lock(m);
			quit = true;
		}
		cvStart.notify_all();
		for (auto &t : threads) t.join();
		threads.clear();
		for (auto c : ctxs) sws_freeContext(c);
		ctxs.clear();
		bandY.clear();
		bandH.clear();
		quit = false;
	}

	void Convert(const uint8_t *src, int srcStride, AVFrame *dst) {
		this->src = src;
		this->srcStride = srcStride;
		this->dst = dst;
		{
			std::lock_guard<std::mutex> lock(m);
			pending = (int)ctxs.size() - 1;
			++generation;
		}
		cvStart.notify_all();
		Run(0);
		std::unique_lock<std::mutex> lock(m);
		cvDone.wait(lock, [&] { return pending == 0; });
	}

private:
	void Run(int i) {
		const uint8_t *srcSlice[1] = {src + (size_t)bandY[i] * srcStride};
		int srcLines[1] = {srcStride};
		uint8_t *dstSlice[4] = {};
		int dstLines[4] = {};
		for (int p = 0; p < nplanes; ++p) {
			int y = p == 0 ? bandY[i] : (bandY[i] >> vshift);
			dstSlice[p] = dst->data[p] + (size_t)y * dst->linesize[p];
			dstLines[p] = dst->linesize[p];
		}
		sws_scale(ctxs[i], srcSlice, srcLines, 0, bandH[i], dstSlice, dstLines);
	}

	void Worker(int i) {
		int seen = 0;
		while (true) {
			std::unique_lock<std::mutex> lock(m);
			cvStart.wait(lock, [&] { return quit || generation != seen; });
			if (quit) return;
			seen = generation;
			lock.unlock();
			Run(i);
			lock.lock();
			if (--pending == 0) cvDone.notify_one();
		}
	}

	int w = 0, h = 0, vshift = 1, nplanes = 3;
	std::vector<SwsContext *> ctxs;
	std::vector<int> bandY, bandH;
	std::vector<std::thread> threads;
	std::mutex m;
	std::condition_variable cvStart, cvDone;
	int generation = 0, pending = 0;
	bool quit = false;
	const uint8_t *src = nullptr;
	int srcStride = 0;
	AVFrame *dst = nullptr;
};

// The global renderer state
static struct
{
	// isRendering: frames/audio are being captured. sessionActive: an output
	// file is open (it stays set until the encoder thread has written it).
	std::atomic<bool> isRendering;
	std::atomic<bool> isPaused;
	std::atomic<bool> sessionActive;
	std::atomic<bool> failed;
	std::atomic<bool> stopError;
	std::atomic<bool> outputClosed;  // the encoder thread has finished the file
	bool useGpu;

	int fps;
	int samplerate;
	int channels;
	Stream videoStream;
	Stream audioStream;
	std::string filename;
	AVFormatContext *outCtx;
	int width, height;
	AVPixelFormat pixFmt;

	// NVENC: video arrives compressed; the file header waits for its
	// extradata, and audio waits for the header
	bool nvenc;
	bool headerWritten;
	std::vector<AVFrame *> pendingAudio;  // encoder thread only
	std::string encoderName;
	std::string deferredQuit;  // main thread
	float savedFpsMax;         // restored when the render stops

	// Audio: the encoder needs frame_size samples per frame, so we collect
	// samples from the game until we have a full frame
	std::mutex audioLock;
	AVFrame *audioFrame;  // S16P, being filled
	int audioFill;
	int64_t audioPts;

	// Blending
	int toBlend;
	int toBlendStart;  // Inclusive
	int toBlendEnd;    // Exclusive
	int nextBlendIdx;  // Main thread: which subframe of the current output frame is next
	std::vector<float> blendWeights;     // normalized, per subframe
	std::vector<uint32_t> blendWeights16;  // same, fixed point (sum = 65536)

	// Video frames: pool of encoder input frames
	BlockingPool<AVFrame *> framePool;
	std::vector<AVFrame *> allFrames;
	int64_t videoPts;  // assigned at submit (producer order)

	// Encoder thread
	std::thread encoder;
	BlockingQueue<EncItem> encQueue;

	// CPU read-back path: main thread reads into raw buffers, conversion
	// thread blends + converts them
	std::thread converter;
	BlockingQueue<ConvItem> convQueue;
	BlockingPool<uint8_t *> rawPool;
	std::vector<uint8_t *> allRaw;
	std::vector<uint32_t> blendSum;  // conversion thread only
	uint8_t *blendOut;
	BandConverter bands;

	// Stats (microseconds)
	int64_t startTime;
	std::atomic<int64_t> captureUs;    // main/render thread capture work
	std::atomic<int64_t> stallUs;      // producers waiting for a free frame
	std::atomic<int64_t> convertUs;    // CPU conversion
	std::atomic<int64_t> encodeUs;     // encoder thread busy
	std::atomic<int64_t> encodeIdleUs; // encoder thread waiting for work
	std::atomic<int> framesCaptured;
	std::string errorMsg;
	std::mutex errorLock;
} g_render;

void RendererCore::AddCaptureTime(int64_t us) {
	g_render.captureUs += us;
}
void RendererCore::AddStallTime(int64_t us) {
	g_render.stallUs += us;
}

void RendererCore::ReportError(const char *msg) {
	{
		std::lock_guard<std::mutex> lock(g_render.errorLock);
		if (g_render.errorMsg.empty()) g_render.errorMsg = msg;
	}
	g_render.failed.store(true);
}

AVFrame *RendererCore::AcquireVideoFrame() {
	AVFrame *f = nullptr;
	int64_t stall = 0;
	if (!g_render.framePool.Take(f, &stall)) return nullptr;
	g_render.stallUs += stall;
	// The encoder may still hold a reference to this frame's buffers
	if (av_frame_make_writable(f) < 0) {
		g_render.framePool.Put(f);
		ReportError("Failed to make video frame writable");
		return nullptr;
	}
	return f;
}

void RendererCore::SubmitVideoFrame(AVFrame *frame) {
	frame->pts = g_render.videoPts++;
	++g_render.framesCaptured;
	g_render.encQueue.Push({EncItem::VIDEO, frame});
}

void RendererCore::ReturnVideoFrame(AVFrame *frame) {
	g_render.framePool.Put(frame);
}

void RendererCore::SubmitEnd() {
	g_render.encQueue.Push({EncItem::END, nullptr});
}

void RendererCore::SubmitVideoHeader(const uint8_t *data, int size) {
	AVPacket *pkt = av_packet_alloc();
	if (!pkt || av_new_packet(pkt, size) < 0) {
		av_packet_free(&pkt);
		ReportError("Out of memory");
		return;
	}
	memcpy(pkt->data, data, size);
	g_render.encQueue.Push({EncItem::HEADER, nullptr, pkt});
}

void RendererCore::SubmitVideoPacket(AVPacket *pkt) {
	++g_render.framesCaptured;
	g_render.encQueue.Push({EncItem::PACKET, nullptr, pkt});
}

static void stopRender(bool error);

ON_EVENT(SAR_UNLOAD) {
	// Quitting mid-render: stop capturing and give the encoder a few
	// seconds to write the rest of the file (the GPU path also needs the
	// render thread to run its final flush, which may not happen any more)
	if (g_render.isRendering.load()) stopRender(false);
	if (g_render.sessionActive.load()) {
		for (int i = 0; i < 500 && !g_render.outputClosed.load(); ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
	if (g_render.encoder.joinable()) g_render.encoder.detach();
	if (g_render.converter.joinable()) g_render.converter.detach();
	RendererGpu::Shutdown();
}

// Utilities {{{

static AVCodecID videoCodecFromName(const char *name) {
	if (!strcmp(name, "h264")) return AV_CODEC_ID_H264;
	if (!strcmp(name, "hevc")) return AV_CODEC_ID_HEVC;
	if (!strcmp(name, "vp8")) return AV_CODEC_ID_VP8;
	if (!strcmp(name, "vp9")) return AV_CODEC_ID_VP9;
	if (!strcmp(name, "dnxhd")) return AV_CODEC_ID_DNXHD;
	return AV_CODEC_ID_NONE;
}

static AVCodecID audioCodecFromName(const char *name) {
	if (!strcmp(name, "aac")) return AV_CODEC_ID_AAC;
	if (!strcmp(name, "ac3")) return AV_CODEC_ID_AC3;
	if (!strcmp(name, "vorbis")) return AV_CODEC_ID_VORBIS;
	if (!strcmp(name, "opus")) return AV_CODEC_ID_OPUS;
	if (!strcmp(name, "flac")) return AV_CODEC_ID_FLAC;
	return AV_CODEC_ID_NONE;
}

static double calcFrameWeight(int mode, double position) {
	if (mode == 1) {
		// Gaussian
		const double mean = 0.0;
		const double stdDev = 1.0;
		const double variance = stdDev * stdDev;
		double x = position * 2.0 - 1.0;  // rescale to [-1,1]
		return 1.0 / (stdDev * sqrt(M_PI)) * exp(-(((x - mean) * (x - mean)) / (2 * variance)));
	}
	// Linear
	return 1.0;
}

// Fills g_render.blendWeights / blendWeights16 for the current blend settings
static void computeBlendWeights() {
	int mode = sar_render_blend_mode.GetInt();
	int n = g_render.toBlendEnd - g_render.toBlendStart;
	g_render.blendWeights.assign(g_render.toBlend, 0.0f);
	g_render.blendWeights16.assign(g_render.toBlend, 0);

	std::vector<double> w(g_render.toBlend, 0.0);
	double total = 0.0;
	for (int i = g_render.toBlendStart; i < g_render.toBlendEnd; ++i) {
		double pos = n > 1 ? (double)(i - g_render.toBlendStart) / (n - 1) : 0.5;
		w[i] = calcFrameWeight(mode, pos);
		total += w[i];
	}

	uint32_t sum16 = 0;
	int last = g_render.toBlendEnd - 1;
	for (int i = g_render.toBlendStart; i < g_render.toBlendEnd; ++i) {
		g_render.blendWeights[i] = (float)(w[i] / total);
		if (i != last) {
			g_render.blendWeights16[i] = (uint32_t)(w[i] / total * 65536.0 + 0.5);
			sum16 += g_render.blendWeights16[i];
		}
	}
	g_render.blendWeights16[last] = 65536 - sum16;
}

// }}}

// Movie command hooks {{{

// We want to stop use of the normal movie system while a SAR render is
// active. This is because we exploit g_movieInfo to make the game think
// it's recording a movie, so if both were running at the same time,
// they'd interact in weird and undefined ways.

static _CommandCallback startmovie_origCbk;
static _CommandCallback endmovie_origCbk;
static void startmovie_cbk(const CCommand &args) {
	if (g_render.isRendering.load()) {
		console->Print("Cannot startmovie while a SAR render is active! Stop the render with sar_render_finish.\n");
		return;
	}
	startmovie_origCbk(args);
}
static void endmovie_cbk(const CCommand &args) {
	if (g_render.isRendering.load()) {
		console->Print("Cannot endmovie while a SAR render is active! Did you mean sar_render_finish?\n");
		return;
	}
	endmovie_origCbk(args);
}

// }}}

// Stream creation and destruction {{{

static AVCodec *findVideoEncoder(AVCodecID codecId) {
	const char *name = sar_render_vencoder.GetString();
	if (name && *name) {
		AVCodec *c = avcodec_find_encoder_by_name(name);
		if (!c || c->type != AVMEDIA_TYPE_VIDEO) {
			console->Print("Unknown video encoder '%s'; using the default for %s\n", name, avcodec_get_name(codecId));
		} else {
			return c;
		}
	}
	return avcodec_find_encoder(codecId);
}

static AVPixelFormat chooseVideoPixFmt(AVCodec *codec) {
	if (codec->id == AV_CODEC_ID_DNXHD) return AV_PIX_FMT_YUV422P;
	if (!codec->pix_fmts) return AV_PIX_FMT_YUV420P;
	for (const AVPixelFormat *p = codec->pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
		if (*p == AV_PIX_FMT_YUV420P) return AV_PIX_FMT_YUV420P;
	}
	for (const AVPixelFormat *p = codec->pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
		if (*p == AV_PIX_FMT_NV12) return AV_PIX_FMT_NV12;
	}
	return codec->pix_fmts[0];
}

// x264-style preset name -> NVENC P1..P7
static int nvencPresetFromName(const char *name) {
	static const struct {
		const char *name;
		int p;
	} map[] = {{"ultrafast", 1}, {"superfast", 2}, {"veryfast", 2}, {"faster", 3}, {"fast", 3}, {"medium", 4}, {"slow", 5}, {"slower", 6}, {"veryslow", 7}};
	for (auto &m : map)
		if (!strcmp(name, m.name)) return m.p;
	if (name[0] == 'p' || name[0] == 'P') {
		int p = atoi(name + 1);
		if (p >= 1 && p <= 7) return p;
	}
	return 3;
}

// framerate is sample rate for audio streams
static bool addStream(Stream *out, AVFormatContext *outputCtx, AVCodec *codec, int64_t bitrate, int framerate, int64_t ptsOff, int width = 0, int height = 0) {
	out->codec = codec;
	AVCodecID codecId = codec->id;

	// dnxhd bitrate selection {{{

	if (codecId == AV_CODEC_ID_DNXHD) {
		// dnxhd is fussy and won't just allow any bitrate or
		// resolution, so check our resolution is supported and find the
		// closest bitrate to what was requested

		// rates here are in Mbps
		int64_t *rates;
		size_t nrates;

		if (width == 1920 && height == 1080) {  // 1080p 16:9
			static int64_t rates1080[] = {36, 45, 75, 90, 115, 120, 145, 175, 180, 190, 220, 240, 365, 440};
			rates = rates1080;
			nrates = sizeof rates1080 / sizeof rates1080[0];
		} else if (width == 1280 && height == 720) {  // 720p 16:9
			static int64_t rates720[] = {60, 75, 90, 110, 120, 145, 180, 220};
			rates = rates720;
			nrates = sizeof rates720 / sizeof rates720[0];
		} else if (width == 1440 && height == 1080) {  // 1080p 4:3
			static int64_t rates1080[] = {63, 84, 100, 110};
			rates = rates1080;
			nrates = sizeof rates1080 / sizeof rates1080[0];
		} else if (width == 960 && height == 720) {  // 720p 4:3
			static int64_t rates720[] = {42, 60, 75, 115};
			rates = rates720;
			nrates = sizeof rates720 / sizeof rates720[0];
		} else {
			console->Print("Resolution not supported by dnxhd\n");
			return false;
		}

		int64_t realBitrate = -1;
		int64_t lastDelta = INT64_MAX;

		for (size_t i = 0; i < nrates; ++i) {
			int64_t rate = rates[i] * 1000000;
			int64_t delta = rate > bitrate ? rate - bitrate : bitrate - rate;
			if (delta < lastDelta) {
				realBitrate = rate;
				lastDelta = delta;
			}
		}

		if (realBitrate != bitrate) {
			console->Print("dnxhd does not support the given bitrate; encoding at %d kb/s instead\n", (int)(realBitrate / 1000));
			bitrate = realBitrate;
		}
	}

	// }}}

	out->stream = avformat_new_stream(outputCtx, NULL);
	if (!out->stream) {
		console->Print("Failed to allocate stream\n");
		return false;
	}

	out->enc = avcodec_alloc_context3(codec);
	if (!out->enc) {
		console->Print("Failed to allocate an encoding context\n");
		return false;
	}

	out->enc->bit_rate = bitrate;
	out->stream->time_base = {1, framerate};

	switch (codec->type) {
	case AVMEDIA_TYPE_AUDIO:
		out->enc->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;  // don't really care about sample format
		out->enc->sample_rate = framerate;
		out->enc->channel_layout = AV_CH_LAYOUT_STEREO;
		out->enc->channels = av_get_channel_layout_nb_channels(out->enc->channel_layout);

		if (codec->channel_layouts) {
			bool foundStereo = false;
			for (const uint64_t *layout = codec->channel_layouts; *layout; ++layout) {
				if (*layout == AV_CH_LAYOUT_STEREO) {
					foundStereo = true;
					break;
				}
			}

			if (!foundStereo) {
				console->Print("Stereo not supported by audio encoder\n");
				avcodec_free_context(&out->enc);
				return false;
			}
		}

		if (codec->supported_samplerates) {
			bool foundRate = false;
			for (const int *rate = codec->supported_samplerates; *rate; ++rate) {
				if (*rate == framerate) {
					foundRate = true;
					break;
				}
			}

			if (!foundRate) {
				console->Print("Sample rate %d not supported by audio encoder\n", framerate);
				avcodec_free_context(&out->enc);
				return false;
			}
		}

		break;

	case AVMEDIA_TYPE_VIDEO: {
		out->enc->codec_id = codecId;
		out->enc->width = width;
		out->enc->height = height;
		out->enc->time_base = out->stream->time_base;
		out->enc->framerate = {framerate, 1};
		int gop = sar_render_gop.GetInt();
		out->enc->gop_size = gop > 0 ? gop : framerate * 4;
		out->enc->pix_fmt = chooseVideoPixFmt(codec);
		out->enc->sample_aspect_ratio = {1, 1};
		// Frames are converted with BT.709 coefficients, limited range
		out->enc->color_primaries = AVCOL_PRI_BT709;
		out->enc->color_trc = AVCOL_TRC_BT709;
		out->enc->colorspace = AVCOL_SPC_BT709;
		out->enc->color_range = AVCOL_RANGE_MPEG;
		break;
	}

	default:
		break;
	}

	if (outputCtx->oformat->flags & AVFMT_GLOBALHEADER) {
		out->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	out->nextPts = ptsOff;

	return true;
}

static void closeStream(Stream *s) {
	avcodec_free_context(&s->enc);
	swr_free(&s->swrCtx);
}

// }}}

// Flushing streams {{{

static bool flushStream(Stream *s, bool isEnd = false) {
	if (!s->enc) return true;
	if (isEnd) {
		avcodec_send_frame(s->enc, NULL);
	}

	while (true) {
		AVPacket pkt = {0};
		int ret = avcodec_receive_packet(s->enc, &pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			return true;
		} else if (ret < 0) {
			return false;
		}
		av_packet_rescale_ts(&pkt, s->enc->time_base, s->stream->time_base);
		pkt.stream_index = s->stream->index;
		av_interleaved_write_frame(g_render.outCtx, &pkt);
		av_packet_unref(&pkt);
	}
}

// }}}

// Opening streams {{{

static bool openVideo(Stream *s, AVDictionary **options) {
	if (avcodec_open2(s->enc, s->codec, options) < 0) {
		console->Print("Failed to open video codec\n");
		return false;
	}

	if (avcodec_parameters_from_context(s->stream->codecpar, s->enc) < 0) {
		console->Print("Failed to copy stream parameters\n");
		return false;
	}

	return true;
}

static bool openAudio(Stream *s, AVDictionary **options) {
	if (avcodec_open2(s->enc, s->codec, options) < 0) {
		console->Print("Failed to open audio codec\n");
		return false;
	}

	if (avcodec_parameters_from_context(s->stream->codecpar, s->enc) < 0) {
		console->Print("Failed to copy stream parameters\n");
		return false;
	}

	s->swrCtx = swr_alloc();
	if (!s->swrCtx) {
		console->Print("Failed to allocate resampler context\n");
		return false;
	}

	av_opt_set_int(s->swrCtx, "in_channel_count", s->enc->channels, 0);
	av_opt_set_int(s->swrCtx, "in_sample_rate", 44100, 0);
	av_opt_set_sample_fmt(s->swrCtx, "in_sample_fmt", AV_SAMPLE_FMT_S16P, 0);
	av_opt_set_int(s->swrCtx, "out_channel_count", s->enc->channels, 0);
	av_opt_set_int(s->swrCtx, "out_sample_rate", s->enc->sample_rate, 0);
	av_opt_set_sample_fmt(s->swrCtx, "out_sample_fmt", s->enc->sample_fmt, 0);

	if (swr_init(s->swrCtx) < 0) {
		console->Print("Failed to initialize resampler context\n");
		return false;
	}

	return true;
}

static AVFrame *allocAudioFrame(AVSampleFormat sampleFmt, uint64_t channelLayout, int sampleRate, int nbSamples) {
	AVFrame *frame = av_frame_alloc();
	if (!frame) return NULL;
	frame->format = sampleFmt;
	frame->channel_layout = channelLayout;
	frame->sample_rate = sampleRate;
	frame->nb_samples = nbSamples;
	if (nbSamples && av_frame_get_buffer(frame, 0) < 0) {
		av_frame_free(&frame);
		return NULL;
	}
	return frame;
}

// }}}

// Encoder thread {{{

static bool encodeVideo(AVFrame *frame) {
	if (avcodec_send_frame(g_render.videoStream.enc, frame) < 0) {
		RendererCore::ReportError("Failed to send video frame for encoding!");
		return false;
	}
	if (!flushStream(&g_render.videoStream)) {
		RendererCore::ReportError("Failed to flush video stream!");
		return false;
	}
	g_render.videoStream.nextPts = frame->pts + 1;
	return true;
}

static bool encodeAudio(AVFrame *in) {
	Stream *s = &g_render.audioStream;
	AVFrame *out = allocAudioFrame(s->enc->sample_fmt, s->enc->channel_layout, s->enc->sample_rate, s->enc->frame_size ? s->enc->frame_size : in->nb_samples);
	if (!out) {
		RendererCore::ReportError("Failed to allocate audio frame!");
		return false;
	}
	bool ok = true;
	if (swr_convert(s->swrCtx, out->data, out->nb_samples, (const uint8_t **)in->data, in->nb_samples) < 0) {
		RendererCore::ReportError("Failed to resample audio frame!");
		ok = false;
	}
	if (ok) {
		out->pts = av_rescale_q(in->pts, {1, s->enc->sample_rate}, s->enc->time_base);
		if (avcodec_send_frame(s->enc, out) < 0) {
			RendererCore::ReportError("Failed to send audio frame for encoding!");
			ok = false;
		} else if (!flushStream(s)) {
			RendererCore::ReportError("Failed to flush audio stream!");
			ok = false;
		}
	}
	av_frame_free(&out);
	return ok;
}

static void finishOutput();

// NVENC: the extradata is known now, so the file header can be written
static bool writeDeferredHeader(AVPacket *hdr) {
	AVCodecParameters *par = g_render.videoStream.stream->codecpar;
	par->extradata = (uint8_t *)av_mallocz(hdr->size + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!par->extradata) return false;
	memcpy(par->extradata, hdr->data, hdr->size);
	par->extradata_size = hdr->size;
	if (avformat_write_header(g_render.outCtx, NULL) < 0) {
		RendererCore::ReportError("Failed to write output file");
		return false;
	}
	g_render.headerWritten = true;
	return true;
}

static bool writeVideoPacket(AVPacket *pkt) {
	Stream *v = &g_render.videoStream;
	av_packet_rescale_ts(pkt, {1, g_render.fps}, v->stream->time_base);
	pkt->duration = av_rescale_q(1, {1, g_render.fps}, v->stream->time_base);
	pkt->stream_index = v->stream->index;
	if (av_interleaved_write_frame(g_render.outCtx, pkt) < 0) {
		RendererCore::ReportError("Failed to write video packet!");
		return false;
	}
	return true;
}

static void encoderThread() {
	bool ok = true;
	while (true) {
		int64_t idle = 0;
		EncItem item = g_render.encQueue.Pop(&idle);
		g_render.encodeIdleUs += idle;
		int64_t t0 = RendererCore::NowMicros();
		switch (item.type) {
		case EncItem::VIDEO:
			// After an error we keep draining so producers never block
			if (ok) ok = encodeVideo(item.frame);
			g_render.framePool.Put(item.frame);
			break;
		case EncItem::AUDIO:
			if (!g_render.headerWritten) {
				g_render.pendingAudio.push_back(item.frame);
				break;
			}
			if (ok) ok = encodeAudio(item.frame);
			av_frame_free(&item.frame);
			break;
		case EncItem::HEADER:
			if (ok && !g_render.headerWritten) ok = writeDeferredHeader(item.pkt);
			av_packet_free(&item.pkt);
			for (AVFrame *f : g_render.pendingAudio) {
				if (ok && g_render.headerWritten) ok = encodeAudio(f);
				av_frame_free(&f);
			}
			g_render.pendingAudio.clear();
			break;
		case EncItem::PACKET:
			if (ok && g_render.headerWritten) ok = writeVideoPacket(item.pkt);
			av_packet_free(&item.pkt);
			break;
		case EncItem::END:
			for (AVFrame *f : g_render.pendingAudio) av_frame_free(&f);
			g_render.pendingAudio.clear();
			finishOutput();
			return;
		}
		g_render.encodeUs += RendererCore::NowMicros() - t0;
	}
}

// }}}

// CPU read-back path: conversion thread {{{

static void convEmit(const uint8_t *pixels) {
	AVFrame *f = RendererCore::AcquireVideoFrame();
	if (!f) return;
	int64_t t0 = RendererCore::NowMicros();
	g_render.bands.Convert(pixels, g_render.width * 4, f);
	g_render.convertUs += RendererCore::NowMicros() - t0;
	RendererCore::SubmitVideoFrame(f);
}

static void converterThread() {
	size_t n = (size_t)g_render.width * g_render.height * 4;
	while (true) {
		ConvItem item = g_render.convQueue.Pop();
		if (item.type == ConvItem::END) {
			RendererCore::SubmitEnd();
			return;
		}

		if (g_render.toBlend == 1) {
			convEmit(item.pixels);
		} else {
			int64_t t0 = RendererCore::NowMicros();
			uint32_t *sum = g_render.blendSum.data();
			if (item.accumulate) {
				uint32_t w = g_render.blendWeights16[item.subframe];
				const uint8_t *px = item.pixels;
				for (size_t i = 0; i < n; ++i) sum[i] += w * px[i];
			}
			if (item.emit) {
				uint8_t *out = g_render.blendOut;
				for (size_t i = 0; i < n; ++i) {
					out[i] = (uint8_t)std::min<uint32_t>((sum[i] + 32768) >> 16, 255);
					sum[i] = 0;
				}
			}
			g_render.convertUs += RendererCore::NowMicros() - t0;
			if (item.emit) convEmit(g_render.blendOut);
		}

		if (item.pixels) g_render.rawPool.Put(item.pixels);
	}
}

// }}}

// Start / finish {{{

static void freeSession() {
	for (AVFrame *f : g_render.allFrames) av_frame_free(&f);
	g_render.allFrames.clear();
	g_render.framePool.Reset();
	for (uint8_t *p : g_render.allRaw) av_free(p);
	g_render.allRaw.clear();
	g_render.rawPool.Reset();
	g_render.blendSum.clear();
	g_render.blendSum.shrink_to_fit();
	av_freep(&g_render.blendOut);
	av_frame_free(&g_render.audioFrame);
	g_render.bands.Destroy();
}

static void closeOutput(bool writeTrailer) {
	if (writeTrailer && g_render.headerWritten) av_write_trailer(g_render.outCtx);
	closeStream(&g_render.videoStream);
	closeStream(&g_render.audioStream);
	if (g_render.outCtx->pb) avio_closep(&g_render.outCtx->pb);
	avformat_free_context(g_render.outCtx);
	g_render.outCtx = nullptr;
}

static void printStats(int64_t wallUs, int frames) {
	if (!sar_render_stats.GetBool() || wallUs <= 0) return;
	auto ms = [](int64_t us) { return us / 1000.0; };
	double secs = wallUs / 1e6;
	console->Print("[render] %d frames in %.2fs (%.1f fps), %s path, encoder %s\n", frames, secs, frames / secs, g_render.useGpu ? "GPU" : "CPU", g_render.encoderName.c_str());
	console->Print("[render] capture %.0fms, waited for encoder %.0fms, convert %.0fms, encode busy %.0fms (%.0f%%), encoder idle %.0fms\n",
		ms(g_render.captureUs), ms(g_render.stallUs), ms(g_render.convertUs), ms(g_render.encodeUs), 100.0 * g_render.encodeUs / wallUs, ms(g_render.encodeIdleUs));
}

// Encoder thread, after END
static void finishOutput() {
	bool error = g_render.stopError.load() || g_render.failed.load();

	bool wroteFile = g_render.headerWritten;
	if (wroteFile) {
		flushStream(&g_render.videoStream, true);
		flushStream(&g_render.audioStream, true);
	}
	closeOutput(true);
	if (!wroteFile) remove(g_render.filename.c_str());
	g_render.outputClosed.store(true);

	int frames = g_render.framesCaptured.load();
	int64_t wall = RendererCore::NowMicros() - g_render.startTime;
	std::string filename = g_render.filename;
	std::string err;
	{
		std::lock_guard<std::mutex> lock(g_render.errorLock);
		err = g_render.errorMsg;
	}

	Scheduler::OnMainThread([=]() {
		if (!err.empty()) console->Print("%s\n", err.c_str());
		console->Print("Rendered %d frames to '%s'\n", frames, filename.c_str());
		printStats(wall, frames);

		if (g_render.converter.joinable()) g_render.converter.join();
		if (g_render.encoder.joinable()) g_render.encoder.join();
		freeSession();

		// Reset all the Source movieinfo struct to its default values
		g_movieInfo->moviename[0] = 0;
		g_movieInfo->movieframe = 0;
		g_movieInfo->type = MovieInfo_t::FMOVIE_TGA | MovieInfo_t::FMOVIE_WAV;
		g_movieInfo->jpeg_quality = 50;

		g_render.sessionActive.store(false);
		(void)error;
		Event::Trigger<Event::RENDERER_FINISH>({});

		if (!g_render.deferredQuit.empty()) {
			std::string cmd = g_render.deferredQuit;
			g_render.deferredQuit.clear();
			engine->ExecuteCommand(cmd.c_str());
		}
	});
}

static void startVirtualClock(int framesPerSecond);
static void advanceVirtualClock();
static void stopVirtualClock();

// Main thread
static void stopRender(bool error) {
	if (!g_render.isRendering.load()) return;

	if (error) {
		console->Print("A fatal error occurred; stopping render early\n");
	} else {
		console->Print("Stopping render...\n");
	}

	g_render.stopError.store(error);
	RendererGpu::SetSkipPresent(false);
	fps_max.SetValue(g_render.savedFpsMax);
	stopVirtualClock();
	{
		// Wait for an audio push in flight; no more audio after this
		std::lock_guard<std::mutex> lock(g_render.audioLock);
		g_render.isRendering.store(false);
	}

	if (g_render.useGpu) {
		RendererGpu::Finish();
	} else {
		g_render.convQueue.Push({ConvItem::END, nullptr, false, false, 0});
	}
}

static bool openOutput(AVCodec *videoCodec, AVCodec *audioCodec, int64_t videoBitrate, int64_t audioBitrate, AVDictionary **videoOptions) {
	g_render.outCtx = nullptr;
	if (avformat_alloc_output_context2(&g_render.outCtx, NULL, NULL, g_render.filename.c_str()) == AVERROR(EINVAL)) {
		console->Print("Failed to deduce output format from file extension - using MP4\n");
		avformat_alloc_output_context2(&g_render.outCtx, NULL, "mp4", g_render.filename.c_str());
	}

	if (!g_render.outCtx) {
		console->Print("Failed to allocate output context\n");
		return false;
	}

	memset(&g_render.videoStream, 0, sizeof g_render.videoStream);
	memset(&g_render.audioStream, 0, sizeof g_render.audioStream);

	auto fail = [](const char *msg) {
		console->Print("%s\n", msg);
		closeOutput(false);
		return false;
	};

	if (g_render.nvenc) {
		// Compressed by NVENC on the render thread; no libavcodec encoder
		Stream *v = &g_render.videoStream;
		v->stream = avformat_new_stream(g_render.outCtx, NULL);
		if (!v->stream) return fail("Failed to create video stream");
		v->stream->time_base = {1, g_render.fps};
		v->stream->avg_frame_rate = {g_render.fps, 1};
		AVCodecParameters *par = v->stream->codecpar;
		par->codec_type = AVMEDIA_TYPE_VIDEO;
		par->codec_id = videoCodec->id;
		par->width = g_render.width;
		par->height = g_render.height;
		par->format = AV_PIX_FMT_YUV420P;
		par->sample_aspect_ratio = {1, 1};
		par->color_primaries = AVCOL_PRI_BT709;
		par->color_trc = AVCOL_TRC_BT709;
		par->color_space = AVCOL_SPC_BT709;
		par->color_range = AVCOL_RANGE_MPEG;
		par->field_order = AV_FIELD_PROGRESSIVE;
	} else if (!addStream(&g_render.videoStream, g_render.outCtx, videoCodec, videoBitrate, g_render.fps, 0, g_render.width, g_render.height)) {
		return fail("Failed to create video stream");
	}

	if (!addStream(&g_render.audioStream, g_render.outCtx, audioCodec, audioBitrate, g_render.samplerate, g_render.samplerate / 10)) {  // offset the start by 0.1s because idk
		return fail("Failed to create audio stream");
	}

	if (!g_render.nvenc && !openVideo(&g_render.videoStream, videoOptions)) {
		return fail("Failed to open video stream");
	}

	AVDictionary *audioOptions = NULL;
	bool audioOk = openAudio(&g_render.audioStream, &audioOptions);
	av_dict_free(&audioOptions);
	if (!audioOk) {
		return fail("Failed to open audio stream");
	}

	if (avio_open(&g_render.outCtx->pb, g_render.filename.c_str(), AVIO_FLAG_WRITE) < 0) {
		return fail("Failed to open output file");
	}

	g_render.headerWritten = false;
	if (!g_render.nvenc) {
		if (avformat_write_header(g_render.outCtx, NULL) < 0) {
			return fail("Failed to write output file");
		}
		g_render.headerWritten = true;
	}

	return true;
}

static bool allocSession() {
	int queue = sar_render_queue.GetInt();
	AVCodecContext *enc = g_render.videoStream.enc;
	for (int i = 0; enc && i < queue; ++i) {
		AVFrame *f = av_frame_alloc();
		if (!f) return false;
		f->format = enc->pix_fmt;
		f->width = enc->width;
		f->height = enc->height;
		f->color_primaries = enc->color_primaries;
		f->color_trc = enc->color_trc;
		f->colorspace = enc->colorspace;
		f->color_range = enc->color_range;
		if (av_frame_get_buffer(f, 64) < 0) {
			av_frame_free(&f);
			return false;
		}
		g_render.allFrames.push_back(f);
		g_render.framePool.Put(f);
	}

	Stream *a = &g_render.audioStream;
	int audioSamples = a->enc->frame_size ? a->enc->frame_size : 1024;
	g_render.audioFrame = allocAudioFrame(AV_SAMPLE_FMT_S16P, a->enc->channel_layout, a->enc->sample_rate, audioSamples);
	if (!g_render.audioFrame) return false;
	g_render.audioFill = 0;
	g_render.audioPts = a->nextPts;
	g_render.channels = a->enc->channels;

	if (!g_render.useGpu) {
		size_t n = (size_t)g_render.width * g_render.height * 4;
		for (int i = 0; i < 3; ++i) {
			uint8_t *p = (uint8_t *)av_malloc(n);
			if (!p) return false;
			g_render.allRaw.push_back(p);
			g_render.rawPool.Put(p);
		}
		if (g_render.toBlend > 1) {
			g_render.blendSum.assign(n, 0);
			g_render.blendOut = (uint8_t *)av_malloc(n);
			if (!g_render.blendOut) return false;
		}
		int threads = sar_render_threads.GetInt();
		if (threads <= 0) threads = std::max(1, std::min(4, g_render.height / 270));
		if (!g_render.bands.Init(g_render.width, g_render.height, AV_PIX_FMT_BGR0, enc->pix_fmt, threads)) return false;
	}
	return true;
}

static void startRender() {
	if (g_render.isRendering.load()) {
		console->Print("Already rendering\n");
		return;
	}

	if (g_render.sessionActive.load() || RendererGpu::Busy()) {
		console->Print("The previous render is still being finished; try again in a moment\n");
		return;
	}

	if (g_render.encoder.joinable()) g_render.encoder.join();
	if (g_render.converter.joinable()) g_render.converter.join();

	if (!sv_cheats.GetBool()) {
		console->Print("sv_cheats must be enabled\n");
		return;
	}

	g_render.samplerate = sar_render_sample_rate.GetInt();
	g_render.fps = sar_render_fps.GetInt();

	if (sar_render_blend.GetInt() == 0 && host_framerate.GetInt() == 0) {
		console->Print("host_framerate or sar_render_blend must be nonzero\n");
		return;
	} else if (sar_render_blend.GetInt() != 0) {
		g_render.toBlend = sar_render_blend.GetInt();
		int framerate = g_render.toBlend * g_render.fps;
		if (host_framerate.GetInt() != 0 && host_framerate.GetInt() != framerate) {
			console->Print("Warning: overriding host_framerate to %d based on sar_render_fps and sar_render_blend\n", framerate);
		}
		host_framerate.SetValue(framerate);
	} else {  // host_framerate nonzero
		int framerate = host_framerate.GetInt();
		if (framerate % g_render.fps != 0) {
			console->Print("host_framerate must be a multiple of sar_render_fps\n");
			return;
		}
		g_render.toBlend = framerate / g_render.fps;
	}

	{
		float shutter = (float)sar_render_shutter_angle.GetInt() / 360.0f;
		int toExclude = (int)round(g_render.toBlend * (1 - shutter) / 2);
		if (toExclude * 2 >= g_render.toBlend) {
			toExclude = std::max(0, g_render.toBlend / 2 - 1);
		}
		g_render.toBlendStart = toExclude;
		g_render.toBlendEnd = g_render.toBlend - toExclude;
	}
	computeBlendWeights();

	if (snd_surround_speakers.GetInt() != 2) {
		console->Print("Note: setting speaker configuration to stereo. You may wish to reset it after the render\n");
		snd_surround_speakers.SetValue(2);
	}

	AVCodecID videoCodecId = videoCodecFromName(sar_render_vcodec.GetString());
	AVCodecID audioCodecId = audioCodecFromName(sar_render_acodec.GetString());

	if (videoCodecId == AV_CODEC_ID_NONE) {
		console->Print("Unknown video codec '%s'\n", sar_render_vcodec.GetString());
		return;
	}

	if (audioCodecId == AV_CODEC_ID_NONE) {
		console->Print("Unknown audio codec '%s'\n", sar_render_acodec.GetString());
		return;
	}

	// NVENC straight from the game's D3D9 device?
	g_render.nvenc = false;
	{
		std::string venc = sar_render_vencoder.GetString();
		bool wantNv = venc == "nvenc" || venc == "auto" || venc == "h264_nvenc" || venc == "hevc_nvenc";
		if (venc == "h264_nvenc") videoCodecId = AV_CODEC_ID_H264;
		if (venc == "hevc_nvenc") videoCodecId = AV_CODEC_ID_HEVC;
		if (wantNv) {
			const char *why = nullptr;
			int w = GetScreenWidth(), h = GetScreenHeight();
			if (videoCodecId != AV_CODEC_ID_H264 && videoCodecId != AV_CODEC_ID_HEVC) {
				why = "only h264 and hevc are supported";
			} else if (!sar_render_gpu.GetBool()) {
				why = "sar_render_gpu is 0";
			} else if (RendererGpu::CanUse(w, h, AV_PIX_FMT_YUV420P, &why) && RendererGpu::NvencAvailable(&why)) {
				g_render.nvenc = true;
			}
			bool codecOk = videoCodecId == AV_CODEC_ID_H264 || videoCodecId == AV_CODEC_ID_HEVC;
			if (!g_render.nvenc && (venc != "auto" || codecOk)) console->Print("NVENC unavailable (%s); using the default encoder\n", why ? why : "unknown");
		}
	}

	AVCodec *videoCodec = g_render.nvenc ? (AVCodec *)avcodec_find_decoder(videoCodecId) : findVideoEncoder(videoCodecId);
	AVCodec *audioCodec = avcodec_find_encoder(audioCodecId);
	if (!videoCodec) {
		console->Print("Failed to find encoder for '%s'!\n", avcodec_get_name(videoCodecId));
		return;
	}
	if (!audioCodec) {
		console->Print("Failed to find encoder for '%s'!\n", avcodec_get_name(audioCodecId));
		return;
	}

	float videoBitrate = sar_render_vbitrate.GetFloat();
	float audioBitrate = sar_render_abitrate.GetFloat();

	int quality = sar_render_quality.GetInt();
	if (quality < 0) quality = 0;
	if (quality > 50) quality = 50;

	AVDictionary *options = NULL;

	// Quality options
	{
		// TODO: CRF scales non-linearly. Do we make this conversion
		// non-linear to better reflect that?

		int min = 0;
		int max = 63;
		if (videoCodec->id == AV_CODEC_ID_H264 || videoCodec->id == AV_CODEC_ID_HEVC) {
			max = 51;
		} else if (videoCodec->id == AV_CODEC_ID_VP8) {
			min = 4;
		}
		int crf = min + (max - min) * (50 - quality) / 50;
		std::string crfStr = std::to_string(crf);

		const char *name = videoCodec->name;
		if (!strcmp(name, "libx264") || !strcmp(name, "libx265")) {
			av_dict_set(&options, "crf", crfStr.c_str(), 0);
			av_dict_set(&options, "preset", sar_render_preset.GetString(), 0);
		} else if (!strncmp(name, "libvpx", 6)) {
			av_dict_set(&options, "crf", crfStr.c_str(), 0);
		} else if (strstr(name, "_mf")) {
			// Media Foundation: prefer the hardware encoder, quality-based
			// rate control (0-100)
			av_dict_set(&options, "hw_encoding", "1", 0);
			av_dict_set(&options, "rate_control", "quality", 0);
			av_dict_set(&options, "quality", std::to_string(quality * 2).c_str(), 0);
		}
	}

	g_render.width = GetScreenWidth();
	g_render.height = GetScreenHeight();

	g_render.failed.store(false);
	g_render.stopError.store(false);
	{
		std::lock_guard<std::mutex> lock(g_render.errorLock);
		g_render.errorMsg.clear();
	}

	if (!openOutput(videoCodec, audioCodec, (int64_t)(videoBitrate * 1000), (int64_t)(audioBitrate * 1000), &options)) {
		av_dict_free(&options);
		return;
	}
	av_dict_free(&options);

	AVPixelFormat pixFmt = g_render.nvenc ? AV_PIX_FMT_YUV420P : g_render.videoStream.enc->pix_fmt;
	const char *why = nullptr;
	g_render.useGpu = g_render.nvenc || (sar_render_gpu.GetBool() && RendererGpu::CanUse(g_render.width, g_render.height, pixFmt, &why));
	if (sar_render_gpu.GetBool() && !g_render.useGpu && why) {
		console->Print("GPU capture unavailable (%s); using CPU read-back\n", why);
	}

	if (!allocSession()) {
		console->Print("Failed to allocate render buffers\n");
		freeSession();
		closeOutput(false);
		return;
	}

	if (g_render.useGpu) {
		RendererGpu::Params p;
		p.width = g_render.width;
		p.height = g_render.height;
		p.pixFmt = pixFmt;
		p.toBlend = g_render.toBlend;
		p.weights = g_render.blendWeights;
		p.slots = 4;
		p.nvenc.enabled = g_render.nvenc;
		if (g_render.nvenc) {
			// Same quality scale as x264's CRF; NVENC's CQ needs ~8 more for
			// similar SSIM and size
			int crf = 51 * (50 - quality) / 50;
			p.nvenc.hevc = videoCodec->id == AV_CODEC_ID_HEVC;
			p.nvenc.cq = std::max(1, std::min(51, crf + 8));
			p.nvenc.preset = nvencPresetFromName(sar_render_preset.GetString());
			int gop = sar_render_gop.GetInt();
			p.nvenc.gop = gop > 0 ? gop : g_render.fps * 4;
			p.nvenc.fps = g_render.fps;
			p.nvenc.bframes = sar_render_nvenc_bframes.GetInt();
			p.nvenc.lookahead = sar_render_nvenc_lookahead.GetInt();
			p.nvenc.multipass = sar_render_nvenc_multipass.GetInt();
		}
		if (!RendererGpu::Begin(p)) {
			if (g_render.nvenc) {
				console->Print("NVENC failed to start\n");
				freeSession();
				closeOutput(false);
				remove(g_render.filename.c_str());
				return;
			}
			console->Print("GPU capture failed to start; using CPU read-back\n");
			g_render.useGpu = false;
			freeSession();
			if (!allocSession()) {
				console->Print("Failed to allocate render buffers\n");
				freeSession();
				closeOutput(false);
				return;
			}
		}
	}

	g_render.nextBlendIdx = 0;
	g_render.videoPts = 0;
	g_render.framesCaptured = 0;
	g_render.captureUs = 0;
	g_render.stallUs = 0;
	g_render.convertUs = 0;
	g_render.encodeUs = 0;
	g_render.encodeIdleUs = 0;
	g_render.startTime = RendererCore::NowMicros();

	g_render.encoderName = g_render.nvenc ? (videoCodec->id == AV_CODEC_ID_HEVC ? "hevc_nvenc (zero-copy)" : "h264_nvenc (zero-copy)") : g_render.videoStream.codec->name;
	g_render.outputClosed.store(false);
	g_render.sessionActive.store(true);
	g_render.encoder = std::thread(encoderThread);
	if (!g_render.useGpu) g_render.converter = std::thread(converterThread);

	g_movieInfo->moviename[0] = 'a';  // Just something nonzero to make the game think there's a movie in progress
	g_movieInfo->moviename[1] = 0;
	g_movieInfo->movieframe = 0;
	g_movieInfo->type = 0;  // Should stop anything actually being output

	// The engine skips simulating a frame when it comes sooner than
	// fps_max allows, but CEngine::Frame (and so our capture) still runs.
	// Faster than fps_max that would capture the same frame many times
	// over, so the cap is lifted while rendering.
	g_render.savedFpsMax = fps_max.GetFloat();
	fps_max.SetValue(0);
	startVirtualClock(host_framerate.GetInt());

	g_render.isRendering.store(true);
	if (g_render.useGpu && !sar_render_display.GetBool()) RendererGpu::SetSkipPresent(true);

	console->Print("Started rendering to '%s'\n", g_render.filename.c_str());

	if (g_render.nvenc) {
		console->Print("    video: %s (%dx%d @ %d fps, constant quality, yuv420p)\n", g_render.encoderName.c_str(), g_render.width, g_render.height, g_render.fps);
	} else {
		console->Print(
			"    video: %s (%dx%d @ %d fps, %" PRId64 " kb/s, %s, %s capture)\n",
			g_render.videoStream.codec->name,
			g_render.width,
			g_render.height,
			g_render.fps,
			g_render.videoStream.enc->bit_rate / 1000,
			av_get_pix_fmt_name(g_render.videoStream.enc->pix_fmt),
			g_render.useGpu ? "GPU" : "CPU");
	}

	console->Print(
		"    audio: %s (%d Hz, %" PRId64 " kb/s, %s)\n",
		g_render.audioStream.codec->name,
		g_render.audioStream.enc->sample_rate,
		g_render.audioStream.enc->bit_rate / 1000,
		av_get_sample_fmt_name(g_render.audioStream.enc->sample_fmt));

	Event::Trigger<Event::RENDERER_START>({});
}

// }}}

// Audio output {{{

static inline short clip16(int x) {
	if (x < -32768) return -32768;  // Source uses -32767, but that's, like, not how two's complement works
	if (x > 32767) return 32767;
	return x;
}

static void (*SND_RecordBuffer)();
static void SND_RecordBuffer_Hook();
static Hook g_RecordBufferHook(&SND_RecordBuffer_Hook);
static void SND_RecordBuffer_Hook() {
	if (!g_render.isRendering.load()) {
		g_RecordBufferHook.Disable();
		SND_RecordBuffer();
		g_RecordBufferHook.Enable();
		return;
	}

	if (g_render.isPaused.load())
		return;

	if (engine->ConsoleVisible()) return;

	if (snd_surround_speakers.GetInt() != 2) {
		console->Print("Speaker configuration changed!\n");
		RendererCore::ReportError("Speaker configuration changed!");
		return;
	}

	std::lock_guard<std::mutex> lock(g_render.audioLock);
	if (!g_render.isRendering.load()) return;

	int channels = g_render.channels;
	AVFrame *f = g_render.audioFrame;
	int16_t **planes = (int16_t **)f->data;
	int count = *g_snd_linear_count;
	int *src = *g_snd_p;
	int vol = *g_snd_vol;

	for (int i = 0; i < count; i += channels) {
		for (int c = 0; c < channels; ++c) {
			planes[c][g_render.audioFill] = clip16((src[i + c] * vol) >> 8);
		}

		if (++g_render.audioFill == f->nb_samples) {
			int samples = f->nb_samples;
			f->pts = g_render.audioPts;
			g_render.audioPts += samples;
			// The encoder thread owns (and may free) f from here on
			g_render.encQueue.Push({EncItem::AUDIO, f});

			Stream *a = &g_render.audioStream;
			g_render.audioFrame = f = allocAudioFrame(AV_SAMPLE_FMT_S16P, a->enc->channel_layout, a->enc->sample_rate, samples);
			g_render.audioFill = 0;
			if (!f) {
				RendererCore::ReportError("Failed to allocate audio frame!");
				return;
			}
			planes = (int16_t **)f->data;
		}
	}
}

// }}}

// Engine movie frame {{{

// While a movie is "recording" (we set g_movieInfo so the engine mixes
// audio into SND_RecordBuffer), the engine calls
// IVideoMode::WriteMovieFrame at the end of every frame. That function
// allocates a w*h*3 buffer and does a full synchronous ReadScreenPixels
// before it checks which outputs are enabled - with our type of 0 it then
// throws the pixels away. That is a hidden GPU stall + CPU conversion per
// frame, so skip it entirely while SAR renders.

#ifdef _WIN32
using WriteMovieFrame_t = void(__thiscall *)(void *thisptr, void *info);
static WriteMovieFrame_t WriteMovieFrame_Target;
static WriteMovieFrame_t WriteMovieFrame_Trampoline;

static void __fastcall WriteMovieFrame_Hook(void *thisptr, int edx, void *info) {
	if (g_render.isRendering.load()) return;
	WriteMovieFrame_Trampoline(thisptr, info);
}

// Sanity check before hooking: the function warns about a missing movie
// name ("Tried to write movie buffer with no filename set!")
static bool looksLikeWriteMovieFrame(const uint8_t *fn) {
	for (int i = 0; i < 0x60; ++i) {
		if (fn[i] != 0x68) continue;  // push imm32
		const char *str = *(const char *const *)(fn + i + 1);
		MEMORY_BASIC_INFORMATION mbi;
		if (!VirtualQuery(str, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
		if (!strncmp(str, "Tried to write movie buffer", 27)) return true;
	}
	return false;
}

static void hookWriteMovieFrame() {
	if (!*g_videomode) return;
	auto fn = Memory::VMT<WriteMovieFrame_t>(*g_videomode, Offsets::WriteMovieFrame);
	if (!fn || !looksLikeWriteMovieFrame((const uint8_t *)fn)) {
		console->Print("Renderer: IVideoMode::WriteMovieFrame not recognized; renders keep the engine's extra read-back\n");
		return;
	}
	MH_Initialize();
	if (MH_CreateHook((LPVOID)fn, (LPVOID)&WriteMovieFrame_Hook, (LPVOID *)&WriteMovieFrame_Trampoline) == MH_OK && MH_EnableHook((LPVOID)fn) == MH_OK) {
		WriteMovieFrame_Target = fn;
	}
}

// The input system polls every controller slot every frame, and XInput
// probes the hardware for empty slots each time (registry + device I/O).
// A demo being rendered takes no input, so report "no controller" without
// asking XInput while rendering.
using XInputGetState_t = DWORD(WINAPI *)(DWORD index, void *state);
static XInputGetState_t XInputGetState_Targets[3];
static XInputGetState_t XInputGetState_Trampolines[3];

template <int N>
static DWORD WINAPI XInputGetState_Hook(DWORD index, void *state) {
	if (g_render.isRendering.load()) return ERROR_DEVICE_NOT_CONNECTED;
	return XInputGetState_Trampolines[N](index, state);
}

static void hookXInput() {
	static const char *dlls[3] = {"XInput1_3.dll", "xinput1_4.dll", "XInput9_1_0.dll"};
	static void *hooks[3] = {(void *)&XInputGetState_Hook<0>, (void *)&XInputGetState_Hook<1>, (void *)&XInputGetState_Hook<2>};
	MH_Initialize();
	for (int i = 0; i < 3; ++i) {
		HMODULE mod = GetModuleHandleA(dlls[i]);
		auto fn = mod ? (XInputGetState_t)GetProcAddress(mod, "XInputGetState") : nullptr;
		if (!fn) continue;
		if (MH_CreateHook((LPVOID)fn, hooks[i], (LPVOID *)&XInputGetState_Trampolines[i]) == MH_OK && MH_EnableHook((LPVOID)fn) == MH_OK) {
			XInputGetState_Targets[i] = fn;
		}
	}
}

static void unhookWriteMovieFrame() {
	if (WriteMovieFrame_Target) {
		MH_DisableHook((LPVOID)WriteMovieFrame_Target);
		MH_RemoveHook((LPVOID)WriteMovieFrame_Target);
		WriteMovieFrame_Target = nullptr;
	}
	for (auto &fn : XInputGetState_Targets) {
		if (fn) {
			MH_DisableHook((LPVOID)fn);
			MH_RemoveHook((LPVOID)fn);
			fn = nullptr;
		}
	}
}
// HUD clock {{{

// Renders run many times faster than real time, but the client's HUD reads
// the wall clock (Plat_FloatTime) directly: the challenge mode timer only
// refreshes every so many real milliseconds, so in a fast render it held
// the same value for several frames. While rendering, the main thread's
// Plat_FloatTime/Plat_MSTime *as imported by the client and VGUI DLLs*
// advance exactly one frame per engine frame, as in real-time playback.
// The engine keeps real time (its frame loop must never see time go
// backwards), and after the render the client's clock eases back to real
// time without ever going backwards.

using PlatFloatTime_t = double (*)();
using PlatMSTime_t = unsigned long (*)();
static PlatFloatTime_t g_realFloatTime;
static PlatMSTime_t g_realMSTime;
static DWORD g_mainThreadId;
static std::atomic<bool> g_hudClockOn{false};
static double g_hudNow;       // main thread, virtual time this frame
static double g_hudStep;
static double g_hudLast;      // main thread, last value handed out
static double g_hudMsOffset;  // Plat_MSTime - Plat_FloatTime * 1000 (their epochs differ)
static unsigned g_hudCalls;

struct IatPatch {
	void **slot;
	void *original;
};
static std::vector<IatPatch> g_iatPatches;

static double hudFloatTime() {
	double real = g_realFloatTime();
	if (GetCurrentThreadId() != g_mainThreadId) return real;
	double t;
	if (g_hudClockOn.load()) {
		// strictly increasing within a frame, never reaching the next frame
		t = g_hudNow + std::min(++g_hudCalls * 1e-7, g_hudStep * 0.5);
	} else if (g_hudLast > real) {
		// After a render the virtual clock is ahead; let it advance at half
		// speed until real time catches up instead of jumping back
		static double lastReal = real;
		t = g_hudLast + std::max(0.0, real - lastReal) * 0.5;
		lastReal = real;
	} else {
		t = real;
	}
	if (t < g_hudLast) t = g_hudLast;
	g_hudLast = t;
	return t;
}

static double HudPlatFloatTime() {
	return hudFloatTime();
}

static unsigned long HudPlatMSTime() {
	if (GetCurrentThreadId() != g_mainThreadId) return g_realMSTime();
	return (unsigned long)(hudFloatTime() * 1000.0 + g_hudMsOffset);
}

// Points a module's import of tier0!name at `fn`
static void patchImport(HMODULE mod, const char *name, void *fn) {
	auto base = (uint8_t *)mod;
	auto nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)mod)->e_lfanew);
	auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress) return;
	for (auto imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; ++imp) {
		if (_stricmp((const char *)(base + imp->Name), "tier0.dll")) continue;
		if (!imp->OriginalFirstThunk) continue;
		auto names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
		auto slots = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
		for (; names->u1.AddressOfData; ++names, ++slots) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
			auto ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
			if (strcmp((const char *)ibn->Name, name)) continue;
			void **slot = (void **)&slots->u1.Function;
			DWORD old;
			if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return;
			g_iatPatches.push_back({slot, *slot});
			*slot = fn;
			VirtualProtect(slot, sizeof(void *), old, &old);
			return;
		}
	}
}

static void hookHudClock() {
	if (!g_iatPatches.empty() || !g_mainThreadId) return;
	HMODULE tier0 = GetModuleHandleA("tier0.dll");
	g_realFloatTime = tier0 ? (PlatFloatTime_t)GetProcAddress(tier0, "Plat_FloatTime") : nullptr;
	g_realMSTime = tier0 ? (PlatMSTime_t)GetProcAddress(tier0, "Plat_MSTime") : nullptr;
	if (!g_realFloatTime || !g_realMSTime) return;
	g_hudMsOffset = (double)g_realMSTime() - g_realFloatTime() * 1000.0;
	for (const char *dll : {"client.dll", "vgui2.dll", "vguimatsurface.dll"}) {
		HMODULE mod = GetModuleHandleA(dll);
		if (!mod) continue;
		patchImport(mod, "Plat_FloatTime", (void *)&HudPlatFloatTime);
		patchImport(mod, "Plat_MSTime", (void *)&HudPlatMSTime);
	}
}

static void unhookHudClock() {
	g_hudClockOn.store(false);
	for (auto &p : g_iatPatches) {
		DWORD old;
		if (VirtualProtect(p.slot, sizeof(void *), PAGE_READWRITE, &old)) {
			*p.slot = p.original;
			VirtualProtect(p.slot, sizeof(void *), old, &old);
		}
	}
	g_iatPatches.clear();
}

// Main thread
static void startVirtualClock(int framesPerSecond) {
	if (!sar_render_virtual_clock.GetBool() || framesPerSecond <= 0) return;
	hookHudClock();
	if (g_iatPatches.empty()) return;
	g_hudStep = 1.0 / framesPerSecond;
	g_hudNow = std::max(g_realFloatTime(), g_hudLast);
	g_hudCalls = 0;
	g_hudClockOn.store(true);
}

static void advanceVirtualClock() {
	if (!g_hudClockOn.load()) return;
	g_hudNow += g_hudStep;
	g_hudCalls = 0;
}

static void stopVirtualClock() {
	g_hudClockOn.store(false);
}

// }}}
#else
static void hookXInput() {}
static void startVirtualClock(int) {}
static void advanceVirtualClock() {}
static void stopVirtualClock() {}
static void unhookHudClock() {}
static void hookWriteMovieFrame() {}
static void unhookWriteMovieFrame() {}
#endif

// }}}

// Video output {{{

static void captureCpu(bool accumulate, bool emit, int subframe) {
	ConvItem item = {ConvItem::FRAME, nullptr, accumulate, emit, subframe};
	if (accumulate) {
		int64_t stall = 0;
		if (!g_render.rawPool.Take(item.pixels, &stall)) return;
		g_render.stallUs += stall;
		int64_t t0 = RendererCore::NowMicros();
		Renderer::ReadScreenPixels(0, 0, g_render.width, g_render.height, item.pixels, IMAGE_FORMAT_BGRX8888);
		g_render.captureUs += RendererCore::NowMicros() - t0;
	}
	g_render.convQueue.Push(item);
}

void Renderer::Frame() {
	if (Renderer::isDemoLoading && sar_render_autostart.GetBool()) {
		bool start = engine->demoplayer->IsPlaybackFixReady();
		start &= session->isRunning;
		if (engine->GetMaxClients() >= 2 && sar_render_skip_coop_videos.GetBool()) {
			start &= engine->startedTransitionFadeout;
		}
		// Wait for a previous render to be written out first
		start &= !g_render.sessionActive.load() || g_render.isRendering.load();
		g_render.isPaused.store(!start);
		if (!start) return;

		Renderer::isDemoLoading = false;

		if (!g_render.isRendering.load()) {
			auto demo = std::string(engine->demoplayer->DemoName);
			if (!Utils::EndsWith(demo, ".dem")) demo += ".dem";
			auto path = fileSystem->FindFileSomewhere(demo).value_or(demo);
			path = path.substr(0, path.size() - 4);
			g_render.filename = path + "." + sar_render_autostart_extension.GetString();
			startRender();
		}
	} else {
		Renderer::isDemoLoading = false;
	}

	if (!g_render.isRendering.load()) return;

	// Something (configs, sar_loads_uncap restoring after a load) put the
	// cap back; frames this call may not have been simulated, so skip it
	if (fps_max.GetFloat() != 0) {
		fps_max.SetValue(0);
		return;
	}

	// One engine frame = one host_framerate step of wall-clock time
	advanceVirtualClock();

	if (g_render.failed.load() || (g_render.useGpu && RendererGpu::Failed())) {
		std::string err;
		{
			std::lock_guard<std::mutex> lock(g_render.errorLock);
			err = g_render.errorMsg;
		}
		if (!err.empty()) console->Print("%s\n", err.c_str());
		stopRender(true);
		return;
	}

	if (engine->GetMaxClients() >= 2 && sar_render_skip_coop_videos.GetBool() && engine->isLevelTransition) {
		// The transition video has begun, pause the render
		g_render.isPaused.store(true);
		return;
	}

	// autostop: if it's the __END__ tick, or the demo is over, stop
	// rendering

	if (sar_render_autostop.GetBool() && Renderer::segmentEndTick != -1 && engine->demoplayer->IsPlaying() && engine->demoplayer->GetTick() > Renderer::segmentEndTick) {
		if (!sar_render_merge.GetBool())
			stopRender(false);
		else
			g_render.isPaused.store(true);
		return;
	}

	if (g_render.isPaused.load())
		return;

	// Don't render if the console is visible
	if (engine->ConsoleVisible()) return;

	if (GetScreenWidth() != g_render.width || GetScreenHeight() != g_render.height) {
		console->Print("Screen resolution has changed!\n");
		stopRender(true);
		return;
	}

	int sub = g_render.nextBlendIdx;
	g_render.nextBlendIdx = (sub + 1) % g_render.toBlend;
	bool accumulate = g_render.toBlend == 1 || (sub >= g_render.toBlendStart && sub < g_render.toBlendEnd);
	bool emit = sub == g_render.toBlend - 1;
	// Subframes outside the shutter don't need to be captured at all
	if (!accumulate && !emit) return;

	if (g_render.useGpu) {
		int64_t t0 = RendererCore::NowMicros();
		RendererGpu::Capture(sub, accumulate, sub == g_render.toBlendStart, emit);
		g_render.captureUs += RendererCore::NowMicros() - t0;
	} else {
		captureCpu(accumulate, emit, sub);
	}
}

// }}}

ON_EVENT(DEMO_STOP) {
	if (g_render.isRendering.load() && sar_render_autostop.GetBool() && !sar_render_merge.GetBool()) {
		stopRender(false);
	}
}

// Init {{{

void Renderer::Init(void **videomode) {
	g_videomode = videomode;
#ifdef _WIN32
	g_mainThreadId = GetCurrentThreadId();
#endif

	snd_surround_speakers = Variable("snd_surround_speakers");

	SND_RecordBuffer = (void (*)())Memory::Scan(engine->Name(), Offsets::SND_RecordBuffer);
#ifdef _WIN32
	g_movieInfo = Memory::Deref<MovieInfo_t *>((uintptr_t)SND_RecordBuffer + Offsets::g_movieInfo);
#else
	uintptr_t base = Memory::Read((uintptr_t)SND_RecordBuffer + Offsets::SND_IsRecording);
	if (sar.game->Is(SourceGame_Portal2)) base = (uintptr_t)SND_RecordBuffer;
	g_movieInfo = Memory::Deref<MovieInfo_t *>(base + Offsets::g_movieInfo);
#endif

	g_RecordBufferHook.SetFunc(SND_RecordBuffer);

	g_snd_linear_count = *(int **)((uintptr_t)SND_RecordBuffer + Offsets::snd_linear_count);
	g_snd_p = *(int ***)((uintptr_t)SND_RecordBuffer + Offsets::snd_p);
	g_snd_vol = *(int **)((uintptr_t)SND_RecordBuffer + Offsets::snd_vol);

	Command::Hook("startmovie", &startmovie_cbk, startmovie_origCbk);
	Command::Hook("endmovie", &endmovie_cbk, endmovie_origCbk);

	hookWriteMovieFrame();
	hookXInput();
}

void Renderer::Cleanup() {
	unhookHudClock();
	unhookWriteMovieFrame();
	g_RecordBufferHook.Disable();
	Command::Unhook("startmovie", startmovie_origCbk);
	Command::Unhook("endmovie", endmovie_origCbk);
}

bool Renderer::DeferQuit(const char *cmd) {
	if (!g_render.sessionActive.load()) return false;
	if (g_render.isRendering.load()) stopRender(false);
	console->Print("Finishing the render before running '%s'...\n", cmd);
	g_render.deferredQuit = cmd;
	return true;
}

bool Renderer::IsRunning() {
	return g_render.isRendering.load();
}

// }}}

// Commands {{{

CON_COMMAND(sar_render_start, "sar_render_start <file> - start rendering frames to the given video file\n") {
	if (args.ArgC() != 2) {
		console->Print(sar_render_start.ThisPtr()->m_pszHelpString);
		return;
	}

	if (g_render.isRendering.load()) {
		console->Print("Already rendering\n");
		return;
	}

	g_render.filename = std::string(args[1]);

	startRender();
}

CON_COMMAND(sar_render_finish, "sar_render_finish - stop rendering frames\n") {
	if (args.ArgC() != 1) {
		console->Print(sar_render_finish.ThisPtr()->m_pszHelpString);
		return;
	}

	if (!g_render.isRendering.load()) {
		console->Print("Not rendering!\n");
		return;
	}

	stopRender(false);
}

// }}}
