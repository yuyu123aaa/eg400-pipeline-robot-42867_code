"""
轨迹推算与 GIS 地图生成模块
============================
- 后台线程：根据系统运行状态累加里程、记录轨迹采样点
- 巡检结束后生成 matplotlib 轨迹地图
"""

import math
import time
import threading

import config


def _mapping_loop():
    """轨迹推算后台线程主循环"""
    last_time = time.time()

    while True:
        current_time = time.time()
        dt = current_time - last_time
        last_time = current_time

        if config.system_status["is_moving"]:
            # 无编码器纯时间推算：钳上限防原地空转时里程虚涨（2026-10-18）
            cap = getattr(config, "TRAJECTORY_MAX_M", 1.8)
            config.total_mileage = min(
                cap, config.total_mileage + config.TRAJECTORY_SPEED_M_S * dt)
            config.trajectory_data.append({
                "time": current_time,
                "yaw": config.system_status["yaw"],
                "sl": config.system_status["sonar_left"],
                "sr": config.system_status["sonar_right"]
            })
        time.sleep(0.1)


def start_mapping_thread():
    """启动轨迹推算后台线程（守护线程）"""
    t = threading.Thread(target=_mapping_loop, daemon=True)
    t.start()
    return t


def generate_gis_map(filepath):
    """
    根据 trajectory_data 生成巡检轨迹报告图（PNG）

    参数:
        filepath: 图片保存路径
    返回:
        bool: 是否成功生成
    """
    if len(config.trajectory_data) < 2:
        print("⚠️ 巡检数据太少，不生成图片报告。")
        return False

    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt

        plt.rcParams['font.sans-serif'] = ['SimHei', 'Microsoft YaHei',
                                           'WenQuanYi Micro Hei', 'sans-serif']
        plt.rcParams['axes.unicode_minus'] = False
        plt.figure(figsize=(10, 6))

        V = config.GIS_SPEED_MM_S
        rx, ry = [0], [0]
        lwx, lwy = [], []
        rwx, rwy = [], []

        yaw_0 = config.trajectory_data[0]['yaw']
        curr_x, curr_y = 0.0, 0.0

        for i in range(1, len(config.trajectory_data)):
            dt = config.trajectory_data[i]['time'] - config.trajectory_data[i - 1]['time']
            yaw_rad = math.radians(config.trajectory_data[i]['yaw'] - yaw_0)

            curr_x += V * dt * math.cos(yaw_rad)
            curr_y += V * dt * math.sin(yaw_rad)
            rx.append(curr_x)
            ry.append(curr_y)

            sl = config.trajectory_data[i]['sl']
            if 0 < sl < 2000:
                lwx.append(curr_x + sl * math.cos(yaw_rad + math.pi / 2))
                lwy.append(curr_y + sl * math.sin(yaw_rad + math.pi / 2))
            else:
                lwx.append(None)
                lwy.append(None)

            sr = config.trajectory_data[i]['sr']
            if 0 < sr < 2000:
                rwx.append(curr_x + sr * math.cos(yaw_rad - math.pi / 2))
                rwy.append(curr_y + sr * math.sin(yaw_rad - math.pi / 2))
            else:
                rwx.append(None)
                rwy.append(None)

        plt.plot(rx, ry, '-', color='#1a73e8', linewidth=2.5, label='机器人轨迹')
        plt.plot(lwx, lwy, '--', color='#34a853', linewidth=1.5, label='左管壁')
        plt.plot(rwx, rwy, '-.', color='#34a853', linewidth=1.5, label='右管壁')

        plt.title("管道巡检轨迹报告 (后台生成)")
        plt.xlabel("X 向空间位移 (mm)")
        plt.ylabel("Y 向空间位移 (mm)")
        plt.legend(loc='best')
        plt.grid(True, linestyle=':', alpha=0.6)
        plt.axis('equal')

        plt.savefig(filepath, dpi=150, bbox_inches='tight')
        plt.close()
        print(f"✅ 巡检结束，报告已成功静默保存至: {filepath}")
        return True

    except Exception as e:
        print(f"❌ 后台生成地图失败: {e}")
        return False
