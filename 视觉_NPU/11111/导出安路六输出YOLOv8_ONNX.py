#!/usr/bin/env python3
"""把Ultralytics YOLOv8检测头导出为安路官方示例使用的六路原始输出。

输出顺序固定为：stride8 box/class、stride16 box/class、stride32 box/class。
box通道数为64（4方向×16个DFL桶），class通道数保持训练模型的5类。
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
import torch
from ultralytics import YOLO
from ultralytics.nn.modules.block import C2f


CLASS_NAMES = ["spall", "crack", "hole", "root", "sediment"]
OUTPUT_NAMES = [
    "box_stride8",
    "class_stride8",
    "box_stride16",
    "class_stride16",
    "box_stride32",
    "class_stride32",
]


class SixHeadYoloV8(torch.nn.Module):
    """复用训练权重，只把Detect层前的三层特征改为六个原始头输出。"""

    def __init__(self, detection_model: torch.nn.Module) -> None:
        super().__init__()
        self.layers = detection_model.model
        self.save = set(detection_model.save)

    def forward(self, x: torch.Tensor) -> tuple[torch.Tensor, ...]:
        saved: list[torch.Tensor | None] = []
        for layer in self.layers[:-1]:
            if layer.f != -1:
                if isinstance(layer.f, int):
                    x = saved[layer.f]
                else:
                    x = [x if index == -1 else saved[index] for index in layer.f]
            x = layer(x)
            saved.append(x if layer.i in self.save else None)

        detect = self.layers[-1]
        features = [saved[index] for index in detect.f]
        outputs: list[torch.Tensor] = []
        for index, feature in enumerate(features):
            outputs.append(detect.cv2[index](feature))
            outputs.append(detect.cv3[index](feature))
        return tuple(outputs)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--imgsz", type=int, default=256)
    parser.add_argument(
        "--prefix",
        default="pipe_fixed4_yolov8n_5class",
        help="输出文件名前缀；保留默认值以兼容既有四卡归档",
    )
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    onnx_path = args.output_dir / f"{args.prefix}_fp32_{args.imgsz}_sixhead.onnx"
    json_path = args.output_dir / f"{args.prefix}.json"
    report_path = args.output_dir / "10_六输出ONNX导出验收.txt"

    yolo = YOLO(str(args.model))
    base = yolo.model.float().eval()
    # 安路官方YOLOv8 UINT8示例使用静态Split图。Ultralytics默认的chunk()
    # 会被导出为Shape/Gather/Div/Slice动态子图，DR1M90的Alnpu优化器不能可靠
    # 接受该图，实板表现为模型初始化阶段段错误。因此在导出前统一改用
    # C2f.forward_split()，生成与官方示例一致的Split算子结构。
    for module in base.modules():
        if isinstance(module, C2f):
            module.forward = module.forward_split
    detect = base.model[-1]
    if detect.nc != len(CLASS_NAMES) or detect.reg_max != 16:
        raise RuntimeError(
            f"模型接口不符合预期：nc={detect.nc}, reg_max={detect.reg_max}"
        )

    wrapper = SixHeadYoloV8(base).eval()
    sample = torch.rand(1, 3, args.imgsz, args.imgsz, dtype=torch.float32)
    with torch.inference_mode():
        torch_outputs = wrapper(sample)

    expected_shapes = []
    for stride in (8, 16, 32):
        side = args.imgsz // stride
        expected_shapes.extend([(1, 64, side, side), (1, 5, side, side)])
    actual_shapes = [tuple(value.shape) for value in torch_outputs]
    if actual_shapes != expected_shapes:
        raise RuntimeError(f"六输出形状不符合预期：{actual_shapes}")

    torch.onnx.export(
        wrapper,
        sample,
        str(onnx_path),
        input_names=["images"],
        output_names=OUTPUT_NAMES,
        opset_version=13,
        do_constant_folding=True,
        dynamo=False,
    )

    graph = onnx.load(str(onnx_path))
    # 官方fire_detect量化图把Split尺寸与Resize缩放值保存为initializer，
    # 而当前PyTorch导出器会生成6个Constant节点。把等价常量前移到
    # initializer，使基础算子集合与官方可运行YOLOv8图完全一致。
    constant_nodes = [node for node in graph.graph.node if node.op_type == "Constant"]
    for node in constant_nodes:
        value_attributes = [attribute for attribute in node.attribute if attribute.name == "value"]
        if len(node.output) != 1 or len(value_attributes) != 1 or not value_attributes[0].HasField("t"):
            raise RuntimeError(f"不能安全转换Constant节点：{node.name}")
        tensor = copy.deepcopy(value_attributes[0].t)
        tensor.name = node.output[0]
        graph.graph.initializer.append(tensor)
        graph.graph.node.remove(node)
    onnx.checker.check_model(graph)
    node_types = [node.op_type for node in graph.graph.node]
    forbidden_dynamic_ops = {"Shape", "Gather", "Div", "Slice"}
    present_forbidden = sorted(forbidden_dynamic_ops.intersection(node_types))
    if present_forbidden:
        raise RuntimeError(f"仍含Alnpu不可靠的动态切分算子：{present_forbidden}")
    if "Split" not in node_types:
        raise RuntimeError("未生成与安路官方YOLOv8示例一致的Split算子")
    if "Constant" in node_types:
        raise RuntimeError("Constant节点尚未按官方图转换为initializer")
    del graph.metadata_props[:]
    meta = graph.metadata_props.add()
    meta.key = "project.interface"
    meta.value = "anlogic-yolov8-sixhead-v1"
    meta = graph.metadata_props.add()
    meta.key = "project.classes"
    meta.value = ",".join(CLASS_NAMES)
    onnx.save(graph, str(onnx_path))

    session = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    ort_outputs = session.run(None, {"images": sample.numpy()})
    errors = [
        float(np.max(np.abs(torch_value.detach().numpy() - ort_value)))
        for torch_value, ort_value in zip(torch_outputs, ort_outputs)
    ]
    if max(errors) > 1e-3:
        raise RuntimeError(f"PyTorch/ONNX原始头输出误差过大：{errors}")

    config = {
        "input_params": {
            "r_mean": 0.0,
            "g_mean": 0.0,
            "b_mean": 0.0,
            "r_scale": 0.00392156862745098,
            "g_scale": 0.00392156862745098,
            "b_scale": 0.00392156862745098,
        },
        "yolo_params": {
            "probThreshold": 0.40,
            "nmsThreshold": 0.30,
            "className": CLASS_NAMES,
        },
    }
    json_path.write_text(json.dumps(config, ensure_ascii=False, indent=2), encoding="utf-8")

    lines = [
        "状态：FP32六输出ONNX导出及电脑侧结构验收通过",
        f"来源PT：{args.model.resolve()}",
        f"ONNX：{onnx_path.resolve()}",
        f"SHA256：{sha256(onnx_path)}",
        f"输入：FLOAT [1,3,{args.imgsz},{args.imgsz}]",
        "输出：stride8/16/32各一组box(64通道)+class(5通道)，共6路",
        f"输出形状：{actual_shapes}",
        f"PyTorch/ONNX逐输出最大绝对误差：{errors}",
        f"图结构：Split={node_types.count('Split')}；Constant/Shape/Gather/Div/Slice均为0",
        "类别编号：0 spall，1 crack，2 hole，3 root，4 sediment",
        "注意：这仍是FP32结构中间件，不是Alnpu可用的INT8/UINT8模型。",
        "下一步：Q/DQ静态量化、电脑侧数值/检测回归、板端严格Alnpu加载。",
    ]
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
