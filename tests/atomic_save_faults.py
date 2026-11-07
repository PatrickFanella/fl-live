"""Fault-inject authored-project saves and interrupt them with SIGKILL.

Usage: atomic_save_faults.py SAVE_FAULT_PROBE FAULT_INJECT_LIBRARY
"""
import json
import os
import pathlib
import random
import selectors
import signal
import subprocess
import sys
import tempfile
import time

probe, library = (str(pathlib.Path(p).resolve()) for p in sys.argv[1:3])
summary = {}
with tempfile.TemporaryDirectory(prefix="fl-live-save-") as temporary:
    root = pathlib.Path(temporary)
    faults_dir = root / "faults"
    faults_dir.mkdir()
    env = dict(os.environ, LD_PRELOAD=library, FL_LIVE_FAULT_DIR=str(faults_dir), QT_QPA_PLATFORM="offscreen")
    env.pop("FL_LIVE_FAULT", None)
    result = subprocess.run([probe, "faults", str(faults_dir)], env=env, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert "PASS: write, sync and rename faults" in result.stdout, result.stdout
    summary["injected_faults"] = [line.split()[-1] for line in result.stdout.splitlines() if line.startswith("PASS fault ")]
    assert summary["injected_faults"] == ["write", "sync", "rename"], summary

    # Interruption: kill the writer at random points during continuous saves.
    loop_dir = root / "loop"
    loop_dir.mkdir()
    plain = {k: v for k, v in env.items() if k not in ("LD_PRELOAD", "FL_LIVE_FAULT_DIR")}
    rng = random.Random(61)
    outcomes = {"a": 0, "b": 0}
    leftovers = 0
    for iteration in range(40):
        writer = subprocess.Popen([probe, "loop", str(loop_dir)], env=plain, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(writer.stdout, selectors.EVENT_READ)
                assert selector.select(timeout=10), "writer did not become ready"
            assert json.loads(writer.stdout.readline())["ready"]
            time.sleep(rng.uniform(0, 0.03))
            writer.send_signal(signal.SIGKILL)
            writer.wait(timeout=5)
        finally:
            if writer.poll() is None:
                writer.kill()
                writer.wait()
        current = (loop_dir / "project.json").read_bytes()
        a, b = (loop_dir / "a.ref").read_bytes(), (loop_dir / "b.ref").read_bytes()
        assert current in (a, b), f"iteration {iteration}: destination is neither complete save ({len(current)} bytes)"
        outcomes["a" if current == a else "b"] += 1
        # QSaveFile temporaries named project.json.XXXXXX may survive a kill.
        names = [p.name for p in loop_dir.iterdir()]
        stray = [n for n in names if n.startswith("project.json.")]
        leftovers = max(leftovers, len(stray))
        assert set(names) - set(stray) == {"project.json", "a.ref", "b.ref"}, names
    verify = subprocess.run([probe, "verify", str(loop_dir)], env=plain, capture_output=True, text=True, timeout=30)
    assert verify.returncode == 0, (verify.stdout, verify.stderr)
    summary.update(kills=40, destination_complete=outcomes, max_stray_temporaries=leftovers,
                   reopened_after_kills=True, project_bytes=len(a))
    assert outcomes["a"] and outcomes["b"], ("kills never observed both saves", outcomes)
print(json.dumps(dict(passed=True, scope="LD_PRELOAD write/sync/rename faults and SIGKILL during saves; no power loss", **summary)))
