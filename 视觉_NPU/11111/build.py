#!/usr/bin/env python3
"""按模型 I 的量化配方生成 I-max，并建立严格隔离的独立测试集。

模型 I 并未继续训练权重；它保留模型 F 的 FP32 六输出权重，仅重新进行
QDQ UINT8 静态量化。本脚本沿用相同的 256 Letterbox、RGB/255、MinMax、
QUInt8/QUInt8 和安路兼容修正，只扩大校准图片数量并保留独立测试集。
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import shutil
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

import cv2
import numpy as np
import onnx
import onnxruntime as ort
from onnxruntime.quantization import (
    CalibrationDataReader,
    CalibrationMethod,
    QuantFormat,
    QuantType,
    quantize_static,
)


CLASS_NAMES = ["spall", "crack", "hole", "root", "sediment"]
SPLIT_SEED = 20261030
BLOCK_SIZE = 5
TEST_RATIO = 0.20


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def load_helpers(path: Path):
    spec = importlib.util.spec_from_file_location("anlogic_quant_helpers", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载安路量化兼容函数：{path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_image(path: Path) -> np.ndarray:
    image = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)
    if image is None:
        raise RuntimeError(f"无法读取图片：{path}")
    return image


def letterbox(image: np.ndarray, size: int = 256) -> tuple[np.ndarray, float, int, int]:
    height, width = image.shape[:2]
    ratio = min(size / width, size / height)
    new_width = int(round(width * ratio))
    new_height = int(round(height * ratio))
    resized = cv2.resize(image, (new_width, new_height), interpolation=cv2.INTER_LINEAR)
    canvas = np.full((size, size, 3), 114, dtype=np.uint8)
    left = (size - new_width) // 2
    top = (size - new_height) // 2
    canvas[top : top + new_height, left : left + new_width] = resized
    rgb = cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB)
    tensor = np.ascontiguousarray(rgb.transpose(2, 0, 1)[None].astype(np.float32) / 255.0)
    return tensor, ratio, left, top


class Reader(CalibrationDataReader):
    def __init__(self, paths: list[Path]) -> None:
        self.paths = paths
        self.index = 0

    def get_next(self):
        if self.index >= len(self.paths):
            return None
        tensor, _, _, _ = letterbox(read_image(self.paths[self.index]))
        self.index += 1
        return {"images": tensor}

    def rewind(self) -> None:
        self.index = 0


def numeric_files(directory: Path, suffix: str) -> dict[int, Path]:
    result: dict[int, Path] = {}
    for path in directory.glob(f"*{suffix}"):
        if path.stem.isdigit():
            result[int(path.stem)] = path
    return result


def verify_and_group_duplicates(images_dir: Path, labels_dir: Path) -> tuple[list[int], dict[int, list[int]]]:
    images = numeric_files(images_dir, ".jpg")
    labels = numeric_files(labels_dir, ".txt")
    expected = set(range(1, 512))
    if set(images) != expected or set(labels) != expected:
        raise RuntimeError("images/labels 必须都完整包含连续编号 1..511")

    by_hash: dict[str, list[int]] = defaultdict(list)
    for image_id, path in images.items():
        by_hash[sha256(path)].append(image_id)

    groups: dict[int, list[int]] = {}
    for members in by_hash.values():
        members.sort()
        canonical = members[0]
        reference_label = labels[canonical].read_bytes()
        if any(labels[item].read_bytes() != reference_label for item in members[1:]):
            raise RuntimeError(f"重复图片的标签不一致：{members}")
        groups[canonical] = members

    canonical_ids = sorted(groups)
    if canonical_ids != list(range(1, 294)):
        raise RuntimeError(f"去重后的真实图片编号不是 1..293：共 {len(canonical_ids)} 组")
    expected_duplicate_pairs = {item: [item, item + 293] for item in range(1, 219)}
    expected_duplicate_pairs.update({item: [item] for item in range(219, 294)})
    if groups != expected_duplicate_pairs:
        raise RuntimeError("511 个文件的重复关系与预期不符，停止以避免测试泄漏")
    return canonical_ids, groups


def choose_block_test_ids(ids: list[int], rng: np.random.Generator) -> list[int]:
    blocks = [ids[index : index + BLOCK_SIZE] for index in range(0, len(ids), BLOCK_SIZE)]
    target = int(round(len(ids) * TEST_RATIO))
    order = rng.permutation(len(blocks)).tolist()
    selected: list[int] = []
    for index in order:
        if len(selected) >= target:
            break
        selected.extend(blocks[index])
    return sorted(selected)


def make_split(canonical_ids: list[int]) -> tuple[list[int], list[int]]:
    rng = np.random.default_rng(SPLIT_SEED)
    old_ids = [item for item in canonical_ids if item <= 218]
    new_ids = [item for item in canonical_ids if item >= 219]
    test_ids = sorted(choose_block_test_ids(old_ids, rng) + choose_block_test_ids(new_ids, rng))
    calibration_ids = sorted(set(canonical_ids) - set(test_ids))
    if set(calibration_ids) & set(test_ids):
        raise RuntimeError("校准集与测试集发生交叉")
    if set(calibration_ids) | set(test_ids) != set(canonical_ids):
        raise RuntimeError("校准集与测试集未覆盖全部真实图片")
    return calibration_ids, test_ids


def write_split_artifacts(
    output_dir: Path,
    images_dir: Path,
    labels_dir: Path,
    calibration_ids: list[int],
    test_ids: list[int],
    groups: dict[int, list[int]],
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    test_images = output_dir / "independent_test" / "images"
    test_labels = output_dir / "independent_test" / "labels"
    test_images.mkdir(parents=True, exist_ok=True)
    test_labels.mkdir(parents=True, exist_ok=True)

    for image_id in test_ids:
        shutil.copy2(images_dir / f"{image_id}.jpg", test_images / f"{image_id}.jpg")
        shutil.copy2(labels_dir / f"{image_id}.txt", test_labels / f"{image_id}.txt")

    (output_dir / "calibration_unique_ids.txt").write_text(
        "\n".join(map(str, calibration_ids)) + "\n", encoding="utf-8"
    )
    (output_dir / "independent_test_unique_ids.txt").write_text(
        "\n".join(map(str, test_ids)) + "\n", encoding="utf-8"
    )
    physical_calibration = sorted(item for canonical in calibration_ids for item in groups[canonical])
    physical_test = sorted(item for canonical in test_ids for item in groups[canonical])
    (output_dir / "physical_file_split.json").write_text(
        json.dumps(
            {
                "note": "重复图片按 SHA256 绑定在同一侧；模型生成只使用每组的 canonical 图片。",
                "calibration_physical_ids": physical_calibration,
                "test_physical_ids": physical_test,
            },
            ensure_ascii=False,
            indent=2,
        ),
        encoding="utf-8",
    )


def quantize(
    fp32_model: Path,
    output_model: Path,
    image_paths: list[Path],
    helpers,
    metadata: dict[str, str],
    temp_parent: Path,
) -> tuple[int, int, int, int]:
    output_model.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="anlogic_i_max_quant_", dir=temp_parent) as temp_dir:
        staged_input = Path(temp_dir) / "model.onnx"
        staged_output = Path(temp_dir) / "model_qdq.onnx"
        shutil.copy2(fp32_model, staged_input)
        quantize_static(
            model_input=str(staged_input),
            model_output=str(staged_output),
            calibration_data_reader=Reader(image_paths),
            quant_format=QuantFormat.QDQ,
            activation_type=QuantType.QUInt8,
            weight_type=QuantType.QUInt8,
            per_channel=False,
            calibrate_method=CalibrationMethod.MinMax,
            op_types_to_quantize=[],
            extra_options={
                "ActivationSymmetric": False,
                "WeightSymmetric": False,
                "CalibTensorRangeSymmetric": False,
            },
        )
        shutil.copy2(staged_output, output_model)

    graph = onnx.load(str(output_model))
    fixed_bias_scales = helpers.canonicalize_bias_scales_for_anlogic(graph)
    typed_outputs = helpers.infer_all_node_outputs_for_anlogic(graph)
    node_types = [node.op_type for node in graph.graph.node]
    q_count = node_types.count("QuantizeLinear")
    dq_count = node_types.count("DequantizeLinear")
    if q_count == 0 or dq_count == 0 or node_types.count("Split") != 8:
        raise RuntimeError("量化图的 Q/DQ 或 Split 结构不符合安路门禁")
    forbidden = sorted({"Constant", "Shape", "Gather", "Div", "Slice"}.intersection(node_types))
    if forbidden:
        raise RuntimeError(f"量化图仍含安路不可靠算子：{forbidden}")
    del graph.metadata_props[:]
    for key, value in metadata.items():
        item = graph.metadata_props.add()
        item.key = key
        item.value = value
    onnx.save(graph, str(output_model))
    onnx.checker.check_model(str(output_model), full_check=True)
    return fixed_bias_scales, typed_outputs, q_count, dq_count


def sigmoid(value: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-np.clip(value, -30.0, 30.0)))


def box_iou(one: np.ndarray, many: np.ndarray) -> np.ndarray:
    if len(many) == 0:
        return np.zeros(0, dtype=np.float32)
    x0 = np.maximum(one[0], many[:, 0])
    y0 = np.maximum(one[1], many[:, 1])
    x1 = np.minimum(one[2], many[:, 2])
    y1 = np.minimum(one[3], many[:, 3])
    intersection = np.maximum(0.0, x1 - x0) * np.maximum(0.0, y1 - y0)
    area_one = max(0.0, one[2] - one[0]) * max(0.0, one[3] - one[1])
    area_many = np.maximum(0.0, many[:, 2] - many[:, 0]) * np.maximum(0.0, many[:, 3] - many[:, 1])
    return intersection / np.maximum(area_one + area_many - intersection, 1e-9)


def decode(outputs: list[np.ndarray], conf: float = 0.001, nms: float = 0.60):
    boxes: list[list[float]] = []
    scores: list[float] = []
    classes: list[int] = []
    bins = np.arange(16, dtype=np.float32)
    for index in range(0, 6, 2):
        box = outputs[index][0]
        cls = sigmoid(outputs[index + 1][0])
        feature_height, feature_width = box.shape[1:]
        stride = 256 // feature_width
        class_ids = np.argmax(cls, axis=0)
        class_scores = np.max(cls, axis=0)
        ys, xs = np.where(class_scores >= conf)
        for y, x in zip(ys.tolist(), xs.tolist()):
            logits = box[:, y, x].reshape(4, 16)
            logits -= logits.max(axis=1, keepdims=True)
            probabilities = np.exp(logits)
            probabilities /= probabilities.sum(axis=1, keepdims=True)
            left, top, right, bottom = (probabilities * bins).sum(axis=1)
            boxes.append(
                [
                    (x + 0.5 - left) * stride,
                    (y + 0.5 - top) * stride,
                    (x + 0.5 + right) * stride,
                    (y + 0.5 + bottom) * stride,
                ]
            )
            scores.append(float(class_scores[y, x]))
            classes.append(int(class_ids[y, x]))
    if not boxes:
        return np.empty((0, 4), np.float32), np.empty(0, np.float32), np.empty(0, np.int64)
    box_array = np.asarray(boxes, dtype=np.float32)
    score_array = np.asarray(scores, dtype=np.float32)
    class_array = np.asarray(classes, dtype=np.int64)
    kept: list[int] = []
    for class_id in range(len(CLASS_NAMES)):
        order = np.where(class_array == class_id)[0]
        order = order[np.argsort(score_array[order])[::-1]]
        while order.size:
            current = int(order[0])
            kept.append(current)
            if order.size == 1:
                break
            remaining = order[1:]
            order = remaining[box_iou(box_array[current], box_array[remaining]) <= nms]
    kept_array = np.asarray(sorted(kept, key=lambda item: score_array[item], reverse=True), dtype=np.int64)
    return box_array[kept_array], score_array[kept_array], class_array[kept_array]


def read_ground_truth(label_path: Path, width: int, height: int, ratio: float, left: int, top: int):
    boxes: list[list[float]] = []
    classes: list[int] = []
    for line in label_path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        class_id_text, x_text, y_text, w_text, h_text = line.split()[:5]
        class_id = int(class_id_text)
        x, y, box_width, box_height = map(float, (x_text, y_text, w_text, h_text))
        x0 = (x - box_width / 2) * width * ratio + left
        y0 = (y - box_height / 2) * height * ratio + top
        x1 = (x + box_width / 2) * width * ratio + left
        y1 = (y + box_height / 2) * height * ratio + top
        boxes.append([x0, y0, x1, y1])
        classes.append(class_id)
    return np.asarray(boxes, dtype=np.float32), np.asarray(classes, dtype=np.int64)


def average_precision(recalls: np.ndarray, precisions: np.ndarray) -> float:
    recall_curve = np.concatenate(([0.0], recalls, [1.0]))
    precision_curve = np.concatenate(([1.0], precisions, [0.0]))
    precision_curve = np.maximum.accumulate(precision_curve[::-1])[::-1]
    x = np.linspace(0.0, 1.0, 101)
    return float(np.trapezoid(np.interp(x, recall_curve, precision_curve), x))


def evaluate_model(model_path: Path, images_dir: Path, labels_dir: Path, test_ids: list[int]):
    session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    predictions: dict[int, list[tuple[int, float, np.ndarray]]] = defaultdict(list)
    ground_truth: dict[int, dict[int, list[np.ndarray]]] = defaultdict(lambda: defaultdict(list))
    per_image_top: dict[int, tuple[int, float] | None] = {}
    for image_id in test_ids:
        image = read_image(images_dir / f"{image_id}.jpg")
        tensor, ratio, left, top = letterbox(image)
        outputs = session.run(None, {"images": tensor})
        boxes, scores, classes = decode(outputs)
        per_image_top[image_id] = None if len(scores) == 0 else (int(classes[0]), float(scores[0]))
        for box, score, class_id in zip(boxes, scores, classes):
            predictions[int(class_id)].append((image_id, float(score), box))
        gt_boxes, gt_classes = read_ground_truth(
            labels_dir / f"{image_id}.txt", image.shape[1], image.shape[0], ratio, left, top
        )
        for box, class_id in zip(gt_boxes, gt_classes):
            ground_truth[int(class_id)][image_id].append(box)

    thresholds = np.arange(0.50, 0.96, 0.05)
    aps_by_class: dict[int, list[float]] = defaultdict(list)
    for threshold in thresholds:
        for class_id in range(len(CLASS_NAMES)):
            values = sorted(predictions[class_id], key=lambda item: item[1], reverse=True)
            total_gt = sum(len(items) for items in ground_truth[class_id].values())
            matched = {
                image_id: np.zeros(len(items), dtype=bool)
                for image_id, items in ground_truth[class_id].items()
            }
            true_positive = np.zeros(len(values), dtype=np.float32)
            false_positive = np.zeros(len(values), dtype=np.float32)
            for index, (image_id, _, predicted_box) in enumerate(values):
                targets = np.asarray(ground_truth[class_id].get(image_id, []), dtype=np.float32).reshape(-1, 4)
                overlaps = box_iou(predicted_box, targets)
                if len(overlaps):
                    best = int(np.argmax(overlaps))
                    if overlaps[best] >= threshold and not matched[image_id][best]:
                        true_positive[index] = 1.0
                        matched[image_id][best] = True
                        continue
                false_positive[index] = 1.0
            cumulative_tp = np.cumsum(true_positive)
            cumulative_fp = np.cumsum(false_positive)
            recalls = cumulative_tp / max(total_gt, 1)
            precisions = cumulative_tp / np.maximum(cumulative_tp + cumulative_fp, 1e-9)
            aps_by_class[class_id].append(average_precision(recalls, precisions))

    ap50 = {CLASS_NAMES[c]: aps_by_class[c][0] for c in range(len(CLASS_NAMES))}
    ap5095 = {CLASS_NAMES[c]: float(np.mean(aps_by_class[c])) for c in range(len(CLASS_NAMES))}
    return {
        "images": len(test_ids),
        "mAP50": float(np.mean(list(ap50.values()))),
        "mAP50_95": float(np.mean(list(ap5095.values()))),
        "AP50_by_class": ap50,
        "AP50_95_by_class": ap5095,
        "top_detection": per_image_top,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--images", required=True, type=Path)
    parser.add_argument("--labels", required=True, type=Path)
    parser.add_argument("--fp32-model", required=True, type=Path)
    parser.add_argument("--helper-script", required=True, type=Path)
    parser.add_argument("--original-i", required=True, type=Path)
    parser.add_argument("--original-i-list", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--temp-dir", required=True, type=Path)
    args = parser.parse_args()

    args.temp_dir.mkdir(parents=True, exist_ok=True)
    tempfile.tempdir = str(args.temp_dir.resolve())
    helpers = load_helpers(args.helper_script)
    canonical_ids, groups = verify_and_group_duplicates(args.images, args.labels)
    calibration_ids, test_ids = make_split(canonical_ids)
    write_split_artifacts(
        args.output_dir, args.images, args.labels, calibration_ids, test_ids, groups
    )

    original_i_ids = [int(line) for line in args.original_i_list.read_text(encoding="utf-8").splitlines() if line]
    if len(original_i_ids) != 128:
        raise RuntimeError(f"I 原校准清单数量不是 128：{len(original_i_ids)}")
    with tempfile.TemporaryDirectory(prefix="verify_original_i_", dir=args.temp_dir) as temp_dir:
        reproduced_i = Path(temp_dir) / "model_I_reproduced.onnx"
        quantize(
            args.fp32_model,
            reproduced_i,
            [args.images / f"{item}.jpg" for item in original_i_ids],
            helpers,
            {
                "onnx.infer": "onnxruntime.quant",
                "project.interface": "anlogic-yolov8-sixhead-v1",
                "project.classes": ",".join(CLASS_NAMES),
                "project.training_source": "F-weights-unchanged",
                "project.calibration": "64-old-plus-64-new-close-letterbox256",
                "project.status": "candidate-requires-board-test",
            },
            args.temp_dir,
        )
        reproduced_hash = sha256(reproduced_i)
        original_hash = sha256(args.original_i)
        if reproduced_hash != original_hash:
            raise RuntimeError(
                "当前配方未能逐字节复现模型 I，停止生成 I-max："
                f"reproduced={reproduced_hash}, original={original_hash}"
            )

    output_model = args.output_dir / "yolov8.onnx"
    fixed_scales, typed_outputs, q_count, dq_count = quantize(
        args.fp32_model,
        output_model,
        [args.images / f"{item}.jpg" for item in calibration_ids],
        helpers,
        {
            "onnx.infer": "onnxruntime.quant",
            "project.interface": "anlogic-yolov8-sixhead-v1",
            "project.classes": ",".join(CLASS_NAMES),
            "project.training_source": "F-weights-unchanged",
            "project.calibration": f"{len(calibration_ids)}-unique-images-letterbox256-independent-test-excluded",
            "project.status": "candidate-requires-board-test",
        },
        args.temp_dir,
    )

    fp32_metrics = evaluate_model(args.fp32_model, args.images, args.labels, test_ids)
    original_i_metrics = evaluate_model(args.original_i, args.images, args.labels, test_ids)
    qdq_metrics = evaluate_model(output_model, args.images, args.labels, test_ids)
    agreements = 0
    for image_id in test_ids:
        left = fp32_metrics["top_detection"][image_id]
        right = qdq_metrics["top_detection"][image_id]
        if (left is None and right is None) or (
            left is not None and right is not None and left[0] == right[0]
        ):
            agreements += 1
    fp32_metrics.pop("top_detection")
    original_i_metrics.pop("top_detection")
    qdq_metrics.pop("top_detection")

    report_data = {
        "split": {
            "physical_files": 511,
            "unique_images": len(canonical_ids),
            "calibration_unique_images": len(calibration_ids),
            "independent_test_unique_images": len(test_ids),
            "seed": SPLIT_SEED,
            "block_size": BLOCK_SIZE,
        },
        "reproduction_gate": {
            "original_I_sha256": original_hash,
            "reproduced_I_sha256": reproduced_hash,
            "exact_match": True,
        },
        "I_max": {
            "sha256": sha256(output_model),
            "source_FP32_sha256": sha256(args.fp32_model),
            "QuantizeLinear": q_count,
            "DequantizeLinear": dq_count,
            "fixed_INT32_bias_scales": fixed_scales,
            "typed_node_outputs": typed_outputs,
        },
        "independent_test": {
            "FP32_F": fp32_metrics,
            "QDQ_original_I": original_i_metrics,
            "QDQ_I_max": qdq_metrics,
            "FP32_vs_I_max_top_class_agreement": f"{agreements}/{len(test_ids)}",
        },
    }
    (args.output_dir / "I-max_computer_validation.json").write_text(
        json.dumps(report_data, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    lines = [
        "I-max 生成与电脑侧验收完成",
        "",
        "配方：模型 F 权重不训练；与 I 相同的 256 Letterbox、RGB/255、QDQ UINT8、MinMax。",
        f"重复审计：511 个物理文件，SHA256 去重后为 {len(canonical_ids)} 张真实图片。",
        f"校准集：{len(calibration_ids)} 张真实图片；独立测试集：{len(test_ids)} 张真实图片；两者无 SHA256 交叉。",
        f"I 配方复现门禁：原 I={original_hash}；复现 I={reproduced_hash}；逐字节一致。",
        f"I-max SHA256：{sha256(output_model)}",
        f"图结构：QuantizeLinear={q_count}，DequantizeLinear={dq_count}，Split=8。",
        f"独立测试 FP32 F：mAP50={fp32_metrics['mAP50']:.6f}，mAP50-95={fp32_metrics['mAP50_95']:.6f}",
        f"独立测试原 I：mAP50={original_i_metrics['mAP50']:.6f}，mAP50-95={original_i_metrics['mAP50_95']:.6f}",
        f"独立测试 QDQ I-max：mAP50={qdq_metrics['mAP50']:.6f}，mAP50-95={qdq_metrics['mAP50_95']:.6f}",
        f"I-max 相对原 I：mAP50-95 变化={qdq_metrics['mAP50_95'] - original_i_metrics['mAP50_95']:+.6f}",
        "",
        "注意：电脑侧通过不等于 DR1M90 实板通过；仍需验证 Alnpu 加载、五类识别、NPU 耗时与持续运行。",
    ]
    (args.output_dir / "README.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
