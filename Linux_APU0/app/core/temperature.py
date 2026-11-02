"""
USB 温度传感器读取模块
======================
后台轮询 LES11-MC-Dx Modbus RTU 温度传感器，并更新 config.system_status。
"""

import threading
import time

import config

try:
    import serial
except ImportError:
    serial = None


TEMP_READ_CMD = bytes([0x01, 0x03, 0x00, 0x00, 0x00, 0x01, 0x84, 0x0A])


def parse_temperature(resp):
    """解析返回的 7 字节数据，返回 (temperature, error)。"""
    if len(resp) < 7:
        return None, f"响应长度不足: 收到 {len(resp)} 字节，期望 7 字节"

    if resp[0] != 0x01 or resp[1] != 0x03 or resp[2] != 0x02:
        return None, f"响应格式异常: {resp.hex(' ').upper()}"

    raw = (resp[3] << 8) | resp[4]
    if raw == 0x7FFF:
        return None, "传感器未接入或已损坏"

    if raw >= 0x8000:
        raw -= 0x10000

    return raw / 10.0, None


def _set_temperature_error(message):
    config.system_status["temperature_online"] = False
    config.system_status["temperature_error"] = message


def _temperature_loop():
    if serial is None:
        _set_temperature_error("未安装 pyserial")
        print("🌡️ 温度传感器未启动: 未安装 pyserial")
        return

    while True:
        ser = None
        try:
            ser = serial.Serial(
                port=config.TEMP_SENSOR_PORT,
                baudrate=config.TEMP_SENSOR_BAUDRATE,
                bytesize=8,
                parity="N",
                stopbits=1,
                timeout=2,
            )
            print(f"🌡️ 温度传感器已连接 {config.TEMP_SENSOR_PORT}")

            while True:
                if not config.system_status["is_running"]:
                    config.system_status["temperature"] = None
                    config.system_status["temperature_online"] = False
                    time.sleep(config.TEMP_SENSOR_INTERVAL_S)
                    continue

                ser.reset_input_buffer()
                ser.write(TEMP_READ_CMD)
                time.sleep(0.3)
                resp = ser.read(7)

                temperature, err = parse_temperature(resp)
                if err:
                    _set_temperature_error(err)
                else:
                    config.system_status["temperature"] = temperature
                    config.system_status["temperature_online"] = True
                    config.system_status["temperature_error"] = ""

                time.sleep(config.TEMP_SENSOR_INTERVAL_S)

        except Exception as exc:
            _set_temperature_error(str(exc))
            print(f"🌡️ 温度传感器读取失败: {exc}")
            time.sleep(3)
        finally:
            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass


def start_temperature_thread():
    """启动温度传感器轮询线程（守护线程）。"""
    t = threading.Thread(target=_temperature_loop, daemon=True)
    t.start()
    return t
