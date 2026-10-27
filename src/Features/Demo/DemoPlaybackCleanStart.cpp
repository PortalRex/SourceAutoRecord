#include <Event.hpp>
#include <Variable.hpp>
#include <Modules/Client.hpp>
#include <Modules/Engine.hpp>
#include <Features/Renderer.hpp>
#include <Features/Session.hpp>
#include <Interface.hpp>
#include <Utils/Memory.hpp>
#include <Utils/SDK/UtlMemory.hpp>

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
// In co-op it plays on each player once at the load and again when they spawn, as the transition video ends (up to
// ~350 ticks after the load, which is when SAR's render resumes).
const int TRANSITION_SOUND_FRAMES = 15, TRANSITION_SOUND_LOAD_FRAMES = 600, TRANSITION_SOUND_COOP_FRAMES = 600;
const int TRANSITION_SOUND_COOP_SPAWN_FRAMES = 60, TRANSITION_SOUND_COOP_HOLD_FRAMES = 30, TRANSITION_SOUND_COOP_RAMP_FRAMES = 30;
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
	if (coop && stopped && g_transitionSoundFrames < TRANSITION_SOUND_COOP_FRAMES - TRANSITION_SOUND_COOP_SPAWN_FRAMES) {
		g_transitionSoundUnmuteFrames = TRANSITION_SOUND_COOP_HOLD_FRAMES;
		g_transitionSoundRampFrames = 0;
	}
	// SAR's frame event runs after the frame's audio is mixed: keep the frame after the stop muted too, and unmute
	// once the stopped sound is out of the mix. The co-op spawn sound leaves a reverb tail, so it's held longer
	// (the screen is still black) and faded back in.
	if (g_transitionSoundUnmuteFrames > 0 && --g_transitionSoundUnmuteFrames == 0) {
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

ON_EVENT(FRAME) {
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
