// this code sucks but it works lol

#include "Event.hpp"
#include "Modules/Engine.hpp"
#include "Modules/FileSystem.hpp"
#include "Modules/Server.hpp"
#include "Modules/Surface.hpp"
#include "Features/Camera.hpp"
#include "Features/Session.hpp"
#include "Features/OverlayRender.hpp"
#include "Features/EntityList.hpp"
#include "Features/Hud/Hud.hpp"
#include "Features/Renderer.hpp"
#include "Utils/lodepng.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <vector>

#define TRACE_LENGTH 2500
#define TEST_DIST 2
#define TEST_FLOOR_UP_CONTRIBUTION 0.1f
#define MAX_IMAGE_SIZE 65535

Variable sar_pp_scan_color_success("sar_pp_scan_color_success", "131 182 146", "The color rendered by pp scan when portal placement is successful.\n");
Variable sar_pp_scan_color_fail("sar_pp_scan_color_fail", "194 69 45", "The color rendered by pp scan where portal placement fails.\n");
Variable sar_pp_scan_savepath("sar_pp_scan_filename", "pp_scan", "The name of the PNG image to save after pp scan, without extension. Saved in the game directory; an existing file is not overwritten.\n");
Variable sar_pp_scan_screenshot_overlay("sar_pp_scan_screenshot_overlay", "0", "Whether to overlay pp scan screenshot on top of gameplay screenshot.\n");
Variable sar_pp_scan_resolution("sar_pp_scan_resolution", "0.1", 0.01f, "Distance in units between the points an area scan tests.\n");
Variable sar_pp_scan_frame_budget("sar_pp_scan_frame_budget", "8", 1, 100, "Milliseconds of scanning per frame. Higher scans faster but makes the game less responsive.\n");
Variable sar_pp_scan_portal("sar_pp_scan_portal", "0", 0, 2, "Which portal to test: 0 = blue, 1 = orange, 2 = both (succeeds only where both can be placed).\n");
Variable sar_pp_scan_origin("sar_pp_scan_origin", "0", 0, 1, "Where area scans fire from: 0 = straight at the wall from just in front of it, 1 = from your eye position when the scan starts.\n");
Variable sar_pp_scan_step("sar_pp_scan_step", "4", 1, 64, "Adaptive scanning: test every Nth pixel first, then only refine the areas whose results differ. 1 = test every pixel. A spot smaller than N pixels inside an otherwise uniform area can be missed.\n");
Variable sar_pp_scan_warn_points("sar_pp_scan_warn_points", "2000000", 0, "Ask for confirmation before area scans that test more points than this (0 = never ask).\n");

enum class SetupState {
	NONE,
	SET_WALL,
	SET_SCAN_POINT_A,
	SET_SCAN_POINT_B,
	SET_MATCH_POINT_A,
	SET_MATCH_POINT_B,
	READY,
	RUNNING,
};

static struct {
	SetupState state;
	bool isPerspective;
	bool confirmedLargeScan;
	cplane_t wallPlane;
	Vector scanUpDirection;
	Vector scanAreaCorner1;
	Vector scanAreaCorner2;
	Vector matchAreaCorner1;
	Vector matchAreaCorner2;
} g_setup;

// A portal gun set up for TraceFirePortal, which needs the gun to have both
// portals. Remembers the gun's portal handles so they can be put back.
struct BorrowedGun {
	CBaseHandle portalgun;
	CBaseHandle oldPrimary;
	CBaseHandle oldSecondary;
	bool changedPortals;
};

struct ScanCell {
	int x0, y0, x1, y1;  // corner pixels, inclusive
};

static struct {
	Vector startPoint;
	Vector eyePosition;
	bool fromEye;
	float resolution;
	int maxWidth;
	int maxHeight;
	// Adaptive scan: test a grid of every `step`th pixel, then split each
	// grid cell whose corners disagree until the cells are a pixel wide;
	// cells whose corners agree are filled in without testing.
	int step;
	int gridW, gridH;
	long long gridNext;           // next grid point, column by column
	std::vector<ScanCell> cells;  // cells left to refine
	std::vector<uint8_t> result;  // per pixel: 0 = unknown, 1 = fail, 2 = success
	long long resolved;           // pixels with a result
	long long tested;             // pixels actually traced
	Vector wallWidthAxis;
	Vector wallHeightAxis;
	float fovOffset;
	Color successColor;
	Color failColor;
	int portals;  // sar_pp_scan_portal when the scan started
	std::vector<uint8_t> image;
	BorrowedGun gun;  // put back when the scan ends
	std::chrono::steady_clock::time_point started;
} g_scan;

static std::optional<CGameTrace> camTrace() {
	if (!session->isRunning) return {};

	CGameTrace tr;

	engine->TraceFromCamera<false>(TRACE_LENGTH, MASK_SHOT_PORTAL, tr);

	return tr.plane.normal.Length() > 0.9 ? tr : std::optional<CGameTrace>{};
}

// Gets a pair of perpendicular axes on a plane
static inline void getAxesForScanAreaPlane(cplane_t plane, Vector *ax1, Vector *ax2) {
	*ax1 = plane.normal.Cross(g_setup.scanUpDirection);
	*ax2 = plane.normal.Cross(*ax1);

	*ax1 = ax1->Normalize();
	*ax2 = ax2->Normalize();
}

static Vector intersectPlaneView(cplane_t plane) {
	Vector start = camera->GetPosition(GET_SLOT());
	QAngle angles = camera->GetAngles(GET_SLOT());

	Vector dir;
	Math::AngleVectors(angles, &dir);

	float w = dir.Dot(plane.normal);
	if (w == 0) {
		// Line parallel to plane - just return some point on the plane
		return plane.normal * plane.dist;
	}
	float p = (plane.dist - start.Dot(plane.normal)) / w;
	if (p < 0) {
		// Camera looking away from plane - return a random point again
		return plane.normal * plane.dist;
	}
	return start + dir*p;
}

static void drawRectOnPlane(cplane_t plane, Vector a, Vector c, Color col, float draw_dist) {
	Vector ax1, ax2;
	getAxesForScanAreaPlane(plane, &ax1, &ax2);

	Vector v = a - c;
	Vector b = c + ax1*v.Dot(ax1);
	Vector d = c + ax2*v.Dot(ax2);

	Vector zf = plane.normal*draw_dist; // z-fighting

	MeshId m = OverlayRender::createMesh(RenderCallback::constant(col), RenderCallback::none);
	OverlayRender::addQuad(m, a+zf, b+zf, c+zf, d+zf);
}

static void drawPointOnPlane(cplane_t plane, Vector p, Color col, float draw_dist) {
	Vector ax1, ax2;
	getAxesForScanAreaPlane(plane, &ax1, &ax2);

	Vector a = p - ax1 - ax2;
	Vector b = p + ax1 - ax2;
	Vector c = p + ax1 + ax2;
	Vector d = p - ax1 + ax2;

	Vector zf = plane.normal*draw_dist; // z-fighting

	MeshId m = OverlayRender::createMesh(RenderCallback::constant(col), RenderCallback::none);
	OverlayRender::addQuad(m, a+zf, b+zf, c+zf, d+zf);
}

static void drawTracePlane() {
	auto tr = camTrace();
	if (!tr) return;

	Vector ax1, ax2;
	getAxesForScanAreaPlane(tr->plane, &ax1, &ax2);

	Vector center = tr->endpos + tr->plane.normal * 0.04;  // Bump away to prevent z-fighting

	Vector a = center + ax1 * 100 + ax2 * 100;
	Vector b = center + ax1 * 100 - ax2 * 100;
	Vector c = center - ax1 * 100 - ax2 * 100;
	Vector d = center - ax1 * 100 + ax2 * 100;

	MeshId m = OverlayRender::createMesh(RenderCallback::constant({255, 0, 0, 100}), RenderCallback::none);
	OverlayRender::addQuad(m, a, b, c, d);
}

static void updateScanAreaUpDirection() {
	auto tr = camTrace();
	if (!tr) return;

	if (fabsf(tr->plane.normal.z) > 0.999f) {
		// floor or ceiling: "up" on the image is where you're facing. Use the
		// yaw only, so looking steeply down at the floor still gives stable axes
		QAngle angles = camera->GetAngles(GET_SLOT());
		angles.x = 0;
		angles.z = 0;
		Math::AngleVectors(angles, &g_setup.scanUpDirection);
	} else {
		g_setup.scanUpDirection = Vector(0, 0, 1);
	}
}

static bool liesInMatchArea(Vector p) {
	Vector ax1 = g_scan.wallWidthAxis;
	Vector ax2 = g_scan.wallHeightAxis;

	float m1a = g_setup.matchAreaCorner1.Dot(ax1);
	float m2a = g_setup.matchAreaCorner1.Dot(ax2);

	float m1b = g_setup.matchAreaCorner2.Dot(ax1);
	float m2b = g_setup.matchAreaCorner2.Dot(ax2);

	float min1 = m1a < m1b ? m1a : m1b;
	float min2 = m2a < m2b ? m2a : m2b;

	float max1 = m1a > m1b ? m1a : m1b;
	float max2 = m2a > m2b ? m2a : m2b;

	float d1 = p.Dot(ax1);
	float d2 = p.Dot(ax2);

	return min1 <= d1 && d1 <= max1 && min2 <= d2 && d2 <= max2;
}

static uintptr_t getPortalGun(const BorrowedGun &gun) {
	auto portalgun = (uintptr_t)entityList->LookupEntity(gun.portalgun);
	return portalgun && entityList->IsPortalGun(gun.portalgun) ? portalgun : 0;
}

// Finds the player's portal gun and makes sure it has both portals to test
// with. Returns why it couldn't, or null.
static const char *borrowPortalGun(BorrowedGun &gun) {
	if (!server->TraceFirePortal || !server->FindPortal) return "it is not available in this game";

	void *player = server->GetPlayer(GET_SLOT() + 1);
	if (!player) return "cannot find your player";

	auto gunHandle = SE(player)->active_weapon();
	uintptr_t portalgun = (uintptr_t)entityList->LookupEntity(gunHandle);
	if (!portalgun || !entityList->IsPortalGun(gunHandle)) return "you need to hold a portal gun";

	uint8_t linkage = SE(portalgun)->field<unsigned char>("m_iPortalLinkageGroupID");

	auto &primary = SE(portalgun)->field<CBaseHandle>("m_hPrimaryPortal");
	auto &secondary = SE(portalgun)->field<CBaseHandle>("m_hSecondaryPortal");
	gun.portalgun = gunHandle;
	gun.oldPrimary = primary;
	gun.oldSecondary = secondary;
	gun.changedPortals = false;

	auto bluePortal = (uintptr_t)entityList->LookupEntity(primary);
	auto orangePortal = (uintptr_t)entityList->LookupEntity(secondary);

	if (!bluePortal) bluePortal = server->FindPortal(linkage, false, true);
	if (!orangePortal) orangePortal = server->FindPortal(linkage, true, true);
	if (!bluePortal || !orangePortal) return "couldn't get portals for your portal gun";

	auto blueHandle = ((IHandleEntity *)bluePortal)->GetRefEHandle();
	auto orangeHandle = ((IHandleEntity *)orangePortal)->GetRefEHandle();
	// CBaseHandle has no comparison operators (only a bool conversion)
	if (primary.m_Index != blueHandle.m_Index || secondary.m_Index != orangeHandle.m_Index) {
		primary = blueHandle;
		secondary = orangeHandle;
		gun.changedPortals = true;
	}

	return nullptr;
}

static void returnPortalGun(BorrowedGun &gun) {
	if (!gun.changedPortals) return;
	gun.changedPortals = false;
	auto portalgun = getPortalGun(gun);
	if (!portalgun) return;
	SE(portalgun)->field<CBaseHandle>("m_hPrimaryPortal") = gun.oldPrimary;
	SE(portalgun)->field<CBaseHandle>("m_hSecondaryPortal") = gun.oldSecondary;
}

static bool initScan() {
	if (auto why = borrowPortalGun(g_scan.gun)) {
		console->Print("Cannot start placement scan: %s.\n", why);
		return false;
	}
	return true;
}

static void restorePortalGun() {
	returnPortalGun(g_scan.gun);
}

// <game dir>/<name>.png, or <name>_2.png, <name>_3.png... if that exists
static std::string outputPath() {
	std::string base = std::string(engine->GetGameDirectory()) + "/" + sar_pp_scan_savepath.GetString();
	std::string path = base + ".png";
	for (int i = 2; std::filesystem::exists(path); ++i) {
		path = Utils::ssprintf("%s_%d.png", base.c_str(), i);
	}
	return path;
}

// The image is BGRA with the bottom row first; PNG wants RGBA, top row first
static bool writePng(const std::string &path, const std::vector<uint8_t> &image, int w, int h) {
	std::vector<uint8_t> rgba(image.size());
	for (int y = 0; y < h; ++y) {
		const uint8_t *src = &image[(size_t)(h - 1 - y) * w * 4];
		uint8_t *dst = &rgba[(size_t)y * w * 4];
		for (int x = 0; x < w; ++x) {
			dst[x * 4 + 0] = src[x * 4 + 2];
			dst[x * 4 + 1] = src[x * 4 + 1];
			dst[x * 4 + 2] = src[x * 4 + 0];
			dst[x * 4 + 3] = src[x * 4 + 3];
		}
	}
	return lodepng::encode(path, rgba, w, h) == 0;
}

static void loadScreenPixelsIntoImage() {
	std::vector<uint8_t> buf((size_t)g_scan.maxWidth * g_scan.maxHeight * 4);
	Renderer::ReadScreenPixels(0, 0, g_scan.maxWidth, g_scan.maxHeight, buf.data(), IMAGE_FORMAT_RGBA8888);

	// full alpha, inverted y axis, RGBA to BGRA
	for (int x = 0; x < g_scan.maxWidth; ++x) {
		for (int y = 0; y < g_scan.maxHeight; ++y) {
			size_t i = ((size_t)y * g_scan.maxWidth + x) * 4;
			size_t j = ((size_t)(g_scan.maxHeight - 1 - y) * g_scan.maxWidth + x) * 4;
			g_scan.image[i + 0] = buf[j + 2];
			g_scan.image[i + 1] = buf[j + 1];
			g_scan.image[i + 2] = buf[j + 0];
			g_scan.image[i + 3] = 255;
		}
	}
}

static bool prepareForScan() {
	if (g_scan.maxWidth <= 0 || g_scan.maxHeight <= 0) {
		console->Print("The scan area is empty.\n");
		return false;
	}
	if (g_scan.maxWidth > MAX_IMAGE_SIZE || g_scan.maxHeight > MAX_IMAGE_SIZE) {
		console->Print("The scan would be %dx%d pixels; the limit is %d on each side. Raise sar_pp_scan_resolution or pick a smaller area.\n", g_scan.maxWidth, g_scan.maxHeight, MAX_IMAGE_SIZE);
		return false;
	}

	if (!initScan()) return false;

	try {
		g_scan.image.assign((size_t)g_scan.maxWidth * g_scan.maxHeight * 4, 0);
		g_scan.result.assign((size_t)g_scan.maxWidth * g_scan.maxHeight, 0);
	} catch (const std::bad_alloc &) {
		std::vector<uint8_t>().swap(g_scan.image);
		restorePortalGun();
		console->Print("Not enough memory for a %dx%d scan.\n", g_scan.maxWidth, g_scan.maxHeight);
		return false;
	}

	if (sar_pp_scan_screenshot_overlay.GetBool() && g_setup.isPerspective) {
		loadScreenPixelsIntoImage();
	}

	g_scan.successColor = Utils::GetColor(sar_pp_scan_color_success.GetString(), false).value_or(Color(0, 0, 255, 255));
	g_scan.failColor = Utils::GetColor(sar_pp_scan_color_fail.GetString(), false).value_or(Color(255, 0, 0, 255));
	g_scan.portals = sar_pp_scan_portal.GetInt();

	g_scan.step = sar_pp_scan_step.GetInt();
	g_scan.gridW = (g_scan.maxWidth + g_scan.step - 2) / g_scan.step + 1;
	g_scan.gridH = (g_scan.maxHeight + g_scan.step - 2) / g_scan.step + 1;
	g_scan.gridNext = 0;
	g_scan.cells.clear();
	g_scan.resolved = 0;
	g_scan.tested = 0;
	g_scan.started = std::chrono::steady_clock::now();
	g_setup.state = SetupState::RUNNING;
	return true;
}

static void startAreaScan() {
	g_setup.isPerspective = false;

	Vector wallWidthAxis, wallHeightAxis;
	getAxesForScanAreaPlane(g_setup.wallPlane, &wallWidthAxis, &wallHeightAxis);

	Vector startPoint = g_setup.scanAreaCorner1;
	Vector delta = g_setup.scanAreaCorner2 - startPoint;
	float maxWidth = delta.Dot(wallWidthAxis);
	float maxHeight = delta.Dot(wallHeightAxis);

	if (maxWidth < 0) {
		startPoint = startPoint + wallWidthAxis*maxWidth;
		maxWidth = -maxWidth;
	}

	if (maxHeight < 0) {
		startPoint = startPoint + wallHeightAxis*maxHeight;
		maxHeight = -maxHeight;
	}

	g_scan.resolution = sar_pp_scan_resolution.GetFloat();
	double width = maxWidth / g_scan.resolution, height = maxHeight / g_scan.resolution;
	long long points = (long long)width * (long long)height;

	int warn = sar_pp_scan_warn_points.GetInt();
	if (warn > 0 && points > warn && !g_setup.confirmedLargeScan) {
		console->Print("This scan tests %lld points (%.0fx%.0f, %.0f MB). Use sar_pp_scan_set again to start it, or raise sar_pp_scan_resolution.\n", points, width, height, points * 4 / 1048576.0);
		g_setup.confirmedLargeScan = true;
		return;
	}
	g_setup.confirmedLargeScan = false;

	g_scan.startPoint = startPoint;
	g_scan.maxWidth = width > MAX_IMAGE_SIZE + 1 ? MAX_IMAGE_SIZE + 1 : (int)width;
	g_scan.maxHeight = height > MAX_IMAGE_SIZE + 1 ? MAX_IMAGE_SIZE + 1 : (int)height;
	g_scan.wallWidthAxis = wallWidthAxis;
	g_scan.wallHeightAxis = wallHeightAxis;
	g_scan.fromEye = sar_pp_scan_origin.GetBool();
	g_scan.eyePosition = camera->GetPosition(GET_SLOT(), false);

	prepareForScan();
}

static void startPerspectiveScan() {
	g_setup.isPerspective = true;

	engine->GetScreenSize(nullptr, g_scan.maxWidth, g_scan.maxHeight);

	Vector cameraPos = camera->GetPosition(GET_SLOT(), false);
	g_scan.startPoint = cameraPos;

	QAngle cameraAngles = camera->GetAngles(GET_SLOT(), false);
	Vector cameraForward, cameraRight, cameraUp;
	Math::AngleVectors(cameraAngles, &cameraForward, &cameraRight, &cameraUp);
	g_setup.wallPlane = {cameraForward};
	g_scan.wallWidthAxis = cameraRight;
	g_scan.wallHeightAxis = cameraUp;

	// scaled fov calculations (https://developer.valvesoftware.com/wiki/Field_of_View)
	float fov = DEG2RAD(camera->GetFieldOfView(GET_SLOT(), false));
	float aspectRatio = (float)g_scan.maxWidth / (float)g_scan.maxHeight;
	float standardRatio = 4.0f / 3.0f;
	float scaledFov = atanf(tanf(fov * 0.5f) * aspectRatio / standardRatio) * 2.0f;
	g_scan.fovOffset = 1.0f / tanf(scaledFov * 0.5f);

	prepareForScan();
}

// Ends a running scan: saves the image when it finished, puts the portal gun
// back as it was, frees the image and lets sar_on_pp_scan_finish run.
static void endScan(bool finished, const char *why = nullptr) {
	if (g_setup.state != SetupState::RUNNING) {
		g_setup.state = SetupState::NONE;
		return;
	}
	restorePortalGun();

	if (finished) {
		std::string path = outputPath();
		if (writePng(path, g_scan.image, g_scan.maxWidth, g_scan.maxHeight)) {
			double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_scan.started).count();
			long long total = (long long)g_scan.maxWidth * g_scan.maxHeight;
			console->Print("Wrote %lld points to %s (%.1f s, traced %lld = %.1f%%)\n", total, path.c_str(), secs, g_scan.tested, 100.0 * g_scan.tested / total);
		} else {
			console->Print("Couldn't write %s\n", path.c_str());
		}
	} else {
		console->Print("Placement scan stopped%s%s\n", why ? ": " : ".", why ? why : "");
	}

	std::vector<uint8_t>().swap(g_scan.image);
	std::vector<uint8_t>().swap(g_scan.result);
	std::vector<ScanCell>().swap(g_scan.cells);
	g_setup.state = SetupState::NONE;
	Event::Trigger<Event::PP_SCAN_FINISH>({});
}

// Fires the portals selected by `portals` (sar_pp_scan_portal) and stores
// where each one ends up (finalPos[0] blue, [1] orange). False if any fails.
static bool firePortals(uintptr_t portalgun, Vector origin, Vector dir, int portals, Vector finalPos[2]) {
	for (int orange = 0; orange < 2; ++orange) {
		if (portals == 0 && orange) continue;
		if (portals == 1 && !orange) continue;

		TracePortalPlacementInfo_t info;
		server->TraceFirePortal(portalgun, origin, dir, orange != 0, 2, info);

		switch (info.ePlacementResult) {
		case PORTAL_PLACEMENT_SUCCESS:
		case PORTAL_PLACEMENT_USED_HELPER:
		case PORTAL_PLACEMENT_BUMPED:
			break;
		default:
			return false;
		}
		finalPos[orange] = info.finalPos;
	}
	return true;
}

static bool testRay(uintptr_t portalgun, Vector origin, Vector dir, bool checkMatchArea) {
	Vector finalPos[2];
	if (!firePortals(portalgun, origin, dir, g_scan.portals, finalPos)) return false;
	if (!checkMatchArea) return true;
	if (g_scan.portals != 1 && !liesInMatchArea(finalPos[0])) return false;
	if (g_scan.portals != 0 && !liesInMatchArea(finalPos[1])) return false;
	return true;
}

static bool testPerspectivePixel(uintptr_t portalgun, int x, int y) {
	Vector2 screenPointNormalized(
		((float)(x) + 0.5f) / (float)(g_scan.maxWidth) * 2.0f - 1.0f,
		((float)(y) + 0.5f) / (float)(g_scan.maxHeight) * 2.0f - 1.0f);

	screenPointNormalized.y *= ((float)g_scan.maxHeight / (float)g_scan.maxWidth);  // aspect ratio

	Vector dir = g_setup.wallPlane.normal * g_scan.fovOffset
		+ g_scan.wallWidthAxis * screenPointNormalized.x
		+ g_scan.wallHeightAxis * screenPointNormalized.y;

	return testRay(portalgun, g_scan.startPoint, dir.Normalize(), false);
}

static bool testAreaPixel(uintptr_t portalgun, int x, int y) {
	float widthUnits = (g_scan.maxWidth - 1 - x) * g_scan.resolution;
	float heightUnits = (g_scan.maxHeight - 1 - y) * g_scan.resolution;

	Vector target = g_scan.startPoint
		+ g_scan.wallWidthAxis * widthUnits
		+ g_scan.wallHeightAxis * heightUnits;

	if (g_scan.fromEye) {
		Vector dir = (target - g_scan.eyePosition).Normalize();
		return testRay(portalgun, g_scan.eyePosition, dir, true);
	}

	Vector dir = -g_setup.wallPlane.normal;

	// fix for floor/ceiling portals, so that the scan can actually check with correct angles
	if (fabsf(g_setup.wallPlane.normal.z) > 0.999) {
		dir += g_setup.scanUpDirection * TEST_FLOOR_UP_CONTRIBUTION;
		dir = dir.Normalize();
	}

	return testRay(portalgun, target - dir * TEST_DIST, dir, true);
}

static bool testPixel(uintptr_t portalgun, int x, int y) {
	if (g_setup.isPerspective) {
		return testPerspectivePixel(portalgun, x, y);
	} else {
		return testAreaPixel(portalgun, x, y);
	}
}

// A pixel's result, tracing it if it isn't known yet
static uint8_t sample(uintptr_t portalgun, int x, int y) {
	uint8_t &r = g_scan.result[(size_t)y * g_scan.maxWidth + x];
	if (!r) {
		r = testPixel(portalgun, x, y) ? 2 : 1;
		++g_scan.tested;
		++g_scan.resolved;
	}
	return r;
}

static int gridCoord(int i, int max) {
	int v = i * g_scan.step;
	return v < max - 1 ? v : max - 1;
}

static void refineCell(uintptr_t portalgun, ScanCell c) {
	uint8_t r = sample(portalgun, c.x0, c.y0);
	bool same = sample(portalgun, c.x1, c.y0) == r;
	same &= sample(portalgun, c.x0, c.y1) == r;
	same &= sample(portalgun, c.x1, c.y1) == r;

	if (same) {
		for (int y = c.y0; y <= c.y1; ++y) {
			uint8_t *row = &g_scan.result[(size_t)y * g_scan.maxWidth];
			for (int x = c.x0; x <= c.x1; ++x) {
				if (!row[x]) {
					row[x] = r;
					++g_scan.resolved;
				}
			}
		}
		return;
	}

	bool splitX = c.x1 - c.x0 > 1;
	bool splitY = c.y1 - c.y0 > 1;
	if (!splitX && !splitY) return;  // every pixel is a corner
	int xm = splitX ? (c.x0 + c.x1) / 2 : c.x1;
	int ym = splitY ? (c.y0 + c.y1) / 2 : c.y1;
	g_scan.cells.push_back({c.x0, c.y0, xm, ym});
	if (splitX) g_scan.cells.push_back({xm, c.y0, c.x1, ym});
	if (splitY) g_scan.cells.push_back({c.x0, ym, xm, c.y1});
	if (splitX && splitY) g_scan.cells.push_back({xm, ym, c.x1, c.y1});
}

// Blends the results over the screenshot (or the blank image)
static void composeImage() {
	uint8_t *image = g_scan.image.data();
	size_t n = (size_t)g_scan.maxWidth * g_scan.maxHeight;
	for (size_t pixelIndex = 0; pixelIndex < n; ++pixelIndex) {
		Color color = g_scan.result[pixelIndex] == 2 ? g_scan.successColor : g_scan.failColor;

		uint8_t baseB = image[pixelIndex * 4 + 0];
		uint8_t baseG = image[pixelIndex * 4 + 1];
		uint8_t baseR = image[pixelIndex * 4 + 2];
		uint8_t baseA = image[pixelIndex * 4 + 3];

		image[pixelIndex * 4 + 0] = 255 - (255 - baseB * baseA / 255) * (255 - color.b * color.a / 255) / 255;
		image[pixelIndex * 4 + 1] = 255 - (255 - baseG * baseA / 255) * (255 - color.g * color.a / 255) / 255;
		image[pixelIndex * 4 + 2] = 255 - (255 - baseR * baseA / 255) * (255 - color.r * color.a / 255) / 255;
		image[pixelIndex * 4 + 3] = 255 - (255 - baseA) * (255 - color.a) / 255;
	}
}

static void runScan() {
	uintptr_t portalgun = getPortalGun(g_scan.gun);
	if (!portalgun) {
		endScan(false, "the portal gun is gone");
		return;
	}

	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(sar_pp_scan_frame_budget.GetInt());
	auto timeUp = [&]() { return std::chrono::steady_clock::now() >= deadline; };
	long long gridTotal = (long long)g_scan.gridW * g_scan.gridH;

	while (g_scan.gridNext < gridTotal) {
		int i = (int)(g_scan.gridNext / g_scan.gridH);
		int j = (int)(g_scan.gridNext % g_scan.gridH);
		sample(portalgun, gridCoord(i, g_scan.maxWidth), gridCoord(j, g_scan.maxHeight));
		++g_scan.gridNext;
		if (g_scan.gridNext == gridTotal && g_scan.step > 1) {
			for (int ci = g_scan.gridW - 2; ci >= 0; --ci) {
				for (int cj = g_scan.gridH - 2; cj >= 0; --cj) {
					g_scan.cells.push_back({gridCoord(ci, g_scan.maxWidth), gridCoord(cj, g_scan.maxHeight), gridCoord(ci + 1, g_scan.maxWidth), gridCoord(cj + 1, g_scan.maxHeight)});
				}
			}
		}
		if ((g_scan.gridNext & 31) == 0 && timeUp()) return;
	}

	int n = 0;
	while (!g_scan.cells.empty()) {
		ScanCell c = g_scan.cells.back();
		g_scan.cells.pop_back();
		refineCell(portalgun, c);
		if ((++n & 7) == 0 && timeUp()) return;
	}

	composeImage();
	endScan(true);
}

static const char *scanningUnavailableReason() {
	if (!session->isRunning) return "not in a session";
	if (!sv_cheats.GetBool()) return "sv_cheats is disabled";
	return nullptr;
}

static bool checkScanningAvailable() {
	if (auto why = scanningUnavailableReason()) {
		console->Print("Cannot start placement scan: %s.\n", why);
		return false;
	}
	return true;
}

ON_EVENT(RENDER) {
	switch (g_setup.state) {
	case SetupState::NONE:
		return;
	case SetupState::SET_WALL:
		drawTracePlane();
		return;
	case SetupState::SET_SCAN_POINT_A:
		drawPointOnPlane(g_setup.wallPlane, intersectPlaneView(g_setup.wallPlane), {255, 0, 0, 100}, 0.04);
		return;
	case SetupState::SET_SCAN_POINT_B:
		drawRectOnPlane(g_setup.wallPlane, g_setup.scanAreaCorner1, intersectPlaneView(g_setup.wallPlane), {255, 0, 0, 100}, 0.04);
		return;
	case SetupState::SET_MATCH_POINT_A:
		drawRectOnPlane(g_setup.wallPlane, g_setup.scanAreaCorner1, g_setup.scanAreaCorner2, {255, 0, 0, 100}, 0.04);
		drawPointOnPlane(g_setup.wallPlane, intersectPlaneView(g_setup.wallPlane), {0, 0, 255, 100}, 0.12);
		return;
	case SetupState::SET_MATCH_POINT_B:
		drawRectOnPlane(g_setup.wallPlane, g_setup.scanAreaCorner1, g_setup.scanAreaCorner2, {255, 0, 0, 100}, 0.04);
		drawRectOnPlane(g_setup.wallPlane, g_setup.matchAreaCorner1, intersectPlaneView(g_setup.wallPlane), {0, 0, 255, 100}, 0.12);
		return;
	case SetupState::READY:
		drawRectOnPlane(g_setup.wallPlane, g_setup.scanAreaCorner1, g_setup.scanAreaCorner2, {255, 0, 0, 100}, 0.04);
		drawRectOnPlane(g_setup.wallPlane, g_setup.matchAreaCorner1, g_setup.matchAreaCorner2, {0, 0, 255, 100}, 0.12);
		return;
	case SetupState::RUNNING:
		return;
	}
}

ON_EVENT(FRAME) {
	if (g_setup.state == SetupState::NONE) {
		return;
	}

	if (auto why = scanningUnavailableReason()) {
		endScan(false, why);
		return;
	}

	if (g_setup.state == SetupState::SET_WALL) {
		updateScanAreaUpDirection();
	}

	if (g_setup.state == SetupState::RUNNING) {
		runScan();
	}
}

ON_EVENT(SESSION_START) {
	endScan(false, "the level changed");
}

HUD_ELEMENT2_NO_DISABLE(pp_scan, HudType_InGame | HudType_Paused) {
	if (g_setup.state == SetupState::NONE) return;

	std::string status_text = "";
	int sw, sh;
	engine->GetScreenSize(nullptr, sw, sh);

	switch (g_setup.state) {
	case SetupState::SET_WALL:
		status_text = "Select the wall plane to scan on.";
		break;
	case SetupState::SET_SCAN_POINT_A:
		status_text = "Select first corner of the scanning area.";
		break;
	case SetupState::SET_SCAN_POINT_B:
		status_text = "Select second corner of the scanning area.";
		break;
	case SetupState::SET_MATCH_POINT_A:
		status_text = "Select first corner of portal success detection area.";
		break;
	case SetupState::SET_MATCH_POINT_B:
		status_text = "Select second corner of portal success detection area.";
		break;
	case SetupState::READY:
		status_text = g_setup.confirmedLargeScan ? "This is a large scan (see console). Use sar_pp_scan_set again to start it."
		                                         : "Placement scan is ready. Use sar_pp_scan_set to begin.";
		break;
	case SetupState::RUNNING:
		status_text = Utils::ssprintf("Scanning to %s.png... use sar_pp_scan_set to cancel.", sar_pp_scan_savepath.GetString());
		break;
	default:
		break;
	}

	surface->DrawRectAndCenterTxt(Color{0, 0, 0, 0}, 0, 0, sw, sh - 50, 6, Color{255, 255, 255}, status_text.c_str());

	if (g_setup.state == SetupState::RUNNING) {
		long long total = (long long)g_scan.maxWidth * g_scan.maxHeight;
		float fraction = total > 0 ? (float)g_scan.resolved / (float)total : 0.0f;
		double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_scan.started).count();
		if (fraction > 0.01f) {
			int left = (int)(elapsed / fraction - elapsed);
			surface->DrawRectAndCenterTxt(Color{0, 0, 0, 200}, 0, 0, sw, sh + 50, 6, Color{255, 255, 255}, "%.1f%% (%d:%02d left)", 100.0f * fraction, left / 60, left % 60);
		} else {
			surface->DrawRectAndCenterTxt(Color{0, 0, 0, 200}, 0, 0, sw, sh + 50, 6, Color{255, 255, 255}, "%.1f%%", 100.0f * fraction);
		}
	}
}

CON_COMMAND(sar_pp_scan_set, "sar_pp_scan_set - set the ppscan point where you're aiming.\n") {
	if (!checkScanningAvailable()) {
		endScan(false);
		return;
	}

	switch (g_setup.state) {
	case SetupState::NONE:
		g_setup.confirmedLargeScan = false;
		g_setup.state = SetupState::SET_WALL;
		return;
	case SetupState::SET_WALL:
		{
			auto tr = camTrace();
			if (!tr) return;
			g_setup.wallPlane = tr->plane;
			g_setup.state = SetupState::SET_SCAN_POINT_A;
		}
		return;
	case SetupState::SET_SCAN_POINT_A:
		g_setup.scanAreaCorner1 = intersectPlaneView(g_setup.wallPlane);
		g_setup.state = SetupState::SET_SCAN_POINT_B;
		return;
	case SetupState::SET_SCAN_POINT_B:
		g_setup.scanAreaCorner2 = intersectPlaneView(g_setup.wallPlane);
		g_setup.state = SetupState::SET_MATCH_POINT_A;
		return;
	case SetupState::SET_MATCH_POINT_A:
		g_setup.matchAreaCorner1 = intersectPlaneView(g_setup.wallPlane);
		g_setup.state = SetupState::SET_MATCH_POINT_B;
		return;
	case SetupState::SET_MATCH_POINT_B:
		g_setup.matchAreaCorner2 = intersectPlaneView(g_setup.wallPlane);
		g_setup.state = SetupState::READY;
		return;
	case SetupState::READY:
		startAreaScan();
		return;
	case SetupState::RUNNING:
		endScan(false, "cancelled");
		return;
	}
}

CON_COMMAND(sar_pp_scan_reset, "sar_pp_scan_reset - reset ppscan.\n") {
	endScan(false, "reset");
}

CON_COMMAND(sar_pp_scan_screenshot, "sar_pp_scan_screenshot - renders current camera perspective using ppscan rays\n") {
	if (g_setup.state != SetupState::NONE) {
		console->Print("Cannot start perspective scan while another scan is in progress.\n");
		return;
	}

	if (!checkScanningAvailable()) {
		return;
	}

	startPerspectiveScan();
}

// Live placement overlay (sar_pp_live): paints the surface you're aiming at
// with where portals can go, testing a grid of points around the crosshair
// within a time budget each frame. Like the adaptive scan, it tests every
// `step`th point first and only refines the cells whose corners disagree.
//
// Mode 1 fires straight at each point (like sar_pp_scan_origin 0), so the
// results don't depend on where you stand and are kept for every surface
// you've looked at. Mode 2 fires from your eyes, so the results are only
// valid for one eye position: when you move they stay on screen but are
// tested again.

Variable sar_pp_live("sar_pp_live", "0", 0, 2, "Live portal placement overlay on the surface you're aiming at: 0 = off, 1 = where a portal can go (fired straight at each point; results are kept), 2 = what you can hit from your eye position (tested again when you move). Uses sar_pp_scan_portal and the sar_pp_scan_color_ colors. Requires sv_cheats.\n");
Variable sar_pp_live_spacing("sar_pp_live_spacing", "4", 0.5f, "Distance in units between the points the live overlay tests.\n");
Variable sar_pp_live_radius("sar_pp_live_radius", "96", 8, 1024, "How far around the crosshair the live overlay tests, in units.\n");
Variable sar_pp_live_step("sar_pp_live_step", "4", 1, 16, "Live overlay: test every Nth point first, then refine only where results differ. 1 = test every point.\n");
Variable sar_pp_live_budget("sar_pp_live_budget", "2", 0.1f, 50, "Milliseconds per frame the live overlay spends testing.\n");
Variable sar_pp_live_tolerance("sar_pp_live_tolerance", "1", 0, "Live overlay: how far in units a portal can be moved from the point (bumped or snapped to a helper) and still count as landing there.\n");
Variable sar_pp_live_color_bumped("sar_pp_live_color_bumped", "230 190 60", "Live overlay color where a portal lands but is moved away from the point.\n");
Variable sar_pp_live_opacity("sar_pp_live_opacity", "110", 0, 255, "Opacity of the live overlay.\n");
Variable sar_pp_live_draw_dist("sar_pp_live_draw_dist", "1500", 0, "Live overlay: results further than this from you aren't drawn.\n");
Variable sar_pp_live_eye_move("sar_pp_live_eye_move", "1", 0, "Live overlay mode 2: how far in units your eyes can move before the results are tested again.\n");

#define LIVE_FLOOR_YAW_STEP 15.0f  // floor results are kept per facing direction, in these steps
#define LIVE_MAX_POINTS 400000     // the results are cleared beyond this

enum LiveResult : uint8_t {
	LIVE_UNKNOWN = 0,
	LIVE_FAIL = 1,
	LIVE_BUMPED = 2,
	LIVE_SUCCESS = 3,
	LIVE_STALE = 0x80,  // mode 2: tested from an old eye position
};

struct LivePlane {
	Vector normal;
	float dist;
	Vector ax1, ax2;  // grid axes
	Vector up;        // floors: the facing direction the results are for
	bool floor;
	// grid point (i, j) is at normal*dist + ax1*i*spacing + ax2*j*spacing,
	// keyed so the map is ordered row by row
	std::map<uint64_t, uint8_t> points;
	std::set<uint64_t> doneCells;  // coarse cells fully refined
};

static struct {
	std::map<uint64_t, LivePlane> planes;
	uint64_t current;
	bool hasCurrent;
	long long pointCount;
	// settings the results were made with
	int mode, step, portals;
	float spacing, tolerance;
	Vector eye;
	// the player's portals, which change where new ones can go
	Vector portalPos[2];
	bool portalActive[2];
	BorrowedGun gun;
} g_live;

static uint64_t liveKey(int i, int j) {
	return ((uint64_t)((uint32_t)j ^ 0x80000000u) << 32) | ((uint32_t)i ^ 0x80000000u);
}

static void liveUnkey(uint64_t key, int &i, int &j) {
	i = (int)((uint32_t)key ^ 0x80000000u);
	j = (int)((uint32_t)(key >> 32) ^ 0x80000000u);
}

static void liveClear() {
	g_live.planes.clear();
	g_live.hasCurrent = false;
	g_live.pointCount = 0;
}

CON_COMMAND(sar_pp_live_clear, "sar_pp_live_clear - forget the live placement overlay's results and test again.\n") {
	liveClear();
}

static Vector livePoint(const LivePlane &pl, float i, float j) {
	float s = g_live.spacing;
	return pl.normal * pl.dist + pl.ax1 * (i * s) + pl.ax2 * (j * s);
}

static uint8_t liveTest(uintptr_t portalgun, const LivePlane &pl, int i, int j) {
	Vector target = livePoint(pl, (float)i, (float)j);
	Vector origin, dir;
	if (g_live.mode == 2) {
		origin = g_live.eye;
		dir = (target - origin).Normalize();
	} else {
		dir = -pl.normal;
		if (pl.floor) {
			// the portal is turned the way you're facing, as in an area scan
			dir += pl.up * TEST_FLOOR_UP_CONTRIBUTION;
			dir = dir.Normalize();
		}
		origin = target - dir * TEST_DIST;
	}

	Vector finalPos[2];
	if (!firePortals(portalgun, origin, dir, g_live.portals, finalPos)) return LIVE_FAIL;

	bool moved = false;
	for (int k = 0; k < 2; ++k) {
		if (g_live.portals == 0 && k) continue;
		if (g_live.portals == 1 && !k) continue;
		// landing on another surface (something in the way) doesn't count
		if (fabsf(finalPos[k].Dot(pl.normal) - pl.dist) > 2) return LIVE_FAIL;
		if ((finalPos[k] - target).Length() > g_live.tolerance) moved = true;
	}
	return moved ? LIVE_BUMPED : LIVE_SUCCESS;
}

// The result at a grid point, testing it if it isn't known (or is stale)
static uint8_t liveSample(uintptr_t portalgun, LivePlane &pl, int i, int j, bool &tested) {
	auto [it, added] = pl.points.try_emplace(liveKey(i, j), (uint8_t)LIVE_UNKNOWN);
	if (added) ++g_live.pointCount;
	if (it->second != LIVE_UNKNOWN && !(it->second & LIVE_STALE)) return it->second;
	it->second = liveTest(portalgun, pl, i, j);
	tested = true;
	return it->second;
}

// Refines one coarse cell; false if time ran out first
template <typename F>
static bool liveRefine(uintptr_t portalgun, LivePlane &pl, int x0, int y0, int x1, int y1, F timeUp) {
	struct Cell {
		int x0, y0, x1, y1;
	};
	std::vector<Cell> stack{{x0, y0, x1, y1}};
	while (!stack.empty()) {
		Cell c = stack.back();
		stack.pop_back();

		bool tested = false;
		uint8_t r = liveSample(portalgun, pl, c.x0, c.y0, tested);
		bool same = liveSample(portalgun, pl, c.x1, c.y0, tested) == r;
		same &= liveSample(portalgun, pl, c.x0, c.y1, tested) == r;
		same &= liveSample(portalgun, pl, c.x1, c.y1, tested) == r;

		if (same) {
			for (int j = c.y0; j <= c.y1; ++j) {
				for (int i = c.x0; i <= c.x1; ++i) {
					auto [it, added] = pl.points.try_emplace(liveKey(i, j), r);
					if (added) {
						++g_live.pointCount;
					} else if (it->second == LIVE_UNKNOWN || (it->second & LIVE_STALE)) {
						it->second = r;
					}
				}
			}
		} else {
			bool splitX = c.x1 - c.x0 > 1;
			bool splitY = c.y1 - c.y0 > 1;
			if (splitX || splitY) {
				int xm = splitX ? (c.x0 + c.x1) / 2 : c.x1;
				int ym = splitY ? (c.y0 + c.y1) / 2 : c.y1;
				stack.push_back({c.x0, c.y0, xm, ym});
				if (splitX) stack.push_back({xm, c.y0, c.x1, ym});
				if (splitY) stack.push_back({c.x0, ym, xm, c.y1});
				if (splitX && splitY) stack.push_back({xm, ym, c.x1, c.y1});
			}
		}

		if (tested && timeUp()) return false;
	}
	return true;
}

static uint64_t livePlaneKey(Vector n, float dist, int yawBucket) {
	auto q = [](float v) { return (uint64_t)(uint16_t)(int16_t)lroundf(v * 1000); };
	uint64_t d = (uint64_t)(uint8_t)(lroundf(dist * 4) & 0xFF);
	return q(n.x) | q(n.y) << 16 | q(n.z) << 32 | d << 48 | (uint64_t)yawBucket << 56;
}

// Clears the results when something they depend on changed
static void liveCheckSettings() {
	int mode = sar_pp_live.GetInt();
	int step = sar_pp_live_step.GetInt();
	int portals = sar_pp_scan_portal.GetInt();
	float spacing = sar_pp_live_spacing.GetFloat();
	float tolerance = sar_pp_live_tolerance.GetFloat();
	if (mode != g_live.mode || step != g_live.step || portals != g_live.portals || spacing != g_live.spacing || tolerance != g_live.tolerance) {
		liveClear();
		g_live.mode = mode;
		g_live.step = step;
		g_live.portals = portals;
		g_live.spacing = spacing;
		g_live.tolerance = tolerance;
	}

	// a portal you place (or one that fizzles) changes where others can go
	void *player = server->GetPlayer(GET_SLOT() + 1);
	if (!player) return;
	auto gunHandle = SE(player)->active_weapon();
	uintptr_t portalgun = (uintptr_t)entityList->LookupEntity(gunHandle);
	if (!portalgun || !entityList->IsPortalGun(gunHandle)) return;
	const char *fields[2] = {"m_hPrimaryPortal", "m_hSecondaryPortal"};
	for (int k = 0; k < 2; ++k) {
		auto portal = (uintptr_t)entityList->LookupEntity(SE(portalgun)->field<CBaseHandle>(fields[k]));
		bool active = portal && SE(portal)->field<bool>("m_bActivated");
		Vector pos = active ? SE(portal)->abs_origin() : Vector{0, 0, 0};
		if (active != g_live.portalActive[k] || (active && (pos - g_live.portalPos[k]).Length() > 0.01f)) {
			liveClear();
			g_live.portalActive[k] = active;
			g_live.portalPos[k] = pos;
		}
	}
}

static void liveUpdate() {
	liveCheckSettings();

	auto tr = camTrace();
	if (!tr) return;

	Vector n = tr->plane.normal;
	float dist = tr->plane.dist;
	bool floor = fabsf(n.z) > 0.999f;

	Vector eye;
	QAngle eyeAng;
	camera->GetEyePos<true>(GET_SLOT(), eye, eyeAng);

	// mode 1 floors: results are per facing direction (the portal turns with it)
	int yawBucket = 0;
	Vector up{0, 0, 1};
	if (floor && g_live.mode == 1) {
		int buckets = (int)(360.0f / LIVE_FLOOR_YAW_STEP);
		yawBucket = ((int)lroundf(eyeAng.y / LIVE_FLOOR_YAW_STEP) % buckets + buckets) % buckets;
		QAngle yaw{0, yawBucket * LIVE_FLOOR_YAW_STEP, 0};
		Math::AngleVectors(yaw, &up);
	}

	uint64_t key = livePlaneKey(n, dist, yawBucket);
	if (g_live.mode == 2 && (!g_live.hasCurrent || key != g_live.current)) {
		// only the surface you're aiming at is kept
		liveClear();
	}
	if (g_live.pointCount > LIVE_MAX_POINTS) liveClear();

	auto [it, added] = g_live.planes.try_emplace(key);
	LivePlane &pl = it->second;
	if (added) {
		pl.normal = n;
		pl.dist = dist;
		pl.floor = floor;
		pl.up = up;
		// grid axes that don't depend on where you look from
		pl.ax1 = floor ? Vector{1, 0, 0} : n.Cross(Vector{0, 0, 1});
		pl.ax1 = pl.ax1.Normalize();
		pl.ax2 = n.Cross(pl.ax1).Normalize();
		g_live.eye = eye;
	}
	g_live.current = key;
	g_live.hasCurrent = true;

	if (g_live.mode == 2 && (eye - g_live.eye).Length() > sar_pp_live_eye_move.GetFloat()) {
		g_live.eye = eye;
		for (auto &p : pl.points) {
			if (p.second != LIVE_UNKNOWN) p.second |= LIVE_STALE;
		}
		pl.doneCells.clear();
	}

	if (borrowPortalGun(g_live.gun)) return;
	uintptr_t portalgun = getPortalGun(g_live.gun);

	auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds((long long)(sar_pp_live_budget.GetFloat() * 1000));
	auto timeUp = [&]() { return std::chrono::steady_clock::now() >= deadline; };

	float s = g_live.spacing;
	int k = g_live.step;
	int ci = (int)floorf(tr->endpos.Dot(pl.ax1) / s / k);
	int cj = (int)floorf(tr->endpos.Dot(pl.ax2) / s / k);
	int rc = (int)ceilf(sar_pp_live_radius.GetFloat() / (s * k));

	bool out = false;
	// coarse points, nearest the crosshair first
	for (int r = 0; r <= rc && !out; ++r) {
		for (int a = -r; a <= r && !out; ++a) {
			for (int b = -r; b <= r; ++b) {
				if (abs(a) != r && abs(b) != r) continue;
				bool tested = false;
				liveSample(portalgun, pl, (ci + a) * k, (cj + b) * k, tested);
				if (tested && timeUp()) {
					out = true;
					break;
				}
			}
		}
	}
	// then refine the cells between them, nearest first
	if (k > 1) {
		for (int r = 0; r <= rc && !out; ++r) {
			for (int a = -r; a <= r && !out; ++a) {
				for (int b = -r; b <= r; ++b) {
					if (abs(a) != r && abs(b) != r) continue;
					if (a == rc || b == rc) continue;
					int x0 = (ci + a) * k, y0 = (cj + b) * k;
					uint64_t cell = liveKey(x0, y0);
					if (pl.doneCells.count(cell)) continue;
					if (!liveRefine(portalgun, pl, x0, y0, x0 + k, y0 + k, timeUp)) {
						out = true;
						break;
					}
					pl.doneCells.insert(cell);
				}
			}
		}
	}

	returnPortalGun(g_live.gun);
}

static void liveDraw() {
	if (g_live.planes.empty()) return;

	uint8_t alpha = (uint8_t)sar_pp_live_opacity.GetInt();
	Color colors[4];
	colors[LIVE_FAIL] = Utils::GetColor(sar_pp_scan_color_fail.GetString(), false).value_or(Color(255, 0, 0));
	colors[LIVE_BUMPED] = Utils::GetColor(sar_pp_live_color_bumped.GetString(), false).value_or(Color(230, 190, 60));
	colors[LIVE_SUCCESS] = Utils::GetColor(sar_pp_scan_color_success.GetString(), false).value_or(Color(0, 255, 0));
	MeshId meshes[4];
	bool hasMesh[4] = {};

	Vector cam = camera->GetPosition(GET_SLOT());
	float drawDist = sar_pp_live_draw_dist.GetFloat();

	for (auto &[key, pl] : g_live.planes) {
		if (fabsf(cam.Dot(pl.normal) - pl.dist) > drawDist) continue;
		Vector zf = pl.normal * 0.05f;  // z-fighting

		// one quad per run of equal results along a row
		auto flush = [&](int i0, int i1, int j, uint8_t r) {
			if (r == LIVE_UNKNOWN) return;
			Vector a = livePoint(pl, i0 - 0.5f, j - 0.5f);
			if ((a - cam).Length() > drawDist) return;
			Vector b = livePoint(pl, i1 + 0.5f, j - 0.5f);
			Vector c = livePoint(pl, i1 + 0.5f, j + 0.5f);
			Vector d = livePoint(pl, i0 - 0.5f, j + 0.5f);
			if (!hasMesh[r]) {
				Color col = colors[r];
				col.a = alpha;
				meshes[r] = OverlayRender::createMesh(RenderCallback::constant(col), RenderCallback::none);
				hasMesh[r] = true;
			}
			OverlayRender::addQuad(meshes[r], a + zf, b + zf, c + zf, d + zf);
		};

		bool inRun = false;
		int runI0 = 0, runI1 = 0, runJ = 0;
		uint8_t runR = 0;
		for (auto &[pkey, value] : pl.points) {
			int i, j;
			liveUnkey(pkey, i, j);
			uint8_t r = value & ~LIVE_STALE;
			if (inRun && j == runJ && i == runI1 + 1 && r == runR) {
				runI1 = i;
				continue;
			}
			if (inRun) flush(runI0, runI1, runJ, runR);
			inRun = true;
			runI0 = runI1 = i;
			runJ = j;
			runR = r;
		}
		if (inRun) flush(runI0, runI1, runJ, runR);
	}
}

ON_EVENT(FRAME) {
	if (!sar_pp_live.GetBool()) {
		if (!g_live.planes.empty()) liveClear();
		g_live.mode = 0;
		return;
	}
	// the scanner has the portal gun while it runs
	if (scanningUnavailableReason() || g_setup.state == SetupState::RUNNING) return;
	liveUpdate();
}

ON_EVENT(RENDER) {
	if (!sar_pp_live.GetBool() || scanningUnavailableReason()) return;
	liveDraw();
}

ON_EVENT(SESSION_START) {
	liveClear();
}
