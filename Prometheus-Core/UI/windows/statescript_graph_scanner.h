#pragma once
#include "../window_manager/window_manager.h"
#include "Statescript.h"
#include "STU.h"
#include "game.h"
#include "Atlas/STU/RTTI/STURegistry.h"
#include "nlohmann/json.hpp"
#include <vector>
#include <deque>
#include <fstream>

//Scans every STUStatescriptGraph resource in an ID range (loaded or not) for nodes of a given STU type,
//or nodes that use a ConfigVar of that type. Useful to find scripts the server would normally tell the
//client to load (game modes, match UI), which Statescript Node Search can't see.
//Optionally dumps every graph's nodes to a JSON file so they can be searched offline.
class statescript_graph_scanner : public window {
	WINDOW_DEFINE(statescript_graph_scanner, "Statescript", "Graph Scanner", true);

	static constexpr __int64 GRAPH_RESOURCE_BASE = 0x0580000000000000;
	static constexpr int MAX_PENDING = 64;
	static constexpr int LOAD_TIMEOUT_FRAMES = 300;
	static constexpr int MAX_CONFIG_VARS = 16;
	static constexpr int MAX_LINKS = 32;
	static constexpr const char* DUMP_PATH = "./Prometheus/graph_dump.json";

	struct Pending {
		__int64 resource_id;
		MisalignedResourceLoadEntry* entry;
		int frames_waited;
	};

	struct Hit {
		__int64 resource_id;
		int node_count;
		std::vector<int> node_indexes;
	};

	struct ConfigVarInfo {
		STUConfigVar* cv;
		uint field_hash;
		uint type_hash;
		bool has_value; //Only constant (non-dynamic, non-expression) config vars are evaluated
		StatescriptPrimitive_Type value_type;
		__int64 value;
	};

	struct LinkInfo {
		uint plug_hash;
		int target_index;
	};

	struct NodeInfo {
		int index;
		uint type_hash;
		bool is_state;
		bool client_only;
		bool server_only;
		int config_var_count;
		ConfigVarInfo config_vars[MAX_CONFIG_VARS];
		int link_count;
		LinkInfo links[MAX_LINKS];

		bool matches(uint hash) const {
			if (type_hash == hash)
				return true;
			for (int i = 0; i < config_var_count; i++) {
				if (config_vars[i].type_hash == hash)
					return true;
			}
			return false;
		}
	};

	static void read_node(STUStatescriptBase* node, NodeInfo* out) {
		auto typ = node->graph_node.base.vfptr->GetSTUInfo();
		out->index = node->idx_in_nodes;
		out->type_hash = typ->Hash;
		out->is_state = node->m_isState;
		out->client_only = node->m_clientOnly;
		out->server_only = node->m_serverOnly;
		out->config_var_count = 0;
		out->link_count = 0;
		auto reg = STURegistry::Get();
		for (auto arg_info : typ->RangeArgs()) {
			if (arg_info.first->Hash == STU_NAME::STUStatescriptState ||
				arg_info.first->Hash == STU_NAME::STUStatescriptAction)
				break;
			auto arg = arg_info.second;
			if (arg->Constraint->ToConstraintType() != STU_ConstraintType_Object)
				continue;
			auto arg_typ = reg->GetSTUInfoByHash(arg->Constraint->GetSTUType());
			if (arg_typ && arg_typ->AssignableToHash(STU_NAME::STUConfigVar)) {
				STUConfigVar* cv = *(STUConfigVar**)((__int64)node + arg->Offset);
				if (cv && out->config_var_count < MAX_CONFIG_VARS) {
					auto& info = out->config_vars[out->config_var_count++];
					info.cv = cv;
					info.field_hash = arg->Hash;
					info.type_hash = cv->base.vfptr->GetSTUInfo()->Hash;
					info.has_value = false;
				}
			}
		}
	}

	static bool read_node_safe(STUStatescriptBase* node, NodeInfo* out) {
		__try {
			read_node(node, out);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	//Constant config vars only. Evaluated without an instance, so anything that needs one may fail.
	static bool read_value_safe(ConfigVarInfo* info) {
		__try {
			if (info->cv->is_dynamic() || info->cv->is_expression())
				return true;
			StatescriptPrimitive value{};
			if (info->cv->get_value(nullptr, &value)) {
				info->has_value = true;
				info->value_type = value.type;
				info->value = value.value;
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			info->has_value = false;
			return false;
		}
	}

	static void read_links(STUStatescriptBase* node, NodeInfo* out) {
		for (auto& plug : node->get_output_plugs()) {
			for (auto item : plug.second) {
				for (auto link : item->m_links) {
					if (out->link_count < MAX_LINKS && link && link->m_inputPlug && link->m_inputPlug->plug_base.m_parentNode)
						out->links[out->link_count++] = { (uint)plug.first, link->m_inputPlug->plug_base.m_parentNode->idx_in_nodes };
				}
			}
		}
	}

	static bool read_links_safe(STUStatescriptBase* node, NodeInfo* out) {
		__try {
			read_links(node, out);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			out->link_count = 0;
			return false;
		}
	}

	//Returns false if the graph could not be read. Value and link failures only drop that detail.
	inline bool read_graph(STUStatescriptGraph* graph, std::vector<NodeInfo>& out) {
		if (IsBadReadPtr(graph, sizeof(STUStatescriptGraph)))
			return false;
		out.resize(graph->m_nodes.count());
		int i = 0;
		for (auto node : *graph) {
			auto& info = out[i++];
			if (!read_node_safe(node, &info))
				return false;
			for (int c = 0; c < info.config_var_count; c++) {
				if (!read_value_safe(&info.config_vars[c]))
					_value_errors++;
			}
			if (!read_links_safe(node, &info))
				_link_errors++;
		}
		return true;
	}

	static bool is_loaded(MisalignedResourceLoadEntry* entry) {
		if (!entry || !entry->valid())
			return false;
		auto aligned = entry->align();
		if (IsBadReadPtr(aligned, sizeof(MisalignedResourceLoadEntry)))
			return false;
		return aligned->resource_load_entry == 0 && aligned->resource_ptr != 0;
	}

	inline void add_to_dump(__int64 resource_id, const std::vector<NodeInfo>& nodes) {
		auto& graph_json = _dump[std::format("{:016X}", resource_id)];
		graph_json = nlohmann::json::array();
		for (auto& node : nodes) {
			nlohmann::json cvs = nlohmann::json::array();
			for (int i = 0; i < node.config_var_count; i++) {
				auto& cv = node.config_vars[i];
				nlohmann::json cv_json = {
					{ "field", std::format("{:08X}", cv.field_hash) },
					{ "type", std::format("{:08X}", cv.type_hash) }
				};
				if (cv.has_value) {
					cv_json["vtype"] = (int)cv.value_type;
					if (cv.value_type == StatescriptPrimitive_FLT)
						cv_json["value"] = *(float*)&cv.value;
					else
						cv_json["value"] = std::format("{:X}", cv.value);
				}
				cvs.push_back(cv_json);
			}
			nlohmann::json links = nlohmann::json::array();
			for (int i = 0; i < node.link_count; i++)
				links.push_back({ std::format("{:08X}", node.links[i].plug_hash), node.links[i].target_index });
			graph_json.push_back({
				{ "i", node.index },
				{ "type", std::format("{:08X}", node.type_hash) },
				{ "state", node.is_state },
				{ "client_only", node.client_only },
				{ "server_only", node.server_only },
				{ "config_vars", cvs },
				{ "links", links }
			});
		}
	}

	inline void write_dump() {
		std::ofstream file(DUMP_PATH);
		if (!file.is_open()) {
			_dump_status = std::format("Failed to open {}", DUMP_PATH);
			return;
		}
		file << _dump.dump();
		_dump_status = std::format("Wrote {} graphs to {}", _dump.size(), DUMP_PATH);
	}

	inline void tick_scan() {
		while (_running && _pending.size() < MAX_PENDING && _next_index <= _range_end) {
			__int64 id = GRAPH_RESOURCE_BASE | _next_index++;
			auto entry = try_load_resource_2(id);
			if (entry && entry->valid())
				_pending.push_back({ id, entry, 0 });
			else
				_missing++;
		}

		std::vector<NodeInfo> nodes{};
		for (auto it = _pending.begin(); it != _pending.end();) {
			if (is_loaded(it->entry)) {
				auto graph = (STUStatescriptGraph*)it->entry->align()->resource_ptr;
				if (!read_graph(graph, nodes))
					_errors++;
				else {
					_scanned++;
					Hit hit{ it->resource_id, (int)nodes.size(), {} };
					for (auto& node : nodes) {
						if (node.matches(_type_hash))
							hit.node_indexes.push_back(node.index);
					}
					if (!hit.node_indexes.empty())
						_hits.push_back(std::move(hit));
					if (_dump_enabled)
						add_to_dump(it->resource_id, nodes);
				}
				it = _pending.erase(it);
			}
			else if (++it->frames_waited > LOAD_TIMEOUT_FRAMES) {
				_missing++;
				it = _pending.erase(it);
			}
			else {
				it++;
			}
		}

		if (_running && _pending.empty() && _next_index > _range_end) {
			_running = false;
			if (_dump_enabled)
				write_dump();
		}
	}

	inline void render() override {
		if (open_window()) {
			ImGui::BeginDisabled(_running);
			ImGui::InputScalar("Type hash", ImGuiDataType_U32, &_type_hash, nullptr, nullptr, "%08X", ImGuiInputTextFlags_CharsHexadecimal);
			ImGui::InputScalar("First index", ImGuiDataType_S64, &_range_start, nullptr, nullptr, "%llX", ImGuiInputTextFlags_CharsHexadecimal);
			ImGui::InputScalar("Last index", ImGuiDataType_S64, &_range_end, nullptr, nullptr, "%llX", ImGuiInputTextFlags_CharsHexadecimal);
			ImGui::Checkbox("Dump every graph to Prometheus/graph_dump.json", &_dump_enabled);
			ImGui::EndDisabled();
			ImGui::TextDisabled("Graph IDs %p - %p", (void*)(GRAPH_RESOURCE_BASE | _range_start), (void*)(GRAPH_RESOURCE_BASE | _range_end));

			if (!_running) {
				if (ImGui::Button("Scan")) {
					_hits.clear();
					_pending.clear();
					_dump = nlohmann::json::object();
					_dump_status.clear();
					_next_index = _range_start;
					_scanned = _missing = _errors = _value_errors = _link_errors = 0;
					_running = true;
				}
			}
			else if (ImGui::Button("Stop")) {
				_running = false;
				_pending.clear();
			}
			ImGui::SameLine();
			imgui_helpers::display_type(_type_hash, true, false, true);

			tick_scan();

			__int64 total = _range_end - _range_start + 1;
			__int64 done = _next_index - _range_start - (__int64)_pending.size();
			ImGui::ProgressBar(total > 0 ? (float)done / (float)total : 0.f);
			ImGui::Text("Scanned: %d  Not found / timed out: %d  Read errors: %d  Pending: %d  Hits: %d",
				_scanned, _missing, _errors, (int)_pending.size(), (int)_hits.size());
			ImGui::Text("Skipped values: %d  Skipped node links: %d", _value_errors, _link_errors);
			if (!_dump_status.empty())
				ImGui::TextUnformatted(_dump_status.c_str());

			if (imgui_helpers::beginTable("graph_scanner_hits", { "Graph", "Nodes", "Matching node indexes" })) {
				for (auto& hit : _hits) {
					ImGui::PushID((void*)hit.resource_id);
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					if (imgui_helpers::TooltipButton(EMOJI_COPY, "Copy graph ID"))
						imgui_helpers::openCopyWindow(hit.resource_id);
					ImGui::SameLine();
					ImGui::Text("%p", (void*)hit.resource_id);
					ImGui::TableNextColumn();
					ImGui::Text("%d", hit.node_count);
					ImGui::TableNextColumn();
					std::string indexes{};
					for (int idx : hit.node_indexes)
						indexes += std::format("{} ", idx);
					ImGui::TextUnformatted(indexes.c_str());
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		}
		ImGui::End();
	}

	//inline void preStartInitialize() override {}
	//inline void initialize() override {}
private:
	uint _type_hash = 0x7AD0304A; //STUStatescriptStateUXMatchStatus
	__int64 _range_start = 0;
	__int64 _range_end = 0xBFF; //All graphs found in the 0.8 beta are below 0xB70
	__int64 _next_index = 0;
	bool _running = false;
	bool _dump_enabled = true;
	int _scanned = 0;
	int _missing = 0;
	int _errors = 0;
	int _value_errors = 0;
	int _link_errors = 0;
	std::deque<Pending> _pending{};
	std::vector<Hit> _hits{};
	nlohmann::json _dump = nlohmann::json::object();
	std::string _dump_status{};
};

WINDOW_REGISTER(statescript_graph_scanner);
