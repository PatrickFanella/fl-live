"""Check the actual CLI output, format, boundaries and block-size invariance."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import wave

binary = str(pathlib.Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="fl-live-render-") as temporary:
    root = pathlib.Path(temporary)
    baseline = None
    for block in (1, 64, 128, 257, 512):
        output = root / f"demo-{block}.wav"
        subprocess.run([binary, str(output), str(block)], check=True, capture_output=True, text=True)
        with wave.open(str(output), "rb") as stream:
            assert (stream.getnchannels(), stream.getsampwidth(), stream.getframerate(), stream.getnframes()) == (1, 2, 48000, 384000)
            data = stream.readframes(384000)
        assert data[:96000 * 2] == bytes(96000 * 2), "sound before queued bar"
        assert data[288000 * 2:] == bytes(96000 * 2), "sound after stop"
        assert any(data[96000 * 2:288000 * 2]), "musical interval is silent"
        if baseline is None:
            baseline = data
        assert data == baseline, f"PCM depends on block size {block}"
        original = output.read_bytes()
        assert subprocess.run([binary, str(output)], capture_output=True).returncode != 0
        assert output.read_bytes() == original, "existing output was modified"
    for argument in ("0", "-1", "8193", "257garbage"):
        target = root / "invalid.wav"
        assert subprocess.run([binary, str(target), argument], capture_output=True).returncode != 0
        assert not target.exists()
    print("PASS: mono PCM16 WAV, 384000 frames, exact silent boundaries, identical PCM across five block sizes, output preservation")
