"""Recording overflow through the real desktop Session (#49): the fault
injector makes every write into the recordings folder sleep 100 ms, and the
probe records 400 preview ticks (192,000 stereo frames) at once. The 2 s take
ring must overflow without blocking the ticks, the drop must be reported, the
frames on disk plus the dropped frames must equal the captured frames, and
the take must still become a clip.

    recording_overflow.py SAVE_FAULT_PROBE FAULT_INJECT_LIBRARY"""
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

probe, library = sys.argv[1], sys.argv[2]
with tempfile.TemporaryDirectory(prefix="fl-live-overflow-") as temporary:
    root = pathlib.Path(temporary)
    env = dict(os.environ, LD_PRELOAD=library, FL_LIVE_FAULT="slow", FL_LIVE_FAULT_DIR=str(root / "recordings"), QT_QPA_PLATFORM="offscreen")
    run = subprocess.run([probe, "record-overflow", str(root)], env=env, capture_output=True, text=True, timeout=120)
    assert run.returncode == 0, run.stderr
    result = json.loads(run.stdout.strip().splitlines()[-1])
    dropped = re.search(r"dropped (\d+) frames when the disk fell behind", result["message"])
    assert dropped, result
    assert result["message"].startswith("Recorded 1 take as new clips") and result["clips"] == 2, result
    data = pathlib.Path(result["file"]).read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", "take is not a WAV file"
    at, payload = 12, None
    while at + 8 <= len(data):
        tag, size = data[at:at + 4], struct.unpack("<I", data[at + 4:at + 8])[0]
        if tag == b"data":
            payload = data[at + 8:at + 8 + size]
        at += 8 + size + (size & 1)
    frames = len(payload) // 8
    lost = int(dropped.group(1))
    assert lost > 0 and frames + lost == 400 * 480, (frames, lost)
    assert lost % 480 == 0, f"dropped frames {lost} are not whole blocks"
    # Writes take 100 ms each; the ticks must not wait for them.
    assert result["tick_ms"] < 2000, f"400 ticks took {result['tick_ms']} ms: the capture side waited for the disk"
    print(json.dumps({"frames_on_disk": frames, "frames_dropped": lost, "tick_ms": result["tick_ms"], "stop_ms": result["stop_ms"]}))
    print("PASS: a disk slower than the take overflows the ring without blocking the ticks; the drop is reported in whole blocks, "
          "disk + dropped frames equal the captured frames, and the take still becomes a clip")
