# TDD suite for the nanobind fleece_goap_native module - a direct Python
# binding to fleece's real GOAP planner C API (include/planner/fleece_planner.h).
# This exists so tools built on top (the swarmpu-groundstation plan editor) call
# the SAME serializer the firmware deserializes with, instead of a hand-ported
# reimplementation that can drift.
import pytest

fleece_goap_native = pytest.importorskip(
    "fleece_goap_native",
    reason="fleece_goap_native not built - see tests/python/conftest.py / FLEECE_GOAP_NATIVE_DIR",
)


def make_scenario():
    """Small scan/collect scenario mirroring tests/test_planner.c's register_scenario."""
    g = fleece_goap_native.GoapPlanner()
    g.name = "Scan & Collect"

    g.add_action(
        id="deploy", name="Deploy to Zone", dest="zone", dur=2.0,
        pre=["function(bb){ return bb.self.location === 'base'; }"],
        eff=["function(bb){ bb.self.location = 'zone'; return bb; }"],
        exec="function(bb, tick){ return tick >= 2; }",
    )
    g.add_action(
        id="collectSlow", name="Collect Food Slowly", cost="function(bb){ return 3; }",
        pre=["function(bb){ return bb.self.scan >= 1; }"],
        eff=["function(bb){ bb.self.foodCount += 1; return bb; }"],
    )

    g.add_goal(id="gather", name="Gather 1 food", expr="function(bb){ return bb.self.foodCount >= 1; }",
               priority=2.0, curve_id="u2")
    g.add_goal(id="home", name="Back at base", expr="function(bb){ return bb.self.location === 'base'; }",
               priority=1.0, curve_id="u1")

    g.add_utility(id="u1", name="Constant", x_min=0, x_max=1, points=[(0, 0.9), (1, 0.9)])
    g.add_utility(id="u2", name="Hunger", dim="foodCount", x_min=0, x_max=3,
                  points=[(0, 0.9), (1, 0.9), (2, 0.7), (3, 0.05)])

    g.add_mission(id="m1", name="Gather and RTB", goal_ids=["gather", "home"],
                  note="scan, collect one food, return")
    return g


def test_create_and_name_default_empty():
    g = fleece_goap_native.GoapPlanner()
    assert g.name == ""


def test_set_and_get_name():
    g = fleece_goap_native.GoapPlanner()
    g.name = "Scan & Collect"
    assert g.name == "Scan & Collect"


def test_add_action_and_read_back():
    g = make_scenario()
    assert g.action_count == 2
    assert g.action_id(0) == "deploy"
    assert g.action_name(0) == "Deploy to Zone"
    assert g.action_pre(0) == ["function(bb){ return bb.self.location === 'base'; }"]
    assert g.action_eff(0) == ["function(bb){ bb.self.location = 'zone'; return bb; }"]
    assert g.action_dest(0) == "zone"
    assert g.action_dur(0) == 2.0
    assert g.action_exec(0) == "function(bb, tick){ return tick >= 2; }"
    assert g.action_cost_source(1) == "function(bb){ return 3; }"
    assert g.action_cost_source(0) == ""  # default cost source is empty


def test_add_action_oversized_id_raises():
    g = fleece_goap_native.GoapPlanner()
    with pytest.raises(RuntimeError):
        g.add_action(id="x" * 64, name="bad", pre=[], eff=[])
    assert g.action_count == 0


def test_add_goal_and_read_back():
    g = make_scenario()
    assert g.goal_count == 2
    assert g.goal_id(0) == "gather"
    assert g.goal_name(0) == "Gather 1 food"
    assert g.goal_expr(0) == "function(bb){ return bb.self.foodCount >= 1; }"
    assert g.goal_priority(0) == 2.0
    assert g.goal_curve_id(0) == "u2"


def test_add_utility_and_points_round_trip():
    g = make_scenario()
    assert g.utility_count == 2
    assert g.utility_id(1) == "u2"
    assert g.utility_name(1) == "Hunger"
    assert g.utility_dim(1) == "foodCount"
    assert g.utility_x_min(1) == 0.0
    assert g.utility_x_max(1) == 3.0
    assert g.utility_points(1) == [(0.0, 0.9), (1.0, 0.9), (2.0, 0.7), (3.0, 0.05)]
    assert g.utility_dim(0) == ""  # u1 is a constant curve, no dim


def test_add_mission_and_read_back():
    g = make_scenario()
    assert g.mission_count == 1
    assert g.mission_id(0) == "m1"
    assert g.mission_name(0) == "Gather and RTB"
    assert g.mission_goal_ids(0) == ["gather", "home"]
    assert g.mission_note(0) == "scan, collect one food, return"


def test_find_helpers_return_index_or_none():
    g = make_scenario()
    assert g.find_action("collectSlow") == 1
    assert g.find_action("nope") is None
    assert g.find_goal("home") == 1
    assert g.find_goal("nope") is None
    assert g.find_utility("u2") == 1
    assert g.find_utility("nope") is None


def test_out_of_range_getters_raise_index_error():
    g = make_scenario()
    with pytest.raises(IndexError):
        g.action_id(99)
    with pytest.raises(IndexError):
        g.goal_id(99)
    with pytest.raises(IndexError):
        g.utility_id(99)
    with pytest.raises(IndexError):
        g.mission_id(99)


def test_reset_clears_all_tables():
    g = make_scenario()
    g.reset()
    assert g.action_count == 0
    assert g.goal_count == 0
    assert g.utility_count == 0
    assert g.mission_count == 0
    assert g.name == ""


def test_serialize_produces_fp_magic_prefix():
    g = make_scenario()
    blob = g.serialize()
    assert isinstance(blob, bytes)
    assert blob[:2] == b"FP"
    assert len(blob) > 3


def test_deserialize_round_trip_matches_original():
    g = make_scenario()
    blob = g.serialize()

    h = fleece_goap_native.GoapPlanner()
    h.deserialize(blob)

    assert h.name == g.name
    assert h.action_count == g.action_count
    assert h.action_id(0) == g.action_id(0)
    assert h.action_name(0) == g.action_name(0)
    assert h.action_pre(0) == g.action_pre(0)
    assert h.action_exec(0) == g.action_exec(0)
    assert h.goal_count == g.goal_count
    assert h.goal_curve_id(0) == g.goal_curve_id(0)
    assert h.utility_count == g.utility_count
    assert h.utility_points(1) == g.utility_points(1)
    assert h.mission_goal_ids(0) == g.mission_goal_ids(0)


def test_reserialize_is_byte_identical():
    g = make_scenario()
    blob = g.serialize()
    h = fleece_goap_native.GoapPlanner()
    h.deserialize(blob)
    assert h.serialize() == blob


def test_deserialize_rejects_corrupt_blob():
    g = make_scenario()
    blob = bytearray(g.serialize())
    blob[2] = 99  # bad version byte
    h = fleece_goap_native.GoapPlanner()
    with pytest.raises(RuntimeError):
        h.deserialize(bytes(blob))


def test_search_budget_setters_do_not_raise():
    g = fleece_goap_native.GoapPlanner()
    g.set_max_iters(100)
    g.set_max_nodes(50)
    g.set_max_plan_depth(4)


def test_mem_usage_is_nonzero_after_adding_tables():
    g = make_scenario()
    assert g.mem_usage() > 0
