#include <Event.hpp>
#include <Variable.hpp>
#include <Modules/Client.hpp>
#include <Modules/Engine.hpp>
#include <Features/Renderer.hpp>

Variable sar_demo_clean_start("sar_demo_clean_start", "0", 0,  
	"Attempts to minimize visual interpolation of some elements (like post-processing or lighting) when demo playback begins.\n", 0);
Variable sar_demo_clean_start_tonemap("sar_demo_clean_start_tonemap", "0", 0, 
	"Overrides initial tonemap scalar value used in auto-exposure.\n"
	"Setting it to 0 will attempt to skip over to target value for several ticks.\n", 0);


Variable sar_demo_clean_transitions("sar_demo_clean_transitions", "1", 0,
	"When a demo loads another map mid-playback (a spliced demo), starts the new map with the previous map's "
	"auto-exposure and quickly eases to the new scene's, instead of fading in from the default, and skips "
	"projected textures to their color.\n", 0);


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

ON_EVENT(FRAME) {
	updateTonemapScaleContinuousSearch();
	updateTransitionAdapt();
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
}

ON_EVENT(SESSION_START) {
	float tonemapScale = g_transitionTonemapScale;
	g_transitionTonemapScale = 0.0f;
	if (tonemapScale <= 0.0f || !sar_demo_clean_transitions.GetBool() || !engine->demoplayer->IsPlaying()) return;

	triggerTonemapScaleTweak(tonemapScale, false);
	g_transitionAdaptFrames = TRANSITION_ADAPT_FRAMES;
	forceProjectedTexturesTargetColor();
}
