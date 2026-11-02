import subprocess
import sys
from pathlib import Path


def test_ort_session_probe_exposes_model_and_optimization_arguments():
    project_root = Path(__file__).resolve().parents[1]
    result = subprocess.run(
        [sys.executable, "tools/ort_session_probe.py", "--help"],
        cwd=project_root,
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode == 0
    assert "--model" in result.stdout
    assert "--optimization" in result.stdout
