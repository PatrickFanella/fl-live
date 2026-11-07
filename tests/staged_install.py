"""Install the build into a fresh prefix and run the installed programs.

Usage: staged_install.py BUILD_DIR EXPECTED_REVISION
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import wave

build, revision = pathlib.Path(sys.argv[1]).resolve(), sys.argv[2]
with tempfile.TemporaryDirectory(prefix="fl-live-install-") as temporary:
    prefix = pathlib.Path(temporary) / "prefix"
    subprocess.run(["cmake", "--install", str(build), "--prefix", str(prefix)], check=True, capture_output=True, text=True)
    expected = ["bin/fl-live", "bin/fl-live-play", "bin/fl-live-render", "share/applications/fl-live.desktop",
                "share/fl-live/examples/four-track-session.json", "share/fl-live/examples/single-pattern.json", "share/fl-live/examples/notes-and-devices.json",
                "share/doc/fl_live/THIRD-PARTY-NOTICES.md", "share/doc/fl_live/install.md", "share/doc/fl_live/limitations.md"]
    missing = [path for path in expected if not (prefix / path).exists()]
    assert not missing, missing
    # Nothing installed may point back into the build tree.
    for binary in ("fl-live", "fl-live-play", "fl-live-render"):
        dynamic = subprocess.run(["readelf", "-d", str(prefix / "bin" / binary)], capture_output=True, text=True).stdout
        assert str(build) not in dynamic, (binary, "references the build directory")
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software", HOME=temporary,
               XDG_DATA_HOME=str(pathlib.Path(temporary) / "data"))
    version = subprocess.run([str(prefix / "bin/fl-live"), "--version"], env=env, capture_output=True, text=True, timeout=20)
    assert version.returncode == 0 and f"(source {revision})" in version.stdout, (version.stdout, version.stderr)
    out = pathlib.Path(temporary) / "demo.wav"
    render = subprocess.run([str(prefix / "bin/fl-live-render"), str(out), "257"], capture_output=True, text=True, timeout=60)
    assert render.returncode == 0, render.stderr
    with wave.open(str(out)) as w:
        assert w.getframerate() == 48000 and w.getnframes() == 384000, (w.getframerate(), w.getnframes())
    # The installed desktop starts and keeps running until stopped.
    app = subprocess.Popen([str(prefix / "bin/fl-live")], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        app.wait(timeout=3)
        raise AssertionError(("fl-live exited early", app.returncode, app.stderr.read()))
    except subprocess.TimeoutExpired:
        pass
    finally:
        app.terminate(); app.wait(timeout=5)
    print(f"PASS: staged install of {len(expected)} files, installed fl-live {version.stdout.strip()}, render and desktop launch")
