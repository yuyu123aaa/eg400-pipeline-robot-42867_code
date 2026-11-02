"""缺陷类别到固定严重等级的查询模块。"""

import config


_PENDING_PROFILE = {
    "severity": "待定",
    "severity_code": 0,
    "color": "gray",
    "bgr": (128, 128, 128),
    "advice": "建议人工复核",
}


def _normalize_label(label):
    return str(label).strip().lower().replace("-", "_").replace(" ", "_")


def resolve_defect_profile(class_id, model_name=None):
    """根据模型类别返回缺陷名称和固定严重度，未知类别安全回退。"""
    raw_name = model_name
    normalized = _normalize_label(raw_name) if raw_name is not None else ""
    canonical_name = config.DEFECT_TYPE_ALIASES.get(normalized)

    if canonical_name is None:
        fallback_name = config.DEFECT_NAME_MAP.get(int(class_id))
        if fallback_name is not None:
            raw_name = fallback_name
            canonical_name = config.DEFECT_TYPE_ALIASES.get(
                _normalize_label(fallback_name),
                str(fallback_name),
            )

    if canonical_name in config.DEFECT_SEVERITY_PROFILES:
        return dict(config.DEFECT_SEVERITY_PROFILES[canonical_name])

    pending = dict(_PENDING_PROFILE)
    pending["type"] = str(raw_name) if raw_name is not None else str(class_id)
    return {"type": pending.pop("type"), **pending}
