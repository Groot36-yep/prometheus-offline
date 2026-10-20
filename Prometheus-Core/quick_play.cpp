#include "quick_play.h"
#include "game.h"
#include "heroes.h"
#include "Statescript.h"
#include "STU_Editable.h"
#include "player_spawner.h"
#include "stu_resources.h"
#include "Logs/Logs.h"
#include <nlohmann/json.hpp>
#include <fstream>

namespace quick_play {
	enum class Step {
		Idle,
		WaitForMenu,    //Autostart: let the main menu finish loading
		LoadHeroData,   //Hero data loading (shown as "searching" when started from the menu)
		Searching,      //Matchmaking state 1: "searching for game"
		MatchFound,     //Matchmaking state 2: "match found"
		Loading,        //Map requested, waiting for "World Running"
		SpawnHero,      //Spawn the chosen hero, retrying while its entity data loads
		OpenHeroSelect, //Press the hero_select logical button for a few ticks
		Watch,          //In game: check the hero is still on the map a few seconds later
	};

	static constexpr __int64 MAP_HANAMURA = 0x0800000000000165;
	static constexpr __int64 STARTER_HERO = 0x02E0000000000003;  //Tracer
	static constexpr int LOGICAL_BUTTON_HERO_SELECT = 171;       //"hero_select" (H)
	static constexpr int WORLD_STATE_RUNNING = 8;
	static constexpr __int64 RVA_LOAD_LOBBY_DATA_PACKS = 0xc7f090; //Top bar "DP Load"
	static constexpr double MENU_SETTLE_SECS = 8.0;
	static constexpr double HERO_DATA_SETTLE_SECS = 3.0;         //After the hero data appears
	static constexpr double HERO_DATA_TIMEOUT_SECS = 60.0;
	static constexpr double SEARCHING_SECS = 2.0;
	static constexpr double MATCH_FOUND_SECS = 1.5;
	static constexpr double SETTLE_SECS = 1.0;                   //After "World Running", before spawning
	static constexpr double SPAWN_RETRY_SECS = 2.0;
	static constexpr int SPAWN_ATTEMPTS = 5;
	static constexpr int HERO_SELECT_PRESS_TICKS = 3;
	static constexpr double LOADING_TIMEOUT_SECS = 120.0;
	static constexpr double WATCH_SECS = 5.0;
	static constexpr float FELL_DISTANCE = 20.f;
	static constexpr const char* SETTINGS_PATH = "./Prometheus/play_settings.json";

	//Lobby scripts the demo also stops once a match is found, so the lobby UI doesn't stay on top.
	static constexpr __int64 LOBBY_SCRIPTS_TO_STOP[] = {
		0x5800000000002d8, //Chat
		0x580000000000158, //Lobby Menu
		0x580000000000138, //Social Menu
	};

	static Step s_step = Step::Idle;
	static double s_step_started = 0;
	static double s_hero_data_seen = -1;
	static bool s_show_screens = true;
	static int s_press_ticks = 0;
	static int s_spawn_attempts = 0;
	static __int64 s_spawned_hero = 0;
	static Vector4 s_spawn_pos{};

	struct Settings {
		__int64 map = MAP_HANAMURA;
		__int64 hero = 0;
		bool autostart = false;
		bool hero_change_anywhere = true;
	};

	static Settings& settings() {
		static Settings s{};
		static bool loaded = false;
		if (!loaded) {
			loaded = true;
			std::ifstream file(SETTINGS_PATH);
			auto j = nlohmann::json::parse(file, nullptr, false);
			if (j.is_object()) {
				s.map = j.value("map", s.map);
				s.hero = j.value("hero", s.hero);
				s.autostart = j.value("autostart", s.autostart);
				s.hero_change_anywhere = j.value("hero_change_anywhere", s.hero_change_anywhere);
			}
		}
		return s;
	}

	static void save_settings() {
		auto& s = settings();
		std::ofstream file(SETTINGS_PATH, std::ios::out | std::ios::trunc);
		if (file.is_open())
			file << nlohmann::json{ { "map", s.map }, { "hero", s.hero }, { "autostart", s.autostart },
				{ "hero_change_anywhere", s.hero_change_anywhere } }.dump(2);
	}

	static void set_step(Step step) {
		s_step = step;
		s_step_started = GetGameRuntimeSecs();
	}

	static double step_elapsed() {
		return GetGameRuntimeSecs() - s_step_started;
	}

	//Lobby singleton 0x56, +0x20: 0 idle, 1 searching, 2 match found, 3 joining (see the demo in entity_admin.cpp)
	static void set_matchmaking_state(int state) {
		auto matchmake_info = (__int64)LobbyEntityAdmin()->getSingletonComponent<__int64>(0x56);
		if (matchmake_info)
			*(int*)(matchmake_info + 0x20) = state;
	}

	static void stop_lobby_scripts() {
		auto lobby_ss = LobbyEntityAdmin()->getSingletonComponent<Component_23_Statescript>(0x23);
		if (!lobby_ss)
			return;
		for (auto script_id : LOBBY_SCRIPTS_TO_STOP) {
			if (auto inst = lobby_ss->ss_inner.getByResourceId(script_id))
				inst->Stop();
		}
	}

	static void request_map() {
		auto world = get_system27_WorldEngineSystem(GameEntityAdmin());
		//Changing maps with a hero spawned crashes on unload (known issue), so remove our hero first.
		PrometheusSystem::instance()->DeleteLocalEnt();
		world->wanted_map_id = settings().map;
		world->world_state = 2;
		LOG_CORE(Info, "Quick Play: loading map {:X}\n", settings().map);
		set_step(Step::Loading);
	}

	//Same as picking the hero in hero select (SendHeroSelection in entity_admin.cpp).
	static bool spawn_hero(__int64 hero_id) {
		auto hero = stu_resources::GetByID(hero_id);
		auto entity = hero ? hero->to_editable().get_argument_resource("m_gameplayEntity") : nullptr;
		if (!entity || !entity->has_resource()) {
			LOG_CORE(Warn, "Quick Play: hero {:X} not loaded, skipping spawn\n", hero_id);
			return false;
		}
		PrometheusSystem::instance()->DeleteLocalEnt();
		player_spawner spawner(hero_spawn_entity(hero_id, entity->resource_id));
		spawner.controller_info.heroid = hero_id;
		spawner.spawn();
		LOG_CORE(Info, "Quick Play: spawned hero {:X} (attempt {})\n", hero_id, s_spawn_attempts);
		return true;
	}

	static bool hero_position(Vector4& pos) {
		auto controller = GameEntityAdmin()->getLocalEnt();
		auto ref = controller ? controller->getById<Component_20_ModelReference>(0x20) : nullptr;
		auto model = ref ? GameEntityAdmin()->getEntById(ref->cam_attach_entid) : nullptr;
		auto sr = model ? model->getById<Component_1_SceneRendering>(1) : nullptr;
		if (!sr)
			return false;
		pos = sr->position;
		return true;
	}

	//The hero is in the game: log where, then check again in a few seconds.
	static void in_game() {
		hero_position(s_spawn_pos);
		LOG_CORE(Info, "Quick Play: in game at ({:.1f}, {:.1f}, {:.1f})\n", s_spawn_pos.X, s_spawn_pos.Y, s_spawn_pos.Z);
		set_step(Step::Watch);
	}

	//Writes the button into both input buffers; the game reads whichever is current this tick.
	static void set_hero_select_button(Component_50_Input* input, bool down) {
		input->stru1.tick0[LOGICAL_BUTTON_HERO_SELECT] = down;
		input->stru1.tick1[LOGICAL_BUTTON_HERO_SELECT] = down;
	}

	//Hero data is ready: on to the matchmaking screens, or straight to the map.
	static void after_hero_data() {
		if (s_show_screens)
			set_step(Step::Searching);
		else
			request_map();
	}

	static void start(bool show_screens) {
		if (is_running())
			return;
		s_show_screens = show_screens;
		if (show_screens)
			set_matchmaking_state(1);
		if (hero_data_loaded()) {
			after_hero_data();
			return;
		}
		load_hero_data();
		s_hero_data_seen = -1;
		set_step(Step::LoadHeroData);
	}

	__int64 selected_map() {
		return settings().map;
	}

	void select_map(__int64 map_id) {
		settings().map = map_id;
		save_settings();
	}

	__int64 selected_hero() {
		return settings().hero;
	}

	void select_hero(__int64 hero_id) {
		if (settings().hero == hero_id)
			return;
		settings().hero = hero_id;
		save_settings();
	}

	bool autostart() {
		return settings().autostart;
	}

	void set_autostart(bool enabled) {
		settings().autostart = enabled;
		save_settings();
	}

	bool hero_change_anywhere() {
		return settings().hero_change_anywhere;
	}

	void set_hero_change_anywhere(bool enabled) {
		settings().hero_change_anywhere = enabled;
		save_settings();
	}

	void start_from_menu() {
		LOG_CORE(Info, "Quick Play: Play pressed\n");
		start(true);
	}

	void start_now() {
		start(false);
	}

	void switch_map(__int64 map_id) {
		select_map(map_id);
		LOG_CORE(Info, "Quick Play: switching to map {:X}\n", map_id);
		set_step(Step::Idle); //Cancels a Play in progress
		start(false);
	}

	bool is_running() {
		return s_step != Step::Idle;
	}

	bool is_matchmaking() {
		return s_show_screens && (s_step == Step::LoadHeroData || s_step == Step::Searching || s_step == Step::MatchFound);
	}

	void load_hero_data() {
		globals::isDemo = true;
		char buf[0x80]{};
		((__int64(__fastcall*)(__int64, __int64, char*))(globals::gameBase + RVA_LOAD_LOBBY_DATA_PACKS))(0, 0, buf);
		LOG_CORE(Info, "Quick Play: demo mode on, loading lobby data packs\n");
	}

	bool hero_data_loaded() {
		return stu_resources::GetByID(STARTER_HERO) != nullptr;
	}

	void tick(Component_50_Input* input) {
		static bool s_checked_autostart = false;
		if (!s_checked_autostart && LobbyEntityAdmin() && GameEntityAdmin()) {
			s_checked_autostart = true;
			auto auto_test_env = getenv("PROMETHEUS_AUTOTEST");
			if (settings().autostart && s_step == Step::Idle && !(auto_test_env && *auto_test_env))
				set_step(Step::WaitForMenu);
		}

		switch (s_step) {
		case Step::Idle:
			break;
		case Step::WaitForMenu:
			if (step_elapsed() > MENU_SETTLE_SECS) {
				set_step(Step::Idle);
				start_from_menu();
			}
			break;
		case Step::LoadHeroData:
			if (!hero_data_loaded()) {
				if (step_elapsed() > HERO_DATA_TIMEOUT_SECS) {
					LOG_CORE(Warn, "Quick Play: hero data did not load, giving up\n");
					set_matchmaking_state(0);
					set_step(Step::Idle);
				}
				break;
			}
			if (s_hero_data_seen < 0)
				s_hero_data_seen = GetGameRuntimeSecs();
			if (GetGameRuntimeSecs() - s_hero_data_seen > HERO_DATA_SETTLE_SECS)
				after_hero_data();
			break;
		case Step::Searching:
			if (step_elapsed() > SEARCHING_SECS) {
				set_matchmaking_state(2);
				stop_lobby_scripts();
				set_step(Step::MatchFound);
			}
			break;
		case Step::MatchFound:
			if (step_elapsed() > MATCH_FOUND_SECS) {
				set_matchmaking_state(3);
				request_map();
			}
			break;
		case Step::Loading: {
			auto world = get_system27_WorldEngineSystem(GameEntityAdmin());
			if (world->world_state == WORLD_STATE_RUNNING && step_elapsed() > SETTLE_SECS) {
				s_spawned_hero = settings().hero ? settings().hero : STARTER_HERO;
				s_spawn_attempts = 0;
				set_step(Step::SpawnHero);
			}
			else if (step_elapsed() > LOADING_TIMEOUT_SECS) {
				LOG_CORE(Warn, "Quick Play: map did not reach World Running, giving up\n");
				set_step(Step::Idle);
			}
			break;
		}
		case Step::SpawnHero:
			if (s_spawn_attempts == 0 || (!GameEntityAdmin()->getLocalEnt() && step_elapsed() > SPAWN_RETRY_SECS)) {
				if (s_spawn_attempts >= SPAWN_ATTEMPTS) {
					LOG_CORE(Warn, "Quick Play: hero did not spawn\n");
					set_step(Step::Idle);
					break;
				}
				s_spawn_attempts++;
				if (!spawn_hero(s_spawned_hero)) {
					set_step(Step::Idle);
					break;
				}
				set_step(Step::SpawnHero); //Restarts the retry timer
				break;
			}
			if (GameEntityAdmin()->getLocalEnt() && step_elapsed() > SETTLE_SECS) {
				if (settings().hero) {
					in_game();
				}
				else {
					s_press_ticks = 0;
					set_step(Step::OpenHeroSelect);
				}
			}
			break;
		case Step::OpenHeroSelect:
			if (!input)
				break;
			if (s_press_ticks < HERO_SELECT_PRESS_TICKS) {
				set_hero_select_button(input, true);
				s_press_ticks++;
			}
			else {
				set_hero_select_button(input, false);
				LOG_CORE(Info, "Quick Play: opened hero select\n");
				in_game();
			}
			break;
		case Step::Watch:
			if (step_elapsed() > WATCH_SECS) {
				Vector4 pos{};
				if (hero_position(pos)) {
					bool fell = s_spawn_pos.Y - pos.Y > FELL_DISTANCE;
					LOG_CORE(Info, "Quick Play: hero at ({:.1f}, {:.1f}, {:.1f}){}\n", pos.X, pos.Y, pos.Z, fell ? ", fell off the map" : "");
				}
				set_step(Step::Idle);
			}
			break;
		}
	}
}
