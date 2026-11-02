from pathlib import Path


PHYTIUM_ROOT = (
    Path(__file__).resolve().parents[1]
    / "reference"
    / "legacy_phytium"
)
OPENAMP_ROOT = PHYTIUM_ROOT.parent / "openamp_for_linux"
DEPLOYED = (PHYTIUM_ROOT / "rpmsg-demo-single.c").read_text(encoding="utf-8")
REFERENCE = (OPENAMP_ROOT / "rpmsg-demo-single.c").read_text(encoding="utf-8")


def normalized(source):
    return "\n".join(
        line.rstrip() for line in source.splitlines() if line.strip()
    )


def test_deployed_bridge_keeps_heartbeat_and_serialized_writes():
    assert "void *heartbeat_thread(void *arg)" in DEPLOYED
    assert "pthread_mutex_lock(&rpmsg_write_lock);" in DEPLOYED
    assert "DEVICE_CORE_HEARTBEAT" in DEPLOYED


def test_both_bridges_forward_control_feedback():
    for source in (DEPLOYED, REFERENCE):
        assert "test_data_r.command == DEVICE_CORE_CHECK" in source
        assert '"{\\"control_ack\\": \\"%s\\"}"' in source
        assert 'strncmp(buff_r, "FAULT:NAV:", 10)' in source
        assert '"{\\"nav_fault\\": \\"%s\\"}"' in source


def test_both_bridges_forward_sonar_damage_transitions():
    for source in (DEPLOYED, REFERENCE):
        assert 'strncmp(buff_r, "FAULT:SONAR:", 12)' in source
        assert (
            '"{\\"sonar_fault\\": true, '
            '\\"sonar_fault_mask\\": %u}"'
        ) in source
        assert 'strcmp(buff_r, "RECOVER:SONAR") == 0' in source
        assert (
            '"{\\"sonar_fault\\": false, '
            '\\"sonar_fault_mask\\": 0}"'
        ) in source


def test_bridge_sources_have_matching_nonblank_content():
    assert normalized(DEPLOYED) == normalized(REFERENCE)
