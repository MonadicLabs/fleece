// nanobind binding for fleece's real GOAP planner C API
// (include/planner/fleece_planner.h). Exposes table authoring
// (add_action/add_goal/add_utility/add_mission), introspection, and
// serialize()/deserialize() - the exact CBOR plan-blob format the firmware
// deserializes with fleece_goap_deserialize(). This is deliberately a thin,
// mostly mechanical wrapper: one GoapPlanner method per C function, so there
// is only ever one implementation of the plan-blob format to keep correct.
#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include "fleece_alloc.h"
#include "planner/fleece_planner.h"
}

namespace nb = nanobind;
using namespace nb::literals;

class GoapPlanner {
public:
    GoapPlanner() : g_(fleece_goap_create()) {
        if (!g_) throw std::bad_alloc();
    }
    ~GoapPlanner() {
        if (g_) fleece_goap_destroy(g_);
    }
    GoapPlanner(const GoapPlanner&) = delete;
    GoapPlanner& operator=(const GoapPlanner&) = delete;

    std::string get_name() const { return str_or_empty(fleece_goap_name(g_)); }
    void set_name(const std::string& n) {
        if (fleece_goap_set_name(g_, n.c_str()) != 0)
            throw std::runtime_error("set_name failed (name too long?)");
    }

    void reset() { fleece_goap_reset(g_); }

    void add_action(const std::string& id, const std::string& name,
                     const std::string& cost, const std::string& dest, double dur,
                     const std::string& exec, const std::vector<std::string>& pre,
                     const std::vector<std::string>& eff) {
        std::vector<const char*> pre_c, eff_c;
        pre_c.reserve(pre.size());
        eff_c.reserve(eff.size());
        for (auto& s : pre) pre_c.push_back(s.c_str());
        for (auto& s : eff) eff_c.push_back(s.c_str());

        FleeceGoapActionDef def{};
        def.id = id.c_str();
        def.name = name.c_str();
        def.cost = cost.c_str();
        def.dest = dest.c_str();
        def.dur = dur;
        def.exec = exec.empty() ? nullptr : exec.c_str();
        def.pre = pre_c.empty() ? nullptr : pre_c.data();
        def.pre_count = (uint32_t)pre_c.size();
        def.eff = eff_c.empty() ? nullptr : eff_c.data();
        def.eff_count = (uint32_t)eff_c.size();
        if (fleece_goap_add_action(g_, &def) != 0)
            throw std::runtime_error("add_action failed for id '" + id + "'");
    }

    void add_goal(const std::string& id, const std::string& name, const std::string& expr,
                  double priority, const std::string& curve_id) {
        FleeceGoapGoalDef def{};
        def.id = id.c_str();
        def.name = name.c_str();
        def.expr = expr.c_str();
        def.priority = priority;
        def.curve_id = curve_id.c_str();
        if (fleece_goap_add_goal(g_, &def) != 0)
            throw std::runtime_error("add_goal failed for id '" + id + "'");
    }

    void add_utility(const std::string& id, const std::string& name, const std::string& dim,
                      double x_min, double x_max,
                      const std::vector<std::pair<double, double>>& points) {
        std::vector<FleeceGoapPoint> pts;
        pts.reserve(points.size());
        for (auto& p : points) pts.push_back(FleeceGoapPoint{p.first, p.second});

        FleeceGoapUtilityDef def{};
        def.id = id.c_str();
        def.name = name.c_str();
        def.dim = dim.c_str();
        def.x_min = x_min;
        def.x_max = x_max;
        def.points = pts.empty() ? nullptr : pts.data();
        def.point_count = (uint32_t)pts.size();
        if (fleece_goap_add_utility(g_, &def) != 0)
            throw std::runtime_error("add_utility failed for id '" + id + "'");
    }

    void add_mission(const std::string& id, const std::string& name,
                      const std::vector<std::string>& goal_ids, const std::string& note) {
        std::vector<const char*> gid_c;
        gid_c.reserve(goal_ids.size());
        for (auto& s : goal_ids) gid_c.push_back(s.c_str());

        FleeceGoapMissionDef def{};
        def.id = id.c_str();
        def.name = name.c_str();
        def.goal_ids = gid_c.empty() ? nullptr : gid_c.data();
        def.goal_count = (uint32_t)gid_c.size();
        def.note = note.c_str();
        if (fleece_goap_add_mission(g_, &def) != 0)
            throw std::runtime_error("add_mission failed for id '" + id + "'");
    }

    void set_max_iters(uint32_t v) { fleece_goap_set_max_iters(g_, v); }
    void set_max_nodes(uint32_t v) { fleece_goap_set_max_nodes(g_, v); }
    void set_max_plan_depth(uint32_t v) { fleece_goap_set_max_plan_depth(g_, v); }
    uint32_t mem_usage() const { return fleece_goap_get_mem_usage(g_); }

    uint32_t action_count() const { return fleece_goap_action_count(g_); }
    uint32_t goal_count() const { return fleece_goap_goal_count(g_); }
    uint32_t utility_count() const { return fleece_goap_utility_count(g_); }
    uint32_t mission_count() const { return fleece_goap_mission_count(g_); }

    std::string action_id(uint32_t idx) const {
        check(idx, action_count(), "action");
        return str_or_empty(fleece_goap_action_id(g_, idx));
    }
    std::string action_name(uint32_t idx) const {
        check(idx, action_count(), "action");
        return str_or_empty(fleece_goap_action_name(g_, idx));
    }
    std::vector<std::string> action_pre(uint32_t idx) const {
        check(idx, action_count(), "action");
        uint32_t n = fleece_goap_action_pre_count(g_, idx);
        std::vector<std::string> out;
        out.reserve(n);
        for (uint32_t i = 0; i < n; i++) out.push_back(str_or_empty(fleece_goap_action_pre(g_, idx, i)));
        return out;
    }
    std::vector<std::string> action_eff(uint32_t idx) const {
        check(idx, action_count(), "action");
        uint32_t n = fleece_goap_action_eff_count(g_, idx);
        std::vector<std::string> out;
        out.reserve(n);
        for (uint32_t i = 0; i < n; i++) out.push_back(str_or_empty(fleece_goap_action_eff(g_, idx, i)));
        return out;
    }
    std::string action_cost_source(uint32_t idx) const {
        check(idx, action_count(), "action");
        return str_or_empty(fleece_goap_action_cost_source(g_, idx));
    }
    std::string action_exec(uint32_t idx) const {
        check(idx, action_count(), "action");
        return str_or_empty(fleece_goap_action_exec(g_, idx));
    }
    std::string action_dest(uint32_t idx) const {
        check(idx, action_count(), "action");
        return str_or_empty(fleece_goap_action_dest(g_, idx));
    }
    double action_dur(uint32_t idx) const {
        check(idx, action_count(), "action");
        return fleece_goap_action_dur(g_, idx);
    }

    std::string goal_id(uint32_t idx) const {
        check(idx, goal_count(), "goal");
        return str_or_empty(fleece_goap_goal_id(g_, idx));
    }
    std::string goal_name(uint32_t idx) const {
        check(idx, goal_count(), "goal");
        return str_or_empty(fleece_goap_goal_name(g_, idx));
    }
    std::string goal_expr(uint32_t idx) const {
        check(idx, goal_count(), "goal");
        return str_or_empty(fleece_goap_goal_expr(g_, idx));
    }
    double goal_priority(uint32_t idx) const {
        check(idx, goal_count(), "goal");
        return fleece_goap_goal_priority(g_, idx);
    }
    std::string goal_curve_id(uint32_t idx) const {
        check(idx, goal_count(), "goal");
        return str_or_empty(fleece_goap_goal_curve_id(g_, idx));
    }

    std::string utility_id(uint32_t idx) const {
        check(idx, utility_count(), "utility");
        return str_or_empty(fleece_goap_utility_id(g_, idx));
    }
    std::string utility_name(uint32_t idx) const {
        check(idx, utility_count(), "utility");
        return str_or_empty(fleece_goap_utility_name(g_, idx));
    }
    std::string utility_dim(uint32_t idx) const {
        check(idx, utility_count(), "utility");
        return str_or_empty(fleece_goap_utility_dim(g_, idx));
    }
    double utility_x_min(uint32_t idx) const {
        check(idx, utility_count(), "utility");
        return fleece_goap_utility_x_min(g_, idx);
    }
    double utility_x_max(uint32_t idx) const {
        check(idx, utility_count(), "utility");
        return fleece_goap_utility_x_max(g_, idx);
    }
    std::vector<std::pair<double, double>> utility_points(uint32_t idx) const {
        check(idx, utility_count(), "utility");
        uint32_t n = fleece_goap_utility_point_count(g_, idx);
        std::vector<std::pair<double, double>> out;
        out.reserve(n);
        for (uint32_t i = 0; i < n; i++) {
            FleeceGoapPoint p = fleece_goap_utility_point(g_, idx, i);
            out.emplace_back(p.x, p.y);
        }
        return out;
    }

    std::string mission_id(uint32_t idx) const {
        check(idx, mission_count(), "mission");
        return str_or_empty(fleece_goap_mission_id(g_, idx));
    }
    std::string mission_name(uint32_t idx) const {
        check(idx, mission_count(), "mission");
        return str_or_empty(fleece_goap_mission_name(g_, idx));
    }
    std::vector<std::string> mission_goal_ids(uint32_t idx) const {
        check(idx, mission_count(), "mission");
        uint32_t n = fleece_goap_mission_goal_count(g_, idx);
        std::vector<std::string> out;
        out.reserve(n);
        for (uint32_t i = 0; i < n; i++) out.push_back(str_or_empty(fleece_goap_mission_goal_id(g_, idx, i)));
        return out;
    }
    std::string mission_note(uint32_t idx) const {
        check(idx, mission_count(), "mission");
        return str_or_empty(fleece_goap_mission_note(g_, idx));
    }

    std::optional<uint32_t> find_action(const std::string& id) const {
        return idx_or_none(fleece_goap_find_action(g_, id.c_str()));
    }
    std::optional<uint32_t> find_goal(const std::string& id) const {
        return idx_or_none(fleece_goap_find_goal(g_, id.c_str()));
    }
    std::optional<uint32_t> find_utility(const std::string& id) const {
        return idx_or_none(fleece_goap_find_utility(g_, id.c_str()));
    }

    nb::bytes serialize() const {
        uint8_t* blob = nullptr;
        uint32_t size = 0;
        if (fleece_goap_serialize(g_, &blob, &size) != 0 || !blob)
            throw std::runtime_error("fleece_goap_serialize failed");
        nb::bytes result(reinterpret_cast<const char*>(blob), size);
        fleece_free_fn(blob);
        return result;
    }

    void deserialize(nb::bytes blob) {
        if (fleece_goap_deserialize(g_, reinterpret_cast<const uint8_t*>(blob.c_str()),
                                     (uint32_t)blob.size()) != 0) {
            throw std::runtime_error("fleece_goap_deserialize failed (corrupt blob or version mismatch)");
        }
    }

private:
    FleeceGoap* g_;

    static std::string str_or_empty(const char* s) { return s ? s : ""; }
    static void check(uint32_t idx, uint32_t count, const char* what) {
        if (idx >= count) throw nb::index_error((std::string(what) + " index out of range").c_str());
    }
    static std::optional<uint32_t> idx_or_none(uint32_t idx) {
        return idx == UINT32_MAX ? std::nullopt : std::optional<uint32_t>(idx);
    }
};

NB_MODULE(fleece_goap_native, m) {
    m.doc() = "Python binding to fleece's real GOAP planner C API (planner/fleece_planner.h)";

    nb::class_<GoapPlanner>(m, "GoapPlanner")
        .def(nb::init<>())
        .def_prop_rw("name", &GoapPlanner::get_name, &GoapPlanner::set_name)
        .def("reset", &GoapPlanner::reset)
        .def("add_action", &GoapPlanner::add_action, "id"_a, "name"_a, "cost"_a = "", "dest"_a = "",
             "dur"_a = 0.0, "exec"_a = "", "pre"_a = std::vector<std::string>{},
             "eff"_a = std::vector<std::string>{})
        .def("add_goal", &GoapPlanner::add_goal, "id"_a, "name"_a, "expr"_a, "priority"_a = 0.0,
             "curve_id"_a = "")
        .def("add_utility", &GoapPlanner::add_utility, "id"_a, "name"_a, "dim"_a = "", "x_min"_a = 0.0,
             "x_max"_a = 0.0, "points"_a = std::vector<std::pair<double, double>>{})
        .def("add_mission", &GoapPlanner::add_mission, "id"_a, "name"_a,
             "goal_ids"_a = std::vector<std::string>{}, "note"_a = "")
        .def("set_max_iters", &GoapPlanner::set_max_iters, "max_iters"_a)
        .def("set_max_nodes", &GoapPlanner::set_max_nodes, "max_nodes"_a)
        .def("set_max_plan_depth", &GoapPlanner::set_max_plan_depth, "max_depth"_a)
        .def("mem_usage", &GoapPlanner::mem_usage)
        .def_prop_ro("action_count", &GoapPlanner::action_count)
        .def_prop_ro("goal_count", &GoapPlanner::goal_count)
        .def_prop_ro("utility_count", &GoapPlanner::utility_count)
        .def_prop_ro("mission_count", &GoapPlanner::mission_count)
        .def("action_id", &GoapPlanner::action_id, "idx"_a)
        .def("action_name", &GoapPlanner::action_name, "idx"_a)
        .def("action_pre", &GoapPlanner::action_pre, "idx"_a)
        .def("action_eff", &GoapPlanner::action_eff, "idx"_a)
        .def("action_cost_source", &GoapPlanner::action_cost_source, "idx"_a)
        .def("action_exec", &GoapPlanner::action_exec, "idx"_a)
        .def("action_dest", &GoapPlanner::action_dest, "idx"_a)
        .def("action_dur", &GoapPlanner::action_dur, "idx"_a)
        .def("goal_id", &GoapPlanner::goal_id, "idx"_a)
        .def("goal_name", &GoapPlanner::goal_name, "idx"_a)
        .def("goal_expr", &GoapPlanner::goal_expr, "idx"_a)
        .def("goal_priority", &GoapPlanner::goal_priority, "idx"_a)
        .def("goal_curve_id", &GoapPlanner::goal_curve_id, "idx"_a)
        .def("utility_id", &GoapPlanner::utility_id, "idx"_a)
        .def("utility_name", &GoapPlanner::utility_name, "idx"_a)
        .def("utility_dim", &GoapPlanner::utility_dim, "idx"_a)
        .def("utility_x_min", &GoapPlanner::utility_x_min, "idx"_a)
        .def("utility_x_max", &GoapPlanner::utility_x_max, "idx"_a)
        .def("utility_points", &GoapPlanner::utility_points, "idx"_a)
        .def("mission_id", &GoapPlanner::mission_id, "idx"_a)
        .def("mission_name", &GoapPlanner::mission_name, "idx"_a)
        .def("mission_goal_ids", &GoapPlanner::mission_goal_ids, "idx"_a)
        .def("mission_note", &GoapPlanner::mission_note, "idx"_a)
        .def("find_action", &GoapPlanner::find_action, "id"_a)
        .def("find_goal", &GoapPlanner::find_goal, "id"_a)
        .def("find_utility", &GoapPlanner::find_utility, "id"_a)
        .def("serialize", &GoapPlanner::serialize)
        .def("deserialize", &GoapPlanner::deserialize, "blob"_a);
}
