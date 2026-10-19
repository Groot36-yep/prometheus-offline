#include "ultimate.h"
#include "entity_admin.h"
#include "game.h"
#include "Statescript.h"
#include "StatescriptVar.h"
#include "Logs/Logs.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>

namespace ultimate {
	static constexpr __int64 ULTIMATE_CHARGE_VAR = 0x0D80000000000156;
	static constexpr __int64 ULTIMATE_COST_VAR = 0x0D80000000000152;
	static constexpr double CHARGE_SECS = 30.0;
	static constexpr double UPDATE_SECS = 0.25;
	static constexpr const char* SETTINGS_PATH = "./Prometheus/ultimate_settings.json";

	static Mode& setting() {
		static Mode s_mode = Mode::OverTime;
		static bool s_loaded = false;
		if (!s_loaded) {
			s_loaded = true;
			std::ifstream file(SETTINGS_PATH);
			auto j = nlohmann::json::parse(file, nullptr, false);
			if (j.is_object())
				s_mode = (Mode)std::clamp(j.value("mode", (int)s_mode), 0, (int)Mode::Off);
		}
		return s_mode;
	}

	Mode mode() {
		return setting();
	}

	void set_mode(Mode mode) {
		setting() = mode;
		std::ofstream(SETTINGS_PATH, std::ios::trunc) << nlohmann::json{ { "mode", (int)mode } }.dump(2);
	}

	static Component_23_Statescript* local_hero_statescript() {
		auto controller = GameEntityAdmin()->getLocalEnt();
		auto ref = controller ? controller->getById<Component_20_ModelReference>(0x20) : nullptr;
		auto model = ref ? GameEntityAdmin()->getEntById(ref->cam_attach_entid) : nullptr;
		return model ? model->getById<Component_23_Statescript>(0x23) : nullptr;
	}

	//An integer entity variable's value (StatescriptVar_Primitive, value at +0xa0).
	static bool read_int(StatescriptVar_VarBag* bag, __int64 id, int& value) {
		for (auto& bucket : bag->sub_items) {
			for (int i = 0; i < bucket.item_count && bucket.arr_ptr; i++) {
				if (bucket.arr_ptr[i].rid_id == id && bucket.arr_ptr[i].ptr) {
					value = (int)((StatescriptVar_Primitive*)bucket.arr_ptr[i].ptr)->global_var.value;
					return true;
				}
			}
		}
		return false;
	}

	void tick() {
		static double s_last_update = 0;
		static Component_23_Statescript* s_hero = nullptr;
		static double s_charge = 0;
		static int s_written = -1;

		if (setting() == Mode::Off || !GameEntityAdmin())
			return;
		auto now = GetGameRuntimeSecs();
		if (now - s_last_update < UPDATE_SECS)
			return;
		double elapsed = std::min(now - s_last_update, 1.0);
		s_last_update = now;

		auto ss = local_hero_statescript();
		auto bag = ss ? ss->ss_inner.rid_entity_varbag : nullptr;
		int charge = 0, cost = 0;
		if (!bag || !read_int(bag, ULTIMATE_CHARGE_VAR, charge) || !read_int(bag, ULTIMATE_COST_VAR, cost) || cost <= 0)
			return;
		//A new hero, or the value changed underneath (the ultimate spent the charge): continue from its value.
		if (ss != s_hero || charge != s_written) {
			s_hero = ss;
			s_charge = charge;
		}
		int target = cost;
		if (setting() == Mode::OverTime) {
			s_charge = std::min<double>(cost, s_charge + cost / CHARGE_SECS * elapsed);
			target = (int)s_charge;
		}
		if (target != charge)
			bag->SetVar({ StatescriptVar_ENTITY_VARBAG, ULTIMATE_CHARGE_VAR }, StatescriptPrimitive(target, StatescriptPrimitive_INT));
		s_written = target;
	}
}
