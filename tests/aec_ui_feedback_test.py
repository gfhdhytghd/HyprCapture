"""Exercise AEC button outcomes without downloading models or using audio hardware."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="hyprcapture-aec-ui-") as directory:
    root = pathlib.Path(directory)
    executable = root / "hyprcapture-sound-ui-test"
    shutil.copy2(sys.argv[1], executable)
    worker = root / "hyprcapture-aec"
    worker.write_text("#!" + sys.executable + "\n" + '''
import json, os, sys, time
outcome = os.environ.get("HYPRCAPTURE_TEST_AEC_OUTCOME", "failed")
if sys.argv[1] == "--install":
    time.sleep(.15)
    if outcome == "crash":
        print(json.dumps({"error": "simulated worker failure"}))
        sys.exit(1)
    result = {"failed": {"status": "failed", "reason": "Experimental OpenVINO backend unavailable"},
              "success": {"status": "complete"},
              "busy": {"status": "pending", "reason": "computer busy"}}[outcome]
else:
    # A previous successful cache must not turn an incomplete retest into a pass.
    result = ({"aec": "ready", "model": 512, "backend": "cpu", "description": "On · CPU 512"}
              if outcome in ("success", "busy") else
              {"aec": "pending", "model": 0, "description": "AEC · test pending"})
print(json.dumps(result))
''')
    worker.chmod(0o700)
    subprocess.run([str(executable)], check=True, timeout=20,
                   env=dict(os.environ, QT_QPA_PLATFORM="offscreen", HYPRCAPTURE_TEST_AEC_FEEDBACK="1"))
