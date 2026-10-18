#include "display.h"
#include "globals.h"
#include "game.h"
#include "Logs/Logs.h"
#include <d3d11.h>
#include <nlohmann/json.hpp>
#include <fstream>

extern IDXGISwapChain* main_swapchain; //dllmain.cpp

namespace display {
	//Bool setting objects: value at +0x38, runtime ini copy at +0x8 (value pointer at +0x90), change callback at
	//+0x40 with its argument at +0x48 (see the setters at 0x85EC00 and 0x85D270, the Video options apply).
	static constexpr __int64 RVA_SETTING_WINDOWED_FULLSCREEN = 0x17A4CC0; //"Render.WindowedFullscreen": borderless
	static constexpr __int64 RVA_RENDERER = 0x181E3E0;                    //vfunc 0x598 applies the display mode
	static constexpr int RENDERER_APPLY_DISPLAY_MODE = 0x598;
	static constexpr double STARTUP_DELAY_SECS = 3.0;
	static constexpr double CHECK_SECS = 2.0;
	static constexpr int MAX_APPLIES = 5;
	static constexpr const char* SETTINGS_PATH = "./Prometheus/display_settings.json";

	static bool& setting() {
		static bool s_borderless = true;
		static bool s_loaded = false;
		if (!s_loaded) {
			s_loaded = true;
			std::ifstream file(SETTINGS_PATH);
			auto j = nlohmann::json::parse(file, nullptr, false);
			if (j.is_object())
				s_borderless = j.value("borderless", s_borderless);
		}
		return s_borderless;
	}

	//Sets a bool setting like the game does; true if the value changed.
	static bool set_bool_setting(__int64 rva, char value) {
		auto obj = globals::gameBase + rva;
		auto current = (char*)(obj + 0x38);
		bool changed = *current != value;
		*current = value;
		if (auto runtime = *(__int64*)(obj + 0x8))
			**(char**)(runtime + 0x90) = value;
		if (auto callback = *(void(__fastcall**)(__int64, __int64))(obj + 0x40))
			callback(obj, *(__int64*)(obj + 0x48));
		return changed;
	}

	static void apply_display_mode() {
		auto renderer = *(__int64*)(globals::gameBase + RVA_RENDERER);
		if (!renderer)
			return;
		auto apply = *(void(__fastcall**)(__int64))(*(__int64*)renderer + RENDERER_APPLY_DISPLAY_MODE);
		apply(renderer);
	}

	static void log_fullscreen_state(const char* when) {
		BOOL exclusive = FALSE;
		if (main_swapchain && SUCCEEDED(main_swapchain->GetFullscreenState(&exclusive, nullptr)))
			LOG_CORE(Info, "Display: {} (exclusive fullscreen: {})\n", when, exclusive ? "yes" : "no");
	}

	bool borderless() {
		return setting();
	}

	void set_borderless(bool enabled) {
		setting() = enabled;
		std::ofstream(SETTINGS_PATH, std::ios::trunc) << nlohmann::json{ { "borderless", enabled } }.dump(2);
		if (set_bool_setting(RVA_SETTING_WINDOWED_FULLSCREEN, enabled))
			apply_display_mode();
	}

	void tick() {
		static double s_last_check = -1;
		static int s_applies = 0;
		static bool s_logged = false;
		static bool s_verify = false;
		auto now = GetGameRuntimeSecs();
		if (now < STARTUP_DELAY_SECS || (s_last_check >= 0 && now - s_last_check < CHECK_SECS))
			return;
		s_last_check = now;
		if (!s_logged) {
			s_logged = true;
			log_fullscreen_state("at startup");
		}
		if (s_verify) {
			s_verify = false;
			log_fullscreen_state("after switching");
		}
		if (!setting() || s_applies >= MAX_APPLIES)
			return;
		//The game can switch back to fullscreen by itself (e.g. the game state tick at 0xBFF460), so keep checking.
		if (*(char*)(globals::gameBase + RVA_SETTING_WINDOWED_FULLSCREEN + 0x38) == 0) {
			s_applies++;
			if (set_bool_setting(RVA_SETTING_WINDOWED_FULLSCREEN, 1))
				apply_display_mode();
			LOG_CORE(Info, "Display: switching to borderless window\n");
			s_verify = true;
		}
	}
}
