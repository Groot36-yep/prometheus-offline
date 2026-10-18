#include "auto_test.h"
#include "game.h"
#include "Statescript.h"
#include "STU_Editable.h"
#include "player_spawner.h"
#include "quick_play.h"
#include "statescript_logger.h"
#include "stu_resources.h"
#include "stringhash_library.h"
#include "Logs/Logs.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <set>
#include <sstream>

namespace auto_test {
	enum class Step {
		Idle,
		WaitForMenu, //Let the main menu finish loading
		LoadData,    //Demo mode + lobby data packs (the top bar's "Demo" and "DP Load"), wait for the hero data
		Loading,     //Map requested, waiting for "World Running"
		Spawn,       //Spawn the next hero
		Settle,      //Let the hero finish spawning; learn which states change on their own
		Gap,         //Short pause before the next press, also used to learn background state changes
		Press,       //Hold the button
		Observe,     //Button released, still counting reactions
		Wait,        //Ability pressed: wait for it to finish before checking what is left running
		Exiting,     //Report written; give the log a moment to flush before closing the game
		Done,
	};

	struct Action {
		const char* button;
		bool probe; //Only checks the reaction (primary fire before/after each ability)
	};

	//Primary fire is the probe: a stuck ability (e.g. Recall before the timed-wait fix) blocks it.
	static constexpr Action HERO_ACTIONS[] = {
		{ "fire", true },
		{ "secondary_fire", false }, { "fire", true },
		{ "ability_1", false }, { "fire", true },
		{ "ability_2", false }, { "fire", true },
		{ "ability_3", false }, { "fire", true }, //Ultimate; needs charge, so "no reaction" is expected for now
		{ "quick_weapon", false }, { "fire", true }, //Melee
	};

	struct Hero {
		__int64 id;
		const char* name;
	};

	//The heroes the build loads (see the data pack list in dllmain.cpp).
	static constexpr Hero HEROES[] = {
		{ 0x02e0000000000003, "Tracer" }, { 0x02e0000000000002, "Reaper" }, { 0x02e0000000000004, "Mercy" },
		{ 0x02e0000000000005, "Hanzo" }, { 0x02e0000000000006, "Torbjorn" }, { 0x02e0000000000007, "Reinhardt" },
		{ 0x02e0000000000008, "Pharah" }, { 0x02e0000000000009, "Winston" }, { 0x02e000000000000a, "Widowmaker" },
		{ 0x02e0000000000015, "Bastion" }, { 0x02e0000000000016, "Symmetra" }, { 0x02e0000000000020, "Zenyatta" },
		{ 0x02e0000000000029, "Genji" }, { 0x02e0000000000040, "Roadhog" }, { 0x02e0000000000042, "McCree" },
		{ 0x02e0000000000065, "Junkrat" }, { 0x02e0000000000068, "Zarya" }, { 0x02e000000000006e, "Soldier76" },
		{ 0x02e0000000000079, "Lucio" }, { 0x02e000000000007a, "DVa" }, { 0x02e00000000000dd, "Mei" },
	};

	static constexpr int WORLD_STATE_RUNNING = 8;
	static constexpr double MENU_SETTLE_SECS = 8.0;
	static constexpr double MAP_SETTLE_SECS = 3.0;
	static constexpr double LOADING_TIMEOUT_SECS = 120.0;
	static constexpr double EXIT_DELAY_SECS = 2.0;       //The log flushes every second
	static constexpr double SPAWN_SETTLE_SECS = 5.0;
	static constexpr double SPAWN_NOISE_FROM_SECS = 2.5; //Background noise is learned from here to the end of settling
	static constexpr double HERO_DATA_SETTLE_SECS = 3.0; //After the hero data appears, before loading the map
	static constexpr double HERO_DATA_TIMEOUT_SECS = 60.0;
	static constexpr double SPAWN_RETRY_SECS = 2.0;
	static constexpr int SPAWN_ATTEMPTS = 5;
	static constexpr __int64 RVA_LOAD_LOBBY_DATA_PACKS = 0xc7f090; //Top bar "DP Load": loads the hero data
	static constexpr double GAP_SECS = 0.75;
	static constexpr double PRESS_SECS = 0.2;
	static constexpr double REACTION_SECS = 1.0; //From the press, how long a state change still counts as a reaction
	static constexpr double ABILITY_SECS = 8.0;  //From the press, when an ability should be over
	static constexpr const char* REPORT_PATH = "./Prometheus/auto_test_report.json";

	static constexpr __int64 LOBBY_SCRIPTS_TO_STOP[] = {
		0x5800000000002d8, //Chat
		0x580000000000158, //Lobby Menu
		0x580000000000138, //Social Menu
	};

	using StateKey = std::pair<StatescriptInstance*, int>;

	static Step s_step = Step::Idle;
	static double s_step_started = 0;
	static bool s_exit_when_done = false;
	static std::string s_options{};
	static std::vector<Hero> s_heroes{};
	static size_t s_hero = 0;
	static size_t s_action = 0;
	static int s_spawn_attempts = 0;
	static double s_hero_data_seen = -1;
	static double s_press_started = 0;
	static bool s_press_event_sent = false;
	static std::map<StateKey, uint> s_running{};  //Running states of the hero's scripts, with their node type
	static std::set<StateKey> s_noise{};          //States that start/end without any input
	static std::set<StateKey> s_started{};        //Started since the press (minus noise)
	static int s_reactions = 0;                   //State starts/ends since the press (minus noise)
	static bool s_fire_ok = false;                //Result of the last fire probe
	static nlohmann::json s_report{};
	static nlohmann::json s_hero_report{};

	static void set_step(Step step) {
		s_step = step;
		s_step_started = GetGameRuntimeSecs();
	}

	static double step_elapsed() {
		return GetGameRuntimeSecs() - s_step_started;
	}

	static std::string lower(std::string s) {
		for (auto& c : s)
			c = (char)tolower((unsigned char)c);
		return s;
	}

	static Component_23_Statescript* local_hero_statescript() {
		auto controller = GameEntityAdmin()->getLocalEnt();
		if (!controller)
			return nullptr;
		auto pet = controller->getById<Component_20_ModelReference>(0x20);
		if (!pet)
			return nullptr;
		auto model = GameEntityAdmin()->getEntById(pet->cam_attach_entid);
		return model ? model->getById<Component_23_Statescript>(0x23) : nullptr;
	}

	static uint node_type(StatescriptInstance* inst, int idx) {
		if (!inst->graph || idx < 0 || idx >= inst->graph->m_nodes.count())
			return 0;
		return inst->graph->m_nodes.list()[idx]->graph_node.base.vfptr->GetSTUInfo()->Hash;
	}

	static std::map<StateKey, uint> running_states() {
		std::map<StateKey, uint> result{};
		auto ss = local_hero_statescript();
		if (!ss)
			return result;
		for (auto inst : ss->ss_inner) {
			for (auto& [idx, state] : statescript_logger::GetScriptState(inst)) {
				if (!state.is_finish_state)
					result.emplace(StateKey{ inst, idx }, node_type(inst, idx));
			}
		}
		return result;
	}

	enum class Track {
		LearnNoise, //Nothing pressed yet: whatever changes is background noise
		Count,      //Changes are reactions to the press
		Follow,     //Only keep the running states current
	};

	//Compares the running states with the last tick.
	static void track_states(Track mode) {
		auto now = running_states();
		auto changed = [&](const StateKey& key, bool started) {
			if (mode == Track::LearnNoise)
				s_noise.insert(key);
			if (mode != Track::Count || s_noise.contains(key))
				return;
			s_reactions++;
			if (started)
				s_started.insert(key);
		};
		for (auto& [key, type] : now) {
			if (!s_running.contains(key))
				changed(key, true);
		}
		for (auto& [key, type] : s_running) {
			if (!now.contains(key))
				changed(key, false);
		}
		s_running = std::move(now);
	}

	//Holds the button in the input buffers (read by "is the button held" checks) every tick.
	static void set_button(Component_50_Input* input, const char* name, bool down) {
		auto button = LogicalButtonByName(name);
		if (!input || button->key_id < 0)
			return;
		input->stru1.tick0[button->key_id] = down;
		input->stru1.tick1[button->key_id] = down;
	}

	//Button down/up events for the hero's scripts. Writing the input buffers alone does not reach them: the game
	//turns real input into these notifications before Prometheus' tick, and overwrites the buffers next frame.
	static void send_button_event(const char* name, bool down) {
		auto button = LogicalButtonByName(name);
		auto ss = local_hero_statescript();
		if (!ss || button->key_id < 0)
			return;
		for (auto inst : ss->ss_inner) {
			auto e = (StatescriptNotification_LogicalButton*)ss->ss_inner.vfptr->Allocate_StatescriptE(&ss->ss_inner, ETYPE_LOGICALBUTTON);
			e->base.m_timestamp = ss->ss_inner.cf_timestamp;
			e->base.m_instanceId = inst->instance_id;
			e->m_logicalButton = button->key_id;
			e->m_bButtonGoingDown = down;
			inst->vfptr->EnqueueE(inst, (StatescriptNotification_Base*)e);
		}
	}

	static void write_report() {
		std::ofstream file(REPORT_PATH, std::ios::out | std::ios::trunc);
		if (!file.is_open()) {
			LOG_CORE(Warn, "[AUTOTEST] could not write {}\n", REPORT_PATH);
			return;
		}
		file << s_report.dump(2);
	}

	static __int64 requested_map(const std::string& options);

	static void load_map() {
		auto lobby_ss = LobbyEntityAdmin()->getSingletonComponent<Component_23_Statescript>(0x23);
		if (lobby_ss) {
			for (auto script_id : LOBBY_SCRIPTS_TO_STOP) {
				if (auto inst = lobby_ss->ss_inner.getByResourceId(script_id))
					inst->Stop();
			}
		}
		auto world = get_system27_WorldEngineSystem(GameEntityAdmin());
		PrometheusSystem::instance()->DeleteLocalEnt(); //Changing maps with a hero spawned crashes on unload
		auto map = requested_map(s_options);
		if (!map)
			map = quick_play::selected_map();
		world->wanted_map_id = map;
		world->world_state = 2;
		s_report["map"] = std::format("{:X}", map);
		LOG_CORE(Info, "[AUTOTEST] loading map {:X}\n", map);
		set_step(Step::Loading);
	}

	static void finish_hero() {
		s_hero_report["spawn_attempts"] = s_spawn_attempts;
		s_spawn_attempts = 0;
		s_report["heroes"].push_back(s_hero_report);
		write_report();
		s_hero++;
		set_step(Step::Spawn);
	}

	//Same as picking the hero in hero select (SendHeroSelection in entity_admin.cpp).
	static void spawn_hero() {
		auto& hero = s_heroes[s_hero];
		if (s_spawn_attempts == 0)
			s_hero_report = { { "name", hero.name }, { "id", std::format("{:X}", hero.id) }, { "abilities", nlohmann::json::array() } };
		s_spawn_attempts++;
		PrometheusSystem::instance()->DeleteLocalEnt();
		auto hero_stu = stu_resources::GetByID(hero.id);
		auto entity = hero_stu ? hero_stu->to_editable().get_argument_resource("m_gameplayEntity") : nullptr;
		if (!entity || !entity->has_resource()) {
			LOG_CORE(Warn, "[AUTOTEST] {}: hero data not loaded, skipping\n", hero.name);
			s_hero_report["spawned"] = false;
			finish_hero();
			return;
		}
		player_spawner spawner(hero_spawn_entity(hero.id, entity->resource_id));
		spawner.controller_info.heroid = hero.id;
		auto [controller, model] = spawner.spawn();
		LOG_CORE(Info, "[AUTOTEST] {}: spawn attempt {} {}\n", hero.name, s_spawn_attempts, controller ? "ok" : "failed");
		s_running.clear();
		s_noise.clear();
		s_action = 0;
		set_step(Step::Settle);
	}

	//The game's STUConfigVarIsHeroSelectActive getter.
	static bool hero_select_active() {
		StatescriptPrimitive cv{};
		auto get = (char(*)(__int64, __int64, __int64, StatescriptPrimitive*))(globals::gameBase + 0xd8ee00);
		return get(0, 0, 0, &cv) && cv.is_truthy();
	}

	//Debug: STU data as JSON (objects, lists, maps and resource references; primitives are left out). Set
	//PROMETHEUS_DUMP_STU to comma separated resource ids (hex) to write them to Prometheus/stu_dump.json once the
	//hero data is loaded, e.g. to see which entities a hero references.
	static std::string hash_name(uint hash) {
		auto it = stringhash_library::hashes.find((int)hash);
		return it != stringhash_library::hashes.end() ? it->second : std::format("{:08X}", hash);
	}

	static nlohmann::json resref_json(STUResourceReference* ref) {
		if (!ref || !ref->resource_id)
			return nullptr;
		nlohmann::json j = std::format("{:X}", ref->resource_id);
		if (auto stu = stu_resources::GetByID(ref->resource_id))
			j = std::format("{:X} ({})", ref->resource_id, hash_name(stu->to_editable().get_runtime_root().struct_info->Hash));
		return j;
	}

	static nlohmann::json stu_json(STU_Object object, int depth) {
		if (!object.valid())
			return nullptr;
		object = object.get_runtime_root();
		nlohmann::json j = { { "_type", hash_name(object.struct_info->Hash) } };
		if (depth <= 0)
			return j;
		for (auto [info, arg] : object.struct_info->RangeArgs()) {
			auto name = hash_name(arg->Hash);
			switch (arg->Constraint->ToConstraintType()) {
			case STU_ConstraintType_Object:
			case STU_ConstraintType_InlinedObject:
				j[name] = stu_json(object.get_argument_object(arg), depth - 1);
				break;
			case STU_ConstraintType_ResourceRef:
			case STU_ConstraintType_NonSTUResourceRef:
				j[name] = resref_json(object.get_argument_resource(arg));
				break;
			case STU_ConstraintType_BSList_Object:
			case STU_ConstraintType_BSList_InlinedObject: {
				auto list = object.get_argument_objectlist(arg);
				auto& out = j[name] = nlohmann::json::array();
				for (int i = 0; list.valid() && i < list.count(); i++)
					out.push_back(stu_json(list[i], depth - 1));
				break;
			}
			case STU_ConstraintType_BSList_ResourceRef:
			case STU_ConstraintType_BSList_NonSTUResourceRef: {
				auto list = object.get_argument_resreflist(arg);
				auto& out = j[name] = nlohmann::json::array();
				for (int i = 0; list.valid() && i < list.count(); i++)
					out.push_back(resref_json(list[i]));
				break;
			}
			case STU_ConstraintType_Map: {
				auto map = object.get_argument_map(arg);
				auto& out = j[name] = nlohmann::json::object();
				for (int i = 0; map.valid() && i < map.count(); i++) {
					auto [key, value] = map[i];
					out[hash_name(key)] = stu_json(value, depth - 1);
				}
				break;
			}
			default:
				break;
			}
		}
		return j;
	}

	//Debug: with PROMETHEUS_DUMP_ENTITIES set, every loaded entity definition that has weapons, scripts, a first
	//person entity or child entities goes to Prometheus/entities_dump.json (model, weapon scripts, graphs, children).
	static void dump_entities() {
		auto env = getenv("PROMETHEUS_DUMP_ENTITIES");
		if (!env || !*env)
			return;
		nlohmann::json out = nlohmann::json::object();
		for (auto& [id, stu] : stu_resources::GetIfAssignableTo(stringHash("STUEntityDefinition"))) {
			auto j = stu_json(stu->to_editable(), 5);
			auto& components = j["m_componentMap"];
			bool interesting = !j["m_childEntityData"].empty();
			for (auto name : { "STUWeaponComponent", "STUStatescriptComponent", "STUFirstPersonComponent" })
				interesting |= components.contains(name);
			if (!interesting)
				continue;
			nlohmann::json entry = { { "children", j["m_childEntityData"] } };
			if (components.contains("STUModelComponent"))
				entry["model"] = components["STUModelComponent"]["m_model"];
			if (components.contains("STUWeaponComponent"))
				entry["weapons"] = components["STUWeaponComponent"];
			if (components.contains("STUFirstPersonComponent"))
				entry["first_person"] = components["STUFirstPersonComponent"]["m_entity"];
			if (components.contains("STUStatescriptComponent")) {
				auto& graphs = entry["graphs"] = nlohmann::json::array();
				for (auto& [key, value] : components["STUStatescriptComponent"].items()) {
					if (value.is_array())
						for (auto& g : value)
							if (g.is_object() && g.contains("m_graph"))
								graphs.push_back(g["m_graph"]);
				}
			}
			out[std::format("{:X}", id)] = entry;
		}
		std::ofstream("./Prometheus/entities_dump.json", std::ios::trunc) << out.dump(1);
		LOG_CORE(Info, "[AUTOTEST] wrote entities_dump.json ({} entities)\n", out.size());
	}

	static void dump_requested_stus() {
		dump_entities();
		auto env = getenv("PROMETHEUS_DUMP_STU");
		if (!env || !*env)
			return;
		nlohmann::json out = nlohmann::json::object();
		std::stringstream ids(env);
		for (std::string id; std::getline(ids, id, ',');) {
			auto resource_id = std::stoull(id, nullptr, 16);
			auto stu = stu_resources::GetByID(resource_id);
			out[id] = stu ? stu_json(stu->to_editable(), 6) : nlohmann::json("not loaded");
		}
		std::ofstream("./Prometheus/stu_dump.json", std::ios::trunc) << out.dump(2);
		LOG_CORE(Info, "[AUTOTEST] wrote stu_dump.json\n");
	}

	//Ends the run, writing what was collected; closes the game too when started with ",exit".
	static void stop() {
		write_report();
		set_step(s_exit_when_done ? Step::Exiting : Step::Done);
	}

	static bool heroes_loaded() {
		return !s_heroes.empty() && stu_resources::GetByID(s_heroes[0].id);
	}

	static void load_data() {
		globals::isDemo = true;
		char buf[0x80]{};
		((__int64(__fastcall*)(__int64, __int64, char*))(globals::gameBase + RVA_LOAD_LOBBY_DATA_PACKS))(0, 0, buf);
		LOG_CORE(Info, "[AUTOTEST] demo mode on, loading lobby data packs\n");
		s_hero_data_seen = -1;
		set_step(Step::LoadData);
	}

	static void start_press() {
		s_started.clear();
		s_reactions = 0;
		s_press_event_sent = false;
		s_press_started = GetGameRuntimeSecs();
		set_step(Step::Press);
	}

	static std::string verdict(bool reacted, bool fire_before, bool fire_after, size_t left_running) {
		if (!reacted)
			return "no_reaction";
		if (fire_before && !fire_after)
			return "breaks_fire";
		if (left_running > 0)
			return "left_running";
		return "ok";
	}

	//Called when the current action is over (after Observe for probes, after Wait for abilities).
	static void finish_action() {
		auto& action = HERO_ACTIONS[s_action];
		auto& hero = s_heroes[s_hero];
		if (action.probe) {
			bool fire_before = s_fire_ok;
			s_fire_ok = s_reactions > 0;
			if (s_action == 0) {
				s_hero_report["fire"] = s_fire_ok;
				LOG_CORE(Info, "[AUTOTEST] {} fire: {}\n", hero.name, s_fire_ok ? "reacts" : "NO reaction");
			}
			else {
				//Completes the ability entry before it with "does fire still work afterwards".
				auto& entry = s_hero_report["abilities"].back();
				entry["fire_after"] = s_fire_ok;
				entry["verdict"] = verdict(entry["reacted"].get<bool>(), fire_before, s_fire_ok, entry["left_running"].size());
				LOG_CORE(Info, "[AUTOTEST] {} {}: {} ({} state changes, {} left running, fire after: {})\n",
					hero.name, entry["button"].get<std::string>(), entry["verdict"].get<std::string>(),
					entry["state_changes"].get<int>(), entry["left_running"].size(), s_fire_ok ? "yes" : "no");
			}
		}
		else {
			nlohmann::json left = nlohmann::json::array();
			for (auto& key : s_started) {
				auto it = s_running.find(key);
				if (it == s_running.end())
					continue;
				left.push_back({ { "script", std::format("{:X}", key.first->script_id) }, { "node", key.second },
					{ "type", std::format("{:08X}", it->second) } });
			}
			s_hero_report["abilities"].push_back({ { "button", action.button }, { "reacted", s_reactions > 0 },
				{ "state_changes", s_reactions }, { "left_running", left } });
		}

		s_action++;
		if (s_action >= std::size(HERO_ACTIONS))
			finish_hero();
		else
			set_step(Step::Gap);
	}

	//"map=<part of the map's path or name>" in the hero list picks the map, e.g. "dva,map=numbani".
	static __int64 requested_map(const std::string& options) {
		auto pos = options.find("map=");
		if (pos == std::string::npos)
			return 0;
		auto wanted = options.substr(pos + 4, options.find(',', pos) - pos - 4);
		for (auto& [id, stu] : stu_resources::GetIfAssignableTo(stringHash("STUMapHeader"))) {
			auto header = stu->to_editable();
			std::string name = lower(header.get_argument_primitive("m_mapName").get_value<const char*>());
			if (name.find(wanted) != std::string::npos)
				return header.get_argument_resource("m_map")->resource_id;
		}
		LOG_CORE(Warn, "[AUTOTEST] no map matching '{}'\n", wanted);
		return 0;
	}

	void start(const std::string& heroes, bool exit_when_done) {
		if (s_step != Step::Idle && s_step != Step::Done)
			return;
		s_heroes.clear();
		auto wanted = lower(heroes);
		s_options = wanted;
		for (auto& hero : HEROES) {
			if (wanted.find("all") != std::string::npos || wanted.find(lower(hero.name)) != std::string::npos)
				s_heroes.push_back(hero);
		}
		s_exit_when_done = exit_when_done;
		s_hero = 0;
		s_report = { { "map", std::format("{:X}", quick_play::selected_map()) }, { "heroes", nlohmann::json::array() } };
		LOG_CORE(Info, "[AUTOTEST] starting: {} heroes\n", s_heroes.size());
		auto world = get_system27_WorldEngineSystem(GameEntityAdmin());
		if (world->world_state == WORLD_STATE_RUNNING && heroes_loaded())
			set_step(Step::Spawn); //Started from the top bar with a map and heroes already loaded
		else
			set_step(Step::WaitForMenu);
	}

	bool is_running() {
		return s_step != Step::Idle && s_step != Step::Done;
	}

	void tick(Component_50_Input* input) {
		static bool s_checked_env = false;
		if (!s_checked_env && LobbyEntityAdmin() && GameEntityAdmin()) {
			s_checked_env = true;
			if (auto env = getenv("PROMETHEUS_AUTOTEST"); env && *env) {
				std::string value = env;
				start(value, lower(value).find("exit") != std::string::npos);
			}
		}

		switch (s_step) {
		case Step::Idle:
		case Step::Done:
			break;
		case Step::Exiting:
			if (step_elapsed() > EXIT_DELAY_SECS)
				TerminateProcess(GetCurrentProcess(), 0); //Also skips the known shutdown crash
			break;
		case Step::WaitForMenu:
			if (step_elapsed() > MENU_SETTLE_SECS)
				load_data();
			break;
		case Step::LoadData:
			if (!heroes_loaded()) {
				if (step_elapsed() > HERO_DATA_TIMEOUT_SECS) {
					LOG_CORE(Warn, "[AUTOTEST] hero data did not load, giving up\n");
					stop();
				}
				break;
			}
			if (s_hero_data_seen < 0) {
				s_hero_data_seen = GetGameRuntimeSecs();
				dump_requested_stus();
			}
			if (GetGameRuntimeSecs() - s_hero_data_seen > HERO_DATA_SETTLE_SECS) {
				if (get_system27_WorldEngineSystem(GameEntityAdmin())->world_state == WORLD_STATE_RUNNING)
					set_step(Step::Spawn);
				else
					load_map();
			}
			break;
		case Step::Loading: {
			auto world = get_system27_WorldEngineSystem(GameEntityAdmin());
			static int s_last_world_state = -1;
			if (world->world_state != s_last_world_state) {
				s_last_world_state = world->world_state;
				LOG_CORE(Info, "[AUTOTEST] world state {}\n", s_last_world_state);
			}
			if (world->world_state == WORLD_STATE_RUNNING && step_elapsed() > MAP_SETTLE_SECS) {
				set_step(Step::Spawn);
			}
			else if (step_elapsed() > LOADING_TIMEOUT_SECS) {
				LOG_CORE(Warn, "[AUTOTEST] map did not reach World Running, giving up\n");
				stop();
			}
			break;
		}
		case Step::Spawn:
			if (s_hero >= s_heroes.size()) {
				LOG_CORE(Info, "[AUTOTEST] done, report written to {}\n", REPORT_PATH);
				stop();
				break;
			}
			spawn_hero();
			break;
		case Step::Settle:
			//Spawning starts and ends many states once (weapon scripts included); learning those as noise hid real
			//reactions to fire later (Pharah, D.Va), so only learn from states that change after the spawn burst.
			track_states(step_elapsed() > SPAWN_NOISE_FROM_SECS ? Track::LearnNoise : Track::Follow);
			if (!local_hero_statescript() && step_elapsed() > SPAWN_RETRY_SECS && s_spawn_attempts < SPAWN_ATTEMPTS) {
				spawn_hero(); //The hero's entity data may still be loading
				break;
			}
			if (step_elapsed() > SPAWN_SETTLE_SECS) {
				if (!local_hero_statescript()) {
					LOG_CORE(Warn, "[AUTOTEST] {}: no hero after spawning\n", s_heroes[s_hero].name);
					s_hero_report["spawned"] = false;
					finish_hero();
					break;
				}
				s_hero_report["spawned"] = true;
				//The game opens hero select by itself. It does not block the button events sent to the hero's scripts,
				//so only record it (pressing hero_select does not close it).
				s_hero_report["hero_select_open"] = hero_select_active();
				s_hero_report["background_states"] = s_noise.size();
				if (s_hero == 0)
					dump_requested_stus(); //Again, now that the map and the first hero are loaded
				LOG_CORE(Info, "[AUTOTEST] {}: {} states change on their own\n", s_heroes[s_hero].name, s_noise.size());
				start_press();
			}
			break;
		case Step::Gap:
			track_states(Track::Follow);
			if (step_elapsed() > GAP_SECS)
				start_press();
			break;
		case Step::Press:
			if (!s_press_event_sent) {
				send_button_event(HERO_ACTIONS[s_action].button, true);
				s_press_event_sent = true;
			}
			set_button(input, HERO_ACTIONS[s_action].button, true);
			track_states(Track::Count);
			if (step_elapsed() > PRESS_SECS) {
				set_button(input, HERO_ACTIONS[s_action].button, false);
				send_button_event(HERO_ACTIONS[s_action].button, false);
				set_step(Step::Observe);
			}
			break;
		case Step::Observe:
			set_button(input, HERO_ACTIONS[s_action].button, false);
			track_states(Track::Count);
			if (GetGameRuntimeSecs() - s_press_started > REACTION_SECS) {
				if (HERO_ACTIONS[s_action].probe)
					finish_action();
				else
					set_step(Step::Wait);
			}
			break;
		case Step::Wait:
			track_states(Track::Follow); //Follow-ups, not reactions; s_started keeps the reaction's states
			if (GetGameRuntimeSecs() - s_press_started > ABILITY_SECS)
				finish_action();
			break;
		}
	}
}
