import os
import subprocess
import pytest
import time

MODULE_NAME = "clipboard"
MODULE_PATH = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "clipboard.ko"))
DEV_PATH    = "/dev/clipboard"

def _get_major(name):
    with open("/proc/devices") as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2 and parts[1] == name:
                return parts[0]
    raise RuntimeError(f"Module {name} not registered")

@pytest.fixture(scope="session", autouse=True)
def clipboard_module():
    # 1) Insert the module
    subprocess.check_call(["sudo", "insmod", MODULE_PATH])
    # give it a moment to register
    time.sleep(0.1)

    # 2) Create /dev node if needed
    if not os.path.exists(DEV_PATH):
        major = _get_major(MODULE_NAME)
        subprocess.check_call(["sudo", "mknod", DEV_PATH, "c", major, "0"])
        subprocess.check_call(["sudo", "chmod", "666", DEV_PATH])

    yield

    # 3) Teardown: remove module (and optionally /dev)
    subprocess.check_call(["sudo", "rmmod", MODULE_NAME])
    # subprocess.check_call(["sudo", "rm", "-f", DEV_PATH])

