#include <Event.hpp>
#include <Variable.hpp>
#include <Modules/Client.hpp>
#include <Modules/Engine.hpp>
#include <Features/Renderer.hpp>
#include <Features/Session.hpp>
#include <Interface.hpp>
#include <Utils/Memory.hpp>
#include <Utils/SDK/UtlMemory.hpp>
#include <Utils/Math.hpp>
#include <Entity.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

Variable sar_demo_clean_start("sar_demo_clean_start", "0", 0,  
	"Attempts to minimize visual interpolation of some elements (like post-processing or lighting) when demo playback begins.\n", 0);
Variable sar_demo_clean_start_tonemap("sar_demo_clean_start_tonemap", "0", 0, 
	"Overrides initial tonemap scalar value used in auto-exposure.\n"
	"Setting it to 0 will attempt to skip over to target value for several ticks.\n", 0);


Variable sar_demo_clean_transitions("sar_demo_clean_transitions", "1", 0,
	"When a demo loads another map mid-playback (a spliced demo), starts the new map with the previous map's "
	"auto-exposure and quickly eases to the new scene's, instead of fading in from the default, and skips "
	"projected textures to their color, keeps the viewmodel from replaying its draw animation, and stops the portal gun's power-up sound.\n", 0);

Variable sar_demo_clean_portals("sar_demo_clean_portals", "0", 0,
	"In demo playback, a new portal opens four times as fast and shows the view through it without the static the "
	"game fades out over a second after a portal is placed (or its partner is), so a run that enters a portal right "
	"after shooting it sees through it, and particles aren't drawn in the few frames the camera passes through a "
	"portal (its sparks would fill the screen). Unlinked portals keep their static.\n", 0);


bool g_tonemapScaleDemoLookupInProgress = false;
int g_tonemapScaleDemoLookupTick;
bool g_tonemapScaleContinuousSearchEnabled = false;
int g_tonemapScaleTweakStartTick = 0;

const float TONEMAP_SCALE_SEARCH_DISABLE_THRESHOLD = 0.01f;
const int TONEMAP_SCALE_SEARCH_MAX_NOCHANGE_TICKS = 30;

void startTonemapScaleDemoLookup(int tick) {
	g_tonemapScaleDemoLookupInProgress = true;
	g_tonemapScaleDemoLookupTick = tick;
}

void stopTonemapScaleDemoLookup(bool finished = false) {
	if (!g_tonemapScaleDemoLookupInProgress) {
		return;
	}

	if (!finished) {
		console->Print("Failed to sample tonemap scale before the end of the demo.\n");
	}
	g_tonemapScaleDemoLookupInProgress = false;
	g_tonemapScaleDemoLookupTick = -1;
}

void updateTonemapScaleDemoLookup() {
	if (!g_tonemapScaleDemoLookupInProgress || !client->GetCurrentTonemappingSystem) {
		return;
	}

	if (!engine->demoplayer->IsPlaying() || engine->demoplayer->GetTick() < g_tonemapScaleDemoLookupTick) {
		return;
	}

	float tonemapScale = client->GetCurrentTonemappingSystem()->m_flCurrentTonemapScale;
	sar_demo_clean_start_tonemap.SetValue(tonemapScale);
	console->Print("Sampled tonemap scale value %.6f and stored it in \"sar_demo_clean_start_tonemap\" variable.\n", tonemapScale);
	
	stopTonemapScaleDemoLookup(true);
}

void triggerTonemapScaleTweak(float tonemapScaleOverride, bool search) {
	if (!client->GetCurrentTonemappingSystem || !client->ResetToneMapping) {
		return;
	}

	if (search) {
		g_tonemapScaleContinuousSearchEnabled = true;
		auto tonemapSystem = client->GetCurrentTonemappingSystem();
		g_tonemapScaleTweakStartTick = tonemapSystem->m_nCurrentQueryFrame;
	}

	client->ResetToneMapping(tonemapScaleOverride);
}

void disableTonemapScaleContinuousSearch() {
	g_tonemapScaleContinuousSearchEnabled = false;
}

void updateTonemapScaleContinuousSearch() {
	if (!g_tonemapScaleContinuousSearchEnabled || !client->GetCurrentTonemappingSystem) {
		return;
	}

	auto tonemapSystem = client->GetCurrentTonemappingSystem();
	if (tonemapSystem == nullptr) {
		disableTonemapScaleContinuousSearch();
		return;
	}

	float diff = tonemapSystem->m_flTargetTonemapScale - tonemapSystem->m_flCurrentTonemapScale;

	if (fabsf(diff) > TONEMAP_SCALE_SEARCH_DISABLE_THRESHOLD) {
		// last sampled luminance was different enough to change target scale, reset it
		tonemapSystem->m_flCurrentTonemapScale = tonemapSystem->m_flTargetTonemapScale;
		return;
	}
	
	int searchDuration = tonemapSystem->m_nCurrentQueryFrame - g_tonemapScaleTweakStartTick;
	if (searchDuration > TONEMAP_SCALE_SEARCH_MAX_NOCHANGE_TICKS) {
		disableTonemapScaleContinuousSearch();
	}
}

void forceProjectedTexturesTargetColor() {
	for (int i = 0; i < Offsets::NUM_ENT_ENTRIES; ++i) {
		auto clientEntity = client->GetEntity(i);
		if (!clientEntity) {
			continue;
		}

		if (strcmp(clientEntity->GetClientClass()->m_pNetworkName, "CEnvProjectedTexture") != 0) {
			continue;
		}

		auto targetColor = clientEntity->field<color32>("m_LightColor");
		auto &m_CurrentLinearFloatLightColor = clientEntity->fieldOff<Vector>("m_LightColor", 0x04);
		auto &m_flCurrentLinearFloatLightAlpha = clientEntity->fieldOff<float>("m_LightColor", 0x10);

		m_CurrentLinearFloatLightColor = Vector(targetColor.r, targetColor.g, targetColor.b);
		m_flCurrentLinearFloatLightAlpha = targetColor.a;
	}
}

CON_COMMAND(sar_demo_clean_start_tonemap_sample,
            "sar_demo_clean_start_tonemap_sample [tick] - samples tonemap scale from current demo "
            "at given tick and stores it in \"sar_demo_clean_start_tonemap\" variable. "
            "If no tick is given, sampling will happen when `__END__` is seen in demo playback.\n") {
	if (args.ArgC() > 2) {
		return console->Print(sar_demo_clean_start_tonemap_sample.ThisPtr()->m_pszHelpString);
	}

	if (!engine->demoplayer->IsPlaying()) {
		return console->Print("No demo is currently being played back.\n");
	}

	g_tonemapScaleDemoLookupInProgress = true;

	if (args.ArgC() == 2) {
		startTonemapScaleDemoLookup(std::atoi(args[1]));
		return console->Print("Tonemap sampling set up to trigger at tick %d\n", g_tonemapScaleDemoLookupTick);
	} else {
		startTonemapScaleDemoLookup(Renderer::segmentEndTick);
		return console->Print("Tonemap sampling set up to trigger at __END__ tick (%d)\n", g_tonemapScaleDemoLookupTick);
	}
}


// exposure the previous map ended on, while a demo is playing (0: none)
float g_transitionTonemapScale = 0.0f;
// frames left of the fast exposure adaptation after a mid-demo map load
int g_transitionAdaptFrames = 0;

// Fraction of the way to the measured exposure covered each frame after a mid-demo map load: the new map settles
// in about a quarter of a second, and early, noisy measurements are smoothed out instead of jumped to.
const float TRANSITION_ADAPT_RATE = 0.2f;
const int TRANSITION_ADAPT_FRAMES = 60;

void updateTransitionAdapt() {
	if (g_transitionAdaptFrames <= 0) return;
	if (!client->GetCurrentTonemappingSystem || !engine->demoplayer->IsPlaying()) {
		g_transitionAdaptFrames = 0;
		return;
	}
	auto tonemapSystem = client->GetCurrentTonemappingSystem();
	if (!tonemapSystem) {
		g_transitionAdaptFrames = 0;
		return;
	}
	tonemapSystem->m_flCurrentTonemapScale += (tonemapSystem->m_flTargetTonemapScale - tonemapSystem->m_flCurrentTonemapScale) * TRANSITION_ADAPT_RATE;
	--g_transitionAdaptFrames;
}

// After a mid-demo map load the player is given their weapon again, and its viewmodel plays the draw animation:
// the gun drops out of view and is raised, which the loading screen normally hides. The viewmodel's animation
// cycle runs client-side, so while it still plays the sequence it started the map with, it's held at the end.
const int TRANSITION_VIEWMODEL_FRAMES = 180;
int g_transitionViewmodelFrames = 0;
std::vector<int> g_transitionViewmodelSequence; // per entity: the sequence it started with, -1 unseen, -2 done

void updateTransitionViewmodel() {
	if (g_transitionViewmodelFrames <= 0) return;
	if (!engine->demoplayer->IsPlaying()) {
		g_transitionViewmodelFrames = 0;
		return;
	}
	--g_transitionViewmodelFrames;
	for (int i = 0; i < (int)g_transitionViewmodelSequence.size(); ++i) {
		if (g_transitionViewmodelSequence[i] == -2) continue;
		auto ent = client->GetEntity(i);
		if (!ent) continue;
		auto cc = ent->GetClientClass();
		if (!cc || !cc->m_pNetworkName || !strstr(cc->m_pNetworkName, "ViewModel")) continue;
		int sequence = ent->field<int>("m_nSequence");
		if (g_transitionViewmodelSequence[i] == -1) g_transitionViewmodelSequence[i] = sequence;
		if (sequence != g_transitionViewmodelSequence[i]) {
			g_transitionViewmodelSequence[i] = -2;
			continue;
		}
		ent->field<float>("m_flCycle") = 1.0f;
	}
}

// After a mid-demo map load, playback plays the portal gun's power-up sound (Portal.PortalgunActivate, from the
// gun being deployed again) on the player, normally under the loading screen. It isn't a sound the demo carries,
// so it's stopped here: a one-shot on the player's static channel at the sound script's volume (0.9), within the
// first frames after the load (watched from the end of the previous map, so it's caught on the frame it starts).
// In co-op it plays on each player once at the load and again when they spawn, after the transition video's fade
// (from a few frames to ~350 ticks after the load; SAR's render resumes at that fade).
const int TRANSITION_SOUND_FRAMES = 15, TRANSITION_SOUND_LOAD_FRAMES = 600, TRANSITION_SOUND_COOP_FRAMES = 600;
const int TRANSITION_SOUND_COOP_HOLD_FRAMES = 30, TRANSITION_SOUND_COOP_RAMP_FRAMES = 30;
int g_transitionSoundFrames = 0;
int g_transitionSoundUnmuteFrames = 0;
int g_transitionSoundRampFrames = 0;

struct SndInfo_t { // IEngineSound::GetActiveSounds entry (0x34 bytes)
	int m_nGuid;
	int m_filenameHandle;
	int m_nSoundSource;
	int m_nChannel;
	int m_nSpeakerEntity;
	float m_flVolume;
	float m_flLastSpatializedVolume;
	float m_flRadius;
	int m_nPitch;
	void *m_pOrigin;
	void *m_pDirection;
	bool m_bUpdatePositions, m_bIsSentence, m_bDryMix, m_bSpeaker, m_bFromServer;
};
static_assert(sizeof(SndInfo_t) == 0x34, "SndInfo_t size");

const int ENGINESOUND_STOPSOUNDBYGUID = 16, ENGINESOUND_GETACTIVESOUNDS = 18;
const int CHAN_STATIC = 6;
const int FILESYSTEM_STRING = 40; // IFileSystem::String(const FileNameHandle_t &, char *, int)

// Whether an active sound plays the power-up sound's file (weapon_ambient\wpn_portalgun_activation_01.wav). If the
// name can't be read, the channel and volume checks alone decide, as before.
bool isTransitionStartSound(int filenameHandle) {
	static void *fs = Interface::GetPtr(MODULE("filesystem_stdio"), "VFileSystem017");
	if (!fs) return true;
	char name[260] = "";
	if (!Memory::VMT<bool(__rescall *)(void *, const int &, char *, int)>(fs, FILESYSTEM_STRING)(fs, filenameHandle, name, sizeof(name))) return true;
	return strstr(name, "wpn_portalgun_activation") != nullptr;
}

bool stopTransitionStartSounds() {
	static void *engineSound = Interface::GetPtr(MODULE("engine"), "IEngineSoundClient003");
	if (!engineSound) return false;

	// a buffer the engine treats as external (grow size -1), big enough that it never needs to grow it
	const int capacity = 1024;
	std::vector<SndInfo_t> buffer(capacity);
	CUtlVector<SndInfo_t> sounds;
	sounds.m_Memory.m_pMemory = buffer.data();
	sounds.m_Memory.m_nAllocationCount = capacity;
	sounds.m_Memory.m_nGrowSize = -1;
	sounds.m_Size = 0;
	sounds.m_pElements = buffer.data();
	Memory::VMT<void(__rescall *)(void *, CUtlVector<SndInfo_t> &)>(engineSound, ENGINESOUND_GETACTIVESOUNDS)(engineSound, sounds);

	bool stopped = false;
	for (int i = 0; i < sounds.m_Size && i < capacity; ++i) {
		auto &sound = buffer[i];
		if (sound.m_nSoundSource < 1 || sound.m_nSoundSource > engine->GetMaxClients() || sound.m_nChannel != CHAN_STATIC) continue;
		if (sound.m_flVolume < 0.85f || sound.m_flVolume > 0.95f) continue;
		// Other sounds pass the checks above: the falling-wind loop on the same channel climbs through 0.9 while a
		// player falls fast. Stopping it muted the whole mix for a second after a fall early in a co-op map.
		if (!isTransitionStartSound(sound.m_filenameHandle)) continue;
		Memory::VMT<void(__rescall *)(void *, int, bool)>(engineSound, ENGINESOUND_STOPSOUNDBYGUID)(engineSound, sound.m_nGuid, true);
		stopped = true;
	}
	return stopped;
}

// The frame that starts the sound is mixed before it can be stopped (about 20 ms of its attack). The master volume
// is held at 0 from the end of the previous map until the new map's first frames are past (co-op: until the
// players' spawn sound is stopped); the cut is quiet at that point anyway.
float g_transitionSavedVolume = -1.0f;

void restoreTransitionVolume() {
	if (g_transitionSavedVolume < 0.0f) return;
	Variable("volume").SetValue(g_transitionSavedVolume);
	g_transitionSavedVolume = -1.0f;
}

void muteTransitionVolume() {
	Variable volume("volume");
	if (!volume.ThisPtr() || g_transitionSavedVolume >= 0.0f) return;
	g_transitionSavedVolume = volume.GetFloat();
	volume.SetValue(0.0f);
}

void updateTransitionSounds() {
	if (g_transitionSoundFrames <= 0) return;
	if (!engine->demoplayer->IsPlaying()) {
		g_transitionSoundFrames = 0;
		restoreTransitionVolume();
		return;
	}
	--g_transitionSoundFrames;
	bool stopped = stopTransitionStartSounds();
	if (!session->isRunning) return;
	bool coop = engine->GetMaxClients() >= 2;
	if (coop && stopped) {
		// each one (the load's, then the spawn's) mutes again, even if the sound was already coming back
		muteTransitionVolume();
		if (g_transitionSavedVolume >= 0.0f) Variable("volume").SetValue(0.0f);
		g_transitionSoundUnmuteFrames = TRANSITION_SOUND_COOP_HOLD_FRAMES;
		g_transitionSoundRampFrames = 0;
	}
	// SAR's frame event runs after the frame's audio is mixed: keep the frame after the stop muted too, and unmute
	// once the stopped sound is out of the mix. The co-op sounds leave a reverb tail, so they're held longer and
	// faded back in; and nothing comes back while the transition video still shows (the spawn's sound follows its
	// fade, so its first frame is mixed muted).
	bool videoShowing = coop && !engine->startedTransitionFadeout;
	if (!videoShowing && g_transitionSoundUnmuteFrames > 0 && --g_transitionSoundUnmuteFrames == 0) {
		if (coop) {
			g_transitionSoundRampFrames = TRANSITION_SOUND_COOP_RAMP_FRAMES;
		} else {
			restoreTransitionVolume();
		}
	}
	if (g_transitionSoundRampFrames > 0) {
		if (--g_transitionSoundRampFrames == 0) {
			restoreTransitionVolume();
		} else if (g_transitionSavedVolume >= 0.0f) {
			float ramp = 1.0f - (float)g_transitionSoundRampFrames / TRANSITION_SOUND_COOP_RAMP_FRAMES;
			Variable("volume").SetValue(g_transitionSavedVolume * ramp);
		}
	}
	if (g_transitionSoundFrames == 0) restoreTransitionVolume();
}

// A placed portal, and the portal linked to it, get full static (C_Prop_Portal::m_fStaticAmount = 1) that its
// client think fades out by frame time over one second; it's drawn over the view through the portal, so a run that
// enters a portal right after shooting it shows mostly static. Its opening animation (m_fOpenAmount, 0 to 1 in half a
// second) also hides the view through it until it's 85% open. sar_demo_clean_portals clears the static just before
// each view is drawn and runs the opening four times as fast (an eighth of a second).
const int PORTAL_STATIC_AMOUNT = 0x3510, PORTAL_SECONDARY_STATIC_AMOUNT = 0x3514, PORTAL_OPEN_AMOUNT = 0x3518;
const float PORTAL_OPEN_RATE = 2.0f, PORTAL_OPEN_SPEEDUP = 4.0f; // the game opens a portal at 2 per second
std::vector<int> g_demoPortals;
bool g_demoPortalsOpened = false; // this frame's extra opening is done (several views can be drawn per frame)

void findDemoPortals() {
	g_demoPortals.clear();
	g_demoPortalsOpened = false;
	if (!sar_demo_clean_portals.GetBool() || !engine->demoplayer->IsPlaying() || !session->isRunning) return;
	for (int i = 1; i < Offsets::NUM_ENT_ENTRIES; ++i) {
		auto ent = client->GetEntity(i);
		if (!ent) continue;
		auto cc = ent->GetClientClass();
		if (cc && cc->m_pNetworkName && !strcmp(cc->m_pNetworkName, "CProp_Portal")) g_demoPortals.push_back(i);
	}
}

// The sparks that stream into every portal fill the screen as the camera dives into one. While the camera is about to
// pass through an open portal, or just has, particles aren't drawn (r_drawparticles 0). Standing near a portal doesn't
// count: the camera has to be heading in. Turning them off only clears the screen if it's done before the camera gets
// close (once drawn up close they stay on screen), so it looks ahead PORTAL_PASS_SECONDS, and it's decided at the start
// of the frame, from the camera of the frame before moved on by one frame.
const float PORTAL_HALF_WIDTH = 32.0f, PORTAL_HALF_HEIGHT = 56.0f, PORTAL_PASS_MARGIN = 48.0f;
const float PORTAL_PASS_SECONDS = 0.6f, PORTAL_PASS_NEAR = 16.0f, PORTAL_PASS_BEHIND = 32.0f;
float g_savedDrawParticles = -1.0f; // r_drawparticles while it's held at 0
Vector g_demoCamera, g_demoCameraVelocity; // the main view's camera in the last frame, and how fast it moved
bool g_demoCameraValid = false;

void restoreDemoParticles() {
	if (g_savedDrawParticles < 0.0f) return;
	Variable("r_drawparticles").SetValue(g_savedDrawParticles);
	g_savedDrawParticles = -1.0f;
}

bool isOverPortalOpening(const Vector &rel, const Vector &right, const Vector &up) {
	return fabsf(rel.Dot(right)) < PORTAL_HALF_WIDTH + PORTAL_PASS_MARGIN && fabsf(rel.Dot(up)) < PORTAL_HALF_HEIGHT + PORTAL_PASS_MARGIN;
}

// Whether the camera, moving at `velocity`, is passing through the portal: right at its opening or just behind it, or
// heading into the opening and through it within PORTAL_PASS_SECONDS.
bool isPassingThroughPortal(ClientEnt *portal, const Vector &camera, const Vector &velocity) {
	auto activated = EntField::getClientOffset(portal, "m_bActivated");
	if (activated.second == EntField::Type::NONE || !*(bool *)((uintptr_t)portal + activated.first)) return false;
	Vector forward, right, up;
	Math::AngleVectors(portal->abs_angles(), &forward, &right, &up);
	Vector rel = camera - portal->abs_origin();
	float depth = rel.Dot(forward);
	if (depth < PORTAL_PASS_NEAR && depth > -PORTAL_PASS_BEHIND) return isOverPortalOpening(rel, right, up);
	if (depth < 0.0f) return false;
	float approach = -velocity.Dot(forward);
	if (approach <= 0.0f || depth > approach * PORTAL_PASS_SECONDS) return false;
	// where it crosses the portal's plane
	return isOverPortalOpening(rel + velocity * (depth / approach), right, up);
}

bool isDemoPortal(ClientEnt *ent) {
	if (!ent) return false;
	auto cc = ent->GetClientClass();
	return cc && cc->m_pNetworkName && !strcmp(cc->m_pNetworkName, "CProp_Portal");
}

// Start of a frame: particles off while the camera passes through a portal.
void updateDemoPortalParticles() {
	bool hide = false;
	if (!g_demoPortals.empty() && g_demoCameraValid) {
		Vector camera = g_demoCamera + g_demoCameraVelocity * engine->GetHostFrameTime();
		for (int i : g_demoPortals) {
			auto ent = client->GetEntity(i);
			if (isDemoPortal(ent) && isPassingThroughPortal(ent, camera, g_demoCameraVelocity)) {
				hide = true;
				break;
			}
		}
	}
	if (!hide) {
		restoreDemoParticles();
		return;
	}
	Variable drawParticles("r_drawparticles");
	if (drawParticles.ThisPtr() && g_savedDrawParticles < 0.0f) {
		g_savedDrawParticles = drawParticles.GetFloat();
		drawParticles.SetValue(0);
	}
}

// Just before a view is drawn: no static, faster opening; and where the camera is, for the next frame.
void ClearDemoPortalStatic(const Vector &camera) {
	if (g_demoPortals.empty() || !engine->demoplayer->IsPlaying()) return;
	float frameTime = engine->GetHostFrameTime();
	// the main view comes first; the co-op partner's picture-in-picture view doesn't come through here
	if (!g_demoPortalsOpened) {
		g_demoCameraVelocity = g_demoCameraValid && frameTime > 0.0f ? (camera - g_demoCamera) * (1.0f / frameTime) : Vector();
		g_demoCamera = camera;
		g_demoCameraValid = true;
	}
	// the game's think already added this frame's share of the opening
	float extraOpen = g_demoPortalsOpened ? 0.0f : frameTime * PORTAL_OPEN_RATE * (PORTAL_OPEN_SPEEDUP - 1.0f);
	g_demoPortalsOpened = true;
	for (int i : g_demoPortals) {
		auto ent = client->GetEntity(i);
		if (!isDemoPortal(ent)) continue;
		auto base = (char *)ent;
		float &staticAmount = *(float *)(base + PORTAL_STATIC_AMOUNT);
		float &secondaryStaticAmount = *(float *)(base + PORTAL_SECONDARY_STATIC_AMOUNT);
		float &openAmount = *(float *)(base + PORTAL_OPEN_AMOUNT);
		// all three are 0..1; anything else means the layout isn't the one these offsets are for
		if (openAmount < 0.0f || openAmount > 1.0f || staticAmount < 0.0f || staticAmount > 1.0f) continue;
		if (secondaryStaticAmount < 0.0f || secondaryStaticAmount > 1.0f) continue;
		staticAmount = 0.0f;
		secondaryStaticAmount = 0.0f;
		if (openAmount > 0.0f && openAmount < 1.0f) openAmount = std::min(openAmount + extraOpen, 1.0f);
	}
}

ON_EVENT(FRAME) {
	findDemoPortals();
	if (g_demoPortals.empty()) g_demoCameraValid = false;
	updateDemoPortalParticles();
	updateTonemapScaleContinuousSearch();
	updateTransitionSounds();
	updateTransitionAdapt();
	updateTransitionViewmodel();
	updateTonemapScaleDemoLookup();
}

ON_EVENT(DEMO_START) {
	g_transitionTonemapScale = 0.0f;

	if (!sar_demo_clean_start.GetBool()) {
		return;
	}

	float tonemapScaleOverride = sar_demo_clean_start_tonemap.GetFloat();
	triggerTonemapScaleTweak(tonemapScaleOverride, tonemapScaleOverride == 0.0f);
	forceProjectedTexturesTargetColor();
}

ON_EVENT(DEMO_STOP) {
	g_transitionTonemapScale = 0.0f;
	g_transitionAdaptFrames = 0;
	g_transitionViewmodelFrames = 0;
	g_transitionSoundFrames = 0;
	restoreDemoParticles();
	restoreTransitionVolume();
	disableTonemapScaleContinuousSearch();
	stopTonemapScaleDemoLookup();
}

// A level change inside a demo: the client resets auto-exposure as the next map loads (normally behind the
// loading screen), so the map would fade in from a default exposure. Start it from where the previous map was
// and ease to the new scene's value within a few frames.
ON_EVENT(SESSION_END) {
	g_transitionTonemapScale = 0.0f;
	if (!sar_demo_clean_transitions.GetBool() || !engine->demoplayer->IsPlaying() || engine->demoplayer->GetTick() <= 0) return;
	if (!client->GetCurrentTonemappingSystem) return;
	auto tonemapSystem = client->GetCurrentTonemappingSystem();
	if (tonemapSystem) g_transitionTonemapScale = tonemapSystem->m_flCurrentTonemapScale;
	g_transitionSoundFrames = TRANSITION_SOUND_LOAD_FRAMES;
	g_transitionSoundUnmuteFrames = 0;
	g_transitionSoundRampFrames = 0;
	muteTransitionVolume();
}

ON_EVENT(SESSION_START) {
	float tonemapScale = g_transitionTonemapScale;
	g_transitionTonemapScale = 0.0f;
	if (!sar_demo_clean_transitions.GetBool() || !engine->demoplayer->IsPlaying()) return;

	// the power-up sound after a mid-demo load (watched since SESSION_END)
	if (g_transitionSoundFrames > 0) {
		bool coop = engine->GetMaxClients() >= 2;
		g_transitionSoundFrames = coop ? TRANSITION_SOUND_COOP_FRAMES : TRANSITION_SOUND_FRAMES;
		g_transitionSoundUnmuteFrames = coop ? 0 : 2;
		g_transitionSoundRampFrames = 0;
		updateTransitionSounds();
	}

	// The new map's first frame is drawn before its viewmodel exists: the gun is missing (PotatOS floating
	// where it should be) for one frame, a visible jolt at every single-player cut. That frame is left out
	// of a render. (Co-op renders resume later, at the transition video's fade.)
	if (engine->GetMaxClients() < 2) Renderer::SkipFrame();

	if (tonemapScale <= 0.0f) return;
	triggerTonemapScaleTweak(tonemapScale, false);
	g_transitionAdaptFrames = TRANSITION_ADAPT_FRAMES;
	g_transitionViewmodelFrames = TRANSITION_VIEWMODEL_FRAMES;
	g_transitionViewmodelSequence.assign(Offsets::NUM_ENT_ENTRIES, -1);
	forceProjectedTexturesTargetColor();
}
