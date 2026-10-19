#include "auto_test.h"
#include "heroes.h"
#include "game.h"
#include "Statescript.h"
#include "StatescriptVar.h"
#include "STU_Editable.h"
#include "player_spawner.h"
#include "quick_play.h"
#include "statescript_logger.h"
#include "stu_resources.h"
#include "stringhash_library.h"
#include "UI/imgui_helpers.h"
#include "UI/windows/ini_settings_window.h"
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
		CloseSelect, //Close the hero select the game opened, so screenshots show the world
		Gap,         //Short pause before the next press, also used to learn background state changes
		Press,       //Hold the button
		Observe,     //Button released, still counting reactions
		Wait,        //Ability pressed: wait for it to finish before checking what is left running
		UltGap,      //"ultprobe": pause between tries
		UltSettle,   //"ultprobe": variable set, learning its own effects as noise
		UltPress,    //"ultprobe": a variable set to 100, hold ability_3
		UltObserve,  //"ultprobe": count the reactions
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

	using Hero = HeroInfo;

	static constexpr int WORLD_STATE_RUNNING = 8;
	static constexpr double CLOSE_SELECT_KEY_SECS = 1.0; //Between Escape and H
	static constexpr double CLOSE_SELECT_SECS = 7.0;     //The screen stays black ~5 s; then the presses start
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
	static std::vector<__int64> s_maps{};
	static size_t s_map_index = 0;
	static __int64 s_map = 0;
	static size_t s_action = 0;

	//"sequence=<button>+<button>+..." (logical button names, e.g. ability_1+next_weapon+fire+fire) presses those instead,
	//each followed by the fire probe, e.g. to place Torbjorn's turret, take out his hammer and hit the turret.
	static const std::vector<Action>& actions() {
		static std::vector<Action> s_actions{};
		static std::vector<std::string> s_names{};
		if (!s_actions.empty())
			return s_actions;
		auto pos = s_options.find("sequence=");
		if (pos == std::string::npos) {
			s_actions.assign(std::begin(HERO_ACTIONS), std::end(HERO_ACTIONS));
			return s_actions;
		}
		std::string list = s_options.substr(pos + 9, s_options.find(',', pos) - pos - 9) + "+";
		for (size_t start = 0, end; (end = list.find('+', start)) != std::string::npos; start = end + 1)
			if (end > start)
				s_names.push_back(list.substr(start, end - start));
		s_actions.push_back({ "fire", true });
		for (auto& name : s_names) {
			s_actions.push_back({ name.c_str(), false });
			s_actions.push_back({ "fire", true });
		}
		return s_actions;
	}
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

	static std::vector<__int64> requested_maps(const std::string& options);

	static void load_map() {
		if (s_map_index == 0)
			s_maps = requested_maps(s_options);
		auto lobby_ss = LobbyEntityAdmin()->getSingletonComponent<Component_23_Statescript>(0x23);
		if (lobby_ss) {
			for (auto script_id : LOBBY_SCRIPTS_TO_STOP) {
				if (auto inst = lobby_ss->ss_inner.getByResourceId(script_id))
					inst->Stop();
			}
		}
		auto world = get_system27_WorldEngineSystem(GameEntityAdmin());
		PrometheusSystem::instance()->DeleteLocalEnt(); //Changing maps with a hero spawned crashes on unload
		auto map = s_map_index < s_maps.size() ? s_maps[s_map_index] : quick_play::selected_map();
		world->wanted_map_id = map;
		world->world_state = 2;
		s_map = map;
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
			s_hero_report = { { "name", hero.name }, { "id", std::format("{:X}", hero.id) }, { "map", std::format("{:X}", s_map) },
				{ "abilities", nlohmann::json::array() } };
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
			case STU_ConstraintType_Primitive:
			case STU_ConstraintType_Enum: //First 4 bytes as an int; enough to tell teams, flags and counts apart
				j[name] = object.get_argument_primitive(arg).get_value<int>();
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

	//Debug: with PROMETHEUS_DUMP_VARS set, the hero's entity variables (identifier -> type and value) go into the report
	//("vars"), e.g. to find the ultimate charge. A variable's primitive value is at +0xa0 (StatescriptVar_Primitive);
	//for other kinds (dictionaries, nested varbags) the type comes out as something outside 1..11.
	static nlohmann::json varbag_json(StatescriptVar_VarBag* bag) {
		nlohmann::json out = nlohmann::json::object();
		if (!bag)
			return out;
		for (auto& bucket : bag->sub_items) {
			for (int i = 0; i < bucket.item_count && bucket.arr_ptr; i++) {
				auto& item = bucket.arr_ptr[i];
				if (!item.ptr)
					continue;
				auto& value = ((StatescriptVar_Primitive*)item.ptr)->global_var;
				auto key = std::format("{:X}", item.rid_id);
				if (value.type >= StatescriptPrimitive_BYTE && value.type < StatescriptPrimitive_COUNT)
					out[key] = { { "type", (int)value.type }, { "value", value.get_value_str() } };
				else
					out[key] = "other";
			}
		}
		return out;
	}

	static void dump_hero_vars() {
		auto env = getenv("PROMETHEUS_DUMP_VARS");
		auto ss = local_hero_statescript();
		if (env && *env && ss)
			s_hero_report["vars"] = varbag_json(ss->ss_inner.rid_entity_varbag);
	}

	//Debug: with PROMETHEUS_DUMP_MAP_ENTITIES set, every entity of the loaded map goes to Prometheus/map_entities.json
	//(components with their data type, position), e.g. to find the spawn rooms.
	static void dump_map_entities() {
		auto env = getenv("PROMETHEUS_DUMP_MAP_ENTITIES");
		if (!env || !*env)
			return;
		nlohmann::json out = nlohmann::json::array();
		for (auto ent : *GameEntityAdmin()) {
			nlohmann::json e = { { "id", std::format("{:X}", ent->entity_id) } };
			if (!IsBadReadPtr(ent->resload_entry, sizeof(MisalignedResourceLoadEntry)))
				e["resource"] = std::format("{:X}", ent->resload_entry->resource_id);
			if (auto sr = ent->getById<Component_1_SceneRendering>(1)) {
				e["pos"] = { sr->position.X, sr->position.Y, sr->position.Z };
				e["rot"] = { sr->rotation.X, sr->rotation.Y, sr->rotation.Z, sr->rotation.W };
			}
			if (auto ss = ent->getById<Component_23_Statescript>(0x23)) {
				auto& scripts = e["scripts"] = nlohmann::json::array();
				for (auto inst : ss->ss_inner)
					scripts.push_back(std::format("{:X}", inst->script_id));
			}
			auto& comps = e["components"] = nlohmann::json::object();
			for (int i = 0; i < ent->component_list.num - 1; i++) {
				auto compo = ent->component_list.ptr[i];
				if (!compo)
					continue;
				auto mirror = (STUBase<>*)compo->GetMirrorData();
				std::string type = mirror ? hash_name(mirror->vfptr->GetSTUInfo()->Hash) : "";
				auto key = std::format("{:X}", compo->component_id);
				if (mirror && (compo->component_id == 0x2E || compo->component_id == 0x40)) //Spawn point, spawn room volumes
					comps[key] = stu_json(mirror->to_editable(), 3);
				else if (type.find("Spawn") != std::string::npos)
					comps[key] = stu_json(mirror->to_editable(), 2);
				else
					comps[key] = type;
			}
			out.push_back(e);
		}
		std::ofstream("./Prometheus/map_entities.json", std::ios::trunc) << out.dump(1);
		LOG_CORE(Info, "[AUTOTEST] wrote map_entities.json ({} entities)\n", out.size());
	}

	//Debug: with PROMETHEUS_DUMP_INI set, the game's ini settings (name, value as parsed, int value) go to
	//Prometheus/ini_dump.json, the same list the Ini Settings window shows.
	static void dump_ini() {
		auto env = getenv("PROMETHEUS_DUMP_INI");
		if (!env || !*env)
			return;
		auto list = *(ini_settings_window::iniSetting***)(globals::gameBase + 0x1859ed0);
		int count = *(int*)(globals::gameBase + 0x1859ed0 + 8);
		nlohmann::json out = nlohmann::json::array();
		for (int i = 0; i < count; i++) {
			auto ini = list[i];
			nlohmann::json e = { { "name", ini->setting_name.get() }, { "parsed", ini->parsed_value.get() }, { "type", ini->ini_type },
				{ "address", std::format("{:X}", (__int64)ini - globals::gameBase) } };
			if (ini->intVal && (ini->ini_type == 1 || ini->ini_type == 3))
				e["value"] = *ini->charVal;
			else if (ini->intVal && ini->ini_type == 2)
				e["value"] = *ini->intVal;
			out.push_back(e);
		}
		std::ofstream("./Prometheus/ini_dump.json", std::ios::trunc) << out.dump(1);
		LOG_CORE(Info, "[AUTOTEST] wrote ini_dump.json ({} settings)\n", out.size());
	}

	static void dump_requested_stus() {
		dump_entities();
		dump_ini();
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
		quick_play::load_hero_data();
		LOG_CORE(Info, "[AUTOTEST] demo mode on, loading lobby data packs\n");
		s_hero_data_seen = -1;
		set_step(Step::LoadData);
	}

	//Resource id, or the component ids for entities created at runtime without one (e.g. "?:1,3,4").
	static std::string entity_resource(Entity* ent) {
		if (!IsBadReadPtr(ent->resload_entry, sizeof(MisalignedResourceLoadEntry)))
			return std::format("{:X}", ent->resload_entry->resource_id);
		std::string ids = "?:";
		for (int i = 0; i < ent->component_list.num - 1; i++) {
			if (auto compo = ent->component_list.ptr[i])
				ids += std::format("{}{:X}", ids.size() > 2 ? "," : "", compo->component_id);
		}
		return ids;
	}

	//Entities created since the press (projectiles, deployables, effects), by resource id with a count, and how long
	//the ones with a resource id lived (a projectile that hits a wall or the floor goes away at once; one that flies
	//through lives until its lifetime ends).
	struct NewEntity {
		std::string resource;
		double born;
		double died = -1;
		std::set<std::string> scripts{};
	};
	static std::set<uint> s_entities_before{};
	static std::map<uint, NewEntity> s_new_entities{};

	static void track_new_entities() {
		auto now = GetGameRuntimeSecs();
		std::set<uint> alive{};
		for (auto ent : *GameEntityAdmin()) {
			alive.insert(ent->entity_id);
			if (s_entities_before.contains(ent->entity_id))
				continue;
			auto it = s_new_entities.find(ent->entity_id);
			if (it == s_new_entities.end())
				it = s_new_entities.emplace(ent->entity_id, NewEntity{ entity_resource(ent), now }).first;
			if (auto ss = ent->getById<Component_23_Statescript>(0x23)) {
				for (auto inst : ss->ss_inner)
					it->second.scripts.insert(std::format("{:X}", inst->script_id));
			}
		}
		for (auto& [id, entity] : s_new_entities) {
			if (entity.died < 0 && !alive.contains(id))
				entity.died = now;
		}
	}

	//Scripts the created entities ran, e.g. a projectile's impact logic.
	static nlohmann::json new_entity_scripts_json() {
		std::set<std::string> scripts{};
		for (auto& [id, entity] : s_new_entities)
			scripts.insert(entity.scripts.begin(), entity.scripts.end());
		return scripts;
	}

	static nlohmann::json new_entities_json() {
		std::map<std::string, int> counts{};
		for (auto& [id, entity] : s_new_entities)
			counts[entity.resource]++;
		return counts;
	}

	//Seconds each entity with a resource id lived, or "alive" if it outlived the observation.
	static nlohmann::json new_entity_lifetimes_json() {
		nlohmann::json out = nlohmann::json::object();
		for (auto& [id, entity] : s_new_entities) {
			if (entity.resource.starts_with("?"))
				continue;
			if (entity.died < 0)
				out[entity.resource].push_back("alive");
			else
				out[entity.resource].push_back(std::round((entity.died - entity.born) * 100) / 100);
		}
		return out;
	}

	static void start_press();
	static void start_ult_probe();

	static void begin_presses() {
		if (s_options.find("ultprobe") != std::string::npos)
			start_ult_probe();
		else
			start_press();
	}

	static void start_press() {
		s_entities_before.clear();
		s_new_entities.clear();
		for (auto ent : *GameEntityAdmin())
			s_entities_before.insert(ent->entity_id);
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

	//"only=<button>" (e.g. only=ability_3): press just that ability (and the fire probes), so one ability can be tested
	//without the others before it leaving locks behind.
	static bool skipped(size_t action) {
		auto pos = s_options.find("only=");
		if (pos == std::string::npos || actions()[action].probe)
			return false;
		auto only = s_options.substr(pos + 5, s_options.find(',', pos) - pos - 5);
		return only != actions()[action].button;
	}

	//Called when the current action is over (after Observe for probes, after Wait for abilities).
	static void finish_action() {
		auto& action = actions()[s_action];
		auto& hero = s_heroes[s_hero];
		if (action.probe) {
			bool fire_before = s_fire_ok;
			s_fire_ok = s_reactions > 0;
			if (s_action == 0) {
				s_hero_report["fire"] = s_fire_ok;
				s_hero_report["fire_entities"] = new_entities_json();
				s_hero_report["fire_lifetimes"] = new_entity_lifetimes_json();
				s_hero_report["fire_scripts"] = new_entity_scripts_json();
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
			nlohmann::json started = nlohmann::json::array();
			for (auto& key : s_started) {
				nlohmann::json state = { { "script", std::format("{:X}", key.first->script_id) }, { "node", key.second },
					{ "type", std::format("{:08X}", node_type(key.first, key.second)) } };
				started.push_back(state);
				if (s_running.contains(key))
					left.push_back(state);
			}
			s_hero_report["abilities"].push_back({ { "button", action.button }, { "reacted", s_reactions > 0 },
				{ "state_changes", s_reactions }, { "left_running", left }, { "started", started },
				{ "entities", new_entities_json() }, { "lifetimes", new_entity_lifetimes_json() },
				{ "scripts", new_entity_scripts_json() } });
		}

		s_action++;
		while (s_action < actions().size() && skipped(s_action))
			s_action += 2; //The ability and the fire probe after it
		if (s_action >= actions().size())
			finish_hero();
		else
			set_step(Step::Gap);
	}

	//"ultprobe": instead of the normal presses, look for the ultimate charge. Press ability_3 once as is, then once per
	//integer entity variable that is 0 with that variable set to 100; a try with reactions names the charge variable.
	static std::vector<__int64> s_ult_candidates{};
	static size_t s_ult_try = 0; //0: nothing set; n: s_ult_candidates[n - 1] set to 100
	static nlohmann::json s_ult_results{};

	static void set_entity_var(__int64 id, int value) {
		auto ss = local_hero_statescript();
		if (ss && ss->ss_inner.rid_entity_varbag)
			ss->ss_inner.rid_entity_varbag->SetVar({ StatescriptVar_ENTITY_VARBAG, id }, StatescriptPrimitive(value, StatescriptPrimitive_INT));
	}

	static std::set<StateKey> s_ult_baseline_noise{};
	static constexpr double ULT_SETTLE_SECS = 1.5; //After setting the variable: what changes now is its effect, not the press

	//Sets the try's variable, then lets its own effects (e.g. "ultimate ready" sounds) play out as noise.
	static void begin_ult_try() {
		s_noise = s_ult_baseline_noise;
		if (s_ult_try > 0)
			set_entity_var(s_ult_candidates[s_ult_try - 1], 100);
		set_step(Step::UltSettle);
	}

	static void press_ult() {
		s_started.clear();
		s_reactions = 0;
		s_press_event_sent = false;
		s_press_started = GetGameRuntimeSecs();
		set_step(Step::UltPress);
	}

	static void start_ult_probe() {
		s_ult_candidates.clear();
		s_ult_try = 0;
		s_ult_results = nlohmann::json::array();
		s_ult_baseline_noise = s_noise;
		auto ss = local_hero_statescript();
		auto vars = varbag_json(ss ? ss->ss_inner.rid_entity_varbag : nullptr);
		for (auto& [key, var] : vars.items()) {
			if (var.is_object() && var["type"] == (int)StatescriptPrimitive_INT && var["value"] == "0")
				s_ult_candidates.push_back(std::stoll(key, nullptr, 16));
		}
		LOG_CORE(Info, "[AUTOTEST] {}: ultimate probe, {} candidate variables\n", s_heroes[s_hero].name, s_ult_candidates.size());
		begin_ult_try();
	}

	static void finish_ult_try() {
		auto var = s_ult_try > 0 ? std::format("{:X}", s_ult_candidates[s_ult_try - 1]) : std::string("none");
		s_ult_results.push_back({ { "var", var }, { "reactions", s_reactions } });
		if (s_reactions > 0)
			LOG_CORE(Info, "[AUTOTEST] {}: ultimate reacts with {} = 100 ({} state changes)\n", s_heroes[s_hero].name, var, s_reactions);
		if (s_ult_try > 0)
			set_entity_var(s_ult_candidates[s_ult_try - 1], 0);
		s_ult_try++;
		if (s_ult_try > s_ult_candidates.size()) {
			s_hero_report["ult_probe"] = s_ult_results;
			finish_hero();
		}
		else
			set_step(Step::UltGap);
	}

	//Map whose path contains `wanted` (playable maps before login scenes), 0 if none.
	static __int64 find_map(const std::string& wanted) {
		__int64 found = 0;
		for (auto& [id, stu] : stu_resources::GetIfAssignableTo(stringHash("STUMapHeader"))) {
			auto header = stu->to_editable();
			std::string name = lower(header.get_argument_primitive("m_mapName").get_value<const char*>());
			if (name.find(wanted) == std::string::npos)
				continue;
			found = header.get_argument_resource("m_map")->resource_id;
			if (name.find("login") == std::string::npos)
				return found;
		}
		if (found)
			return found;
		std::string names{};
		for (auto& [id, stu] : stu_resources::GetIfAssignableTo(stringHash("STUMapHeader")))
			names += std::format(" {}={:X}", stu->to_editable().get_argument_primitive("m_mapName").get_value<const char*>(), stu->to_editable().get_argument_resource("m_map")->resource_id);
		LOG_CORE(Warn, "[AUTOTEST] no map matching '{}'. Maps:{}\n", wanted, names);
		return 0;
	}

	//"map=<part of the map's path>" in the hero list picks the map, e.g. "dva,map=numbani". Several maps separated by
	//'+' are played one after another in the same session (which also tests switching maps), e.g. "map=numbani+kyoto".
	static std::vector<__int64> requested_maps(const std::string& options) {
		std::vector<__int64> maps{};
		auto pos = options.find("map=");
		if (pos == std::string::npos)
			return maps;
		std::stringstream names(options.substr(pos + 4, options.find(',', pos) - pos - 4));
		for (std::string name; std::getline(names, name, '+');) {
			if (auto map = find_map(name))
				maps.push_back(map);
		}
		return maps;
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
		s_maps.clear();
		s_map_index = 0;
		s_map = get_system27_WorldEngineSystem(GameEntityAdmin())->loaded_map_id;
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
		case Step::Exiting: {
			//"linger=<secs>" keeps the game open that long after the last press, e.g. to watch a turret in screenshots
			auto linger = s_options.find("linger=");
			double delay = linger != std::string::npos ? std::max(EXIT_DELAY_SECS, std::stod(s_options.substr(linger + 7))) : EXIT_DELAY_SECS;
			if (step_elapsed() > delay)
				TerminateProcess(GetCurrentProcess(), 0); //Also skips the known shutdown crash
			break;
		}
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
			if (s_hero >= s_heroes.size() && s_map_index + 1 < s_maps.size()) {
				s_map_index++;
				s_hero = 0;
				load_map(); //Next map, with every hero again
				break;
			}
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
				//"pitch=<degrees>" tilts the view before the presses, e.g. at the floor to see whether projectiles hit it
				//(rotation about the X axis, as a quaternion like Component_2F_LocalPlayer::SetRotation takes).
				if (auto pos = s_options.find("pitch="); pos != std::string::npos) {
					float radians = std::stof(s_options.substr(pos + 6)) * 3.14159265f / 180.f;
					if (auto local = GameEntityAdmin()->getLocalEnt()->getById<Component_2F_LocalPlayer>(0x2F))
						local->SetRotation(Vector4(std::sin(radians / 2), 0, 0, std::cos(radians / 2)));
				}
				//The game opens hero select by itself. It does not block the button events sent to the hero's scripts,
				//so only record it (pressing hero_select does not close it).
				s_hero_report["hero_select_open"] = hero_select_active();
				s_hero_report["background_states"] = s_noise.size();
				dump_hero_vars();
				if (s_hero == 0) {
					dump_requested_stus(); //Again, now that the map and the first hero are loaded
					dump_map_entities();
				}
				LOG_CORE(Info, "[AUTOTEST] {}: {} states change on their own\n", s_heroes[s_hero].name, s_noise.size());
				if (hero_select_active()) {
					set_step(Step::CloseSelect);
					break;
				}
				begin_presses();
			}
			break;
		case Step::CloseSelect: {
			//The game opens hero select after every spawn and leaves it over the world (hero_select_active() stays true
			//after it closes). Escape then H close it, so screenshots show the world. Keys only go to the game's window.
			static int s_sent = 0;
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			BYTE key = s_sent == 0 ? VK_ESCAPE : 'H';
			if (s_sent < 2 && step_elapsed() > s_sent * CLOSE_SELECT_KEY_SECS && pid == GetCurrentProcessId()) {
				keybd_event(key, 0, 0, 0);
				keybd_event(key, 0, KEYEVENTF_KEYUP, 0);
				s_sent++;
			}
			if (step_elapsed() > CLOSE_SELECT_SECS) {
				LOG_CORE(Info, "[AUTOTEST] closing hero select: {} keys sent\n", s_sent);
				s_sent = 0;
				begin_presses();
			}
			break;
		}
		case Step::UltGap:
			track_states(Track::Follow);
			if (step_elapsed() > GAP_SECS)
				begin_ult_try();
			break;
		case Step::UltSettle:
			track_states(Track::LearnNoise);
			if (step_elapsed() > ULT_SETTLE_SECS)
				press_ult();
			break;
		case Step::UltPress:
			if (!s_press_event_sent) {
				send_button_event("ability_3", true);
				s_press_event_sent = true;
			}
			set_button(input, "ability_3", true);
			track_states(Track::Count);
			if (step_elapsed() > PRESS_SECS) {
				set_button(input, "ability_3", false);
				send_button_event("ability_3", false);
				set_step(Step::UltObserve);
			}
			break;
		case Step::UltObserve:
			set_button(input, "ability_3", false);
			track_states(Track::Count);
			if (GetGameRuntimeSecs() - s_press_started > REACTION_SECS)
				finish_ult_try();
			break;
		case Step::Gap:
			track_states(Track::Follow);
			if (step_elapsed() > GAP_SECS)
				start_press();
			break;
		case Step::Press:
			if (!s_press_event_sent) {
				send_button_event(actions()[s_action].button, true);
				s_press_event_sent = true;
			}
			set_button(input, actions()[s_action].button, true);
			track_states(Track::Count);
			track_new_entities();
			if (step_elapsed() > PRESS_SECS) {
				set_button(input, actions()[s_action].button, false);
				send_button_event(actions()[s_action].button, false);
				set_step(Step::Observe);
			}
			break;
		case Step::Observe:
			set_button(input, actions()[s_action].button, false);
			track_states(Track::Count);
			track_new_entities();
			if (GetGameRuntimeSecs() - s_press_started > REACTION_SECS) {
				if (actions()[s_action].probe)
					finish_action();
				else
					set_step(Step::Wait);
			}
			break;
		case Step::Wait:
			track_new_entities();
			track_states(Track::Follow); //Follow-ups, not reactions; s_started keeps the reaction's states
			if (GetGameRuntimeSecs() - s_press_started > ABILITY_SECS)
				finish_action();
			break;
		}
	}
}
