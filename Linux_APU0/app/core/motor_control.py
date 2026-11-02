"""网页与手柄共用的推进器启停操作。"""

import config
from core.comm import send_str_to_linux
from core.inspection import begin_inspection, end_inspection


def start_motor():
    """执行网页“向前”的完整操作。"""
    begin_inspection()
    config.system_status["motor_command_state"] = "pending"
    config.system_status["motor_control_message"] = "等待裸机启动确认"
    config.system_status["nav_fault"] = ""
    config.system_status["is_moving"] = False
    send_str_to_linux("CMD:MOTOR_START")


def stop_motor():
    """执行网页“停止”的完整操作。"""
    send_str_to_linux("CMD:MOTOR_STOP")
    config.system_status["motor_command_state"] = "stopped"
    config.system_status["motor_control_message"] = "停止命令已发送"
    config.system_status["is_moving"] = False
    end_inspection()
    config.current_inspect_dir = ""
