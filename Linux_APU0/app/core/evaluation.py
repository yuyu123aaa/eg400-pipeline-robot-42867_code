"""
CJJ 181-2012 管段结构状况评估引擎
==================================
参照《城镇排水管道检测与评估技术规程》CJJ 181-2012 第 9 章实现：

1. 单缺陷定级（1~4 级）：按缺陷类别基准等级，结合置信度、
   检测框尺寸占比做修正（高置信/大占比上调，封顶 4 级）。
2. 当量缺陷密度 F = Σ(缺陷当量数) / 管段长度 (处/m)
3. 修复指数 RI = 0.7×F + 0.3×S_max   （结构性缺陷）
   养护指数 MI = 0.7×F + 0.3×Y_max   （功能性缺陷）
4. 管段评级（Ⅰ~Ⅳ级）与处置建议：
   Ⅰ级 ≤1 结构状况总体良好，无需修复
   Ⅱ级 (1,3] 缺陷较轻，计划修复
   Ⅲ级 (3,6] 缺陷中等，尽快安排修复
   Ⅳ级 >6  缺陷严重，立即处置

结构性缺陷（管壁脱落/裂缝/破洞/树根侵入）影响结构安全；
功能性缺陷（泥沙淤积）影响过流能力，按养护指数评估。
"""

import json
import os

# ---------------- 缺陷分类与基准等级 ----------------
# 值为该类缺陷的基准等级（CJJ 181-2012 附录分级表的项目对应）
STRUCTURAL_BASE_GRADE = {
    "管壁脱落": 3,   # 脱落/剥落：轻→重 2~4 级，取基准 3
    "裂缝": 2,       # 裂纹 1 级 / 裂缝 2 级 / 严重破裂 3~4 级，取基准 2
    "破洞": 4,       # 破洞（塌陷前兆）最严重，基准 4
    "树根侵入": 3,   # 异物侵入（树根）2~3 级，取基准 3
}
FUNCTIONAL_BASE_GRADE = {
    "泥沙淤积": 2,   # 沉积：轻度 1 / 中度 2 / 重度 3，取基准 2
}

# 各等级缺陷的当量数（用于缺陷密度计算）
GRADE_EQUIVALENT = {1: 0.25, 2: 0.5, 3: 1.0, 4: 2.0}

# 评级区间: (上限, 级别码, 级别名, 结论, 建议)
_RATING_BANDS = [
    (1.0, 1, "Ⅰ级", "结构状况总体良好", "无需修复，纳入下一检测周期"),
    (3.0, 2, "Ⅱ级", "缺陷较轻", "计划修复，持续观察发展趋势"),
    (6.0, 3, "Ⅲ级", "缺陷中等", "尽快安排修复，防止缺陷扩展"),
    (float("inf"), 4, "Ⅳ级", "缺陷严重", "立即处置并安排开挖复核"),
]
_FUNCTIONAL_BANDS = [
    (1.0, 1, "Ⅰ级", "过流状况总体良好", "无需养护，纳入下一检测周期"),
    (3.0, 2, "Ⅱ级", "轻度淤积", "计划养护，安排清淤疏浚"),
    (6.0, 3, "Ⅲ级", "中等淤积", "尽快安排清淤，恢复过流能力"),
    (float("inf"), 4, "Ⅳ级", "严重淤积", "立即清淤，检查淤积来源"),
]

SEVERITY_TEXT = {1: "轻微", 2: "中度", 3: "严重", 4: "紧急"}
ADVICE_TEXT = {
    1: "建议持续观察",
    2: "建议计划检修",
    3: "建议尽快安排检修",
    4: "建议立即处置并复测",
}

# ---------------- 缺陷聚类合并 ----------------
# 同类缺陷里程差 ≤ 窗口值时视为同一处物理病害：
# 机器人停留观察时同一缺陷被反复检出（每 10s 一条）、长裂缝边走
# 边看重新检出，均属同一处病害，不应按多条参与密度计算。
# 窗口随管段长度自适应（实测演示管道仅 2~3m，固定大窗口会把
# 两处真实缺陷误并成一处）:
#   窗口 = 0.2 × 检测长度，下限 0.3m，上限 2.5m
#   → 2.5m 管段 → 0.5m；10m 管段 → 2.0m；≥12.5m 封顶 2.5m
MERGE_WINDOW_M = 2.5          # 窗口上限
MIN_MERGE_WINDOW_M = 0.3      # 窗口下限（静止抖动 + 定位误差）
MERGE_WINDOW_RATIO = 0.2      # 窗口 = 管长 × 此比例


def adaptive_merge_window(length_m):
    """按管段检测长度自适应聚类窗口（米）。"""
    try:
        length_m = float(length_m or 0)
    except (TypeError, ValueError):
        length_m = 0.0
    return max(MIN_MERGE_WINDOW_M,
               min(MERGE_WINDOW_M, MERGE_WINDOW_RATIO * length_m))


def merge_defects(defects, window_m=MERGE_WINDOW_M):
    """
    同类缺陷按"类型 + 里程"聚类合并。

    规则:
    - 归一化类型相同、里程差 ≤ window_m（与当前簇的末端里程链式比较）→ 同一处
    - mileage 无法解析的记录原样保留、不参与合并
    - 合并保留: 起止里程 / 观测次数 / 最高置信度 / 最大画面占比 / 首次记录时间
      （评估等级按"最高置信 + 最大占比"重新计算，天然取最严重观测）

    返回: (merged_list, raw_count, merged_count)
    """
    raw = list(defects or [])
    groups = {}      # canonical type -> [(mileage, record)]
    standalone = []  # mileage 缺失的记录
    type_order = []  # 保持类型首次出现顺序

    for d in raw:
        canonical = normalize_type(d.get("type", ""))
        try:
            mileage = float(d.get("mileage"))
        except (TypeError, ValueError):
            standalone.append({
                "type": d.get("type", ""),
                "mileage": d.get("mileage"),
                "mileage_start": None,
                "mileage_end": None,
                "observations": 1,
                "confidence": d.get("confidence"),
                "size_ratio": d.get("size_ratio"),
                "time": d.get("time"),
            })
            continue
        if canonical not in groups:
            groups[canonical] = []
            type_order.append(canonical)
        groups[canonical].append((mileage, d))

    merged = []
    for canonical in type_order:
        items = sorted(groups[canonical], key=lambda x: x[0])
        cluster = None
        for mileage, d in items:
            conf = _safe_float(d.get("confidence"))
            ratio = _safe_float(d.get("size_ratio"))
            if cluster is not None and mileage - cluster["mileage_end"] <= window_m:
                cluster["mileage_end"] = mileage
                cluster["observations"] += 1
                cluster["confidence"] = max(cluster["confidence"], conf)
                cluster["size_ratio"] = max(cluster["size_ratio"], ratio)
            else:
                if cluster is not None:
                    merged.append(_finish_cluster(cluster, canonical))
                cluster = {
                    "mileage_start": mileage,
                    "mileage_end": mileage,
                    "observations": 1,
                    "confidence": conf,
                    "size_ratio": ratio,
                    "time": d.get("time"),
                }
        if cluster is not None:
            merged.append(_finish_cluster(cluster, canonical))

    merged.extend(standalone)
    return merged, len(raw), len(merged)


def _safe_float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def _finish_cluster(cluster, canonical):
    return {
        "type": canonical,
        "mileage": cluster["mileage_start"],
        "mileage_start": cluster["mileage_start"],
        "mileage_end": cluster["mileage_end"],
        "observations": cluster["observations"],
        "confidence": cluster["confidence"],
        "size_ratio": cluster["size_ratio"],
        "time": cluster["time"],
    }


def normalize_type(raw):
    """英文标签/别名 → 规范中文名。"""
    return config_alias_lookup(raw) if raw else raw


def config_alias_lookup(raw):
    import config
    key = str(raw).strip().lower().replace("-", "_").replace(" ", "_")
    return config.DEFECT_TYPE_ALIASES.get(key, raw)


def grade_defect(defect_type, confidence=0.0, size_ratio=0.0):
    """
    单缺陷定级（1~4 级）。

    参数:
        defect_type: 中文缺陷名（或英文标签，自动归一化）
        confidence:  检测置信度 0~1
        size_ratio:  检测框面积 / 模型输入面积（0~1），缺省 0

    返回:
        int: 1~4 级
    """
    canonical = normalize_type(defect_type)
    if canonical in STRUCTURAL_BASE_GRADE:
        grade = STRUCTURAL_BASE_GRADE[canonical]
    elif canonical in FUNCTIONAL_BASE_GRADE:
        grade = FUNCTIONAL_BASE_GRADE[canonical]
    else:
        grade = 2  # 未知类别按 2 级保守处理

    # 高置信度（模型非常确定）上调一级
    if confidence >= 0.7:
        grade += 1
    # 大尺寸占比（画面中占比 ≥25%，说明贴近镜头/缺陷大）上调一级
    if size_ratio >= 0.25:
        grade += 1

    return max(1, min(4, grade))


def _rate(value, bands):
    for upper, code, name, conclusion, advice in bands:
        if value <= upper:
            return {
                "rating_code": code,
                "rating": name,
                "conclusion": conclusion,
                "advice": advice,
            }
    upper, code, name, conclusion, advice = bands[-1]
    return {
        "rating_code": code,
        "rating": name,
        "conclusion": conclusion,
        "advice": advice,
    }


def evaluate_defects(defects, length_m, merge=True):
    """
    对一次巡检的缺陷列表做 CJJ 181-2012 管段评估。

    参数:
        defects: 缺陷记录列表，每条至少含
                 {type, confidence, mileage, time}（兼容英文 type）
        length_m: 管段检测长度（米），用巡检总里程
        merge:   是否先按"类型 + 里程"聚类合并（默认开；
                 传 False 可退回旧的逐条评估行为）

    返回:
        dict: 完整评估结果（可直接 jsonify / 存 JSON）
    """
    length_m = float(length_m or 0)
    length = max(length_m, 0.1)  # 防除零：最短按 0.1m 计

    window_m = adaptive_merge_window(length_m)
    if merge:
        merged, raw_count, merged_count = merge_defects(defects, window_m)
    else:
        merged = list(defects or [])
        raw_count = merged_count = len(merged)

    graded = []
    for d in merged:
        canonical = normalize_type(d.get("type", ""))
        grade = grade_defect(
            canonical,
            float(d.get("confidence", 0) or 0),
            float(d.get("size_ratio", 0) or 0),
        )
        graded.append({
            "type": canonical,
            "grade": grade,
            "severity": SEVERITY_TEXT[grade],
            "equivalent": GRADE_EQUIVALENT[grade],
            "confidence": d.get("confidence"),
            "size_ratio": round(float(d.get("size_ratio", 0) or 0), 4),
            "mileage": d.get("mileage"),
            "mileage_start": d.get("mileage_start"),
            "mileage_end": d.get("mileage_end"),
            "observations": int(d.get("observations", 1) or 1),
            "time": d.get("time"),
            "is_structural": canonical in STRUCTURAL_BASE_GRADE,
            "advice": ADVICE_TEXT[grade],
        })

    # 按发生里程排序（无里程的排最后），报告台账更可读
    graded.sort(key=lambda g: (g["mileage_start"] is None,
                               g["mileage_start"] if g["mileage_start"] is not None else 0))

    structurals = [g for g in graded if g["is_structural"]]
    functionals = [g for g in graded if not g["is_structural"]]

    def _group(items):
        if not items:
            return {
                "count": 0, "density_F": 0.0, "max_grade": 0,
                "index": 0.0, "index_name": "",
                "rating_code": 1, "rating": "Ⅰ级",
                "conclusion": "未检出缺陷", "advice": "无需处置，纳入下一检测周期",
                "equivalent_total": 0.0,
            }
        eq_total = sum(it["equivalent"] for it in items)
        f_density = round(eq_total / length, 3)
        max_grade = max(it["grade"] for it in items)
        return {"count": len(items), "equivalent_total": round(eq_total, 2),
                "density_F": f_density, "max_grade": max_grade}

    s = _group(structurals)
    if structurals:
        s["index"] = round(0.7 * s["density_F"] + 0.3 * s["max_grade"], 2)
        s["index_name"] = "修复指数 RI"
        s.update(_rate(s["index"], _RATING_BANDS))

    y = _group(functionals)
    if functionals:
        y["index"] = round(0.7 * y["density_F"] + 0.3 * y["max_grade"], 2)
        y["index_name"] = "养护指数 MI"
        y.update(_rate(y["index"], _FUNCTIONAL_BANDS))

    # 管段综合等级 = 结构/功能二者中更严重者
    overall_code = max(s.get("rating_code", 1), y.get("rating_code", 1))
    overall = {
        "rating_code": overall_code,
        "rating": ["Ⅰ级", "Ⅱ级", "Ⅲ级", "Ⅳ级"][overall_code - 1],
        "summary": s.get("conclusion", "") + ("；" + y.get("conclusion", "") if functionals else ""),
        "advice": s.get("advice", ""),
    }

    return {
        "standard": "CJJ 181-2012《城镇排水管道检测与评估技术规程》",
        "section_length_m": round(length_m, 2),
        "defect_total": len(graded),
        "merged": bool(merge),
        "raw_count": raw_count,
        "merged_count": merged_count,
        "merge_window_m": round(window_m, 2) if merge else None,
        "structural": s,
        "functional": y,
        "overall": overall,
        "defects": graded,
    }


def save_evaluation_report(inspect_dir, defects, length_m):
    """评估结果写入巡检目录（评估报告.json），返回文件路径。"""
    report = evaluate_defects(defects, length_m)
    path = os.path.join(inspect_dir, "评估报告.json")
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)
        return path
    except Exception as e:
        print(f"⚠️ 评估报告保存失败: {e}")
        return None


def load_evaluation_report(inspect_dir):
    """读取巡检目录下已保存的评估报告；无则返回 None。"""
    path = os.path.join(inspect_dir, "评估报告.json")
    if os.path.isfile(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            return None
    return None


def defects_from_csv(inspect_dir):
    """
    从巡检目录的 巡检缺陷报告.csv 重建缺陷列表（历史兜底数据源）。

    返回: (defects, length_m)；CSV 不存在时返回 ([], 0.0)
    """
    import csv as csv_module

    csv_path = os.path.join(inspect_dir, "巡检缺陷报告.csv")
    if not os.path.isfile(csv_path):
        return [], 0.0

    defects = []
    length_m = 0.0
    with open(csv_path, mode="r", encoding="utf-8-sig") as f:
        for row in csv_module.DictReader(f):
            conf = _safe_float(row.get("识别置信度"))
            try:
                mileage = float(row.get("发生里程(m)") or 0)
            except (TypeError, ValueError):
                mileage = 0.0
            defects.append({
                "type": row.get("病害类型", ""),
                "confidence": conf,
                "mileage": mileage,
                "time": row.get("记录时间", ""),
                "size_ratio": _safe_float(row.get("画面占比")),
            })
            length_m = max(length_m, mileage)
    return defects, length_m


def evaluate_history(inspect_dir):
    """
    历史巡检评估（评估报告页 / 管理模块共用的唯一入口）：
    1. 优先读已存档的 评估报告.json（新版含 merged 标记，直接可用）；
    2. 旧版存档（无 merged 标记）或无存档 → 从 CSV 重建并按
       聚类合并现算（旧报告自动升级为合并口径，数据口径统一）；
    3. CSV 也没有 → 返回 None。

    返回: dict 评估结果（含 is_live=False），或 None
    """
    report = load_evaluation_report(inspect_dir)
    if report and report.get("merged"):
        report["is_live"] = False
        return report

    defects, length_m = defects_from_csv(inspect_dir)
    if not defects:
        # 无 CSV 可重建：旧存档原样返回（总比没有好），否则 None
        if report:
            report["is_live"] = False
            return report
        return None

    report = evaluate_defects(defects, length_m)
    report["is_live"] = False
    return report


# ---------------- 标准符合性自查表 ----------------
def compliance_checklist():
    """
    生成 CJJ 181-2012 / T/CAS 731-2023 符合性自查表（静态对照，
    内容与本系统实际实现逐项对应；"部分符合"为诚实标注的近似项）。
    """
    return [
        {
            "standard": "CJJ 181-2012",
            "requirement": "管道缺陷分类与分级（附录分级表）",
            "implementation": "五类缺陷（裂缝/管壁脱落/破洞/树根侵入/泥沙淤积）"
                              "按类别基准等级定级，置信度与画面占比修正",
            "status": "部分符合",
            "note": "检测数据无绝对尺寸维度，等级修正采用置信度/占比近似方案",
        },
        {
            "standard": "CJJ 181-2012",
            "requirement": "缺陷按处计数（同一处病害不重复计数）",
            "implementation": "同类缺陷按里程聚类合并（窗口随管长自适应，"
                              "0.3~2.5m），停留重复检出与长缺陷分段检出归并为一处",
            "status": "符合",
            "note": "",
        },
        {
            "standard": "CJJ 181-2012",
            "requirement": "当量缺陷密度 F = Σ当量数 / 检测长度",
            "implementation": "1~4 级当量数 0.25/0.5/1.0/2.0，"
                              "除以本次巡检总里程",
            "status": "符合",
            "note": "",
        },
        {
            "standard": "CJJ 181-2012",
            "requirement": "修复指数 RI / 养护指数 MI（第 9 章）",
            "implementation": "RI = 0.7×F(结构) + 0.3×最严重等级；"
                              "MI 同式取功能性缺陷",
            "status": "符合",
            "note": "",
        },
        {
            "standard": "CJJ 181-2012",
            "requirement": "管段等级评定（Ⅰ~Ⅳ级）",
            "implementation": "按 RI/MI 分区（≤1 / ≤3 / ≤6 / >6）评级，"
                              "综合等级取结构/功能更严重者",
            "status": "符合",
            "note": "",
        },
        {
            "standard": "CJJ 181-2012",
            "requirement": "评估报告成果（缺陷明细 + 处置建议）",
            "implementation": "评估报告页实时计算 + 结束巡检自动存档 JSON，"
                              "支持打印/导出 PDF 与历史回溯",
            "status": "符合",
            "note": "",
        },
        {
            "standard": "T/CAS 731-2023",
            "requirement": "智能判读流程：影像数据 → 缺陷自动判定",
            "implementation": "NPU 端 YOLO 推理（约 23fps）实时判读，"
                              "输出缺陷存在性/位置/类型",
            "status": "符合",
            "note": "",
        },
        {
            "standard": "T/CAS 731-2023",
            "requirement": "缺陷等级智能判定输出",
            "implementation": "识别结果按 CJJ 181 分级表自动定级（1~4 级），"
                              "随记录实时输出",
            "status": "部分符合",
            "note": "等级由类别基准 + 检测特征修正，未含人工复核环节",
        },
        {
            "standard": "T/CAS 731-2023",
            "requirement": "成果输出与归档（缺陷图片 + 记录）",
            "implementation": "每处缺陷自动抓拍快照 + 巡检缺陷报告 CSV + "
                              "独立巡检目录归档",
            "status": "符合",
            "note": "",
        },
        {
            "standard": "T/CAS 731-2023",
            "requirement": "缺陷样本数据库 / 模型训练",
            "implementation": "YOLO 模型基于标注样本集训练，"
                              "五类典型缺陷覆盖",
            "status": "部分符合",
            "note": "样本规模以赛场检测类别为准，未建在线增量样本库",
        },
    ]
