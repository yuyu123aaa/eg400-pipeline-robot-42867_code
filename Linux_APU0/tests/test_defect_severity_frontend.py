from pathlib import Path


APP_JS = Path("dashboard/app.js").read_text(encoding="utf-8")


def test_live_defect_card_renders_severity_fields():
    for token in (
        "log.severity",
        "log.severity_code",
        "log.confidence",
        "log.advice",
    ):
        assert token in APP_JS
    assert "severityBadgeClass" in APP_JS
