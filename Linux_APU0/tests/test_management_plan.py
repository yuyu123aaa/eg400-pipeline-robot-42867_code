"""
巡检管理模块（年度体检计划 + 管网健康总览）测试
"""
import json

import pytest

import config
from core.management import (
    _add_months,
    _parse_dir_date,
    build_maintenance_plan,
    build_network_overview,
)
from core.evaluation import evaluate_defects


# ---------------- 日期工具 ----------------

def test_parse_dir_date():
    assert _parse_dir_date("Inspect_20261007_103000").isoformat() == "2026-10-07"
    assert _parse_dir_date("Inspect_2026107_103000") is None
    assert _parse_dir_date("not_a_dir") is None


def test_add_months_month_end_clamped():
    from datetime import date
    assert _add_months(date(2026, 1, 31), 1) == date(2026, 2, 28)
    assert _add_months(date(2026, 10, 7), 12) == date(2027, 10, 7)
    assert _add_months(date(2026, 12, 15), 6) == date(2027, 6, 15)


# ---------------- 计划 / 总览（临时 SAVE_DIR） ----------------

@pytest.fixture
def fake_history(tmp_path, monkeypatch):
    """构造四趟历史巡检：Ⅰ/Ⅱ 用真实评估引擎生成，Ⅲ/Ⅳ 用伪造存档
    （评估公式下单管段很难到 Ⅲ/Ⅳ，直接给存档以验证排期规则映射）。"""
    monkeypatch.setattr(config, "SAVE_DIR", str(tmp_path))

    def make_real(name, defects, length):
        folder = tmp_path / name
        folder.mkdir()
        report = evaluate_defects(defects, length)
        (folder / "评估报告.json").write_text(
            json.dumps(report, ensure_ascii=False), encoding="utf-8")
        return folder

    def make_fake(name, rating_code, length, defects, raw=None):
        names = {1: "Ⅰ级", 2: "Ⅱ级", 3: "Ⅲ级", 4: "Ⅳ级"}
        report = {
            "standard": "CJJ 181-2012", "merged": True,
            "section_length_m": length,
            "merged_count": defects, "raw_count": raw if raw else defects,
            "structural": {"index": round(0.7 * defects * 2.0 / length + 0.3 * 4, 2)},
            "functional": {"index": None},
            "overall": {"rating_code": rating_code, "rating": names[rating_code]},
        }
        folder = tmp_path / name
        folder.mkdir()
        (folder / "评估报告.json").write_text(
            json.dumps(report, ensure_ascii=False), encoding="utf-8")
        return folder

    # Ⅰ级：1 处轻微淤积（真实评估）
    make_real("Inspect_20260901_090000",
              [{"type": "泥沙淤积", "confidence": 0.3, "mileage": 2.0}], 8.0)
    # Ⅱ级：2 处裂缝（真实评估，1.0/1.2 合并 + 5.0 独立）
    make_real("Inspect_20260915_140000",
              [{"type": "裂缝", "confidence": 0.75, "mileage": 1.0},
               {"type": "裂缝", "confidence": 0.75, "mileage": 1.2},
               {"type": "裂缝", "confidence": 0.75, "mileage": 5.0}], 9.0)
    # Ⅲ级 / Ⅳ级：伪造存档（merged=True 时 evaluate_history 直读）
    make_fake("Inspect_20261001_090000", 3, 6.0, defects=2, raw=5)
    make_fake("Inspect_20261007_103000", 4, 7.0, defects=1, raw=3)
    return tmp_path


def test_maintenance_plan_rules(fake_history):
    plan = build_maintenance_plan()["plan"]

    assert len(plan) == 4
    by_date = {row["date"]: row for row in plan}

    good = by_date["2026-09-01"]
    assert good["rating_code"] == 1
    assert good["action_deadline"] == "纳入常规检测周期"
    assert good["next_check"] == "2027-09-01"      # Ⅰ级 +12 个月

    mild = by_date["2026-09-15"]
    assert mild["rating_code"] == 2
    assert "6 个月内计划修复" in mild["action_deadline"]
    assert mild["next_check"] == "2027-03-15"       # Ⅱ级 +6 个月

    mid = by_date["2026-10-01"]
    assert mid["rating_code"] == 3
    assert mid["action_deadline"] == "30 天内安排修复"
    assert mid["next_check"] is None                # Ⅲ级修复后复测

    severe = by_date["2026-10-07"]
    assert severe["rating_code"] == 4
    assert severe["action_deadline"] == "立即处置"
    assert severe["next_check"] is None             # Ⅳ级处置前加密监测


def test_maintenance_plan_uses_merged_defect_count(fake_history):
    """Ⅳ级段原始检出 3 次、合并后 1 处 → 计划行缺陷数按合并后 1 处计。"""
    plan = build_maintenance_plan()["plan"]
    severe = [row for row in plan if row["rating_code"] == 4][0]

    assert severe["defect_total"] == 1


def test_network_overview_segments(fake_history):
    data = build_network_overview()

    assert data["section_count"] == 4
    assert data["total_length_m"] == 30.0
    codes = {seg["rating_code"] for seg in data["segments"]}
    assert codes == {1, 2, 3, 4}
    # 每段带 RI/MI 与日期，前端着色与标注所需字段齐全
    for seg in data["segments"]:
        assert seg["date"] and seg["rating"] and seg["length_m"] is not None
        assert seg["ri"] is not None


def test_empty_save_dir(tmp_path, monkeypatch):
    monkeypatch.setattr(config, "SAVE_DIR", str(tmp_path))

    assert build_maintenance_plan()["plan"] == []
    assert build_network_overview()["segments"] == []
