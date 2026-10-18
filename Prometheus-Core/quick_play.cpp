#include "quick_play.h"
#include "game.h"
#include "Statescript.h"
#include "STU_Editable.h"
#include "player_spawner.h"
#include "stu_resources.h"
#include "Logs/Logs.h"

namespace quick_play {
	enum class Step {
		Idle,
		Searching,      //Matchmaking state 1: "searching for game"
		MatchFound,     //Matchmaking state 2: "match found"
		Loading,        //Map requested, waiting for "World Running"
		SpawnHero,      //Spawn the starter hero
		OpenHeroSelect, //Press the hero_select logical button for a few ticks
	};

	static constexpr __int64 MAP_HANAMURA = 0x0800000000000165;
	static constexpr __int64 STARTER_HERO = 0x02E0000000000003;  //Tracer
	static constexpr int LOGICAL_BUTTON_HERO_SELECT = 171;       //"hero_select" (H)
	static constexpr int WORLD_STATE_RUNNING = 8;
	static constexpr double SEARCHING_SECS = 2.0;
	static constexpr double MATCH_FOUND_SECS = 1.5;
	static constexpr double SETTLE_SECS = 1.0;                   //After "World Running", before spawning
	static constexpr int HERO_SELECT_PRESS_TICKS = 3;
	static constexpr double LOADING_TIMEOUT_SECS = 120.0;

	//Lobby scripts the demo also stops once a match is found, so the lobby UI doesn't stay on top.
	static constexpr __int64 LOBBY_SCRIPTS_TO_STOP[] = {
		0x5800000000002d8, //Chat
		0x580000000000158, //Lobby Menu
		0x580000000000138, //Social Menu
	};

	static Step s_step = Step::Idle;
	static double s_step_started = 0;
	static __int64 s_map = MAP_HANAMURA;
	static int s_press_ticks = 0;

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
		world->wanted_map_id = s_map;
		world->world_state = 2;
		LOG_CORE(Info, "Quick Play: loading map {:X}\n", s_map);
		set_step(Step::Loading);
	}

	static void spawn_starter_hero() {
		auto hero = stu_resources::GetByID(STARTER_HERO);
		if (!hero) {
			LOG_CORE(Warn, "Quick Play: starter hero {:X} not loaded, skipping spawn\n", STARTER_HERO);
			return;
		}
		auto model_entid = hero->to_editable().get_argument_resource("m_gameplayEntity")->resource_id;
		player_spawner spawner(model_entid);
		spawner.controller_info.heroid = STARTER_HERO;
		spawner.spawn();
		LOG_CORE(Info, "Quick Play: spawned starter hero {:X}\n", STARTER_HERO);
	}

	//Writes the button into both input buffers; the game reads whichever is current this tick.
	static void set_hero_select_button(Component_50_Input* input, bool down) {
		input->stru1.tick0[LOGICAL_BUTTON_HERO_SELECT] = down;
		input->stru1.tick1[LOGICAL_BUTTON_HERO_SELECT] = down;
	}

	__int64 selected_map() {
		return s_map;
	}

	void select_map(__int64 map_id) {
		s_map = map_id;
	}

	void start_from_menu() {
		if (s_step != Step::Idle)
			return;
		LOG_CORE(Info, "Quick Play: Play pressed, searching\n");
		set_matchmaking_state(1);
		set_step(Step::Searching);
	}

	void start_now() {
		request_map();
	}

	bool is_matchmaking() {
		return s_step == Step::Searching || s_step == Step::MatchFound;
	}

	void tick(Component_50_Input* input) {
		switch (s_step) {
		case Step::Idle:
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
				set_step(Step::SpawnHero);
			}
			else if (step_elapsed() > LOADING_TIMEOUT_SECS) {
				LOG_CORE(Warn, "Quick Play: map did not reach World Running, giving up\n");
				set_step(Step::Idle);
			}
			break;
		}
		case Step::SpawnHero:
			if (!GameEntityAdmin()->getLocalEnt())
				spawn_starter_hero();
			s_press_ticks = 0;
			set_step(Step::OpenHeroSelect);
			break;
		case Step::OpenHeroSelect:
			if (!input)
				break;
			if (step_elapsed() < SETTLE_SECS)
				break; //Let the hero finish spawning first
			if (s_press_ticks < HERO_SELECT_PRESS_TICKS) {
				set_hero_select_button(input, true);
				s_press_ticks++;
			}
			else {
				set_hero_select_button(input, false);
				LOG_CORE(Info, "Quick Play: opened hero select\n");
				set_step(Step::Idle);
			}
			break;
		}
	}
}
