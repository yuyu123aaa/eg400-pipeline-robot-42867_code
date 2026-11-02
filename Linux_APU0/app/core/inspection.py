"""
巡检业务：缺陷记录管理、历史数据读取
=====================================
"""

import os
import csv
from datetime import datetime

import config


def begin_inspection():
    from core.defect_recorder import reset_state
    reset_state()
    """
    开始一次新巡检：创建专属目录、清空上次的日志/里程/轨迹。

    返回:
        本次巡检目录的路径
    """
    folder_name = f"Inspect_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    inspect_dir = os.path.join(config.SAVE_DIR, folder_name)
    os.makedirs(inspect_dir, exist_ok=True)

    config.defect_logs.clear()
    config.total_mileage = 0.0
    config.trajectory_data.clear()

    config.current_inspect_dir = inspect_dir
    print(f"🚀 开始巡检！专属日志目录: {inspect_dir}")
    return inspect_dir


def end_inspection():
    """
    结束当前巡检，生成轨迹地图 + CJJ 181-2012 评估报告。

    返回:
        地图文件路径，或 None（数据不足时）
    """
    map_filepath = None
    if config.current_inspect_dir and len(config.trajectory_data) > 0:
        from core.mapping import generate_gis_map

        map_filename = f"Trajectory_Map_{datetime.now().strftime('%H%M%S')}.png"
        map_filepath = os.path.join(config.current_inspect_dir, map_filename)
        generate_gis_map(map_filepath)

    # CJJ 181-2012 管段评估报告（有缺陷数据时生成）
    if config.current_inspect_dir:
        from core.evaluation import save_evaluation_report
        report_path = save_evaluation_report(
            config.current_inspect_dir,
            config.defect_logs,
            config.total_mileage,
        )
        if report_path:
            print(f"📊 CJJ 181-2012 评估报告已生成: {report_path}")

    return map_filepath


def get_defect_logs(limit=20):
    """获取最近 N 条缺陷记录（最新在前）"""
    return list(reversed(config.defect_logs))[:limit]


def get_history_list():
    """获取所有历史巡检目录名称列表（按时间倒序）"""
    history = []
    if os.path.exists(config.SAVE_DIR):
        for f in os.listdir(config.SAVE_DIR):
            full_path = os.path.join(config.SAVE_DIR, f)
            if f.startswith("Inspect_") and os.path.isdir(full_path):
                history.append(f)
    history.sort(reverse=True)
    return history


def get_history_detail(folder_name):
    """
    获取某次巡检的详细数据。

    参数:
        folder_name: 巡检目录名

    返回:
        dict: {"csv_data": [...], "images": [...]} 或 {"status": "error", ...}
    """
    folder_path = os.path.join(config.SAVE_DIR, folder_name)
    if not os.path.exists(folder_path) or not os.path.isdir(folder_path):
        return {"status": "error", "message": "目录不存在"}

    # 读取 CSV
    csv_data = []
    csv_path = os.path.join(folder_path, "巡检缺陷报告.csv")
    if os.path.isfile(csv_path):
        with open(csv_path, mode='r', encoding='utf-8-sig') as f:
            reader = csv.DictReader(f)
            for row in reader:
                csv_data.append(row)

    # 收集图片：缺陷快照（defects/）+ 轨迹图（顶层 PNG）。
    # 走廊关键帧（corridor/，原始无标注帧）不进抓拍库。
    images = []
    for root, dirs, files in os.walk(folder_path):
        dirs[:] = [d for d in dirs if d != "corridor"]
        for f in files:
            if f.lower().endswith(('.jpg', '.jpeg', '.png')):
                rel = os.path.relpath(os.path.join(root, f),
                                      folder_path).replace(os.sep, '/')
                images.append(f"/images/{folder_name}/{rel}")

    return {
        "status": "success",
        "csv_data": csv_data,
        "images": sorted(images),
    }
