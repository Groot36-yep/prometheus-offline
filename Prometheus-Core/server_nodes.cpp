#include "server_nodes.h"
#include "entity_admin.h"
#include "Statescript.h"
#include "StatescriptVar.h"
#include "STUConfigVar_Custom.h"
#include "STUConfigVarDynamic_Custom.h"
#include "STU_Editable.h"
#include "game.h"
#include "Logs/Logs.h"
#include "statescript_logger.h"
#include "ultimate.h"
#include "MovementState.h"
#include <MinHook.h>
#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>

namespace server_nodes {
	//Traverses a link into a node (0xF471F0): skips it when the instance's flags (+0x258) have bit 2, or the
	//instance's vfunc 0x88 says it does not run the node; an action runs through its implementation (0xF4E1A0),
	//and an action type with no implementation registered (0xF433A0 finds none) gets the base action's, which only
	//continues through the node's out plugs (0xF47740).
	static constexpr __int64 RVA_TRAVERSE_LINK = 0xF471F0;
	static constexpr __int64 RVA_FIND_NODE_IMPL = 0xF433A0;
	static constexpr int INSTANCE_FLAGS = 0x258;
	static constexpr int INSTANCE_RUNS_NODE_VFUNC = 0x88 / 8;
	static constexpr int STATESCRIPT_SYNC_MGR = 0xC0; //Embedded in the statescript component (Component_23_Statescript)
	//Instance vfunc 0x88 (0xD739A0 for entity scripts) runs a node only if it is not server-only and the instance has
	//this flag (or the node is client-only). The hero's scripts have it (0x400081); scripts on entities the server
	//created for us do not (Torbjorn's turret 0, Hanzo's dragon 0x208009), so they refused nearly every node.
	static constexpr uint32_t INSTANCE_RUNS_SHARED_NODES = 1 << 22;
	static constexpr int PLUG_PARENT_NODE = 0x18;

	using TraverseLinkFn = char(__fastcall*)(__int64 from_impl, StatescriptInstance* inst, STUStatescriptBase* from, __int64 a4, __int64 plug, int a6);
	static TraverseLinkFn s_traverse_link_orig = nullptr;

	static unsigned node_type(STUStatescriptBase* node) {
		return node->graph_node.base.vfptr->GetSTUInfo()->Hash;
	}

	static bool runs_node(StatescriptInstance* inst, STUStatescriptBase* node) {
		if ((*(uint32_t*)((char*)inst + INSTANCE_FLAGS) >> 2) & 1)
			return false;
		auto runs = (__int64(__fastcall*)(StatescriptInstance*, STUStatescriptBase*))((__int64*)inst->vfptr)[INSTANCE_RUNS_NODE_VFUNC];
		return runs(inst, node) != 0;
	}

	static void trace(StatescriptInstance* inst, STUStatescriptBase* node) {
		static std::set<std::pair<__int64, int>> s_seen{};
		bool blocked = (*(uint32_t*)((char*)inst + INSTANCE_FLAGS) >> 2) & 1;
		bool runs = runs_node(inst, node);
		bool implemented = node->m_isState
			|| ((__int64(__fastcall*)(__int64, STUStatescriptBase*))(globals::gameBase + RVA_FIND_NODE_IMPL))(0, node) != 0;
		if ((runs && implemented) || !s_seen.insert({ inst->script_id, node->idx_in_nodes }).second)
			return;
		LOG_CORE(Info, "[NODES] script {:X} node {} type {:08X}: {}{}{}\n", inst->script_id, node->idx_in_nodes, node_type(node),
			blocked ? "instance blocks nodes " : "", runs || blocked ? "" : "not run by this instance ", implemented ? "" : "no implementation");
	}

	//
	// Server entity creation (node type 1BB99C36, no client implementation). Used by Torbjorn's turret, Symmetra's
	// turrets and teleporter, Hanzo's Dragonstrike, Junkrat's RIP-Tire and D.Va calling her mech. Fields:
	//   m_entityDefinition  entity to create (m_entityDef)
	//   679AF128            owner, usually GetEntityController(GetOwner)
	//   m_position          position (vec3, or an entity: its position)
	//   87018F61            forward vector (optional)
	//   99EEFF9F            up vector, e.g. the surface normal a turret is placed on (optional)
	//   m_basedEntity       entity it stands on (moving platforms); not used here
	//   D7C59487            dynamic var that receives the new entity (the graph's next nodes use it, e.g. PlayScript)
	//
	static constexpr unsigned NODE_CREATE_ENTITY = 0x1BB99C36;
	static constexpr unsigned FIELD_OWNER = 0x679AF128;
	static constexpr unsigned FIELD_FORWARD = 0x87018F61;
	static constexpr unsigned FIELD_UP = 0x99EEFF9F;
	static constexpr unsigned FIELD_OUT_ENTITY = 0xD7C59487;
	static constexpr double CREATE_REPEAT_SECS = 0.1; //A node re-entered this quickly is the same activation (resimulated)

	//A field of a node, or an invalid object when its type has no such field (get_argument_object would crash).
	static STU_Object field_object(STU_Object node, unsigned field) {
		if (!node.valid())
			return {};
		for (auto [info, arg] : node.struct_info->RangeArgs())
			if (arg->Hash == field)
				return node.get_argument_object(arg);
		return {};
	}

	static STU_Object field_object(STU_Object node, const char* field) {
		return field_object(node, stringHash(field));
	}

	//An int field of an object (its runtime type), or the fallback when it has none.
	static int field_int(STU_Object object, const char* field, int fallback) {
		object = object.get_runtime_root();
		if (!object.valid())
			return fallback;
		auto hash = stringHash(field);
		for (auto [info, arg] : object.struct_info->RangeArgs())
			if (arg->Hash == hash)
				return object.get_argument_primitive(arg).get_value<int>();
		return fallback;
	}

	static bool eval(StatescriptInstance* inst, STU_Object node, unsigned field, StatescriptPrimitive& out) {
		auto cv = field_object(node, field);
		out = {};
		return cv.valid() && ((STUConfigVar*)cv.value)->get_value(inst, &out) && out.type != 0;
	}

	static bool eval(StatescriptInstance* inst, STU_Object node, const char* field, StatescriptPrimitive& out) {
		return eval(inst, node, stringHash(field), out);
	}

	//A vec3 value, or an entity's position (read from its component 1: the game's GetEntityPos crashed for some).
	static Vector3 to_vec3(const StatescriptPrimitive& value) {
		if (value.type == StatescriptPrimitive_VEC3) {
			Vector3 v;
			memcpy(&v, &value.value, sizeof(v));
			return v;
		}
		if (value.type == StatescriptPrimitive_ENTITY) {
			auto ent = GameEntityAdmin()->getEntById((uint32_t)value.value);
			auto scene = ent ? ent->getById<Component_1_SceneRendering>(1) : nullptr;
			if (scene)
				return Vector3(scene->position.X, scene->position.Y, scene->position.Z);
		}
		return Vector3();
	}

	static Vector3 normalized(Vector3 v) {
		float len = std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
		return len > 1e-6f ? Vector3(v.X / len, v.Y / len, v.Z / len) : Vector3(0, 0, 0);
	}

	static Vector3 cross(Vector3 a, Vector3 b) {
		return Vector3(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X);
	}

	//Rotation (quaternion x, y, z, w) whose Z axis is forward and Y axis is up; yaw only without an up vector.
	static Vector4 look_rotation(Vector3 forward, Vector3 up) {
		auto f = normalized(forward);
		if (f.X == 0 && f.Y == 0 && f.Z == 0)
			return Vector4(0, 0, 0, 1);
		auto u = normalized(up);
		if (u.X == 0 && u.Y == 0 && u.Z == 0)
			u = Vector3(0, 1, 0);
		auto r = normalized(cross(u, f));
		if (r.X == 0 && r.Y == 0 && r.Z == 0)
			return Vector4(0, 0, 0, 1);
		u = cross(f, r);
		//Matrix columns r, u, f
		float m00 = r.X, m01 = u.X, m02 = f.X;
		float m10 = r.Y, m11 = u.Y, m12 = f.Y;
		float m20 = r.Z, m21 = u.Z, m22 = f.Z;
		float trace = m00 + m11 + m22;
		Vector4 q;
		if (trace > 0) {
			float s = std::sqrt(trace + 1.f) * 2;
			q = Vector4((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s);
		}
		else if (m00 > m11 && m00 > m22) {
			float s = std::sqrt(1.f + m00 - m11 - m22) * 2;
			q = Vector4(0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s);
		}
		else if (m11 > m22) {
			float s = std::sqrt(1.f + m11 - m00 - m22) * 2;
			q = Vector4((m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s);
		}
		else {
			float s = std::sqrt(1.f + m22 - m00 - m11) * 2;
			q = Vector4((m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s);
		}
		return q;
	}

	//The entity whose statescript component runs the instance (instance vfunc 0x1C8 gives its owner instead, e.g.
	//Junkrat's controller for the RIP-Tire's script).
	static Entity* script_entity(StatescriptInstance* inst) {
		auto mgr = inst ? inst->ss_inner : nullptr;
		auto component = mgr ? (ComponentBase*)((char*)mgr - STATESCRIPT_SYNC_MGR) : nullptr;
		return component && component->component_id == 0x23 ? component->entity_backref : nullptr;
	}

	static uint32_t s_possession_hero(); //The hero while it possesses another entity (defined with possession below)

	//The hero model an owner (a player's controller) drives, or the owner itself.
	static Entity* hero_of(Entity* owner) {
		auto ref = owner ? owner->getById<Component_20_ModelReference>(0x20) : nullptr;
		auto hero = ref ? GameEntityAdmin()->getEntById(ref->movement_attach_entid) : nullptr;
		return hero ? hero : owner;
	}

	//Entities created here; only these are removed by DestroyEntity below (never the hero or map entities).
	static std::set<uint32_t> s_created{};
	//The hero each was created for: the server removes a hero's turrets and the like when the player changes hero
	//(their scripts then read the gone hero's position as the origin and moved there).
	static std::map<uint32_t, uint32_t> s_created_for{};

	static void run_shared_nodes(StatescriptInstance* inst) {
		*(uint32_t*)((char*)inst + INSTANCE_FLAGS) |= INSTANCE_RUNS_SHARED_NODES;
	}

	static void run_shared_nodes(Entity* ent) {
		if (auto ss = ent->getById<Component_23_Statescript>(0x23))
			for (auto inst : ss->ss_inner)
				if (inst)
					run_shared_nodes(inst);
	}

	//Offline this client is also the server for the entities it creates, so their scripts run their server-only nodes
	//(+0x76) too: the RIP-Tire's release from Junkrat, its camera, movement and detonation, the dragons' flight
	//(ChaseVar), the pillars' and turrets' server logic. The run-node check of entity scripts (instance vfunc 0x88,
	//0xD739A0) refuses those on a client. A node type the client has no implementation for does nothing harmful: an
	//action continues through its out plugs, a state gets the base state (0xF4E230).
	static constexpr __int64 RVA_ENTITY_RUNS_NODE = 0xD739A0;
	using EntityRunsNodeFn = __int64(__fastcall*)(StatescriptInstance* inst, STUStatescriptBase* node);
	static EntityRunsNodeFn s_entity_runs_node_orig = nullptr;

	static bool is_created(StatescriptInstance* inst) {
		auto ent = s_created.empty() ? nullptr : script_entity(inst);
		return ent && s_created.count(ent->entity_id);
	}

	static __int64 __fastcall entity_runs_node_hook(StatescriptInstance* inst, STUStatescriptBase* node) {
		return is_created(inst) ? 1 : s_entity_runs_node_orig(inst, node);
	}

	//Every update recomputes instance flag bit 22 from instance vfunc 0x190 (0xD73BE0): set unless the script has
	//authority (bit 3) on an entity the local player does not control. The checks for running a node (0x88) and for
	//ending or changing a state (0x170, 0xD73E90; 0x178) need it, so scripts started with authority on created
	//entities lost it at their first update and no state of theirs could end (the RIP-Tire's wait for its release
	//fired and was refused). Created entities count as locally controlled.
	static constexpr __int64 RVA_ENTITY_LOCALLY_CONTROLLED = 0xD73BE0;
	using EntityLocallyControlledFn = __int64(__fastcall*)(StatescriptInstance* inst);
	static EntityLocallyControlledFn s_entity_locally_controlled_orig = nullptr;

	static __int64 __fastcall entity_locally_controlled_hook(StatescriptInstance* inst) {
		return is_created(inst) ? 1 : s_entity_locally_controlled_orig(inst);
	}

	//STUStatescriptActionCreateEntity (0E3689F3) names its fields differently: m_entityDef, m_out_Entity, and
	//2108984C, a random yaw range in degrees (m_min, m_max; Mei's wall pillars: 0-359). Its owner is the script's.
	static constexpr unsigned FIELD_RANDOM_YAW = 0x2108984C;

	//An entity with a character mover (component 0x15: the RIP-Tire, Mei's Blizzard drone) is placed by the movement
	//state in it, not by its scene position: spawned with only the scene position set it started at the map's origin
	//and fell through the map. Same as the hero's placement at spawn (entity_admin.cpp).
	static void set_mover_position(Entity* ent, Vector3 position) {
		auto mover = ent->getById<Component_15_STUCharacterMoverComponent>(0x15);
		if (!mover)
			return;
		auto& state = mover->movement_state_2;
		state.absolute_position = Vector4(position.X, position.Y, position.Z, 0);
		state.position_delta = Vector4(0, 0, 0, 0);
		state.gravity_delta = Vector4(0, 0, 0, 0);
	}

	//Spawns an entity definition as the server would for its owner: owned by it (GetOwner in the new entity's scripts),
	//on the team of the hero, its scripts run here with the server's authority.
	static Entity* spawn(__int64 def, __int64 res, Vector3 position, Vector4 rotation, Entity* owner, Entity* hero) {
		auto loader = EntityLoader::Create(def, res, false, false);
		if (!loader) {
			LOG_CORE(Warn, "[NODES] create entity: no loader for {:X}\n", def);
			return nullptr;
		}
		auto init = new Component_1_SceneRendering::InitData{};
		init->position = Vector4(position.X, position.Y, position.Z, 0);
		init->rotation = rotation;
		init->scale = Vector4(1, 1, 1, 1);
		loader->loader_entries[1].init_data = (__int64)init;
		auto ent = loader->Spawn(GameEntityAdmin());
		delete init;
		if (!ent) {
			LOG_CORE(Warn, "[NODES] create entity: spawning {:X} failed\n", def);
			return nullptr;
		}
		set_mover_position(ent, position);
		if (auto backref = ent->getById<Component_1F_Backref>(0x1F); backref && owner) {
			backref->backref_entid = owner->entity_id;
			backref->backref_global_entid = owner->entity_global_id;
		}
		//On the hero's team (its pre_filter_bits: the final bits and the team the other components see derive from them)
		auto hero_filter = hero ? hero->getById<Component_10_FilterBits>(0x10) : nullptr; //The hero's, not its controller's
		if (auto filter = ent->getById<Component_10_FilterBits>(0x10); filter && hero_filter) {
			filter->pre_filter_bits = hero_filter->pre_filter_bits;
			filter->FilterBitsMakeFinal();
		}
		s_created.insert(ent->entity_id);
		if (hero)
			s_created_for[ent->entity_id] = hero->entity_id;
		run_shared_nodes(ent);
		return ent;
	}

	//Spawns the entity a creation node (1BB99C36, 0E3689F3) or pet state (4CCED352) describes.
	static Entity* spawn_from_node(StatescriptInstance* inst, STUStatescriptBase* stu) {
		auto node = stu->graph_node.base.to_editable();
		auto def_cv = field_object(node, "m_entityDefinition").get_runtime_root();
		if (!def_cv.valid())
			def_cv = field_object(node, "m_entityDef").get_runtime_root();
		auto def = def_cv.valid() ? def_cv.get_argument_resource("m_entityDef")->resource_id : 0;
		auto res = def ? try_load_resource(def) : 0;
		if (!res) {
			LOG_CORE(Warn, "[NODES] create entity: script {:X} node {}: entity {:X} not loaded\n", inst->script_id, stu->idx_in_nodes, def);
			return nullptr;
		}

		auto self = script_entity(inst);
		StatescriptPrimitive owner_value{}, position_value{}, forward_value{}, up_value{};
		Entity* owner = eval(inst, node, FIELD_OWNER, owner_value) && owner_value.type == StatescriptPrimitive_ENTITY
			? GameEntityAdmin()->getEntById((uint32)owner_value.value) : nullptr;
		if (!owner && !field_object(node, FIELD_OWNER).valid())
			owner = self;
		auto hero = hero_of(owner ? owner : self);
		if (self && !(hero && hero->getById<Component_1_SceneRendering>(1)))
			hero = self; //The owner controls no hero (any more)
		auto hero_scene = hero ? hero->getById<Component_1_SceneRendering>(1) : nullptr;
		Vector3 position = hero_scene ? Vector3(hero_scene->position.X, hero_scene->position.Y, hero_scene->position.Z) : Vector3();
		//Without a position (pets), at the hero. A constant is used as is (Hanzo's dragon: the origin, its script moves
		//its parts); a variable still at the origin was never filled, and the server falls back to the hero (RIP-Tire:
		//the placement found no valid spot, so its position var 0x6BC stayed empty)
		auto position_field = field_object(node, "m_position").get_runtime_root();
		bool constant = position_field.valid() && position_field.struct_info->Hash == stringHash("STUConfigVarVec3");
		if (eval(inst, node, "m_position", position_value)) {
			auto p = to_vec3(position_value);
			if (constant || p.X != 0 || p.Y != 0 || p.Z != 0)
				position = p;
		}
		Vector3 forward = eval(inst, node, FIELD_FORWARD, forward_value) ? to_vec3(forward_value) : Vector3();
		Vector3 up = eval(inst, node, FIELD_UP, up_value) ? to_vec3(up_value) : Vector3();

		Vector4 rotation(0, 0, 0, 1);
		auto yaw_range = field_object(node, FIELD_RANDOM_YAW);
		if (forward.X != 0 || forward.Y != 0 || forward.Z != 0)
			rotation = look_rotation(forward, up);
		else if (yaw_range.valid()) {
			int lo = field_int(yaw_range, "m_min", 0);
			int hi = field_int(yaw_range, "m_max", 0);
			float yaw = (lo + (hi > lo ? rand() % (hi - lo + 1) : 0)) * 3.14159265f / 180.f;
			rotation = Vector4(0, std::sin(yaw / 2), 0, std::cos(yaw / 2));
		}
		else if (hero_scene && !position_value.type)
			rotation = hero_scene->rotation;

		auto ent = spawn(def, res, position, rotation, owner, hero);
		if (!ent)
			return nullptr;
		auto out = field_object(node, FIELD_OUT_ENTITY);
		if (!out.valid())
			out = field_object(node, "m_out_Entity");
		if (out.valid() && inst->rid_instance_varbag) {
			STUConfigVar_Custom value(StatescriptPrimitive(ent->entity_id, StatescriptPrimitive_ENTITY));
			auto bag = inst->rid_instance_varbag;
			bag->base.stu_base.vfptr->SetConfigVarValueFromSTUConfigVar(bag, (STUConfigVarDynamic*)out.value, value.get(), 0);
		}

		std::string scripts;
		if (auto ss = ent->getById<Component_23_Statescript>(0x23))
			for (auto script : ss->ss_inner)
				if (script)
					scripts += std::format(" {:X}/{:X}", script->script_id, *(uint32_t*)((char*)script + INSTANCE_FLAGS));
		std::string components;
		for (int i = 0; i < ent->component_list.num - 1; i++)
			components += std::format(" {:X}", ent->component_list.ptr[i]->component_id);
		LOG_CORE(Info, "[NODES] entity {} components:{} scripts:{}\n", ent->entity_id, components, scripts);
		auto q = [](Vector4 r) { return std::format("({:.3f}, {:.3f}, {:.3f}, {:.3f})", r.X, r.Y, r.Z, r.W); };
		LOG_CORE(Info, "[NODES] created entity {:X} (id {}) for script {:X} node {}: pos ({:.2f}, {:.2f}, {:.2f}) forward ({:.2f}, {:.2f}, {:.2f}) up ({:.2f}, {:.2f}, {:.2f}) rot {} hero rot {}\n",
			def, ent->entity_id, inst->script_id, stu->idx_in_nodes, position.X, position.Y, position.Z, forward.X, forward.Y, forward.Z,
			up.X, up.Y, up.Z, q(rotation), hero_scene ? q(hero_scene->rotation) : std::string("-"));
		return ent;
	}

	//
	// Game messages: the client's STUStatescriptActionSendGameMessage (0xD794F0) delivers only to the local player's
	// own scripts and leaves every other target to the server, and the server's own messages never come. Deliver one
	// the way it arrives: run the graph's STUStatescriptEntryGameMessage nodes (BE49A4C2) for that message.
	//
	static constexpr unsigned NODE_ENTRY_GAME_MESSAGE = 0xBE49A4C2;

	static __int64 entry_message(STUStatescriptBase* node) {
		auto cv = field_object(node->graph_node.base.to_editable(), "m_gameMessage").get_runtime_root();
		auto ref = cv.valid() ? cv.get_argument_resource("m_gameMessage") : nullptr;
		return ref ? ref->resource_id : 0;
	}

	static int deliver_game_message(StatescriptInstance* inst, __int64 message) {
		int delivered = 0;
		for (int i = 0; inst->graph && i < inst->graph->m_nodes.count(); i++) {
			auto node = inst->graph->m_nodes.list()[i];
			if (node && node_type(node) == NODE_ENTRY_GAME_MESSAGE && entry_message(node) == message) {
				inst->ExecuteNode(i);
				delivered++;
			}
		}
		return delivered;
	}

	//
	// Landing projectiles: Mei's Blizzard drone (entity 400000000000946, script 0x65E) is a projectile whose script waits
	// for the server's message 0x24000000000059D, sent when the drone has landed: it then deploys the storm (a 5 s
	// freeze aura and its effects at the drone) and removes the drone. Offline the projectile system removes the drone
	// when it hits the ground and nobody sends the message, so the drone vanished into the floor. Follow such
	// projectiles; when one is removed, place the same entity where it was last and deliver the message to it.
	//
	struct LandingProjectile {
		__int64 script;     //Script that identifies the projectile entity
		__int64 entity_def; //Entity placed where it lands
		__int64 message;    //Message the server sends it once it has landed
	};
	static constexpr LandingProjectile LANDING_PROJECTILES[] = {
		{ 0x58000000000065E, 0x400000000000946, 0x24000000000059D }, //Mei's Blizzard drone
	};
	static constexpr int MESSAGE_TRIES = 60; //Ticks to wait for the placed entity's graph (built lazily)

	struct Flying {
		const LandingProjectile* kind;
		uint32_t owner;
		Vector3 position;
	};
	static std::map<uint32_t, Flying> s_flying{};

	struct PendingMessage {
		uint32_t target;
		__int64 script;
		__int64 message;
		int tries = 0;
	};
	static std::vector<PendingMessage> s_pending_messages{};

	void projectile_created(Entity* projectile) {
		auto ss = projectile ? projectile->getById<Component_23_Statescript>(0x23) : nullptr;
		if (!ss)
			return;
		for (auto& kind : LANDING_PROJECTILES) {
			if (!ss->ss_inner.getByResourceId(kind.script))
				continue;
			auto backref = projectile->getById<Component_1F_Backref>(0x1F);
			auto scene = projectile->getById<Component_1_SceneRendering>(1);
			s_flying[projectile->entity_id] = { &kind, backref ? backref->backref_entid : 0,
				scene ? Vector3(scene->position.X, scene->position.Y, scene->position.Z) : Vector3() };
			return;
		}
	}

	static void land_projectiles() {
		for (auto it = s_flying.begin(); it != s_flying.end();) {
			auto& [id, flying] = *it;
			if (auto ent = GameEntityAdmin()->getEntById(id)) {
				if (auto scene = ent->getById<Component_1_SceneRendering>(1))
					flying.position = Vector3(scene->position.X, scene->position.Y, scene->position.Z);
				++it;
				continue;
			}
			auto kind = flying.kind;
			auto owner = flying.owner ? GameEntityAdmin()->getEntById(flying.owner) : nullptr;
			auto hero = owner ? hero_of(owner) : hero_of(GameEntityAdmin()->getLocalEnt()); //Projectiles have no owner set
			auto hero_scene = hero ? hero->getById<Component_1_SceneRendering>(1) : nullptr;
			Vector4 rotation = hero_scene ? hero_scene->rotation : Vector4(0, 0, 0, 1);
			auto res = try_load_resource(kind->entity_def);
			auto landed = res ? spawn(kind->entity_def, res, flying.position, rotation, owner, hero) : nullptr;
			LOG_CORE(Info, "[NODES] projectile {} landed at ({:.2f}, {:.2f}, {:.2f}): placed {:X} as {}\n", id, flying.position.X,
				flying.position.Y, flying.position.Z, kind->entity_def, landed ? landed->entity_id : 0);
			if (landed)
				s_pending_messages.push_back({ landed->entity_id, kind->script, kind->message });
			it = s_flying.erase(it);
		}
	}

	static void send_pending_messages() {
		auto pending = std::move(s_pending_messages);
		s_pending_messages.clear();
		for (auto& msg : pending) {
			auto ent = GameEntityAdmin()->getEntById(msg.target);
			auto ss = ent ? ent->getById<Component_23_Statescript>(0x23) : nullptr;
			auto inst = ss ? ss->ss_inner.getByResourceId(msg.script) : nullptr;
			int delivered = inst && inst->graph ? deliver_game_message(inst, msg.message) : 0;
			if (delivered)
				LOG_CORE(Info, "[NODES] message {:X} delivered to {} ({} entries)\n", msg.message, msg.target, delivered);
			else if (ent && ++msg.tries < MESSAGE_TRIES)
				s_pending_messages.push_back(msg);
			else
				LOG_CORE(Warn, "[NODES] message {:X} for {}: script {:X} not ready\n", msg.message, msg.target, msg.script);
		}
	}

	//What the server answers after one of these creation nodes created its entity, a tick later: copy the new entity
	//into another of the instance's variables and send the instance a game message.
	struct CreateReply {
		__int64 script;
		int node;
		__int64 copy_to_var;
		__int64 message;
	};
	static constexpr CreateReply CREATE_REPLIES[] = {
		//RIP-Tire: 0x13E completes the join (node 19) waiting on Junkrat's wind-up; it then takes the tire from var
		//0x28 and switches to driving it. Without it the graph went back to "ready" after 1.5 s and let go.
		{ 0x580000000000232, 32, 0xD80000000000028, 0x24000000000013E },
	};

	struct PendingReply {
		StatescriptInstance* inst;
		const CreateReply* reply;
		uint32_t entity;
	};
	static std::vector<PendingReply> s_pending_replies{};

	static bool instance_alive(StatescriptInstance* inst) {
		auto controller = GameEntityAdmin()->getLocalEnt();
		auto hero = hero_of(controller);
		uint32_t ids[] = { hero ? hero->entity_id : 0, s_possession_hero() };
		for (auto id : ids) {
			auto ent = id ? GameEntityAdmin()->getEntById(id) : nullptr;
			auto ss = ent ? ent->getById<Component_23_Statescript>(0x23) : nullptr;
			if (ss)
				for (auto i : ss->ss_inner)
					if (i == inst)
						return true;
		}
		return false;
	}

	static void send_pending_replies() {
		auto pending = std::move(s_pending_replies);
		s_pending_replies.clear();
		for (auto& p : pending) {
			if (!instance_alive(p.inst))
				continue;
			if (p.reply->copy_to_var && p.inst->rid_instance_varbag)
				p.inst->rid_instance_varbag->SetVar({ p.reply->copy_to_var }, StatescriptPrimitive(p.entity, StatescriptPrimitive_ENTITY));
			int delivered = p.reply->message ? deliver_game_message(p.inst, p.reply->message) : 0;
			LOG_CORE(Info, "[NODES] server reply for script {:X} node {}: entity {} -> var {:X}, message {:X} ({} entries)\n",
				p.reply->script, p.reply->node, p.entity, p.reply->copy_to_var, p.reply->message, delivered);
		}
	}

	static void create_entity(StatescriptInstance* inst, STUStatescriptBase* stu) {
		static std::map<std::pair<StatescriptInstance*, int>, double> s_last_created{};
		auto now = GetGameRuntimeSecs();
		auto& last = s_last_created[{ inst, stu->idx_in_nodes }];
		if (last && now - last < CREATE_REPEAT_SECS)
			return;
		last = now;
		auto ent = spawn_from_node(inst, stu);
		if (!ent)
			return;
		for (auto& reply : CREATE_REPLIES)
			if (reply.script == inst->script_id && reply.node == stu->idx_in_nodes)
				s_pending_replies.push_back({ inst, &reply, ent->entity_id });
	}

	//
	// Pets (STUStatescriptStatePet 4CCED352, no client implementation): Zarya's barriers, Mei's Cryo-Freeze and
	// others. Same fields as the creation node; the pet lives while the state runs (its on-begin plug starts the
	// pet's script with PlayScript). Spawned when a link enters the state's begin plug, removed once it finished.
	//
	static constexpr unsigned NODE_PET = 0x4CCED352;
	static constexpr double PET_MIN_SECS = 0.25; //Before this the state may not have begun yet

	struct Pet {
		uint32_t entity;
		double created;
	};
	static std::map<std::pair<StatescriptInstance*, int>, Pet> s_pets{};

	static void begin_pet(StatescriptInstance* inst, STUStatescriptBase* node, __int64 plug) {
		auto begin = field_object(node->graph_node.base.to_editable(), "m_beginPlug");
		if (!begin.valid() || (__int64)begin.value != plug)
			return;
		auto key = std::make_pair(inst, node->idx_in_nodes);
		if (auto it = s_pets.find(key); it != s_pets.end() && GameEntityAdmin()->getEntById(it->second.entity))
			return; //Already has its pet
		if (auto ent = spawn_from_node(inst, node))
			s_pets[key] = { ent->entity_id, GetGameRuntimeSecs() };
	}

	static void update_pets() {
		auto now = GetGameRuntimeSecs();
		for (auto it = s_pets.begin(); it != s_pets.end();) {
			auto& [key, pet] = *it;
			auto ent = GameEntityAdmin()->getEntById(pet.entity);
			bool finished = now - pet.created > PET_MIN_SECS && statescript_logger::GetScriptNodeState(key.first, key.second).is_finish_state;
			if (ent && !finished) {
				++it;
				continue;
			}
			if (ent) {
				LOG_CORE(Info, "[NODES] pet {} removed (its state ended)\n", pet.entity);
				s_created.erase(pet.entity);
				GameEntityAdmin()->delEnt(ent);
			}
			it = s_pets.erase(it);
		}
	}

	//
	// Placement (node type 160C4F1D): Torbjorn's turret, Symmetra's turrets, Junkrat's RIP-Tire find a spot in front of
	// the hero. The client registers a stub for it (vtable 0x15D6F40, every slot a no-op); the class that computes
	// the spot (factory 0xD64380, vtable 0x15D5E68) is in the client too: its tick (0x1096500) traces for a valid
	// spot and exposes the result as state values (position +0xc0, valid +0xb0), which its output vars (e.g. 0x6BC,
	// the position) read while it runs. Placing is the server's: input plug C63FD388 ("place now": Torbjorn's fire
	// button, RIP-Tire right on begin) answers with output 02701BF5 (placed: the graph creates the entity) or
	// D3894419 (no valid spot). Offline that input only restarted the state, so nothing was ever placed.
	//
	static constexpr unsigned NODE_PLACEMENT = 0x160C4F1D;
	static constexpr __int64 RVA_FIND_STATE_FACTORY = 0xF43450; //Creates a state's implementation for its node
	static constexpr __int64 RVA_PLACEMENT_STUB_VTABLE = 0x15D6F40;
	static constexpr __int64 RVA_PLACEMENT_UPDATE = 0x1096500;
	static constexpr __int64 RVA_TRAVERSE_PLUG = 0xF46F80; //Follows every link of an output plug
	static constexpr int PLACEMENT_VALID = 0xb0;
	static constexpr int NODE_STATE_INDEX = 0xf8;

	//State types the client registers twice: the shared implementation (which the server runs) and a client one
	//registered over it. For these the client one is a stub that does nothing (every slot a no-op), so offline the
	//state did nothing at all; create the shared implementation instead.
	struct ServerState {
		unsigned type;
		__int64 stub_vtable;
		__int64 factory;
	};
	static constexpr ServerState SERVER_STATES[] = {
		{ NODE_PLACEMENT, RVA_PLACEMENT_STUB_VTABLE, 0xD64380 }, //Placement: turrets, RIP-Tire
		{ 0x72509D3B, 0x15D6598, 0xD64110 },  //STUStatescriptStateFindGround: Ice Wall
		{ 0x12AA4F0F, 0x15D6CC8, 0xD645D0 },  //Ice Wall start
		{ 0x614C3A0A, 0x15D6A70, 0xD647D0 },  //Ice Wall segments
	};

	using FindStateFactoryFn = __int64(__fastcall*)(__int64 a1, STUStatescriptBase* node);
	static FindStateFactoryFn s_find_state_factory_orig = nullptr;

	static __int64 __fastcall find_state_factory_hook(__int64 a1, STUStatescriptBase* node) {
		auto state = s_find_state_factory_orig(a1, node);
		if (!state || !node)
			return state;
		auto type = node_type(node);
		static std::set<unsigned> s_logged{};
		for (auto& server : SERVER_STATES) {
			if (server.type != type)
				continue;
			bool stub = *(__int64*)state == globals::gameBase + server.stub_vtable;
			if (s_logged.insert(type).second)
				LOG_CORE(Info, "[NODES] state {:08X}: client implementation vtable {:#x}{}\n", type, *(__int64*)state - globals::gameBase,
					stub ? ", a stub: using the shared one" : "");
			if (!stub)
				break;
			if (auto replacement = ((__int64(__fastcall*)())(globals::gameBase + server.factory))())
				state = replacement; //The stub (0xb8-0x120 bytes) is left allocated
			break;
		}
		return state;
	}

	static StatescriptState* running_state(StatescriptInstance* inst, STUStatescriptBase* node) {
		int idx = *(int*)((char*)node + NODE_STATE_INDEX);
		return idx >= 0 && idx < inst->state_impl_list.num ? inst->state_impl_list.ptr[idx] : nullptr;
	}

	//The state's spot (bound to its output vars while it runs): position, surface normal, forward.
	static constexpr int PLACEMENT_POSITION = 0xc0;
	static constexpr int PLACEMENT_NORMAL = 0xd0;
	static constexpr int PLACEMENT_FORWARD = 0xe0;
	//Output vars only the server writes, when it places: where the entity goes (Torbjorn's and Symmetra's create
	//nodes read them, e.g. vars 0x65A, 0x9B1, 0x9B2).
	static constexpr unsigned FIELD_PLACED_POSITION = 0xC500D8CF;
	static constexpr unsigned FIELD_PLACED_NORMAL = 0x57216848;
	static constexpr unsigned FIELD_PLACED_FORWARD = 0x2650F0BE;

	static void set_dynamic(StatescriptInstance* inst, STU_Object var, StatescriptPrimitive value) {
		if (!var.valid() || !inst->rid_instance_varbag)
			return;
		STUConfigVar_Custom cv(value);
		auto bag = inst->rid_instance_varbag;
		bag->base.stu_base.vfptr->SetConfigVarValueFromSTUConfigVar(bag, (STUConfigVarDynamic*)var.value, cv.get(), 0);
	}

	static StatescriptPrimitive vec3(const void* xyz) {
		StatescriptPrimitive value(0, StatescriptPrimitive_VEC3);
		memcpy(&value.value, xyz, sizeof(float) * 3);
		return value;
	}

	static void write_placed(StatescriptInstance* inst, STU_Object node, StatescriptState* state) {
		set_dynamic(inst, field_object(node, FIELD_PLACED_POSITION), vec3((char*)state + PLACEMENT_POSITION));
		set_dynamic(inst, field_object(node, FIELD_PLACED_NORMAL), vec3((char*)state + PLACEMENT_NORMAL));
		set_dynamic(inst, field_object(node, FIELD_PLACED_FORWARD), vec3((char*)state + PLACEMENT_FORWARD));
	}

	//"Place now" inputs (type 8DCB62EA) the server answers; offline that input only restarted the state. Mei's Ice
	//Wall chains them: StateFindGround's answer 02701BF5 is the "place now" of the wall start (12AA4F0F), whose answer
	//8CF01B33 is the next segment's (614C3A0A, five of them), and the last one's leads to the nodes creating the wall.
	struct PlaceRule {
		unsigned type;
		unsigned input;      //"Place now" input plug field
		unsigned placed;     //Output plug field: placed
		unsigned not_placed; //Output plug field: no valid spot (0: none)
		int valid;           //Byte in the state that says the spot is valid (-1: always placed)
		__int64 update;      //Recomputes the spot first (0: none)
		bool write_placed;   //Write the placed position/normal/forward vars
	};
	static constexpr PlaceRule PLACE_RULES[] = {
		{ NODE_PLACEMENT, 0xC63FD388, 0x02701BF5, 0xD3894419, PLACEMENT_VALID, RVA_PLACEMENT_UPDATE, true },
		{ 0x72509D3B, 0xC63FD388, 0x02701BF5, 0, 0xc8, 0, false }, //STUStatescriptStateFindGround: +0xc8 ground found
		{ 0x12AA4F0F, 0x5556C513, 0x8CF01B33, 0, -1, 0, false },
		{ 0x614C3A0A, 0x5556C513, 0x8CF01B33, 0, -1, 0, false },
	};

	static const PlaceRule* place_rule(unsigned type) {
		for (auto& rule : PLACE_RULES)
			if (rule.type == type)
				return &rule;
		return nullptr;
	}

	static bool is_stub(StatescriptState* state, unsigned type) {
		for (auto& server : SERVER_STATES)
			if (server.type == type && *(__int64*)state == globals::gameBase + server.stub_vtable)
				return true;
		return false;
	}

	//Answers "place now" like the server, once per activation of the state. Returns whether the link was that input;
	//it is handled here either way (the client would restart the state for it).
	static bool place(StatescriptInstance* inst, STUStatescriptBase* node, STUStatescriptBase* from, __int64 plug, const PlaceRule& rule) {
		static std::map<std::pair<StatescriptInstance*, int>, __int64> s_placed_activation{};
		auto editable = node->graph_node.base.to_editable();
		auto input = field_object(editable, rule.input);
		if (!input.valid() || (__int64)input.value != plug)
			return false;
		auto& logged = statescript_logger::GetScriptNodeState(inst, node->idx_in_nodes);
		auto state = running_state(inst, node);
		if (logged.is_finish_state || !state || is_stub(state, rule.type))
			return true;
		auto& placed = s_placed_activation[{ inst, node->idx_in_nodes }];
		if (placed == logged.change_timestamp)
			return true;
		placed = logged.change_timestamp;
		if (rule.update)
			((void(__fastcall*)(StatescriptState*))(globals::gameBase + rule.update))(state);
		bool valid = rule.valid < 0 || *((char*)state + rule.valid) != 0;
		if (valid && rule.write_placed)
			write_placed(inst, editable, state);
		unsigned out_field = valid ? rule.placed : rule.not_placed;
		auto out = out_field ? field_object(editable, out_field) : STU_Object();
		LOG_CORE(Info, "[NODES] place: script {:X} node {} type {:08X} (from node {}): {}\n", inst->script_id, node->idx_in_nodes,
			rule.type, from ? from->idx_in_nodes : -1, valid ? "placed" : "no valid spot");
		if (out.valid())
			((char(__fastcall*)(StatescriptState*, StatescriptInstance*, STUStatescriptBase*, __int64, int))(globals::gameBase + RVA_TRAVERSE_PLUG))(
				state, inst, node, (__int64)out.value, 0x80);
		return true;
	}

	//
	// STUStatescriptActionCreateEntity (0E3689F3): the client's implementation (work 0xD6EDA0) only binds an entity the
	// server announced for the node (looked up in a map at component +0x500) and does nothing without one. Created
	// here for the scripts below; it is also in Torbjorn's turret (0xC9) and two scripts every hero runs (0x1C7,
	// 0x5AD), which are left alone until checked.
	// STUStatescriptActionDestroyEntity (5C46D801, field m_entity) has no client implementation; it removes entities
	// created here (Mei's previous pillars, Hanzo's dragon when its script ends).
	//
	static constexpr unsigned NODE_ACTION_CREATE_ENTITY = 0x0E3689F3;
	static constexpr unsigned NODE_DESTROY_ENTITY = 0x5C46D801;
	static constexpr __int64 ACTION_CREATE_SCRIPTS[] = {
		0x580000000000622, //Mei's Ice Wall pillars
	};

	static void action_create_entity(StatescriptInstance* inst, STUStatescriptBase* node) {
		static std::set<std::pair<__int64, int>> s_logged{};
		if (std::find(std::begin(ACTION_CREATE_SCRIPTS), std::end(ACTION_CREATE_SCRIPTS), inst->script_id) == std::end(ACTION_CREATE_SCRIPTS)) {
			if (s_logged.insert({ inst->script_id, node->idx_in_nodes }).second)
				LOG_CORE(Info, "[NODES] script {:X} node {}: CreateEntity not emulated for this script\n", inst->script_id, node->idx_in_nodes);
			return;
		}
		create_entity(inst, node);
	}

	static void destroy_entity(StatescriptInstance* inst, STUStatescriptBase* node) {
		StatescriptPrimitive value{};
		if (!eval(inst, node->graph_node.base.to_editable(), "m_entity", value) || value.type != StatescriptPrimitive_ENTITY)
			return;
		auto id = (uint32_t)value.value;
		auto ent = GameEntityAdmin()->getEntById(id);
		if (!ent || !s_created.erase(id))
			return;
		LOG_CORE(Info, "[NODES] destroyed entity {} for script {:X} node {}\n", id, inst->script_id, node->idx_in_nodes);
		GameEntityAdmin()->delEnt(ent);
	}

	//
	// STUStatescriptActionPlayScript (F9EF59A6): the client's implementation (0xD79C90) plays the script only when the
	// target is the local player and sends every other target to the server. Hanzo's dragons are drawn by the script
	// it plays on the dragon entity (0x132), Mei's pillars and the RIP-Tire get theirs the same way. For entities
	// created here: load the script on the target, write the caller's m_params (param id C76B6366, m_value) into the
	// variables the script's STUStatescriptEntryPlayScript node names for them (m_params: param id, m_out_Var), its
	// m_out_Sender, and run that entry. A tick later, outside the caller's execution.
	//
	static constexpr unsigned NODE_PLAY_SCRIPT = 0xF9EF59A6;
	static constexpr unsigned NODE_ENTRY_PLAY_SCRIPT = 0x46C48944;
	static constexpr unsigned FIELD_PARAM_ID = 0xC76B6366;

	struct PendingPlay {
		uint32_t target;
		__int64 script;
		uint32_t sender;
		std::vector<std::pair<__int64, StatescriptPrimitive>> params;
		int tries = 0;
	};
	static constexpr int PLAY_TRIES = 60; //Ticks to wait for the loaded script's graph (built lazily)
	static std::vector<PendingPlay> s_pending_plays{};

	static bool has_field(STU_Object object, unsigned field) {
		if (!object.valid())
			return false;
		for (auto [info, arg] : object.struct_info->RangeArgs())
			if (arg->Hash == field)
				return true;
		return false;
	}

	//The resource id in a param's id field (C76B6366), 0 without one.
	static __int64 param_id(STU_Object item) {
		for (auto [info, arg] : item.struct_info->RangeArgs())
			if (arg->Hash == FIELD_PARAM_ID) {
				auto ref = item.get_argument_resource(arg);
				return ref ? ref->resource_id : 0;
			}
		return 0;
	}

	static __int64 resource_field(STU_Object object, const char* field) {
		object = object.get_runtime_root();
		if (!object.valid())
			return 0;
		auto hash = stringHash(field);
		for (auto [info, arg] : object.struct_info->RangeArgs())
			if (arg->Hash == hash) {
				auto ref = object.get_argument_resource(arg);
				return ref ? ref->resource_id : 0;
			}
		return 0;
	}

	static void play_script(StatescriptInstance* inst, STUStatescriptBase* stu) {
		auto node = stu->graph_node.base.to_editable();
		StatescriptPrimitive target{};
		if (!eval(inst, node, "m_targets", target) || target.type != StatescriptPrimitive_ENTITY || !s_created.count((uint32_t)target.value))
			return;
		auto script = resource_field(field_object(node, "m_script"), "m_graph");
		if (!script)
			return;
		PendingPlay play{ (uint32_t)target.value, script, 0, {} };
		if (auto self = script_entity(inst))
			play.sender = hero_of(self) ? hero_of(self)->entity_id : self->entity_id;
		if (has_field(node, stringHash("m_params"))) {
			for (auto item : node.get_argument_objectlist(stringHash("m_params"))) {
				item = item.get_runtime_root();
				StatescriptPrimitive value{};
				auto id = item.valid() ? param_id(item) : 0;
				if (id && eval(inst, item, "m_value", value))
					play.params.push_back({ id, value });
			}
		}
		s_pending_plays.push_back(std::move(play));
	}

	static void run_pending_plays() {
		auto pending = std::move(s_pending_plays);
		s_pending_plays.clear();
		for (auto& play : pending) {
			auto ent = GameEntityAdmin()->getEntById(play.target);
			auto ss = ent ? ent->getById<Component_23_Statescript>(0x23) : nullptr;
			if (!ss)
				continue;
			try_load_resource(play.script);
			auto inst = ss->ss_inner.getByResourceId(play.script);
			if (!inst) {
				int before = ss->ss_inner.g1_instanceArr.num;
				//With authority (the vfunc behind exec flag 0x8), as the server would run it
				auto loaded = ss->vfptr->load_statescript_script_bit3(ss, play.script, 0, ent->entity_id);
				inst = ss->ss_inner.getByResourceId(play.script);
				if (!inst)
					LOG_CORE(Info, "[NODES] play script {:X}: load returned {:#x}, instances {} -> {}\n", play.script, loaded, before,
						ss->ss_inner.g1_instanceArr.num);
			}
			if (!inst || !inst->graph) {
				if (++play.tries < PLAY_TRIES)
					s_pending_plays.push_back(std::move(play));
				else
					LOG_CORE(Warn, "[NODES] play script {:X} on {}: not loaded\n", play.script, play.target);
				continue;
			}
			run_shared_nodes(inst);
			int entries = 0, set = 0;
			for (int i = 0; i < inst->graph->m_nodes.count(); i++) {
				auto entry = inst->graph->m_nodes.list()[i];
				if (!entry || node_type(entry) != NODE_ENTRY_PLAY_SCRIPT)
					continue;
				auto editable = entry->graph_node.base.to_editable();
				if (has_field(editable, stringHash("m_params"))) for (auto item : editable.get_argument_objectlist(stringHash("m_params"))) {
					item = item.get_runtime_root();
					auto id = item.valid() ? param_id(item) : 0;
					for (auto& [param, value] : play.params)
						if (param == id) {
							set_dynamic(inst, field_object(item, "m_out_Var"), value);
							set++;
						}
				}
				if (play.sender)
					set_dynamic(inst, field_object(editable, "m_out_Sender"), StatescriptPrimitive(play.sender, StatescriptPrimitive_ENTITY));
				inst->ExecuteNode(i);
				entries++;
			}
			LOG_CORE(Info, "[NODES] played script {:X} on {} ({} params, {} set, {} entries; instance flags {:#x}, vtable {:#x}, runs-node {:#x})\n",
				play.script, play.target, play.params.size(), set, entries, *(uint32_t*)((char*)inst + INSTANCE_FLAGS),
				(__int64)inst->vfptr - globals::gameBase, ((__int64*)inst->vfptr)[INSTANCE_RUNS_NODE_VFUNC] - globals::gameBase);
		}
	}

	//An ultimate starting: a STUStatescriptStateAbility whose start button is the ultimate button follows its links
	//(it does when it activates). Only the hero's own graphs; once per activation.
	static constexpr unsigned NODE_ABILITY = 0xF53C5DA8;
	static constexpr int ULTIMATE_BUTTON = 0x79;
	static constexpr double ULTIMATE_REPEAT_SECS = 1.0;

	static void check_ultimate(StatescriptInstance* inst, STUStatescriptBase* from) {
		static std::map<std::pair<StatescriptInstance*, int>, double> s_last{};
		StatescriptPrimitive button{};
		if (!eval(inst, from->graph_node.base.to_editable(), "m_startButton", button) || (int)button.value != ULTIMATE_BUTTON)
			return;
		auto now = GetGameRuntimeSecs();
		auto& last = s_last[{ inst, from->idx_in_nodes }];
		if (last && now - last < ULTIMATE_REPEAT_SECS)
			return;
		last = now;
		LOG_CORE(Info, "[NODES] ultimate started: script {:X} node {}\n", inst->script_id, from->idx_in_nodes);
		ultimate::spend();
	}

	//Debug: PROMETHEUS_TRACE_LINKS=<script id> (hex) logs every link that script's graph follows.
	static __int64 traced_script() {
		static __int64 s_script = -1;
		if (s_script == -1) {
			auto env = getenv("PROMETHEUS_TRACE_LINKS");
			s_script = env && *env ? std::stoll(env, nullptr, 16) : 0;
		}
		return s_script;
	}

	static char __fastcall traverse_link_hook(__int64 from_impl, StatescriptInstance* inst, STUStatescriptBase* from, __int64 a4, __int64 plug, int a6) {
		auto node = plug ? *(STUStatescriptBase**)(plug + PLUG_PARENT_NODE) : nullptr;
		if (node && inst) {
			trace(inst, node);
			if (from && from->m_isState && node_type(from) == NODE_ABILITY)
				check_ultimate(inst, from);
			if (traced_script() && inst->script_id == traced_script())
				LOG_CORE(Info, "[LINK] script {:X} inst {}: node {} -> node {} ({:08X}, input plug type {:08X})\n", inst->script_id, inst->instance_id,
					from ? from->idx_in_nodes : -1, node->idx_in_nodes, node_type(node), ((STUBase<>*)plug)->vfptr->GetSTUInfo()->Hash);
			if (!node->m_isState && node_type(node) == NODE_CREATE_ENTITY && runs_node(inst, node))
				create_entity(inst, node);
			if (node->m_isState && node_type(node) == NODE_PET && runs_node(inst, node))
				begin_pet(inst, node, plug);
			if (!node->m_isState && node_type(node) == NODE_ACTION_CREATE_ENTITY && runs_node(inst, node))
				action_create_entity(inst, node);
			if (!node->m_isState && node_type(node) == NODE_DESTROY_ENTITY && runs_node(inst, node))
				destroy_entity(inst, node);
			if (!node->m_isState && node_type(node) == NODE_PLAY_SCRIPT && runs_node(inst, node))
				play_script(inst, node);
			if (node->m_isState && runs_node(inst, node))
				if (auto rule = place_rule(node_type(node)); rule && place(inst, node, from, plug, *rule))
					return 1;
		}
		return s_traverse_link_orig(from_impl, inst, from, a4, plug, a6);
	}

	//
	// Possession (StatePossessControl 74CB0002 / StatePossessView 3A846155, no client implementation): Junkrat drives
	// his RIP-Tire, D.Va calls her mech. The player's controller entity sends movement and aim to the entity in its
	// component 0x20 (movement_attach / aim) and the camera follows cam_attach; Prometheus points them at the hero
	// when it spawns one. While such a state runs on the hero, point them at the possessed entity (field 743B15A6
	// for control, D7148C41 for the view) and back afterwards.
	//
	static constexpr unsigned NODE_POSSESS_CONTROL = 0x74CB0002;
	static constexpr unsigned NODE_POSSESS_VIEW = 0x3A846155;
	static constexpr unsigned FIELD_CONTROL_TARGET = 0x743B15A6;
	static constexpr unsigned FIELD_VIEW_TARGET = 0xD7148C41;

	struct Possession {
		uint32_t hero = 0; //Model the controller drove before
		uint32_t control = 0;
		uint32_t view = 0;
		__int64 script = 0; //Script whose PossessControl state runs
		double started = 0;
		double fired = 0; //When primary fire was pressed during it
		Vector3 hero_position{}; //Where the hero stands while it possesses
		bool driving = false;    //The player's movement drives the possessed entity (not while it is attached)
	};

	//Possessed entities that end on their own: the RIP-Tire's script (run with authority, see entity_runs_node_hook)
	//detonates it on primary fire or when its time runs out, then destroys it (removing it ends the possession:
	//Junkrat's graph sees it gone and returns to normal). Fallback if it is still there: remove it this long after
	//primary fire or after this long in all.
	struct TimedPossession {
		__int64 script;
		double secs;
	};
	static constexpr TimedPossession TIMED_POSSESSIONS[] = {
		{ 0x580000000000232, 15.0 }, //RIP-Tire
	};
	static constexpr int BUTTON_FIRE = 93;
	static constexpr double POSSESSION_FIRE_GRACE_SECS = 0.5; //The press that started it
	static constexpr double POSSESSION_AFTER_FIRE_SECS = 3.0; //The tire's detonation takes 2 s
	static Possession s_possession{};

	static uint32_t s_possession_hero() {
		return s_possession.hero;
	}

	static uint32_t entity_value(StatescriptInstance* inst, STUStatescriptBase* node, unsigned field) {
		StatescriptPrimitive value{};
		if (!eval(inst, node->graph_node.base.to_editable(), field, value) || value.type != StatescriptPrimitive_ENTITY)
			return 0;
		return GameEntityAdmin()->getEntById((uint32_t)value.value) ? (uint32_t)value.value : 0;
	}

	static bool fire_pressed() {
		static bool s_was_down = false;
		auto input = LobbyEntityAdmin() ? LobbyEntityAdmin()->getSingletonComponent<Component_50_Input>(0x50) : nullptr;
		if (!input)
			return false;
		char* buttons = input->tick10 ? input->stru1.tick0 : input->stru1.tick1;
		bool down = buttons[BUTTON_FIRE] != 0;
		bool pressed = down && !s_was_down;
		s_was_down = down;
		return pressed;
	}

	//The possessing hero stays where it was (Junkrat while he drives his tire). Not held, it slid away at ~60 m/s during
	//the tire's wind-up, taking the attached tire off the map: the tire sits 1.4 m in front of Junkrat, and the hero
	//followed the entity the player now controls.
	static constexpr float HOLD_TOLERANCE = 0.25f;

	static void hold_hero(Entity* hero) {
		auto scene = hero ? hero->getById<Component_1_SceneRendering>(1) : nullptr;
		if (!scene)
			return;
		auto& held = s_possession.hero_position;
		float dx = scene->position.X - held.X, dy = scene->position.Y - held.Y, dz = scene->position.Z - held.Z;
		if (dx * dx + dy * dy + dz * dz < HOLD_TOLERANCE * HOLD_TOLERANCE)
			return;
		scene->position.X = held.X;
		scene->position.Y = held.Y;
		scene->position.Z = held.Z;
		set_mover_position(hero, held);
	}

	//An entity attached to another (STUStatescriptStateAttach running on it: the RIP-Tire in Junkrat's hands during its
	//wind-up) cannot be driven: driven, its mover took the attach offset for a position change and it shot across the
	//map. The player drives it once released.
	static constexpr unsigned NODE_ATTACH = 0x971645FE;
	static constexpr double POSSESSION_ATTACH_SECS = 0.25; //Its scripts (and the attach) start a tick or two after it

	static bool attached(uint32_t id) {
		auto ent = GameEntityAdmin()->getEntById(id);
		auto ss = ent ? ent->getById<Component_23_Statescript>(0x23) : nullptr;
		if (!ss)
			return false;
		for (auto inst : ss->ss_inner) {
			if (!inst || !inst->graph)
				continue;
			for (auto& [idx, state] : statescript_logger::GetScriptState(inst))
				if (!state.is_finish_state && idx >= 0 && idx < inst->graph->m_nodes.count() && node_type(inst->graph->m_nodes.list()[idx]) == NODE_ATTACH)
					return true;
		}
		return false;
	}

	//Hands the player's movement and aim to the possessed entity, its mover starting where it is.
	static void drive(Component_20_ModelReference* ref, uint32_t id) {
		auto ent = GameEntityAdmin()->getEntById(id);
		auto scene = ent ? ent->getById<Component_1_SceneRendering>(1) : nullptr;
		if (scene)
			set_mover_position(ent, Vector3(scene->position.X, scene->position.Y, scene->position.Z));
		ref->movement_attach_entid = id;
		ref->aim_entid = id;
		s_possession.driving = true;
		LOG_CORE(Info, "[NODES] possession: driving {}\n", id);
	}

	static void end_timed_possession() {
		auto now = GetGameRuntimeSecs();
		if (fire_pressed() && !s_possession.fired && now - s_possession.started > POSSESSION_FIRE_GRACE_SECS)
			s_possession.fired = now;
		for (auto& timed : TIMED_POSSESSIONS) {
			if (timed.script != s_possession.script)
				continue;
			bool after_fire = s_possession.fired && now - s_possession.fired > POSSESSION_AFTER_FIRE_SECS;
			if (now - s_possession.started < timed.secs && !after_fire)
				return;
			if (auto ent = GameEntityAdmin()->getEntById(s_possession.control)) {
				LOG_CORE(Info, "[NODES] possessed entity {} still there, removed ({})\n", s_possession.control, after_fire ? "fire" : "time");
				s_created.erase(s_possession.control);
				GameEntityAdmin()->delEnt(ent);
			}
			return;
		}
	}

	static void update_possession() {
		auto controller = GameEntityAdmin()->getLocalEnt();
		auto ref = controller ? controller->getById<Component_20_ModelReference>(0x20) : nullptr;
		if (!ref)
			return;
		uint32_t hero_id = s_possession.hero ? s_possession.hero : ref->movement_attach_entid;
		auto hero = GameEntityAdmin()->getEntById(hero_id);
		auto ss = hero ? hero->getById<Component_23_Statescript>(0x23) : nullptr;
		uint32_t control = 0, view = 0;
		__int64 control_script = 0;
		if (ss) {
			for (auto inst : ss->ss_inner) {
				if (!inst || !inst->graph)
					continue;
				for (auto& [idx, state] : statescript_logger::GetScriptState(inst)) {
					if (state.is_finish_state || idx < 0 || idx >= inst->graph->m_nodes.count())
						continue;
					auto node = inst->graph->m_nodes.list()[idx];
					auto type = node_type(node);
					if (type == NODE_POSSESS_CONTROL && !control) {
						control = entity_value(inst, node, FIELD_CONTROL_TARGET);
						control_script = inst->script_id;
					}
					else if (type == NODE_POSSESS_VIEW && !view)
						view = entity_value(inst, node, FIELD_VIEW_TARGET);
				}
			}
		}
		if (control && control == s_possession.control) {
			hold_hero(hero);
			if (!s_possession.driving && GetGameRuntimeSecs() - s_possession.started > POSSESSION_ATTACH_SECS && !attached(control))
				drive(ref, control);
			end_timed_possession();
			if (view == s_possession.view)
				return;
		}
		if (control == s_possession.control && view == s_possession.view)
			return;
		if (control != s_possession.control) {
			s_possession.script = control_script;
			s_possession.started = GetGameRuntimeSecs();
			s_possession.fired = 0;
			if (auto scene = hero ? hero->getById<Component_1_SceneRendering>(1) : nullptr)
				s_possession.hero_position = Vector3(scene->position.X, scene->position.Y, scene->position.Z);
		}
		if (!s_possession.hero)
			s_possession.hero = hero_id;
		//Ending, control goes back only if it is still on the possessed entity (a hero switch meanwhile moved it on)
		if (control || ref->movement_attach_entid == s_possession.control || ref->movement_attach_entid == s_possession.hero) {
			ref->movement_attach_entid = s_possession.hero;
			ref->aim_entid = s_possession.hero;
			ref->cam_attach_entid = view ? view : s_possession.hero;
			s_possession.driving = false;
		}
		LOG_CORE(Info, "[NODES] possession: control {} view {} (hero {}; previous {} {})\n", control, view, s_possession.hero,
			s_possession.control, GameEntityAdmin()->getEntById(s_possession.control) ? "exists" : "is gone");
		s_possession.control = control;
		s_possession.view = view;
		if (!control && !view)
			s_possession = {};
	}

	//
	// Clocks: a statescript component's clock (sync manager +0x2c4 frame, +0x2d0/+0x2d8 time) advances in its update
	// (sync manager vfunc 0xD13C20 -> 0xF44EF0(frame, frames)), which runs timers and timed states. The game runs it
	// for the entities it simulates; the entities the server creates follow the server instead. For the entities
	// created here it ran only on their first frame or two, so their clocks stopped right after their creation: no
	// wait ever ended (the RIP-Tire's 2 s release from Junkrat, the dragons' flight) and no timed state moved.
	// Advance each by as many frames as the local player's clock advanced since the last tick (the player's frame
	// count restarts with a new hero), one update per frame.
	//
	static constexpr __int64 RVA_SYNC_MGR_UPDATE = 0xD13C20;
	static constexpr int VTABLE_SEARCH_SLOTS = 256;
	static constexpr int MAX_FRAMES_PER_TICK = 8;

	//The local player's statescript clock: the entity (its hero; its controller's clock does not run) and its frame;
	//0 without one.
	static std::pair<uint32_t, int> local_frame() {
		auto controller = GameEntityAdmin()->getLocalEnt();
		uint32_t ids[] = { s_possession.hero, hero_of(controller) ? hero_of(controller)->entity_id : 0 };
		for (auto id : ids) {
			auto ent = id && !s_created.count(id) ? GameEntityAdmin()->getEntById(id) : nullptr;
			if (auto ss = ent ? ent->getById<Component_23_Statescript>(0x23) : nullptr)
				return { id, ss->ss_inner.increasing_counter1 };
		}
		return { 0, 0 };
	}

	static void run_clocks() {
		static int s_slot = -1; //Of the update in the sync manager's vtable, found on first use (-2: not found)
		static std::pair<uint32_t, int> s_last{};
		static std::map<uint32_t, int> s_frames{}; //Entity -> the frame its clock was last set to here
		auto now = local_frame();
		int frames = now.first && now.first == s_last.first ? std::clamp(now.second - s_last.second, 0, MAX_FRAMES_PER_TICK) : 0;
		s_last = now;
		if (!frames || s_slot == -2)
			return;
		std::vector<uint32_t> ids(s_created.begin(), s_created.end()); //Their scripts create and destroy entities
		for (auto id : ids) {
			auto ent = GameEntityAdmin()->getEntById(id);
			auto ss = ent ? ent->getById<Component_23_Statescript>(0x23) : nullptr;
			if (!ss) {
				s_frames.erase(id);
				continue;
			}
			auto& mgr = ss->ss_inner;
			//The game runs a new entity's update itself for its first frame or two (setting its clock), and sets every
			//clock back to the new frame count when a hero spawns; skip the ticks it ran, keep a clock from going back
			//(its pending timers would wait that much longer)
			int frame = mgr.increasing_counter1;
			auto it = s_frames.find(id);
			if (!frame)
				continue;
			if (it != s_frames.end()) {
				if (frame > it->second) {
					it->second = frame;
					continue;
				}
				frame = it->second;
			}
			auto vtable = *(__int64**)&mgr;
			if (s_slot == -1) {
				for (int i = 0; i < VTABLE_SEARCH_SLOTS && s_slot == -1; i++)
					if (vtable[i] == globals::gameBase + RVA_SYNC_MGR_UPDATE)
						s_slot = i;
				LOG_CORE(Info, "[NODES] statescript update: vtable slot {}\n", s_slot);
				if (s_slot == -1) {
					s_slot = -2;
					return;
				}
			}
			for (int i = 0; i < frames; i++)
				((void(__fastcall*)(StatescriptSyncMgr*, int, int))vtable[s_slot])(&mgr, ++frame, 1);
			s_frames[id] = frame;
		}
	}

	//
	// MoveTo (STUStatescriptStateMoveTo, C1035274): the server moves the entity (its character mover) to
	// m_destinationPos at m_speed and ends the state on arrival. Nothing moves the entities created here, so the state
	// never ended (Mei's Blizzard drone rises before its storm starts). Move their scene position there at that speed
	// and end the state on arrival.
	//
	static constexpr unsigned NODE_MOVE_TO = 0xC1035274;
	static constexpr __int64 RVA_END_STATE = 0xF4A9C0;
	static constexpr float MOVE_TO_ARRIVED = 0.05f;
	static constexpr double MAX_TICK_SECS = 0.1;

	static void run_move_to() {
		static double s_last = 0;
		//Destination of each activation, evaluated when it is first seen (it is relative to the entity: the drone's is
		//2 m above itself)
		static std::map<std::pair<StatescriptState*, __int64>, Vector3> s_destinations{};
		auto now = GetGameRuntimeSecs();
		float dt = s_last ? (float)std::min(now - s_last, MAX_TICK_SECS) : 0.f;
		s_last = now;
		std::map<std::pair<StatescriptState*, __int64>, Vector3> destinations;
		std::vector<StatescriptState*> arrived;
		for (auto id : s_created) {
			auto ent = GameEntityAdmin()->getEntById(id);
			auto ss = ent ? ent->getById<Component_23_Statescript>(0x23) : nullptr;
			auto scene = ent ? ent->getById<Component_1_SceneRendering>(1) : nullptr;
			if (!ss || !scene)
				continue;
			for (auto inst : ss->ss_inner) {
				if (!inst || !inst->graph)
					continue;
				for (auto& [idx, info] : statescript_logger::GetScriptState(inst)) {
					if (info.is_finish_state || idx < 0 || idx >= inst->graph->m_nodes.count())
						continue;
					auto node = inst->graph->m_nodes.list()[idx];
					if (!node || node_type(node) != NODE_MOVE_TO)
						continue;
					auto state = running_state(inst, node);
					auto editable = node->graph_node.base.to_editable();
					auto key = std::make_pair(state, info.change_timestamp);
					StatescriptPrimitive destination{}, speed{};
					if (!state)
						continue;
					if (auto it = s_destinations.find(key); it != s_destinations.end())
						destinations[key] = it->second;
					else if (eval(inst, editable, "m_destinationPos", destination))
						destinations[key] = to_vec3(destination);
					else
						continue;
					auto to = destinations[key];
					float step = (eval(inst, editable, "m_speed", speed) ? speed.get_convert_float(1.f) : 1.f) * dt;
					Vector3 delta(to.X - scene->position.X, to.Y - scene->position.Y, to.Z - scene->position.Z);
					float distance = std::sqrt(delta.X * delta.X + delta.Y * delta.Y + delta.Z * delta.Z);
					if (distance > std::max(step, MOVE_TO_ARRIVED)) {
						scene->position.X += delta.X / distance * step;
						scene->position.Y += delta.Y / distance * step;
						scene->position.Z += delta.Z / distance * step;
						set_mover_position(ent, Vector3(scene->position.X, scene->position.Y, scene->position.Z));
						continue;
					}
					scene->position.X = to.X;
					scene->position.Y = to.Y;
					scene->position.Z = to.Z;
					set_mover_position(ent, to);
					LOG_CORE(Info, "[NODES] script {:X} on {}: moved to ({:.2f}, {:.2f}, {:.2f})\n", inst->script_id, id, to.X, to.Y, to.Z);
					destinations.erase(key);
					arrived.push_back(state);
				}
			}
		}
		s_destinations = std::move(destinations);
		for (auto state : arrived) //Ending one starts the next states
			((void(__fastcall*)(StatescriptState*, unsigned short, __int64))(globals::gameBase + RVA_END_STATE))(state, 1, 0);
	}

	//Debug: PROMETHEUS_WATCH_VARS=<script>:<var>,<var>,... (hex; a var's low digits, "4.1B0" for the path 0x...4 ->
	//0x...1B0) logs those variables of that script on created entities twice a second, e.g. 580000000000132:497,4.1B0
	static void watch_vars() {
		static __int64 s_script = -1;
		static std::vector<std::vector<__int64>> s_vars{};
		static double s_last = 0;
		if (s_script == -1) {
			s_script = 0;
			std::string env = getenv("PROMETHEUS_WATCH_VARS") ? getenv("PROMETHEUS_WATCH_VARS") : "";
			auto colon = env.find(':');
			if (colon != std::string::npos) {
				s_script = std::stoll(env.substr(0, colon), nullptr, 16);
				std::string list = env.substr(colon + 1) + ",";
				for (size_t start = 0, end; (end = list.find(',', start)) != std::string::npos; start = end + 1) {
					std::vector<__int64> path;
					std::string var = list.substr(start, end - start) + ".";
					for (size_t s = 0, e; (e = var.find('.', s)) != std::string::npos; s = e + 1)
						if (e > s)
							path.push_back(0xD80000000000000 | std::stoll(var.substr(s, e - s), nullptr, 16));
					if (!path.empty())
						s_vars.push_back(path);
				}
			}
		}
		auto now = GetGameRuntimeSecs();
		if (!s_script || now - s_last < 0.5)
			return;
		s_last = now;
		auto hero = hero_of(GameEntityAdmin()->getLocalEnt());
		if (auto ss = hero ? hero->getById<Component_23_Statescript>(0x23) : nullptr)
			LOG_CORE(Info, "[VARS] hero {}: cf {} counters {} {}\n", hero->entity_id, ss->ss_inner.cf_timestamp, ss->ss_inner.increasing_counter1, ss->ss_inner.increasing_counter3);
		for (auto id : s_created) {
			auto ent = GameEntityAdmin()->getEntById(id);
			auto ss = ent ? ent->getById<Component_23_Statescript>(0x23) : nullptr;
			auto inst = ss ? ss->ss_inner.getByResourceId(s_script) : nullptr;
			if (!inst)
				continue;
			std::string values;
			for (auto& path : s_vars) {
				STUConfigVarDynamic_Custom cv(path);
				StatescriptPrimitive value{};
				bool ok = cv.cv_base.get_value(inst, &value);
				values += std::format(" {:X}={}", path.back() & 0xFFFFFFFF, ok ? value.get_value_str() : std::string("-"));
			}
			auto head = (char*)inst->updates_1;
			LOG_CORE(Info, "[VARS] script {:X} on {}:{} (cf {} counters {} {}, flags {:#x}, queue head {} at {})\n", s_script, id, values, inst->ss_inner->cf_timestamp,
				inst->ss_inner->increasing_counter1, inst->ss_inner->increasing_counter3, inst->execution_flags, (void*)head, head ? *(__int64*)(head + 0x18) : 0);
		}
	}

	static void remove_orphans() {
		for (auto it = s_created_for.begin(); it != s_created_for.end();) {
			auto ent = GameEntityAdmin()->getEntById(it->first);
			if (ent && GameEntityAdmin()->getEntById(it->second)) {
				++it;
				continue;
			}
			if (ent) {
				LOG_CORE(Info, "[NODES] entity {} removed: its hero {} is gone\n", it->first, it->second);
				s_created.erase(it->first);
				GameEntityAdmin()->delEnt(ent);
			}
			it = s_created_for.erase(it);
		}
	}

	void tick() {
		if (!GameEntityAdmin())
			return;
		remove_orphans();
		watch_vars();
		run_clocks();
		land_projectiles();
		send_pending_messages();
		run_move_to();
		send_pending_replies();
		run_pending_plays();
		update_possession();
		update_pets();
	}

	void install() {
		MH_VERIFY(MH_CreateHook((PVOID)(globals::gameBase + RVA_TRAVERSE_LINK), (LPVOID)traverse_link_hook, (PVOID*)&s_traverse_link_orig));
		MH_VERIFY(MH_EnableHook((PVOID)(globals::gameBase + RVA_TRAVERSE_LINK)));
		MH_VERIFY(MH_CreateHook((PVOID)(globals::gameBase + RVA_FIND_STATE_FACTORY), (LPVOID)find_state_factory_hook, (PVOID*)&s_find_state_factory_orig));
		MH_VERIFY(MH_EnableHook((PVOID)(globals::gameBase + RVA_FIND_STATE_FACTORY)));
		MH_VERIFY(MH_CreateHook((PVOID)(globals::gameBase + RVA_ENTITY_RUNS_NODE), (LPVOID)entity_runs_node_hook, (PVOID*)&s_entity_runs_node_orig));
		MH_VERIFY(MH_EnableHook((PVOID)(globals::gameBase + RVA_ENTITY_RUNS_NODE)));
		MH_VERIFY(MH_CreateHook((PVOID)(globals::gameBase + RVA_ENTITY_LOCALLY_CONTROLLED), (LPVOID)entity_locally_controlled_hook, (PVOID*)&s_entity_locally_controlled_orig));
		MH_VERIFY(MH_EnableHook((PVOID)(globals::gameBase + RVA_ENTITY_LOCALLY_CONTROLLED)));
	}
}
