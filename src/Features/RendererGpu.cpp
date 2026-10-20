#include "RendererGpu.hpp"

#include "RendererInternal.hpp"

#ifndef _WIN32

// The GPU path is D3D9 only; Linux renders keep the CPU read-back path.
bool RendererGpu::CanUse(int, int, AVPixelFormat, const char **why) {
	if (why) *why = "not supported on this platform";
	return false;
}
bool RendererGpu::Begin(const Params &) { return false; }
void RendererGpu::Capture(int, bool, bool, bool) {}
void RendererGpu::Finish() {}
bool RendererGpu::Failed() { return false; }
bool RendererGpu::Busy() { return false; }
bool RendererGpu::SetSkipPresent(bool) { return false; }
void RendererGpu::Shutdown() {}

#else

#	include "Modules/Console.hpp"
#	include "Modules/MaterialSystem.hpp"
#	include "SAR.hpp"
#	include "Utils/Platform.hpp"

#	include <Windows.h>
#	include <d3d9.h>

#	include <atomic>
#	include <cstring>
#	include <string>
#	include <vector>

#	include "RendererShaders.hpp"
#	include "../../lib/nvenc/nvEncodeAPI.h"

// IMatRenderContext::GetCallQueue (Portal 2 engine branch). The queued
// context returns its call queue (lea eax,[ecx+X]; ret), the immediate one
// returns null (xor eax,eax; ret); anything else means the offset is wrong.
#	define GET_CALL_QUEUE_INDEX 142

namespace {
	enum class Pass { Y, C, UV, ACC_FIRST, ACC, COPY, COUNT };

	enum SlotState { SLOT_FREE, SLOT_RENDERED, SLOT_COPYING };

	struct Slot {
		IDirect3DTexture9 *tex[3];
		IDirect3DSurface9 *rt[3];
		IDirect3DSurface9 *sys[3];
		IDirect3DQuery9 *query;
		SlotState state;
		int64_t renderedTick;
	};

	const int MAX_SLOTS = 8;

	// Render-thread state (only touched from queued functors / Reset)
	struct {
		RendererGpu::Params p;
		bool active;
		bool broken;  // an error happened this session; just drain
		int nplanes;
		int planeW[3], planeH[3];  // in A8R8G8B8 texels
		D3DFORMAT copyFormat;

		IDirect3DTexture9 *copyTex;
		IDirect3DSurface9 *copySurf;
		IDirect3DTexture9 *frameTex;
		IDirect3DSurface9 *frameSurf;
		IDirect3DTexture9 *accTex[2];
		IDirect3DSurface9 *accSurf[2];
		int accCur;

		Slot slots[MAX_SLOTS];
		int nslots, head, count;
		int64_t tick;

		IDirect3DPixelShader9 *ps[(int)Pass::COUNT];
		IDirect3DDevice9 *psDevice;
		IDirect3DStateBlock9 *sb;

		int64_t runUs, waitUs;
		int emitted;
	} g_gpu;

	IDirect3DDevice9 *g_device;
	std::atomic<bool> g_failed{false};
	std::atomic<int> g_pending{0};
	std::atomic<bool> g_sessionOpen{false};

	using ResetFn = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
	ResetFn Reset_Target;
	ResetFn Reset_Trampoline;

	// While rendering, frames don't need to reach the window. Presenting
	// them (and the driver throttling on it) was most of the render
	// thread's time, so Present is skipped; a flush keeps the GPU fed.
	using PresentFn = HRESULT(WINAPI *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
	PresentFn Present_Target;
	PresentFn Present_Trampoline;
	std::atomic<bool> g_skipPresent{false};
	IDirect3DQuery9 *g_flushQuery;
	IDirect3DDevice9 *g_flushQueryDevice;

	template <typename T>
	void safeRelease(T *&p) {
		if (p) {
			p->Release();
			p = nullptr;
		}
	}

	void fail(const char *what, HRESULT hr = S_OK) {
		char buf[256];
		snprintf(buf, sizeof buf, "GPU capture: %s failed (0x%08lX)", what, (unsigned long)hr);
		RendererCore::ReportError(buf);
		g_gpu.broken = true;
		g_failed.store(true);
	}

	// Device discovery {{{

	struct Range {
		uintptr_t lo = 0, hi = 0;
		bool Has(uintptr_t a) const { return a >= lo && a < hi; }
	};

	struct ModuleInfo {
		Range image;
		Range exec[8];
		int nexec = 0;
		Range data[8];
		int ndata = 0;
		bool InExec(uintptr_t a) const {
			for (int i = 0; i < nexec; ++i)
				if (exec[i].Has(a)) return true;
			return false;
		}
	};

	bool readModule(HMODULE mod, ModuleInfo &info) {
		if (!mod) return false;
		auto base = (uintptr_t)mod;
		auto dos = (IMAGE_DOS_HEADER *)mod;
		auto nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
		info.image = {base, base + nt->OptionalHeader.SizeOfImage};
		IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
		for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
			Range r{base + sec[i].VirtualAddress, base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize};
			if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) && info.nexec < 8) info.exec[info.nexec++] = r;
			if ((sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) && info.ndata < 8) info.data[info.ndata++] = r;
		}
		return true;
	}

	const char *moduleOf(uintptr_t a, char *buf, size_t n) {
		HMODULE m = nullptr;
		buf[0] = 0;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
			char path[MAX_PATH];
			if (GetModuleFileNameA(m, path, MAX_PATH)) {
				const char *slash = strrchr(path, '\\');
				snprintf(buf, n, "%s", slash ? slash + 1 : path);
			}
		}
		if (!buf[0]) snprintf(buf, n, "?");
		return buf;
	}

	// Is `p` a D3D9 device? `strict` (used when scanning unknown memory)
	// only calls into objects whose vtable lives in the D3D9 module; a
	// pointer from a known device global may also be wrapped/hooked.
	IDirect3DDevice9 *probeDevice(uintptr_t p, const ModuleInfo &d3d, bool strict, const char **reason) {
		__try {
			if (p < 0x10000 || (p & 3)) return *reason = "not a pointer", nullptr;
			uintptr_t vt = *(uintptr_t *)p;
			if (strict && !d3d.image.Has(vt)) return *reason = "vtable outside the D3D9 module", nullptr;
			uintptr_t *fns = (uintptr_t *)vt;
			for (int i = 0; i < 3; ++i) {
				if (strict && !d3d.InExec(fns[i])) return *reason = "IUnknown outside the D3D9 module", nullptr;
				HMODULE m = nullptr;
				if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fns[i], &m) || !m) {
					return *reason = "IUnknown not in any module", nullptr;
				}
			}
			void *out = nullptr;
			if (FAILED(((IUnknown *)p)->QueryInterface(__uuidof(IDirect3DDevice9), &out)) || !out) return *reason = "QueryInterface(IDirect3DDevice9) failed", nullptr;
			auto dev = (IDirect3DDevice9 *)out;
			dev->Release();  // the game owns it; we only borrow the pointer
			return dev;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			*reason = "access violation";
			return nullptr;
		}
	}

	// Pattern scan over a module's code; '?' bytes are wildcards
	uintptr_t findPattern(const ModuleInfo &m, const char *pattern) {
		std::vector<int> bytes;
		for (const char *c = pattern; *c;) {
			if (*c == ' ') {
				++c;
			} else if (*c == '?') {
				bytes.push_back(-1);
				while (*c == '?') ++c;
			} else {
				bytes.push_back((int)strtol(c, (char **)&c, 16));
			}
		}
		for (int i = 0; i < m.nexec; ++i) {
			const uint8_t *lo = (const uint8_t *)m.exec[i].lo, *hi = (const uint8_t *)m.exec[i].hi;
			for (const uint8_t *q = lo; q + bytes.size() <= hi; ++q) {
				size_t k = 0;
				while (k < bytes.size() && (bytes[k] < 0 || q[k] == bytes[k])) ++k;
				if (k == bytes.size()) return (uintptr_t)q;
			}
		}
		return 0;
	}

	// The shader API keeps the device in a global. Find it through the code
	// that stores it (Portal 2 shaderapidx9/shaderapivk), falling back to
	// scanning the shader API's data for any pointer to a device.
	IDirect3DDevice9 *findDevice(const char **why) {
		HMODULE shaderApi = GetModuleHandleA("shaderapivk.dll");
		HMODULE d3dMod = nullptr;
		if (shaderApi) {
			d3dMod = GetModuleHandleA("dxvk_d3d9.dll");
		} else {
			shaderApi = GetModuleHandleA("shaderapidx9.dll");
			d3dMod = GetModuleHandleA("d3d9.dll");
		}
		ModuleInfo api, d3d;
		if (!readModule(shaderApi, api) || !readModule(d3dMod, d3d)) {
			*why = "D3D9 shader API not loaded";
			return nullptr;
		}

		const char *reason = "";
		// push ebx; mov ecx,esi; call ..; push ..; mov ecx,esi; mov [g_pD3DDevice],ebx
		uintptr_t store = findPattern(api, "53 8B CE E8 ? ? ? ? 68 ? ? ? ? 8B CE 89 1D ? ? ? ? E8 ? ? ? ? 8B 55 08");
		if (store) {
			uintptr_t global = *(uintptr_t *)(store + 17);
			if (api.image.Has(global)) {
				uintptr_t v = *(uintptr_t *)global;
				if (auto dev = probeDevice(v, d3d, false, &reason)) return dev;
				char a[64], b[64];
				uintptr_t vt = v >= 0x10000 ? *(uintptr_t *)v : 0;
				console->Print("GPU capture: device global %p -> %p (vtable %p in %s, QI in %s): %s\n", (void *)global, (void *)v, (void *)vt, moduleOf(vt, a, sizeof a), vt ? moduleOf(*(uintptr_t *)vt, b, sizeof b) : "-", reason);
			}
		}

		for (int s = 0; s < api.ndata; ++s) {
			for (uintptr_t a = api.data[s].lo; a + 4 <= api.data[s].hi; a += 4) {
				uintptr_t v = *(uintptr_t *)a;
				if (v < 0x10000 || (v & 3) || api.image.Has(v)) continue;
				if (auto dev = probeDevice(v, d3d, true, &reason)) return dev;
			}
		}
		*why = "D3D9 device not found";
		return nullptr;
	}

	// }}}

	// Resources {{{

	void releaseSessionResources() {
		safeRelease(g_gpu.copySurf);
		safeRelease(g_gpu.copyTex);
		safeRelease(g_gpu.frameSurf);
		safeRelease(g_gpu.frameTex);
		for (int i = 0; i < 2; ++i) {
			safeRelease(g_gpu.accSurf[i]);
			safeRelease(g_gpu.accTex[i]);
		}
		for (int s = 0; s < MAX_SLOTS; ++s) {
			Slot &sl = g_gpu.slots[s];
			for (int p = 0; p < 3; ++p) {
				safeRelease(sl.rt[p]);
				safeRelease(sl.tex[p]);
				safeRelease(sl.sys[p]);
			}
			safeRelease(sl.query);
			sl.state = SLOT_FREE;
		}
		g_gpu.head = g_gpu.count = 0;
		safeRelease(g_gpu.sb);
	}

	void releaseShaders() {
		for (auto &ps : g_gpu.ps) safeRelease(ps);
		g_gpu.psDevice = nullptr;
	}

	bool createShaders(IDirect3DDevice9 *dev) {
		if (g_gpu.psDevice == dev) return true;
		releaseShaders();
		const DWORD *code[(int)Pass::COUNT] = {
			(const DWORD *)g_ps_y,
			(const DWORD *)g_ps_c,
			(const DWORD *)g_ps_uv,
			(const DWORD *)g_ps_acc_first,
			(const DWORD *)g_ps_acc,
			(const DWORD *)g_ps_copy,
		};
		for (int i = 0; i < (int)Pass::COUNT; ++i) {
			HRESULT hr = dev->CreatePixelShader(code[i], &g_gpu.ps[i]);
			if (FAILED(hr)) {
				fail("CreatePixelShader", hr);
				releaseShaders();
				return false;
			}
		}
		g_gpu.psDevice = dev;
		return true;
	}

	bool createRT(IDirect3DDevice9 *dev, int w, int h, D3DFORMAT fmt, IDirect3DTexture9 **tex, IDirect3DSurface9 **surf, const char *what) {
		HRESULT hr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, tex, NULL);
		if (FAILED(hr)) {
			fail(what, hr);
			return false;
		}
		hr = (*tex)->GetSurfaceLevel(0, surf);
		if (FAILED(hr)) {
			fail(what, hr);
			return false;
		}
		return true;
	}

	bool createSessionResources(IDirect3DDevice9 *dev) {
		const RendererGpu::Params &p = g_gpu.p;
		if (!createShaders(dev)) return false;

		HRESULT hr = dev->CreateStateBlock(D3DSBT_ALL, &g_gpu.sb);
		if (FAILED(hr)) return fail("CreateStateBlock", hr), false;

		IDirect3DSurface9 *bb = nullptr;
		hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
		if (FAILED(hr)) return fail("GetBackBuffer", hr), false;
		D3DSURFACE_DESC desc;
		bb->GetDesc(&desc);
		bb->Release();
		g_gpu.copyFormat = desc.Format;
		if ((int)desc.Width != p.width || (int)desc.Height != p.height) {
			return fail("back buffer size check"), false;
		}

		if (!createRT(dev, p.width, p.height, g_gpu.copyFormat, &g_gpu.copyTex, &g_gpu.copySurf, "back buffer copy target")) return false;

		if (p.toBlend > 1) {
			if (!createRT(dev, p.width, p.height, D3DFMT_A8R8G8B8, &g_gpu.frameTex, &g_gpu.frameSurf, "blend output target")) return false;
			for (int i = 0; i < 2; ++i) {
				hr = dev->CreateTexture(p.width, p.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &g_gpu.accTex[i], NULL);
				if (FAILED(hr)) {
					hr = dev->CreateTexture(p.width, p.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_gpu.accTex[i], NULL);
				}
				if (FAILED(hr)) return fail("blend accumulator", hr), false;
				g_gpu.accTex[i]->GetSurfaceLevel(0, &g_gpu.accSurf[i]);
			}
			g_gpu.accCur = 0;
		}

		if (p.pixFmt == AV_PIX_FMT_NV12) {
			g_gpu.nplanes = 2;
			g_gpu.planeW[0] = p.width / 4;
			g_gpu.planeH[0] = p.height;
			g_gpu.planeW[1] = p.width / 4;
			g_gpu.planeH[1] = p.height / 2;
		} else {
			g_gpu.nplanes = 3;
			g_gpu.planeW[0] = p.width / 4;
			g_gpu.planeH[0] = p.height;
			g_gpu.planeW[1] = g_gpu.planeW[2] = p.width / 8;
			g_gpu.planeH[1] = g_gpu.planeH[2] = p.height / 2;
		}

		g_gpu.head = g_gpu.count = 0;
		g_gpu.tick = 0;
		if (p.nvenc.enabled) return true;

		g_gpu.nslots = p.slots < 2 ? 2 : p.slots > MAX_SLOTS ? MAX_SLOTS : p.slots;
		for (int s = 0; s < g_gpu.nslots; ++s) {
			Slot &sl = g_gpu.slots[s];
			for (int i = 0; i < g_gpu.nplanes; ++i) {
				if (!createRT(dev, g_gpu.planeW[i], g_gpu.planeH[i], D3DFMT_A8R8G8B8, &sl.tex[i], &sl.rt[i], "plane target")) return false;
				hr = dev->CreateOffscreenPlainSurface(g_gpu.planeW[i], g_gpu.planeH[i], D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sl.sys[i], NULL);
				if (FAILED(hr)) return fail("staging surface", hr), false;
			}
			hr = dev->CreateQuery(D3DQUERYTYPE_EVENT, &sl.query);
			if (FAILED(hr)) return fail("CreateQuery", hr), false;
			sl.state = SLOT_FREE;
		}
		g_gpu.head = g_gpu.count = 0;
		g_gpu.tick = 0;
		return true;
	}

	// }}}

	// Drawing {{{

	// Saves everything our passes touch and puts it back afterwards, so the
	// shader API's cached device state stays valid.
	struct StateSave {
		IDirect3DDevice9 *dev;
		IDirect3DSurface9 *rt[4] = {};
		IDirect3DSurface9 *ds = nullptr;
		D3DVIEWPORT9 vp;

		bool active;

		// inactive: nothing to save (no draw calls will be made)
		StateSave(IDirect3DDevice9 *d, bool active)
			: dev(d)
			, active(active) {
			if (!active) return;
			g_gpu.sb->Capture();
			for (DWORD i = 0; i < 4; ++i) {
				if (FAILED(dev->GetRenderTarget(i, &rt[i]))) rt[i] = nullptr;
			}
			if (FAILED(dev->GetDepthStencilSurface(&ds))) ds = nullptr;
			dev->GetViewport(&vp);
		}
		~StateSave() {
			if (!active) return;
			for (DWORD i = 0; i < 4; ++i) {
				if (i == 0 || rt[i]) dev->SetRenderTarget(i, rt[i]);
				else dev->SetRenderTarget(i, NULL);
				safeRelease(rt[i]);
			}
			dev->SetDepthStencilSurface(ds);
			safeRelease(ds);
			g_gpu.sb->Apply();
			dev->SetViewport(&vp);
		}
	};

	void setSampler(IDirect3DDevice9 *dev, DWORD s, bool linear) {
		D3DTEXTUREFILTERTYPE f = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
		dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		dev->SetSamplerState(s, D3DSAMP_MAGFILTER, f);
		dev->SetSamplerState(s, D3DSAMP_MINFILTER, f);
		dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
		dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
		dev->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
		dev->SetSamplerState(s, D3DSAMP_MAXANISOTROPY, 1);
	}

	void setCommonState(IDirect3DDevice9 *dev) {
		dev->SetVertexShader(NULL);
		dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		dev->SetDepthStencilSurface(NULL);
		for (DWORD i = 1; i < 4; ++i) dev->SetRenderTarget(i, NULL);
		dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		dev->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
		dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
		dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
		dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
		dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
		for (DWORD s = 0; s < 2; ++s) {
			dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, s);
			dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		}
	}

	void drawPass(IDirect3DDevice9 *dev, Pass pass, IDirect3DSurface9 *target, int w, int h, IDirect3DBaseTexture9 *t0, bool linear0, IDirect3DBaseTexture9 *t1, const float *consts, int nconsts) {
		dev->SetRenderTarget(0, target);
		D3DVIEWPORT9 vp = {0, 0, (DWORD)w, (DWORD)h, 0.0f, 1.0f};
		dev->SetViewport(&vp);
		dev->SetPixelShader(g_gpu.ps[(int)pass]);
		dev->SetTexture(0, t0);
		setSampler(dev, 0, linear0);
		dev->SetTexture(1, t1);
		if (t1) setSampler(dev, 1, false);
		dev->SetPixelShaderConstantF(0, consts, nconsts);

		// Pretransformed quad; the -0.5 lines pixel centres up with texel
		// centres (D3D9 rasterization rules). TEXCOORD0 = pixel coord + 0.5.
		struct Vtx {
			float x, y, z, rhw, u, v;
		};
		float fw = (float)w, fh = (float)h;
		Vtx quad[4] = {
			{-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
			{fw - 0.5f, -0.5f, 0.0f, 1.0f, fw, 0.0f},
			{-0.5f, fh - 0.5f, 0.0f, 1.0f, 0.0f, fh},
			{fw - 0.5f, fh - 0.5f, 0.0f, 1.0f, fw, fh},
		};
		dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vtx));
	}

	// BT.709 limited-range chroma coefficients (+ offset in w)
	const float kCb[4] = {-0.114572f * 224.0f / 255.0f, -0.385428f * 224.0f / 255.0f, 0.5f * 224.0f / 255.0f, 128.0f / 255.0f};
	const float kCr[4] = {0.5f * 224.0f / 255.0f, -0.454153f * 224.0f / 255.0f, -0.045847f * 224.0f / 255.0f, 128.0f / 255.0f};

	void convertInto(IDirect3DDevice9 *dev, IDirect3DTexture9 *src, Slot &sl) {
		const RendererGpu::Params &p = g_gpu.p;
		float c[12] = {1.0f / p.width, 1.0f / p.height, 0.0f, 0.0f};
		drawPass(dev, Pass::Y, sl.rt[0], g_gpu.planeW[0], g_gpu.planeH[0], src, false, nullptr, c, 1);
		if (g_gpu.nplanes == 2) {
			memcpy(c + 4, kCb, sizeof kCb);
			memcpy(c + 8, kCr, sizeof kCr);
			drawPass(dev, Pass::UV, sl.rt[1], g_gpu.planeW[1], g_gpu.planeH[1], src, true, nullptr, c, 3);
		} else {
			memcpy(c + 4, kCb, sizeof kCb);
			drawPass(dev, Pass::C, sl.rt[1], g_gpu.planeW[1], g_gpu.planeH[1], src, true, nullptr, c, 2);
			memcpy(c + 4, kCr, sizeof kCr);
			drawPass(dev, Pass::C, sl.rt[2], g_gpu.planeW[2], g_gpu.planeH[2], src, true, nullptr, c, 2);
		}
	}

	// }}}

	// Read-back ring {{{

	void issueCopy(IDirect3DDevice9 *dev, Slot &sl) {
		for (int i = 0; i < g_gpu.nplanes; ++i) {
			HRESULT hr = dev->GetRenderTargetData(sl.rt[i], sl.sys[i]);
			if (FAILED(hr)) {
				fail("GetRenderTargetData", hr);
				break;
			}
		}
		sl.query->Issue(D3DISSUE_END);
		sl.state = SLOT_COPYING;
	}

	void readOut(Slot &sl) {
		AVFrame *f = g_gpu.broken ? nullptr : RendererCore::AcquireVideoFrame();
		if (f) {
			for (int i = 0; i < g_gpu.nplanes; ++i) {
				D3DLOCKED_RECT lr;
				HRESULT hr = sl.sys[i]->LockRect(&lr, NULL, D3DLOCK_READONLY);
				if (FAILED(hr)) {
					fail("LockRect", hr);
					RendererCore::ReturnVideoFrame(f);
					f = nullptr;
					break;
				}
				size_t rowBytes = (size_t)g_gpu.planeW[i] * 4;
				const uint8_t *src = (const uint8_t *)lr.pBits;
				uint8_t *dst = f->data[i];
				for (int y = 0; y < g_gpu.planeH[i]; ++y) {
					memcpy(dst + (size_t)y * f->linesize[i], src + (size_t)y * lr.Pitch, rowBytes);
				}
				sl.sys[i]->UnlockRect();
			}
			if (f) {
				RendererCore::SubmitVideoFrame(f);
				++g_gpu.emitted;
			}
		}
		sl.state = SLOT_FREE;
	}

	void waitQuery(Slot &sl) {
		int64_t t0 = RendererCore::NowMicros();
		while (true) {
			HRESULT hr = sl.query->GetData(NULL, 0, D3DGETDATA_FLUSH);
			if (hr != S_FALSE) break;  // S_OK, or device lost / error: don't hang
			SwitchToThread();
		}
		g_gpu.waitUs += RendererCore::NowMicros() - t0;
	}

	// Moves the ring forward without waiting
	void pump(IDirect3DDevice9 *dev) {
		// Finished copies leave in order
		while (g_gpu.count > 0) {
			Slot &sl = g_gpu.slots[g_gpu.head];
			if (sl.state != SLOT_COPYING) break;
			if (sl.query->GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) break;
			readOut(sl);
			g_gpu.head = (g_gpu.head + 1) % g_gpu.nslots;
			--g_gpu.count;
		}
		// Start copies for frames rendered on an earlier tick; by then the GPU
		// has normally finished them, so the copy doesn't stall the CPU
		for (int i = 0; i < g_gpu.count; ++i) {
			Slot &sl = g_gpu.slots[(g_gpu.head + i) % g_gpu.nslots];
			if (sl.state == SLOT_RENDERED && sl.renderedTick < g_gpu.tick) issueCopy(dev, sl);
		}
	}

	// Retires the oldest slot, waiting for the GPU if needed
	void retireOldest(IDirect3DDevice9 *dev) {
		Slot &sl = g_gpu.slots[g_gpu.head];
		if (sl.state == SLOT_RENDERED) issueCopy(dev, sl);
		waitQuery(sl);
		readOut(sl);
		g_gpu.head = (g_gpu.head + 1) % g_gpu.nslots;
		--g_gpu.count;
	}

	Slot &newSlot(IDirect3DDevice9 *dev) {
		if (g_gpu.count == g_gpu.nslots) retireOldest(dev);
		Slot &sl = g_gpu.slots[(g_gpu.head + g_gpu.count) % g_gpu.nslots];
		++g_gpu.count;
		return sl;
	}

	// }}}

	// NVENC (zero-copy hardware encoding) {{{

	// The back buffer is copied (StretchRect, which also resolves MSAA) into
	// a ring of render targets registered with NVENC, which converts the
	// ARGB input to YUV itself (BT.709, matching the VUI we write) and
	// encodes it. Nothing is read back; only the compressed bitstream leaves
	// the GPU. Frames move through the ring as:
	//   FREE -> RENDERED (copy queued, event query issued)
	//        -> ENCODING (query done: mapped + submitted to NVENC)
	//        -> FREE (bitstream locked, packet sent to the muxer, unmapped)
	// Everything runs on the render thread (the D3D9 device's thread).

	enum NvState { NV_FREE, NV_RENDERED, NV_ENCODING };

	struct NvSlot {
		IDirect3DSurface9 *rt;
		NV_ENC_REGISTERED_PTR reg;
		NV_ENC_INPUT_PTR mapped;
		NV_ENC_OUTPUT_PTR bs;
		IDirect3DQuery9 *query;
		NvState state;
		uint64_t pts;
	};

	const int NV_MAX_SLOTS = 32;

	struct {
		HMODULE lib;
		NV_ENCODE_API_FUNCTION_LIST fn;
		void *enc;
		NvSlot slots[NV_MAX_SLOTS];
		int nslots, head, count;  // FIFO over the non-free slots
		int encoding;             // slots in NV_ENCODING (always the oldest)
		int delay;                // encoded frames kept in flight before locking
		int bframes;
		uint64_t nextPts;
		int64_t outIndex;
		int64_t bytes;
	} g_nv;

	bool nvLoad(const char **why) {
		if (g_nv.lib) return true;
		HMODULE lib = LoadLibraryA("nvEncodeAPI.dll");
		if (!lib) return *why = "nvEncodeAPI.dll not found (no NVIDIA driver)", false;
		auto getMax = (NVENCSTATUS(NVENCAPI *)(uint32_t *))GetProcAddress(lib, "NvEncodeAPIGetMaxSupportedVersion");
		auto create = (NVENCSTATUS(NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST *))GetProcAddress(lib, "NvEncodeAPICreateInstance");
		uint32_t maxv = 0;
		if (!getMax || !create || getMax(&maxv) != NV_ENC_SUCCESS) {
			FreeLibrary(lib);
			return *why = "nvEncodeAPI.dll is unusable", false;
		}
		if (maxv < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION)) {
			FreeLibrary(lib);
			return *why = "NVIDIA driver too old (needs NVENC API 12.0, driver 522 or newer)", false;
		}
		NV_ENCODE_API_FUNCTION_LIST fn = {NV_ENCODE_API_FUNCTION_LIST_VER};
		if (create(&fn) != NV_ENC_SUCCESS) {
			FreeLibrary(lib);
			return *why = "NvEncodeAPICreateInstance failed", false;
		}
		g_nv.lib = lib;
		g_nv.fn = fn;
		return true;
	}

	void nvFail(const char *what, NVENCSTATUS st) {
		char buf[384];
		const char *detail = g_nv.enc && g_nv.fn.nvEncGetLastErrorString ? g_nv.fn.nvEncGetLastErrorString(g_nv.enc) : "";
		snprintf(buf, sizeof buf, "NVENC: %s failed (%d) %s", what, (int)st, detail ? detail : "");
		RendererCore::ReportError(buf);
		g_gpu.broken = true;
		g_failed.store(true);
	}

	void nvDestroy() {
		auto &fn = g_nv.fn;
		for (int i = 0; i < NV_MAX_SLOTS; ++i) {
			NvSlot &sl = g_nv.slots[i];
			if (g_nv.enc) {
				if (sl.mapped) fn.nvEncUnmapInputResource(g_nv.enc, sl.mapped);
				if (sl.reg) fn.nvEncUnregisterResource(g_nv.enc, sl.reg);
				if (sl.bs) fn.nvEncDestroyBitstreamBuffer(g_nv.enc, sl.bs);
			}
			safeRelease(sl.rt);
			safeRelease(sl.query);
			sl = NvSlot{};
		}
		if (g_nv.enc) fn.nvEncDestroyEncoder(g_nv.enc);
		g_nv.enc = nullptr;
		g_nv.head = g_nv.count = g_nv.encoding = 0;
	}

	bool nvOpen(IDirect3DDevice9 *dev) {
		const RendererGpu::Params &p = g_gpu.p;
		const char *why = nullptr;
		if (!nvLoad(&why)) {
			RendererCore::ReportError(why);
			g_gpu.broken = true;
			g_failed.store(true);
			return false;
		}
		auto &fn = g_nv.fn;
		NVENCSTATUS st;

		NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op = {NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER};
		op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
		op.device = dev;
		op.apiVersion = NVENCAPI_VERSION;
		st = fn.nvEncOpenEncodeSessionEx(&op, &g_nv.enc);
		if (st != NV_ENC_SUCCESS) return nvFail("opening a session on the D3D9 device", st), false;

		GUID codec = p.nvenc.hevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
		auto cap = [&](NV_ENC_CAPS c) {
			NV_ENC_CAPS_PARAM cp = {NV_ENC_CAPS_PARAM_VER};
			cp.capsToQuery = c;
			int v = 0;
			fn.nvEncGetEncodeCaps(g_nv.enc, codec, &cp, &v);
			return v;
		};
		if (cap(NV_ENC_CAPS_WIDTH_MAX) < p.width || cap(NV_ENC_CAPS_HEIGHT_MAX) < p.height) {
			return nvFail("resolution check (too large for this GPU)", NV_ENC_ERR_UNSUPPORTED_PARAM), false;
		}
		int maxB = cap(NV_ENC_CAPS_NUM_MAX_BFRAMES);
		bool lookahead = cap(NV_ENC_CAPS_SUPPORT_LOOKAHEAD) != 0;

		static const GUID presets[7] = {NV_ENC_PRESET_P1_GUID, NV_ENC_PRESET_P2_GUID, NV_ENC_PRESET_P3_GUID, NV_ENC_PRESET_P4_GUID, NV_ENC_PRESET_P5_GUID, NV_ENC_PRESET_P6_GUID, NV_ENC_PRESET_P7_GUID};
		int pi = p.nvenc.preset < 1 ? 1 : p.nvenc.preset > 7 ? 7 : p.nvenc.preset;
		GUID preset = presets[pi - 1];

		NV_ENC_PRESET_CONFIG pc = {NV_ENC_PRESET_CONFIG_VER, {NV_ENC_CONFIG_VER}};
		st = fn.nvEncGetEncodePresetConfigEx(g_nv.enc, codec, preset, NV_ENC_TUNING_INFO_HIGH_QUALITY, &pc);
		if (st != NV_ENC_SUCCESS) return nvFail("reading the preset", st), false;

		NV_ENC_CONFIG cfg = pc.presetCfg;
		g_nv.bframes = p.nvenc.bframes < maxB ? p.nvenc.bframes : maxB;
		if (g_nv.bframes < 0) g_nv.bframes = 0;
		cfg.gopLength = p.nvenc.gop;
		cfg.frameIntervalP = g_nv.bframes + 1;
		cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
		cfg.rcParams.averageBitRate = 0;
		cfg.rcParams.maxBitRate = 0;
		cfg.rcParams.targetQuality = (uint8_t)p.nvenc.cq;
		cfg.rcParams.targetQualityLSB = 0;
		cfg.rcParams.enableAQ = 1;
		if (p.nvenc.multipass >= 0) {
			cfg.rcParams.multiPass = p.nvenc.multipass == 0 ? NV_ENC_MULTI_PASS_DISABLED : p.nvenc.multipass == 1 ? NV_ENC_TWO_PASS_QUARTER_RESOLUTION : NV_ENC_TWO_PASS_FULL_RESOLUTION;
		}
		// Set explicitly: some presets (P7) turn look-ahead on themselves,
		// and the output delay below has to account for it
		int laDepth = 0;
		if (lookahead && p.nvenc.lookahead > 0) {
			laDepth = p.nvenc.lookahead;
			cfg.rcParams.enableLookahead = 1;
			cfg.rcParams.lookaheadDepth = (uint16_t)laDepth;
		} else {
			cfg.rcParams.enableLookahead = 0;
			cfg.rcParams.lookaheadDepth = 0;
		}

		NV_ENC_CONFIG_H264_VUI_PARAMETERS *vui;
		if (p.nvenc.hevc) {
			cfg.encodeCodecConfig.hevcConfig.idrPeriod = p.nvenc.gop;
			vui = &cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters;
		} else {
			cfg.encodeCodecConfig.h264Config.idrPeriod = p.nvenc.gop;
			vui = &cfg.encodeCodecConfig.h264Config.h264VUIParameters;
		}
		// BT.709 limited range, like the other paths
		vui->videoSignalTypePresentFlag = 1;
		vui->videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
		vui->videoFullRangeFlag = 0;
		vui->colourDescriptionPresentFlag = 1;
		vui->colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
		vui->transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
		vui->colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;

		NV_ENC_INITIALIZE_PARAMS ip = {NV_ENC_INITIALIZE_PARAMS_VER};
		ip.encodeGUID = codec;
		ip.presetGUID = preset;
		ip.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
		ip.encodeWidth = ip.darWidth = ip.maxEncodeWidth = p.width;
		ip.encodeHeight = ip.darHeight = ip.maxEncodeHeight = p.height;
		ip.frameRateNum = p.nvenc.fps;
		ip.frameRateDen = 1;
		ip.enablePTD = 1;
		ip.encodeConfig = &cfg;
		st = fn.nvEncInitializeEncoder(g_nv.enc, &ip);
		if (st != NV_ENC_SUCCESS) return nvFail("initializing the encoder", st), false;

		// NVIDIA's own pipelining: B-frames + look-ahead + 3 frames in flight
		g_nv.delay = (g_nv.bframes + 1) + laDepth + 2;
		g_nv.nslots = g_nv.delay + 4;
		if (g_nv.nslots > NV_MAX_SLOTS) g_nv.nslots = NV_MAX_SLOTS;

		D3DFORMAT fmt = g_gpu.copyFormat == D3DFMT_X8R8G8B8 || g_gpu.copyFormat == D3DFMT_A8R8G8B8 ? g_gpu.copyFormat : D3DFMT_A8R8G8B8;
		for (int i = 0; i < g_nv.nslots; ++i) {
			NvSlot &sl = g_nv.slots[i];
			HRESULT hr = dev->CreateRenderTarget(p.width, p.height, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &sl.rt, NULL);
			if (FAILED(hr)) return fail("NVENC input surface", hr), false;
			hr = dev->CreateQuery(D3DQUERYTYPE_EVENT, &sl.query);
			if (FAILED(hr)) return fail("CreateQuery", hr), false;
			NV_ENC_REGISTER_RESOURCE rr = {NV_ENC_REGISTER_RESOURCE_VER};
			rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
			rr.resourceToRegister = sl.rt;
			rr.width = p.width;
			rr.height = p.height;
			rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
			rr.bufferUsage = NV_ENC_INPUT_IMAGE;
			st = fn.nvEncRegisterResource(g_nv.enc, &rr);
			if (st != NV_ENC_SUCCESS) return nvFail("registering an input surface", st), false;
			sl.reg = rr.registeredResource;
			NV_ENC_CREATE_BITSTREAM_BUFFER cb = {NV_ENC_CREATE_BITSTREAM_BUFFER_VER};
			st = fn.nvEncCreateBitstreamBuffer(g_nv.enc, &cb);
			if (st != NV_ENC_SUCCESS) return nvFail("creating a bitstream buffer", st), false;
			sl.bs = cb.bitstreamBuffer;
			sl.state = NV_FREE;
		}
		g_nv.head = g_nv.count = g_nv.encoding = 0;
		g_nv.nextPts = 0;
		g_nv.outIndex = 0;
		g_nv.bytes = 0;

		// SPS/PPS (+VPS) for the container header
		uint8_t hdr[1024];
		uint32_t hdrSize = 0;
		NV_ENC_SEQUENCE_PARAM_PAYLOAD sp = {NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER};
		sp.inBufferSize = sizeof hdr;
		sp.spsppsBuffer = hdr;
		sp.outSPSPPSPayloadSize = &hdrSize;
		st = fn.nvEncGetSequenceParams(g_nv.enc, &sp);
		if (st != NV_ENC_SUCCESS) return nvFail("reading sequence parameters", st), false;
		RendererCore::SubmitVideoHeader(hdr, (int)hdrSize);
		return true;
	}

	void nvSubmit(NvSlot &sl) {
		auto &fn = g_nv.fn;
		NV_ENC_MAP_INPUT_RESOURCE mp = {NV_ENC_MAP_INPUT_RESOURCE_VER};
		mp.registeredResource = sl.reg;
		NVENCSTATUS st = fn.nvEncMapInputResource(g_nv.enc, &mp);
		if (st != NV_ENC_SUCCESS) return nvFail("mapping an input surface", st);
		sl.mapped = mp.mappedResource;

		NV_ENC_PIC_PARAMS pic = {NV_ENC_PIC_PARAMS_VER};
		pic.inputBuffer = mp.mappedResource;
		pic.bufferFmt = mp.mappedBufferFmt;
		pic.inputWidth = g_gpu.p.width;
		pic.inputHeight = g_gpu.p.height;
		pic.outputBitstream = sl.bs;
		pic.inputTimeStamp = sl.pts;
		pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
		st = fn.nvEncEncodePicture(g_nv.enc, &pic);
		if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT) return nvFail("encoding a frame", st);
		sl.state = NV_ENCODING;
		++g_nv.encoding;
	}

	// Output buffers fill in coding order, one per submitted frame
	void nvLockOldest() {
		auto &fn = g_nv.fn;
		NvSlot &sl = g_nv.slots[g_nv.head];
		NV_ENC_LOCK_BITSTREAM lb = {NV_ENC_LOCK_BITSTREAM_VER};
		lb.outputBitstream = sl.bs;
		NVENCSTATUS st = fn.nvEncLockBitstream(g_nv.enc, &lb);
		if (st != NV_ENC_SUCCESS) return nvFail("reading the bitstream", st);

		AVPacket *pkt = av_packet_alloc();
		if (pkt && av_new_packet(pkt, (int)lb.bitstreamSizeInBytes) == 0) {
			memcpy(pkt->data, lb.bitstreamBufferPtr, lb.bitstreamSizeInBytes);
			pkt->pts = (int64_t)lb.outputTimeStamp;
			pkt->dts = g_nv.outIndex - g_nv.bframes;
			if (lb.pictureType == NV_ENC_PIC_TYPE_IDR || lb.pictureType == NV_ENC_PIC_TYPE_I) pkt->flags |= AV_PKT_FLAG_KEY;
			g_nv.bytes += lb.bitstreamSizeInBytes;
		} else {
			av_packet_free(&pkt);
		}
		++g_nv.outIndex;
		fn.nvEncUnlockBitstream(g_nv.enc, sl.bs);
		fn.nvEncUnmapInputResource(g_nv.enc, sl.mapped);
		sl.mapped = nullptr;
		sl.state = NV_FREE;
		g_nv.head = (g_nv.head + 1) % g_nv.nslots;
		--g_nv.count;
		--g_nv.encoding;
		if (pkt) {
			RendererCore::SubmitVideoPacket(pkt);
			++g_gpu.emitted;
		}
	}

	// Submits finished copies in order, then collects old outputs. With
	// `drain`, waits for every copy and flushes the encoder.
	void nvPump(bool drain) {
		for (int i = g_nv.encoding; i < g_nv.count && !g_gpu.broken; ++i) {
			NvSlot &sl = g_nv.slots[(g_nv.head + i) % g_nv.nslots];
			if (sl.state != NV_RENDERED) continue;
			if (drain) {
				int64_t t0 = RendererCore::NowMicros();
				while (sl.query->GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) SwitchToThread();
				g_gpu.waitUs += RendererCore::NowMicros() - t0;
			} else if (sl.query->GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) {
				break;  // keep input order
			}
			nvSubmit(sl);
		}
		if (drain && !g_gpu.broken) {
			NV_ENC_PIC_PARAMS eos = {NV_ENC_PIC_PARAMS_VER};
			eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
			g_nv.fn.nvEncEncodePicture(g_nv.enc, &eos);
		}
		int keep = drain ? 0 : g_nv.delay;
		while (g_nv.encoding > keep && !g_gpu.broken) nvLockOldest();
	}

	NvSlot &nvNewSlot() {
		while (g_nv.count == g_nv.nslots && !g_gpu.broken) {
			// Ring full: the GPU or NVENC is behind; wait for the oldest copy
			NvSlot &old = g_nv.slots[(g_nv.head + g_nv.encoding) % g_nv.nslots];
			if (g_nv.encoding < g_nv.count && old.state == NV_RENDERED) {
				int64_t t0 = RendererCore::NowMicros();
				while (old.query->GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) SwitchToThread();
				g_gpu.waitUs += RendererCore::NowMicros() - t0;
				nvSubmit(old);
			}
			if (g_nv.encoding > 0) nvLockOldest();
		}
		NvSlot &sl = g_nv.slots[(g_nv.head + g_nv.count) % g_nv.nslots];
		++g_nv.count;
		return sl;
	}

	// }}}

	// Queued work {{{

	void runBegin(const RendererGpu::Params &p) {
		releaseSessionResources();
		g_gpu.p = p;
		g_gpu.active = true;
		g_gpu.broken = false;
		g_gpu.runUs = g_gpu.waitUs = 0;
		g_gpu.emitted = 0;
		if (!g_device) {
			fail("device lookup");
			return;
		}
		if (!createSessionResources(g_device)) {
			releaseSessionResources();
			return;
		}
		if (p.nvenc.enabled && !nvOpen(g_device)) {
			nvDestroy();
			releaseSessionResources();
		}
	}

	void runCapture(int subframe, bool accumulate, bool first, bool emit) {
		if (!g_gpu.active || g_gpu.broken) return;
		IDirect3DDevice9 *dev = g_device;
		const RendererGpu::Params &p = g_gpu.p;
		++g_gpu.tick;

		IDirect3DSurface9 *bb = nullptr;
		HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
		if (FAILED(hr)) return fail("GetBackBuffer", hr);

		{
			// NVENC without blending only copies the back buffer
			bool draws = !p.nvenc.enabled || p.toBlend > 1;
			StateSave save(dev, draws);
			if (draws) setCommonState(dev);

			IDirect3DTexture9 *src = g_gpu.copyTex;
			bool nvenc = p.nvenc.enabled;
			if (accumulate && !(nvenc && p.toBlend == 1)) {
				// Copy (and MSAA-resolve) the finished frame
				hr = dev->StretchRect(bb, NULL, g_gpu.copySurf, NULL, D3DTEXF_NONE);
				if (FAILED(hr)) fail("StretchRect", hr);
			}

			if (p.toBlend > 1) {
				if (accumulate) {
					int dst = g_gpu.accCur ^ 1;
					float c[4] = {1.0f / p.width, 1.0f / p.height, p.weights[subframe], 0.0f};
					drawPass(dev, first ? Pass::ACC_FIRST : Pass::ACC, g_gpu.accSurf[dst], p.width, p.height, g_gpu.accTex[g_gpu.accCur], false, g_gpu.copyTex, c, 1);
					g_gpu.accCur = dst;
				}
				if (emit && !nvenc) {
					float c[4] = {1.0f / p.width, 1.0f / p.height, 0.0f, 0.0f};
					drawPass(dev, Pass::COPY, g_gpu.frameSurf, p.width, p.height, g_gpu.accTex[g_gpu.accCur], false, nullptr, c, 1);
					src = g_gpu.frameTex;
				}
			}

			if (emit && nvenc && !g_gpu.broken) {
				NvSlot &sl = nvNewSlot();
				if (!g_gpu.broken) {
					if (p.toBlend == 1) {
						hr = dev->StretchRect(bb, NULL, sl.rt, NULL, D3DTEXF_NONE);
						if (FAILED(hr)) fail("StretchRect", hr);
					} else {
						float c[4] = {1.0f / p.width, 1.0f / p.height, 0.0f, 0.0f};
						drawPass(dev, Pass::COPY, sl.rt, p.width, p.height, g_gpu.accTex[g_gpu.accCur], false, nullptr, c, 1);
					}
					sl.query->Issue(D3DISSUE_END);
					sl.state = NV_RENDERED;
					sl.pts = g_nv.nextPts++;
				}
			} else if (emit && !g_gpu.broken) {
				Slot &sl = newSlot(dev);
				convertInto(dev, src, sl);
				sl.state = SLOT_RENDERED;
				sl.renderedTick = g_gpu.tick;
			}

			// Unbind our textures before the state block restores
			if (draws) {
				dev->SetTexture(0, NULL);
				dev->SetTexture(1, NULL);
			}
		}
		bb->Release();

		if (p.nvenc.enabled) {
			if (!g_gpu.broken) nvPump(false);
		} else {
			pump(dev);
		}
	}

	void runFinish() {
		if (g_gpu.active && !g_gpu.broken && g_device) {
			if (g_gpu.p.nvenc.enabled) {
				nvPump(true);
			} else {
				while (g_gpu.count > 0) retireOldest(g_device);
			}
		}
		bool wasNvenc = g_gpu.p.nvenc.enabled;
		int64_t nvBytes = g_nv.bytes;
		nvDestroy();
		int emitted = g_gpu.emitted;
		int64_t runUs = g_gpu.runUs, waitUs = g_gpu.waitUs;
		releaseSessionResources();
		g_gpu.active = false;
		g_sessionOpen.store(false);
		RendererCore::SubmitEnd();
		if (wasNvenc) {
			THREAD_PRINT("[render] NVENC: %d frames, %.1f MB, render-thread work %.0fms (of which waiting for the GPU %.0fms)\n", emitted, nvBytes / 1048576.0, runUs / 1000.0, waitUs / 1000.0);
		} else {
			THREAD_PRINT("[render] GPU path: %d frames read back, render-thread work %.0fms (of which waiting for the GPU %.0fms)\n", emitted, runUs / 1000.0, waitUs / 1000.0);
		}
	}

	// Source's CFunctor: AddRef, Release, (destructor), operator()
	class GpuFunctor {
	public:
		enum Kind { BEGIN, CAPTURE, FINISH };

		GpuFunctor(Kind kind)
			: kind(kind) {
			++g_pending;
		}

		virtual int AddRef() {
			return ++refs;
		}
		virtual int Release() {
			int r = --refs;
			if (r == 0) delete this;
			return r;
		}
		virtual void Destroy() {}
		virtual void Run() {
			int64_t t0 = RendererCore::NowMicros();
			switch (kind) {
			case BEGIN:
				runBegin(params);
				break;
			case CAPTURE:
				runCapture(subframe, accumulate, first, emit);
				break;
			case FINISH:
				runFinish();
				break;
			}
			if (kind != FINISH) g_gpu.runUs += RendererCore::NowMicros() - t0;
			--g_pending;
		}

		Kind kind;
		std::atomic<int> refs{1};
		RendererGpu::Params params;
		int subframe = 0;
		bool accumulate = false, first = false, emit = false;
	};

	bool callQueueShape(void *fn, bool *queued) {
		auto b = (const uint8_t *)fn;
		if (b[0] == 0x8D && b[1] == 0x41 && b[3] == 0xC3) return *queued = true, true;
		if (b[0] == 0x8D && b[1] == 0x81 && b[6] == 0xC3) return *queued = true, true;
		if ((b[0] == 0x33 || b[0] == 0x31) && b[1] == 0xC0 && b[2] == 0xC3) return *queued = false, true;
		return false;
	}

	// Runs `f` on the render thread, in order with the frame's draw calls
	bool queueFunctor(GpuFunctor *f) {
		IMatRenderContext *ctx = materialSystem->GetRenderContext();
		if (!ctx) {
			f->Run();
			f->Release();
			return false;
		}
		void **vt = *(void ***)ctx;
		bool queued = false;
		bool ok = callQueueShape(vt[GET_CALL_QUEUE_INDEX], &queued);
		void *queue = ok && queued ? ((void *(__thiscall *)(void *))vt[GET_CALL_QUEUE_INDEX])(ctx) : nullptr;
		if (queue) {
			// The queue AddRefs the functor and drops our reference
			((void(__thiscall *)(void *, void *))(*(void ***)queue)[0])(queue, f);
		} else {
			f->Run();
			f->Release();
		}
		((int(__thiscall *)(void *))vt[1])(ctx);  // Release the context
		return ok;
	}

	// }}}

	HRESULT WINAPI Reset_Hook(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp) {
		if (dev == g_device) {
			// D3DPOOL_DEFAULT resources and state blocks must go before Reset
			bool hadSession = g_gpu.active && !g_gpu.broken;
			nvDestroy();
			releaseSessionResources();
			safeRelease(g_flushQuery);
			g_flushQueryDevice = nullptr;
			if (hadSession) fail("device reset during render");
		}
		return Reset_Trampoline(dev, pp);
	}

	HRESULT WINAPI Present_Hook(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty) {
		if (dev != g_device || !g_skipPresent.load()) return Present_Trampoline(dev, src, dst, wnd, dirty);
		// Submit the frame's commands without presenting it
		if (g_flushQueryDevice != dev) {
			safeRelease(g_flushQuery);
			g_flushQueryDevice = dev;
			if (FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_flushQuery))) g_flushQuery = nullptr;
		}
		if (g_flushQuery) {
			g_flushQuery->Issue(D3DISSUE_END);
			g_flushQuery->GetData(NULL, 0, D3DGETDATA_FLUSH);
		}
		return D3D_OK;
	}

	bool engineSupported() {
		return sar.game->Is(SourceGame_Portal2 | SourceGame_ApertureTag | SourceGame_PortalStoriesMel | SourceGame_ThinkingWithTimeMachine | SourceGame_PortalReloaded);
	}
}  // namespace

bool RendererGpu::CanUse(int width, int height, AVPixelFormat pixFmt, const char **why) {
	if (!engineSupported()) return *why = "unsupported game", false;
	if (pixFmt != AV_PIX_FMT_YUV420P && pixFmt != AV_PIX_FMT_NV12) return *why = "encoder pixel format", false;
	if (width % 8 || height % 2) return *why = "width must be a multiple of 8", false;

	IMatRenderContext *ctx = materialSystem->GetRenderContext();
	if (!ctx) return *why = "no render context", false;
	void **vt = *(void ***)ctx;
	bool queued;
	bool shapeOk = callQueueShape(vt[GET_CALL_QUEUE_INDEX], &queued);
	((int(__thiscall *)(void *))vt[1])(ctx);
	if (!shapeOk) return *why = "unknown material system layout", false;

	if (!g_device) {
		g_device = findDevice(why);
		if (!g_device) return false;
		Reset_Target = (ResetFn)(*(void ***)g_device)[16];
		MH_Initialize();
		if (MH_CreateHook((LPVOID)Reset_Target, (LPVOID)&Reset_Hook, (LPVOID *)&Reset_Trampoline) != MH_OK || MH_EnableHook((LPVOID)Reset_Target) != MH_OK) {
			g_device = nullptr;
			return *why = "could not hook device Reset", false;
		}
		Present_Target = (PresentFn)(*(void ***)g_device)[17];
		if (MH_CreateHook((LPVOID)Present_Target, (LPVOID)&Present_Hook, (LPVOID *)&Present_Trampoline) != MH_OK || MH_EnableHook((LPVOID)Present_Target) != MH_OK) {
			Present_Target = nullptr;  // optional; renders just keep presenting
		}
		console->Print("GPU capture: using D3D9 device %p (%s)\n", g_device, GetModuleHandleA("shaderapivk.dll") ? "DXVK" : "native");
	}
	return true;
}

bool RendererGpu::NvencAvailable(const char **why) {
	if (GetModuleHandleA("shaderapivk.dll")) return *why = "NVENC needs the native D3D9 renderer (not -vulkan)", false;
	if (!g_device) return *why = "D3D9 device not located", false;
	IDirect3D9 *d3d = nullptr;
	D3DDEVICE_CREATION_PARAMETERS cp;
	D3DADAPTER_IDENTIFIER9 id;
	bool nvidia = false;
	if (SUCCEEDED(g_device->GetDirect3D(&d3d)) && SUCCEEDED(g_device->GetCreationParameters(&cp)) && SUCCEEDED(d3d->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &id))) {
		nvidia = id.VendorId == 0x10DE;
	}
	safeRelease(d3d);
	if (!nvidia) return *why = "the game is not running on an NVIDIA GPU", false;
	return nvLoad(why);
}

bool RendererGpu::Begin(const Params &params) {
	g_failed.store(false);
	g_sessionOpen.store(true);
	auto f = new GpuFunctor(GpuFunctor::BEGIN);
	f->params = params;
	if (queueFunctor(f) && !g_failed.load()) return true;
	// Failed right away (immediate context): nothing is queued, the caller
	// falls back to the CPU path
	g_sessionOpen.store(false);
	g_failed.store(false);
	return false;
}

void RendererGpu::Capture(int subframe, bool accumulate, bool first, bool emit) {
	auto f = new GpuFunctor(GpuFunctor::CAPTURE);
	f->subframe = subframe;
	f->accumulate = accumulate;
	f->first = first;
	f->emit = emit;
	queueFunctor(f);
}

void RendererGpu::Finish() {
	queueFunctor(new GpuFunctor(GpuFunctor::FINISH));
}

bool RendererGpu::Failed() {
	return g_failed.load();
}

bool RendererGpu::Busy() {
	return g_sessionOpen.load() || g_pending.load() > 0;
}

bool RendererGpu::SetSkipPresent(bool skip) {
	if (!Present_Target) return false;
	g_skipPresent.store(skip);
	return true;
}

void RendererGpu::Shutdown() {
	g_skipPresent.store(false);
	if (Present_Target) {
		MH_DisableHook((LPVOID)Present_Target);
		MH_RemoveHook((LPVOID)Present_Target);
		Present_Target = nullptr;
	}
	if (Reset_Target) {
		MH_DisableHook((LPVOID)Reset_Target);
		MH_RemoveHook((LPVOID)Reset_Target);
		Reset_Target = nullptr;
	}
}

#endif
