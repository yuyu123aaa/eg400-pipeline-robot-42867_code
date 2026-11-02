#!/usr/bin/env python3
"""用固定四卡及确定性增强校准六输出YOLOv8，生成安路Q/DQ UINT8候选。

电脑侧通过只代表模型结构和量化数值可用；最终是否被DR1M90的Alnpu支持，
必须在板卡上用只允许Alnpu的命令验证，禁止把本脚本结果直接写成最终通过。
"""

from __future__ import annotations

import argparse
import hashlib
import math
import shutil
import tempfile
from pathlib import Path

import cv2
import numpy as np
import onnx
import onnxruntime as ort
from onnx import TensorProto, numpy_helper
from onnxruntime.quantization import (
    CalibrationDataReader,
    CalibrationMethod,
    QuantFormat,
    QuantType,
    quantize_static,
)


CLASS_NAMES = ["spall", "crack", "hole", "root", "sediment"]


def read_image(path: Path) -> np.ndarray | None:
    """兼容OpenCV在Windows上无法直接打开中文路径的情况。"""
    try:
        return cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)
    except (OSError, ValueError):
        return None


def letterbox(image: np.ndarray, size: int = 256) -> np.ndarray:
    height, width = image.shape[:2]
    ratio = min(size / width, size / height)
    new_width, new_height = int(round(width * ratio)), int(round(height * ratio))
    resized = cv2.resize(image, (new_width, new_height), interpolation=cv2.INTER_LINEAR)
    canvas = np.full((size, size, 3), 114, dtype=np.uint8)
    x0 = (size - new_width) // 2
    y0 = (size - new_height) // 2
    canvas[y0 : y0 + new_height, x0 : x0 + new_width] = resized
    rgb = cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB)
    return np.ascontiguousarray(rgb.transpose(2, 0, 1)[None].astype(np.float32) / 255.0)


def calibration_variants(paths: list[Path]) -> list[np.ndarray]:
    values: list[np.ndarray] = []
    # 覆盖打印件常见的亮度、反光和左右视角变化，同时保持结果完全可复现。
    settings = [
        (1.00, 0, False),
        (0.78, 0, False),
        (1.18, 0, False),
        (1.00, 22, False),
        (1.00, -22, False),
        (0.90, 12, True),
        (1.10, -12, True),
        (1.00, 0, True),
    ]
    for path in paths:
        image = read_image(path)
        if image is None:
            raise RuntimeError(f"无法读取校准图片：{path}")
        for alpha, beta, flip in settings:
            variant = cv2.convertScaleAbs(image, alpha=alpha, beta=beta)
            if flip:
                variant = cv2.flip(variant, 1)
            values.append(letterbox(variant))
    return values


class Reader(CalibrationDataReader):
    def __init__(self, values: list[np.ndarray]) -> None:
        self.values = values
        self.index = 0

    def get_next(self) -> dict[str, np.ndarray] | None:
        if self.index >= len(self.values):
            return None
        value = self.values[self.index]
        self.index += 1
        return {"images": value}

    def rewind(self) -> None:
        self.index = 0


def sigmoid(value: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-np.clip(value, -30.0, 30.0)))


def iou(one: np.ndarray, many: np.ndarray) -> np.ndarray:
    x0 = np.maximum(one[0], many[:, 0])
    y0 = np.maximum(one[1], many[:, 1])
    x1 = np.minimum(one[2], many[:, 2])
    y1 = np.minimum(one[3], many[:, 3])
    inter = np.maximum(0.0, x1 - x0) * np.maximum(0.0, y1 - y0)
    area_one = max(0.0, one[2] - one[0]) * max(0.0, one[3] - one[1])
    area_many = np.maximum(0.0, many[:, 2] - many[:, 0]) * np.maximum(0.0, many[:, 3] - many[:, 1])
    return inter / np.maximum(area_one + area_many - inter, 1e-9)


def decode(outputs: list[np.ndarray], conf: float = 0.40, nms: float = 0.30) -> list[tuple[int, float]]:
    boxes: list[list[float]] = []
    scores: list[float] = []
    classes: list[int] = []
    bins = np.arange(16, dtype=np.float32)
    for index in range(0, 6, 2):
        box = outputs[index][0]
        cls = sigmoid(outputs[index + 1][0])
        feat_h, feat_w = box.shape[1:]
        stride = 256 // feat_w
        cls_ids = np.argmax(cls, axis=0)
        cls_scores = np.max(cls, axis=0)
        ys, xs = np.where(cls_scores >= conf)
        for y, x in zip(ys.tolist(), xs.tolist()):
            logits = box[:, y, x].reshape(4, 16)
            logits = logits - logits.max(axis=1, keepdims=True)
            probs = np.exp(logits)
            probs /= probs.sum(axis=1, keepdims=True)
            left, top, right, bottom = (probs * bins).sum(axis=1)
            boxes.append([
                (x + 0.5 - left) * stride,
                (y + 0.5 - top) * stride,
                (x + 0.5 + right) * stride,
                (y + 0.5 + bottom) * stride,
            ])
            scores.append(float(cls_scores[y, x]))
            classes.append(int(cls_ids[y, x]))

    if not boxes:
        return []
    box_array = np.asarray(boxes, dtype=np.float32)
    order = np.argsort(np.asarray(scores))[::-1]
    kept: list[int] = []
    while order.size:
        current = int(order[0])
        kept.append(current)
        if order.size == 1:
            break
        remaining = order[1:]
        overlaps = iou(box_array[current], box_array[remaining])
        same_class = np.asarray(classes)[remaining] == classes[current]
        order = remaining[~((overlaps > nms) & same_class)]
    return [(classes[index], scores[index]) for index in kept]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def canonicalize_bias_scales_for_anlogic(graph: onnx.ModelProto) -> int:
    """把ORT的一元素bias scale改成安路官方模型使用的FLOAT标量。

    通用ONNX运行时同时接受dims=[1]和dims=[]，但DR1M90随板ArmNN 24.6
    的LoadGraphQuantORT会在处理INT32 bias的dims=[1] scale时解引用空资源指针。
    官方fire YOLOv8模型的同类scale全部是dims=[]，并以float_data保存。
    """
    initializers = {item.name: item for item in graph.graph.initializer}
    changed = 0
    for node in graph.graph.node:
        if node.op_type != "DequantizeLinear" or len(node.input) < 2:
            continue
        quantized = initializers.get(node.input[0])
        scale = initializers.get(node.input[1])
        if quantized is None or scale is None or quantized.data_type != TensorProto.INT32:
            continue
        values = np.asarray(numpy_helper.to_array(scale), dtype=np.float32).reshape(-1)
        if values.size != 1:
            raise RuntimeError(
                f"INT32 bias的scale必须只有一个值：{scale.name} shape={list(scale.dims)}"
            )
        if list(scale.dims) != [] or not scale.float_data:
            replacement = onnx.helper.make_tensor(
                name=scale.name,
                data_type=TensorProto.FLOAT,
                dims=[],
                vals=[float(values[0])],
            )
            scale.CopyFrom(replacement)
            changed += 1
    return changed


def infer_all_node_outputs_for_anlogic(graph: onnx.ModelProto) -> int:
    """补齐旧版安路ArmNN解析器需要的每个节点输出类型/形状。

    通用ONNX允许中间张量不出现在value_info；DR1M90随板ArmNN 24.6
    的LoadGraphQuantORT却会对缺少类型信息的第一个DequantizeLinear输出
    解引用空资源指针。官方可运行YOLOv8模型的全部节点输出都有类型信息。
    """
    inferred = onnx.shape_inference.infer_shapes(
        graph,
        strict_mode=True,
        data_prop=True,
    )
    known = {
        item.name
        for item in (
            list(inferred.graph.input)
            + list(inferred.graph.output)
            + list(inferred.graph.value_info)
        )
    }
    outputs = [
        output
        for node in inferred.graph.node
        for output in node.output
        if output
    ]
    missing = [output for output in outputs if output not in known]
    if missing:
        raise RuntimeError(
            f"shape inference后仍有{len(missing)}个节点输出缺少类型信息：{missing[:10]}"
        )
    graph.CopyFrom(inferred)
    return len(outputs)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--images", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    image_paths = sorted(
        path for path in args.images.iterdir() if path.suffix.lower() in {".jpg", ".jpeg", ".png"}
    )
    if len(image_paths) != 4:
        raise RuntimeError(f"固定校准集必须正好4张，当前为{len(image_paths)}张：{args.images}")
    values = calibration_variants(image_paths)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    # onnxruntime量化器生成中间*-inferred.onnx时不能可靠处理Windows中文路径，
    # 所以在系统ASCII临时目录内完成量化，再复制回正式目录。
    with tempfile.TemporaryDirectory(prefix="anlogic_yolo_quant_") as temp_dir:
        staged_input = Path(temp_dir) / "model.onnx"
        staged_output = Path(temp_dir) / "model_qdq.onnx"
        shutil.copy2(args.model, staged_input)
        quantize_static(
            model_input=str(staged_input),
            model_output=str(staged_output),
            calibration_data_reader=Reader(values),
            quant_format=QuantFormat.QDQ,
            activation_type=QuantType.QUInt8,
            weight_type=QuantType.QUInt8,
            per_channel=False,
            calibrate_method=CalibrationMethod.MinMax,
            # 必须让ORT按默认注册表量化整张YOLOv8图，而不只是Conv。
            # 安路官方可运行模型连Sigmoid/Mul/Split/Concat/Add/MaxPool/Resize
            # 都带Q/DQ；只量化Conv的通用CPU候选会在Alnpu优化阶段段错误。
            op_types_to_quantize=[],
            extra_options={
                "ActivationSymmetric": False,
                "WeightSymmetric": False,
                "CalibTensorRangeSymmetric": False,
            },
        )
        shutil.copy2(staged_output, args.output)

    graph = onnx.load(str(args.output))
    canonicalized_bias_scales = canonicalize_bias_scales_for_anlogic(graph)
    if canonicalized_bias_scales == 0:
        raise RuntimeError("没有发现需要规范化的INT32 bias scale，量化器输出结构可能已变化")
    typed_node_outputs = infer_all_node_outputs_for_anlogic(graph)
    onnx.checker.check_model(graph)
    node_types = [node.op_type for node in graph.graph.node]
    q_count = node_types.count("QuantizeLinear")
    dq_count = node_types.count("DequantizeLinear")
    if q_count == 0 or dq_count == 0:
        raise RuntimeError("生成文件不含Q/DQ节点")
    forbidden_ops = {"Constant", "Shape", "Gather", "Div", "Slice"}
    present_forbidden = sorted(forbidden_ops.intersection(node_types))
    if present_forbidden:
        raise RuntimeError(f"仍含Alnpu不可靠的动态切分算子：{present_forbidden}")
    if node_types.count("Split") != 8:
        raise RuntimeError(f"Split数量与官方YOLOv8图不一致：{node_types.count('Split')} != 8")
    del graph.metadata_props[:]
    for key, value in (
        ("onnx.infer", "onnxruntime.quant"),
        ("project.interface", "anlogic-yolov8-sixhead-v1"),
        ("project.classes", ",".join(CLASS_NAMES)),
        ("project.status", "candidate-requires-strict-Alnpu-board-test"),
    ):
        item = graph.metadata_props.add()
        item.key = key
        item.value = value
    onnx.save(graph, str(args.output))

    fp_session = ort.InferenceSession(str(args.model), providers=["CPUExecutionProvider"])
    q_session = ort.InferenceSession(str(args.output), providers=["CPUExecutionProvider"])
    results: list[str] = []
    passed = 0
    expected_by_stem = {"spall": 0, "crack": 1, "hole": 2, "sediment": 4}
    for path in image_paths:
        image = read_image(path)
        tensor = letterbox(image)
        fp_outputs = fp_session.run(None, {"images": tensor})
        q_outputs = q_session.run(None, {"images": tensor})
        fp_detections = decode(fp_outputs)
        q_detections = decode(q_outputs)
        expected = expected_by_stem[path.stem.lower()]
        correct = bool(q_detections and q_detections[0][0] == expected)
        passed += int(correct)
        fp_text = ", ".join(f"{CLASS_NAMES[c]} {s:.4f}" for c, s in fp_detections[:5]) or "无框"
        q_text = ", ".join(f"{CLASS_NAMES[c]} {s:.4f}" for c, s in q_detections[:5]) or "无框"
        results.append(f"{path.name}: 期望={CLASS_NAMES[expected]}；FP32={fp_text}；QDQ={q_text}；{'PASS' if correct else 'FAIL'}")

    report = args.output.parent / "11_QDQ_UINT8电脑侧验收.txt"
    lines = [
        "状态：电脑侧Q/DQ UINT8候选已生成；板端Alnpu尚未验证",
        f"模型：{args.output.resolve()}",
        f"SHA256：{sha256(args.output)}",
        f"校准：4张固定数字原图×8种确定性亮度/翻转变化={len(values)}个输入",
        f"Q/DQ节点：QuantizeLinear={q_count}，DequantizeLinear={dq_count}",
        f"安路解析器兼容处理：{canonicalized_bias_scales}个INT32 bias scale已由dims=[1]/raw_data规范为dims=[]/float_data",
        f"安路解析器类型门禁：{typed_node_outputs}/{typed_node_outputs}个节点输出均有value_info类型/形状",
        "图结构：Split=8；Constant/Shape/Gather/Div/Slice均为0（与官方YOLOv8示例同类结构）",
        f"四张数字原图首框类别验收：{passed}/4",
        *results,
        "禁止误写为最终模型：下一步必须在DR1M90上以Alnpu,CpuAcc,CpuRef加载，确认主要卷积层由Alnpu执行、记录辅助层回退，并测试类别、框、NPU耗时和整链路FPS。",
    ]
    report.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
