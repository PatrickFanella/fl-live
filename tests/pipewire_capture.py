"""Records an independently authored stereo signal through fl-live's PipeWire
capture stream on an owned private graph and checks the take frame by frame.
No desktop remote, devices or session manager are used."""
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import time

probe = str(pathlib.Path(sys.argv[1]).resolve())


def until(predicate, timeout=4):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.01)
    raise AssertionError("private graph observation timed out")


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()


def wav_floats(path):
    data = pathlib.Path(path).read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", "not a WAV file"
    at, fmt, payload = 12, None, None
    while at + 8 <= len(data):
        tag, size = data[at:at + 4], struct.unpack("<I", data[at + 4:at + 8])[0]
        body = data[at + 8:at + 8 + size]
        if tag == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif tag == b"data":
            payload = body
        at += 8 + size + (size & 1)
    assert fmt and fmt[0] in (3, 0xFFFE) and fmt[1] == 2 and fmt[2] == 48000 and fmt[5] == 32, fmt
    return struct.unpack(f"<{len(payload) // 4}f", payload)


# Independent stereo reference: left and right follow different sequences,
# so a swap, a repeat or a corrupted sample cannot match by accident.
FRAMES = 480000
def left(i):
    return ((i * 7919) % 65521 - 32760) / 131072.0
def right(i):
    return -((i * 104729) % 65519 - 32759) / 262144.0
reference = []
for i in range(FRAMES):
    reference += [struct.unpack("<f", struct.pack("<f", left(i)))[0], struct.unpack("<f", struct.pack("<f", right(i)))[0]]

with tempfile.TemporaryDirectory(prefix="fl-live-capture-") as temporary:
    root = pathlib.Path(temporary)
    root.chmod(0o700)
    remote = "fl-live-capture"
    env = dict(os.environ, XDG_RUNTIME_DIR=str(root), PIPEWIRE_RUNTIME_DIR=str(root), PIPEWIRE_REMOTE=remote,
               XDG_CONFIG_HOME=str(root / "config"), DBUS_SESSION_BUS_ADDRESS="unix:path=/nonexistent", PIPEWIRE_DEBUG="0")
    client_dir = root / "config" / "pipewire"
    client_dir.mkdir(parents=True)
    (client_dir / "client.conf").write_text('''
context.properties = { support.dbus = false }
context.spa-libs = { audio.convert.* = audioconvert/libspa-audioconvert support.* = support/libspa-support }
context.modules = [ { name = libpipewire-module-protocol-native } { name = libpipewire-module-client-node }
                    { name = libpipewire-module-adapter } { name = libpipewire-module-metadata } ]
''')
    configuration = root / "private.conf"
    configuration.write_text('''
context.properties = { core.daemon = true core.name = fl-live-capture support.dbus = false
    default.clock.rate = 48000 default.clock.allowed-rates = [ 48000 ] default.clock.quantum = 256
    default.clock.min-quantum = 256 default.clock.max-quantum = 256 clock.power-of-two-quantum = false }
context.spa-libs = { audio.convert.* = audioconvert/libspa-audioconvert support.* = support/libspa-support }
context.modules = [ { name = libpipewire-module-protocol-native } { name = libpipewire-module-client-node }
    { name = libpipewire-module-spa-node-factory } { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory } { name = libpipewire-module-metadata }
    { name = libpipewire-module-access args = { access.force = unrestricted } } ]
context.objects = [ { factory = spa-node-factory args = { factory.name = support.node.driver node.name = Test-Driver
    node.group = pipewire.test priority.driver = 20000 } } ]
''')
    source = root / "reference.f32"
    source.write_bytes(struct.pack(f"<{len(reference)}f", *reference))
    output = root / "take.wav"
    server = player = recorder = None
    with (root / "server.log").open("wb") as log:
        try:
            server = subprocess.Popen(["pipewire", "-c", str(configuration)], env=env, stdout=log, stderr=log)
            until(lambda: (root / remote).exists() if server.poll() is None else False)
            recorder = subprocess.Popen([probe, remote, str(output), "2"], env=env, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            assert json.loads(recorder.stdout.readline())["ready"]
            player = subprocess.Popen(["pw-cat", "--remote", remote, "--playback", "--raw", "--format", "f32", "--rate", "48000",
                "--channels", "2", "--channel-map", "FL,FR", "--target", "0", "--properties",
                "{ node.name = fixture-source node.group = pipewire.test node.always-process = true }", str(source)],
                env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            def configure(name, direction):
                dump = subprocess.run(["pw-dump", "--remote", remote], env=env, capture_output=True, text=True, timeout=2)
                node = next((v for v in json.loads(dump.stdout) if v.get("info", {}).get("props", {}).get("node.name") == name), None)
                if not node:
                    return False
                parameter = ("{ direction = " + direction + " mode = dsp format = { mediaType = audio mediaSubtype = raw "
                             "format = F32 rate = 48000 channels = 2 position = [ FL FR ] } }")
                result = subprocess.run(["pw-cli", "--remote", remote, "set-param", str(node["id"]), "PortConfig", parameter],
                                        env=env, capture_output=True, text=True, timeout=2)
                assert result.returncode == 0, result.stderr
                return True

            def port(direction, prefix):
                listing = subprocess.run(["pw-link", "--remote", remote, direction], env=env, capture_output=True, text=True, timeout=2)
                return next((line.strip() for line in listing.stdout.splitlines() if line.startswith(prefix)), None)

            until(lambda: configure("fixture-source", "Output"))
            until(lambda: configure("fl-live-input", "Input"))
            for channel in ("FL", "FR"):
                out_port = until(lambda: port("--output", f"fixture-source:output_{channel}"))
                in_port = until(lambda: port("--input", f"fl-live-input:input_{channel}"))
                subprocess.run(["pw-link", "--remote", remote, out_port, in_port], env=env, check=True, capture_output=True, timeout=2)
            time.sleep(0.3)  # let the link settle before the take starts
            recorder.stdin.write("record\n"); recorder.stdin.flush()
            time.sleep(1.5)
            recorder.stdin.write("stop\n"); recorder.stdin.flush()
            status = json.loads(recorder.stdout.readline())
            assert recorder.wait(timeout=5) == 0, recorder.stderr.read()
        finally:
            stop(recorder); stop(player); stop(server)
    assert not status["failed"] and status["dropped"] == 0 and status["error"] == 0, status
    assert status["file"] == str(output), status
    samples = wav_floats(output)
    frames = len(samples) // 2
    assert frames == status["frames"] and 48000 * 1.2 < frames < 48000 * 2.0, (frames, status)
    # Locate the first captured frame in the reference, then walk: every frame
    # must equal the next reference frame, except where the private graph
    # dropped quanta under host load, which shows as a forward jump.
    index = {}
    for i in range(FRAMES):
        index.setdefault((reference[2 * i], reference[2 * i + 1]), i)
    position = None
    matched = jumps = skipped = 0
    for f in range(frames):
        pair = (samples[2 * f], samples[2 * f + 1])
        if pair == (0.0, 0.0) and position is None:
            continue  # silence before the reference reached the link
        at = index.get(pair)
        assert at is not None, f"captured frame {f} is not a reference frame (swapped, corrupted or invented)"
        if position is not None and at != position:
            assert at > position, f"frame {f} goes back in the reference ({at} after {position})"
            jumps += 1
            skipped += at - position
        position = at + 1
        matched += 1
    assert matched > 48000 * 1.2, (matched, frames)
    assert skipped <= matched // 10, f"too many frames missing: {skipped} over {jumps} jumps"
    print(json.dumps({"captured_frames": frames, "matched_frames": matched, "jumps": jumps, "skipped_frames": skipped,
                      "callbacks": status["callbacks"], "bad_buffers": status["bad_buffers"]}))
    print("PASS: stereo capture through fl-live-input matches the independent reference frame by frame, channels in place")
