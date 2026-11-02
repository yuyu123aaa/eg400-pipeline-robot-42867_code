"""
缺陷聚类合并（merge_defects / evaluate_defects / evaluate_history）测试
"""
import json
import os

from core.evaluation import (
    MERGE_WINDOW_M,
    adaptive_merge_window,
    compliance_checklist,
    evaluate_defects,
    evaluate_history,
    merge_defects,
)


def _crack(mileage, confidence=0.88, time="2026-10-07 10:00:00"):
    return {"type": "裂缝", "confidence": confidence, "mileage": mileage, "time": time}


# ---------------- merge_defects ----------------

def test_standstill_duplicates_merge_into_one():
    """机器人停在裂缝前 60 秒 → 7 条记录合并为 1 处。"""
    defects = [_crack(12.0 + i * 0.01) for i in range(7)]
    merged, raw, count = merge_defects(defects)

    assert raw == 7
    assert count == 1
    assert merged[0]["observations"] == 7
    assert merged[0]["mileage_start"] == 12.0
    assert merged[0]["mileage_end"] == 12.06


def test_long_crack_chain_merges():
    """5 米长裂缝边走边看（每 1.5m 检出一次）→ 1 处。"""
    defects = [_crack(m) for m in (0.0, 1.5, 3.0, 4.5)]
    merged, _, count = merge_defects(defects)

    assert count == 1
    assert merged[0]["mileage_start"] == 0.0
    assert merged[0]["mileage_end"] == 4.5
    assert merged[0]["observations"] == 4


def test_same_type_far_apart_stay_separate():
    """同类缺陷里程差超窗口 → 两处独立病害。"""
    defects = [_crack(0.0), _crack(MERGE_WINDOW_M + 1.0)]
    _, _, count = merge_defects(defects)

    assert count == 2


def test_different_types_same_mileage_stay_separate():
    """不同类型同一里程 → 各自一处。"""
    defects = [_crack(5.0), {"type": "破洞", "confidence": 0.9, "mileage": 5.0}]
    _, _, count = merge_defects(defects)

    assert count == 2


def test_merge_keeps_max_confidence():
    defects = [_crack(3.0, 0.55), _crack(3.2, 0.88), _crack(3.4, 0.71)]
    merged, _, _ = merge_defects(defects)

    assert merged[0]["confidence"] == 0.88


def test_missing_mileage_kept_standalone():
    defects = [_crack(None), _crack(1.0)]
    merged, raw, count = merge_defects(defects)

    assert raw == 2
    assert count == 2
    standalone = [m for m in merged if m["mileage_start"] is None]
    assert len(standalone) == 1
    assert standalone[0]["observations"] == 1


def test_english_label_merges_with_chinese():
    """英文标签与中文归一化为同一类型后参与同一聚类。"""
    defects = [
        {"type": "crack", "confidence": 0.8, "mileage": 2.0},
        {"type": "裂缝", "confidence": 0.8, "mileage": 2.2},
    ]
    _, _, count = merge_defects(defects)

    assert count == 1


# ---------------- 2.5m 实测管道场景（窗口随管长自适应） ----------------

def test_adaptive_window_scales_with_pipe_length():
    """窗口 = 0.2×管长，钳制 [0.3, 2.5]。"""
    assert adaptive_merge_window(2.5) == 0.5      # 实测演示管道
    assert adaptive_merge_window(10.0) == 2.0     # 长管道
    assert adaptive_merge_window(20.0) == 2.5     # 封顶
    assert adaptive_merge_window(1.0) == 0.3      # 下限
    assert adaptive_merge_window(None) == 0.3


def test_small_pipe_two_same_type_defects_stay_separate():
    """2.5m 管：两处真实裂缝相距 1.2m → 必须是两处（旧固定 2.5m 窗口会误并）。"""
    defects = [_crack(0.8), _crack(2.0)]

    report = evaluate_defects(defects, 2.5)

    assert report["merge_window_m"] == 0.5
    assert report["merged_count"] == 2
    spans = [(d["mileage_start"], d["mileage_end"]) for d in report["defects"]]
    assert (0.8, 0.8) in spans and (2.0, 2.0) in spans


def test_small_pipe_standstill_duplicates_merge():
    """2.5m 管：停在裂缝前讲解 60 秒（里程不动）→ 只算一处。"""
    defects = [_crack(1.0) for _ in range(6)]

    report = evaluate_defects(defects, 2.5)

    assert report["raw_count"] == 6
    assert report["merged_count"] == 1


def test_small_pipe_crawling_chain_merges():
    """2.5m 管：缓行经过同一缺陷每 0.3m 检出一次 → 链式归并为一处。"""
    defects = [_crack(m) for m in (0.6, 0.9, 1.2)]

    report = evaluate_defects(defects, 2.5)

    assert report["merged_count"] == 1
    d = report["defects"][0]
    assert (d["mileage_start"], d["mileage_end"]) == (0.6, 1.2)


def test_small_pipe_bend_two_defects_after_turn():
    """2.5m 管（直段+拐弯）：拐弯前后各一处同类缺陷 → 两处。"""
    defects = [_crack(0.9), _crack(2.1)]

    report = evaluate_defects(defects, 2.5)

    assert report["merged_count"] == 2


# ---------------- 记录源头空间去重（defect_recorder） ----------------

def test_recorder_standstill_does_not_duplicate(monkeypatch):
    """静止观察（里程不变）不再每 10s 刷一条。"""
    from core import defect_recorder
    import config as config_module

    defect_recorder._state["last_seen"].clear()
    monkeypatch.setattr(config_module, "total_mileage", 1.0)

    assert defect_recorder._is_duplicate("裂缝") is False   # 首见记录
    defect_recorder._state["last_seen"]["裂缝"] = (1.0, 0)
    assert defect_recorder._is_duplicate("裂缝") is True    # 停在原地 → 跳过
    assert defect_recorder._is_duplicate("裂缝") is True    # 继续停 → 仍跳过


def test_recorder_records_again_after_moving(monkeypatch):
    """前进 ≥0.5m 后同类再次检出允许落盘（长缺陷分段，由评估层归并）。"""
    from core import defect_recorder
    import config as config_module

    defect_recorder._state["last_seen"].clear()
    monkeypatch.setattr(config_module, "total_mileage", 1.0)
    defect_recorder._state["last_seen"]["裂缝"] = (1.0, 0)

    monkeypatch.setattr(config_module, "total_mileage", 1.3)
    assert defect_recorder._is_duplicate("裂缝") is True    # 0.3m 内仍视为同一处
    monkeypatch.setattr(config_module, "total_mileage", 1.5)
    assert defect_recorder._is_duplicate("裂缝") is False   # ≥0.5m 允许再记


def test_recorder_dedup_window_matches_merge_floor():
    """记录层窗口（0.5m）≥ 小管道评估窗口（0.5m），两层口径一致。"""
    from core import defect_recorder

    assert defect_recorder.DEDUP_MILEAGE_M == 0.5
    assert adaptive_merge_window(2.5) == defect_recorder.DEDUP_MILEAGE_M


# ---------------- evaluate_defects 集成 ----------------

def test_evaluation_uses_merged_count_for_density():
    """同一条裂缝 7 条 vs 1 条：合并后 RI 不跨级（Ⅱ→Ⅰ）。"""
    defects = [_crack(12.0) for _ in range(7)]  # 停留 60s 的典型重复

    merged_report = evaluate_defects(defects, 10.0)
    raw_report = evaluate_defects(defects, 10.0, merge=False)

    assert merged_report["raw_count"] == 7
    assert merged_report["merged_count"] == 1
    assert merged_report["merged"] is True
    assert raw_report["merged"] is False

    ri_merged = merged_report["structural"]["index"]
    ri_raw = raw_report["structural"]["index"]
    assert ri_merged < ri_raw

    # 重复记录会把等级从 Ⅰ 抬到 Ⅱ，合并后保持真实 Ⅰ 级
    assert merged_report["overall"]["rating_code"] == 1
    assert raw_report["overall"]["rating_code"] == 2


def test_merged_defect_fields_present():
    report = evaluate_defects([_crack(3.0), _crack(3.1)], 10.0)
    d = report["defects"][0]

    assert d["observations"] == 2
    assert d["mileage_start"] == 3.0
    assert d["mileage_end"] == 3.1


# ---------------- evaluate_history（存档升级 + CSV 兜底） ----------------

def _make_inspect_dir(tmp_path, csv_rows=None, archive=None):
    folder = tmp_path / "Inspect_20261007_100000"
    folder.mkdir()
    if csv_rows:
        lines = ["记录时间,发生里程(m),病害类型,严重等级,识别置信度,处置建议,关联图片文件,缺陷等级(1-4),画面占比"]
        lines += csv_rows
        (folder / "巡检缺陷报告.csv").write_text("\n".join(lines), encoding="utf-8-sig")
    if archive is not None:
        (folder / "评估报告.json").write_text(
            json.dumps(archive, ensure_ascii=False), encoding="utf-8")
    return str(folder)


def test_history_rebuilds_from_csv_with_merge(tmp_path):
    """无存档 → 从 CSV 现算且启用合并。"""
    rows = [
        "2026-10-07 10:00:01,12.00,裂缝,中度,0.88,建议计划检修,,3,0.1",
        "2026-10-07 10:00:11,12.02,裂缝,中度,0.88,建议计划检修,,3,0.1",
        "2026-10-07 10:00:21,12.05,裂缝,中度,0.88,建议计划检修,,3,0.1",
    ]
    folder = _make_inspect_dir(tmp_path, csv_rows=rows)

    report = evaluate_history(folder)

    assert report is not None
    assert report["merged"] is True
    assert report["raw_count"] == 3
    assert report["merged_count"] == 1
    assert report["is_live"] is False


def test_history_old_archive_without_merged_upgrades(tmp_path):
    """旧版存档（无 merged 标记）→ 自动用 CSV 重算升级为合并口径。"""
    old_archive = {"standard": "CJJ 181-2012", "defect_total": 3}
    rows = [
        "2026-10-07 10:00:01,5.00,破洞,重度,0.9,建议尽快安排检修,,4,0.3",
        "2026-10-07 10:00:11,5.02,破洞,重度,0.9,建议尽快安排检修,,4,0.3",
    ]
    folder = _make_inspect_dir(tmp_path, csv_rows=rows, archive=old_archive)

    report = evaluate_history(folder)

    assert report["merged"] is True
    assert report["raw_count"] == 2
    assert report["merged_count"] == 1


def test_history_new_archive_returned_directly(tmp_path):
    """新版存档（含 merged）→ 直读不重算。"""
    new_archive = {
        "standard": "CJJ 181-2012", "merged": True,
        "raw_count": 2, "merged_count": 1, "marker": "direct",
    }
    folder = _make_inspect_dir(tmp_path, archive=new_archive)

    report = evaluate_history(folder)

    assert report["marker"] == "direct"
    assert report["is_live"] is False


def test_history_no_data_returns_none(tmp_path):
    folder = _make_inspect_dir(tmp_path)
    assert evaluate_history(folder) is None


# ---------------- 标准符合性自查表 ----------------

def test_compliance_checklist_covers_both_standards():
    items = compliance_checklist()

    standards = {item["standard"] for item in items}
    assert "CJJ 181-2012" in standards
    assert "T/CAS 731-2023" in standards
    for item in items:
        assert item["requirement"]
        assert item["implementation"]
        assert item["status"] in ("符合", "部分符合")
