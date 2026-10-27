#include "InputSystem.hpp"

#include "Cheats.hpp"
#include "Command.hpp"
#include "Interface.hpp"
#include "Module.hpp"
#include "Offsets.hpp"
#include "SAR.hpp"
#include "Utils.hpp"
#include "Modules/Engine.hpp"

Variable sar_demo_free_cursor("sar_demo_free_cursor", "1", "While a demo plays, don't let the game move the mouse cursor. It re-centres the cursor every frame, even in the background, which takes the mouse away from other programs during a render.\n");

REDECL(InputSystem::SleepUntilInput);
REDECL(InputSystem::MoveCursor);
#ifdef _WIN32
REDECL(InputSystem::GetRawMouseAccumulators);
#endif

ButtonCode_t InputSystem::GetButton(const char *pString) {
	return this->StringToButtonCode(this->g_InputSystem->ThisPtr(), pString);
}

bool InputSystem::IsKeyDown(ButtonCode_t key) {
	return this->IsButtonDown(this->g_InputSystem->ThisPtr(), key);
}

void InputSystem::GetCursorPos(int &x, int &y) {
	return this->GetCursorPosition(this->g_InputSystem->ThisPtr(), x, y);
}

void InputSystem::SetCursorPos(int x, int y) {
	return this->SetCursorPosition(this->g_InputSystem->ThisPtr(), x, y);
}

Variable sar_dpi_scale("sar_dpi_scale", "1", 1, "Fraction to scale mouse DPI down by.\n");
void InputSystem::DPIScaleDeltas(int &x, int &y) {
	static int saved_x = 0;
	static int saved_y = 0;

	static int last_dpi_scale = 1;

	int scale = sar_dpi_scale.GetInt();
	if (scale < 1) scale = 1;

	if (scale != last_dpi_scale) {
		saved_x = 0;
		saved_y = 0;
		last_dpi_scale = scale;
	}

	saved_x += x;
	saved_y += y;

	x = saved_x / scale;
	y = saved_y / scale;

	saved_x %= scale;
	saved_y %= scale;
}

// CInputSystem::SleepUntilInput
DETOUR(InputSystem::SleepUntilInput, int nMaxSleepTimeMS) {
	if (sar_disable_no_focus_sleep.GetBool()) {
		nMaxSleepTimeMS = 0;
	}

	return InputSystem::SleepUntilInput(thisptr, nMaxSleepTimeMS);
}

// The game re-centres the cursor on its window for mouse look; demo playback doesn't use the mouse.
DETOUR_T(void, InputSystem::MoveCursor, int x, int y) {
	if (sar_demo_free_cursor.GetBool() && engine->demoplayer && engine->demoplayer->IsPlaying()) return;
	InputSystem::MoveCursor(thisptr, x, y);
}

#ifdef _WIN32
// CInputSystem::GetRawMouseAccumulators
DETOUR_T(void, InputSystem::GetRawMouseAccumulators, int &x, int &y) {
	InputSystem::GetRawMouseAccumulators(thisptr, x, y);
	inputSystem->DPIScaleDeltas(x, y);
}
#endif

bool InputSystem::Init() {
	this->g_InputSystem = Interface::Create(this->Name(), "InputSystemVersion001");
	if (this->g_InputSystem) {
		this->StringToButtonCode = this->g_InputSystem->Original<_StringToButtonCode>(Offsets::StringToButtonCode);

		this->g_InputSystem->Hook(InputSystem::SleepUntilInput_Hook, InputSystem::SleepUntilInput, Offsets::SleepUntilInput);
		this->g_InputSystem->Hook(InputSystem::MoveCursor_Hook, InputSystem::MoveCursor, Offsets::SetCursorPosition);
#ifdef _WIN32
		this->g_InputSystem->Hook(InputSystem::GetRawMouseAccumulators_Hook, InputSystem::GetRawMouseAccumulators, Offsets::GetRawMouseAccumulators);
#endif
		this->IsButtonDown = this->g_InputSystem->Original<_IsButtonDown>(Offsets::IsButtonDown);
		this->GetCursorPosition = this->g_InputSystem->Original<_GetCursorPosition>(Offsets::GetCursorPosition);
		this->SetCursorPosition = this->g_InputSystem->Original<_SetCursorPosition>(Offsets::SetCursorPosition);
	}

	auto unbind = Command("unbind");
	if (!!unbind) {
		auto cc_unbind_callback = (uintptr_t)unbind.ThisPtr()->m_pCommandCallback;
		this->KeySetBinding = Memory::Read<_KeySetBinding>(cc_unbind_callback + Offsets::Key_SetBinding);
	}

	return this->hasLoaded = this->g_InputSystem && !!unbind;
}
void InputSystem::Shutdown() {
	Interface::Delete(this->g_InputSystem);
}

InputSystem *inputSystem;
