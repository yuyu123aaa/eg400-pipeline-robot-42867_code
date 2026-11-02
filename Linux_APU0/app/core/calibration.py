"""IMU 航向重新标定的统一入口。"""

import config
from core.comm import send_str_to_linux


def recalibrate_heading():
    """通知裸机捕获当前航向，并通知网页同步执行软件归零。"""
    config.system_status["heading_calibration_seq"] += 1
    send_str_to_linux("CMD:CALIBRATE")
    return config.system_status["heading_calibration_seq"]
