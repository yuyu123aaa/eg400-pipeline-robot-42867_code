"""只启动 Flask 网页层的冒烟入口，不启动任何后台硬件服务。"""

import config
from main import app


if __name__ == "__main__":
    print(
        "Web-only 冒烟模式：不启动 YOLO、摄像头、RPMsg、手柄、温度和电机服务。",
        flush=True,
    )
    app.run(
        host=config.WEB_HOST,
        port=config.WEB_PORT,
        threaded=True,
    )
