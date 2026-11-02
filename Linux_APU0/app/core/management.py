"""
巡检管理模块：年度体检计划 + 管网健康总览
==========================================
基于全部历史巡检（Inspect_* 目录）的 CJJ 181 评估结果，
为管理单位输出"接下来怎么办"：

1. 年度体检计划（build_maintenance_plan）
   - 每条管段按综合等级排修复时限与下次检测建议日期
   - 排期规则（依据本系统评估等级，非标准条文）:
       Ⅰ级  下次检测 +12 个月（纳入常规周期）
       Ⅱ级  计划修复，+6 个月复测
       Ⅲ级  30 天内安排修复，修复后复测
       Ⅳ级  立即处置，处置完成前加密监测
2. 管网健康总览（build_network_overview）
   - 多趟巡检拼成一张管网图数据：每趟 = 一段管段，
     长度按检测长度比例、按综合等级着色
"""

import os
import re
from datetime import date

import config
from core.evaluation import evaluate_history

# 等级 → (修复时限, 下次检测周期月数, 说明)
PLAN_RULES = {
    1: ("纳入常规检测周期", 12, "状况良好，按常规周期检测"),
    2: ("6 个月内计划修复", 6, "缺陷较轻，修复后持续观察"),
    3: ("30 天内安排修复", 0, "尽快修复，修复完成后复测"),
    4: ("立即处置", 0, "缺陷严重，处置完成前加密监测"),
}

_DIR_PATTERN = re.compile(r"^Inspect_(\d{4})(\d{2})(\d{2})_\d{6}$")


def _parse_dir_date(folder_name):
    """Inspect_YYYYMMDD_HHMMSS → date；解析失败返回 None。"""
    m = _DIR_PATTERN.match(folder_name or "")
    if not m:
        return None
    try:
        return date(int(m.group(1)), int(m.group(2)), int(m.group(3)))
    except ValueError:
        return None


def _add_months(d, months):
    """日期加 N 个月（月末日钳制）。"""
    if not d:
        return None
    month_index = d.month - 1 + months
    year = d.year + month_index // 12
    month = month_index % 12 + 1
    day = d.day
    # 钳制到目标月最后一天
    while True:
        try:
            return date(year, month, day)
        except ValueError:
            day -= 1
            if day < 28:
                return None


def _collect_history():
    """
    遍历 SAVE_DIR 下全部 Inspect_* 目录，按时间正序返回:
    [{dir, date, report}]（report 为评估 dict 或 None）
    """
    save_dir = config.SAVE_DIR
    if not os.path.isdir(save_dir):
        return []

    entries = []
    for name in os.listdir(save_dir):
        full = os.path.join(save_dir, name)
        if not (name.startswith("Inspect_") and os.path.isdir(full)):
            continue
        d = _parse_dir_date(name)
        if d is None:
            continue
        entries.append((name, d, full))

    entries.sort(key=lambda e: e[0])  # 目录名含时间戳，字典序即时间序

    result = []
    for name, d, full in entries:
        try:
            report = evaluate_history(full)
        except Exception:
            report = None
        result.append({"dir": name, "date": d, "report": report})
    return result


def build_maintenance_plan():
    """
    年度体检计划：每条历史巡检管段一行。

    返回: {"plan": [...], "generated_at": ISO 日期}
    行字段: section, date, length_m, rating_code, rating, ri, mi,
            defect_total, action_deadline, next_check, note
    """
    plan = []
    for entry in _collect_history():
        report = entry["report"]
        if not report:
            continue
        overall = report.get("overall") or {}
        code, rating = _clamp_rating(overall.get("rating_code", 1),
                                     overall.get("rating"))
        deadline, months, note = PLAN_RULES[code]

        row = {
            "section": entry["dir"].replace("Inspect_", ""),
            "date": entry["date"].isoformat(),
            "length_m": report.get("section_length_m", 0),
            "rating_code": code,
            "rating": rating,
            "ri": (report.get("structural") or {}).get("index"),
            "mi": (report.get("functional") or {}).get("index"),
            "defect_total": report.get("merged_count",
                                       report.get("defect_total", 0)),
            "action_deadline": deadline,
            "next_check": None,
            "note": note,
        }
        if months > 0:
            nxt = _add_months(entry["date"], months)
            row["next_check"] = nxt.isoformat() if nxt else None
        plan.append(row)

    return {
        "generated_at": date.today().isoformat(),
        "plan": plan,
        "rule_note": "排期规则：Ⅰ级 +12 个月常规周期；Ⅱ级 6 个月内计划修复；"
                     "Ⅲ级 30 天内修复后复测；Ⅳ级 立即处置（依据本系统评估等级生成）",
    }


_PLAN_RATING_NAMES = {1: "Ⅰ级", 2: "Ⅱ级", 3: "Ⅲ级", 4: "Ⅳ级"}

# 总览最多展示的段数（历史存档可能有几百段，全画出来标签挤成一团，
# 2026-10-18 用户反馈"分级混乱"的根因；只展示最近 N 段，合计数单列）
MAX_OVERVIEW_SEGMENTS = 20


def _clamp_rating(code, rating):
    """等级钳制：非法 code（0/None/超界）回退 Ⅰ级，rating 文本与 code 对齐。"""
    try:
        code = int(code)
    except (TypeError, ValueError):
        code = 1
    if code not in _PLAN_RATING_NAMES:
        code = 1
    if rating not in _PLAN_RATING_NAMES.values():
        rating = _PLAN_RATING_NAMES[code]
    return code, rating


def build_network_overview():
    """
    管网健康总览：全部历史巡检（每趟一段）汇总成一张管网图数据。

    返回: {"segments": [...最近 MAX_OVERVIEW_SEGMENTS 段...],
           "total_length_m", "section_count",
           "all_count", "all_length_m"}  # 全部存档的合计（含未展示部分）
    段字段: section, date, length_m, rating_code, rating,
            defect_total, ri, mi
    """
    segments = []
    total_length = 0.0
    for entry in _collect_history():
        report = entry["report"]
        if not report:
            continue
        overall = report.get("overall") or {}
        length_m = float(report.get("section_length_m", 0) or 0)
        if length_m <= 0:
            continue            # 空跑/无数据存档不进总览
        code, rating = _clamp_rating(overall.get("rating_code", 1),
                                     overall.get("rating"))
        total_length += length_m
        segments.append({
            "section": entry["dir"].replace("Inspect_", ""),
            "date": entry["date"].isoformat(),
            "length_m": round(length_m, 1),
            "rating_code": code,
            "rating": rating,
            "defect_total": report.get("merged_count",
                                       report.get("defect_total", 0)),
            "ri": (report.get("structural") or {}).get("index"),
            "mi": (report.get("functional") or {}).get("index"),
        })

    all_count = len(segments)
    all_length = round(total_length, 1)
    # 只展示最近 N 段（_collect_history 按时间正序，尾部=最新）
    shown = segments[-MAX_OVERVIEW_SEGMENTS:]
    return {
        "segments": shown,
        "total_length_m": round(sum(s["length_m"] for s in shown), 1),
        "section_count": len(shown),
        "all_count": all_count,
        "all_length_m": all_length,
    }
