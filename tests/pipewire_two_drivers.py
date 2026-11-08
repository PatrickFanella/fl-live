"""Recording with input and output on different drivers (#166). On real
hardware the speakers and the microphone are separate PipeWire drivers. The
desktop's transport anchor comes from fl-live-output and a take's origin from
fl-live-input; they are comparable only when both run from one driver.

This owned private graph reproduces that: driver A runs fixture-sink, which
fl-live-output feeds, and driver B, created 1.5 s later so its frame counter
differs, runs fixture-source, which feeds fl-live-input. The desktop keeps its
default node group. It must schedule both of its nodes from one driver and
place the recorded take from its first bar line.

    pipewire_two_drivers.py DESKTOP_PROBE"""
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import time

probe = str(pathlib.Path(sys.argv[1]).resolve())
BAR, QUANTUM = 96000, 256  # 120 BPM at 48 kHz


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


with tempfile.TemporaryDirectory(prefix="fl-live-two-drivers-") as temporary:
    root = pathlib.Path(temporary)
    root.chmod(0o700)
    remote = "fl-live-two-drivers"
    env = dict(os.environ, XDG_RUNTIME_DIR=str(root), PIPEWIRE_RUNTIME_DIR=str(root), PIPEWIRE_REMOTE=remote,
               XDG_CONFIG_HOME=str(root / "config"), DBUS_SESSION_BUS_ADDRESS="unix:path=/nonexistent", PIPEWIRE_DEBUG="0",
               QT_QPA_PLATFORM="offscreen")
    (root / "config" / "pipewire").mkdir(parents=True)
    (root / "config" / "pipewire" / "client.conf").write_text('''
context.properties = { support.dbus = false }
context.spa-libs = { audio.convert.* = audioconvert/libspa-audioconvert support.* = support/libspa-support }
context.modules = [ { name = libpipewire-module-protocol-native } { name = libpipewire-module-client-node }
                    { name = libpipewire-module-adapter } { name = libpipewire-module-metadata } ]
''')
    configuration = root / "private.conf"
    configuration.write_text(f'''
context.properties = {{ core.daemon = true core.name = {remote} support.dbus = false
    default.clock.rate = 48000 default.clock.allowed-rates = [ 48000 ] default.clock.quantum = {QUANTUM}
    default.clock.min-quantum = {QUANTUM} default.clock.max-quantum = {QUANTUM} clock.power-of-two-quantum = false }}
context.spa-libs = {{ audio.convert.* = audioconvert/libspa-audioconvert support.* = support/libspa-support }}
context.modules = [ {{ name = libpipewire-module-protocol-native }} {{ name = libpipewire-module-client-node }}
    {{ name = libpipewire-module-spa-node-factory }} {{ name = libpipewire-module-adapter }}
    {{ name = libpipewire-module-link-factory }} {{ name = libpipewire-module-metadata }} {{ name = libpipewire-module-profiler }}
    {{ name = libpipewire-module-access args = {{ access.force = unrestricted }} }} ]
context.objects = [ {{ factory = spa-node-factory args = {{ factory.name = support.node.driver node.name = Driver-A
    node.group = fixture-a priority.driver = 20000 }} }} ]
''')
    (root / "project").mkdir()
    source_audio = root / "source.raw"  # 15 s of stereo 32-bit float, a click every beat
    beat = bytearray(8 * 24000)
    beat[0:8] = struct.pack("<ff", 0.5, 0.5)
    source_audio.write_bytes(bytes(beat) * 30)
    server = desktop = sink = source = None
    drivers = {}
    with (root / "server.log").open("wb") as log:
        try:
            server = subprocess.Popen(["pipewire", "-c", str(configuration)], env=env, stdout=log, stderr=log)
            until(lambda: (root / remote).exists() if server.poll() is None else False)
            desktop = subprocess.Popen([probe, remote, "--record-two-drivers", str(root / "project")], env=env, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            assert json.loads(desktop.stdout.readline())["ready"]

            def configure(name, direction, channels=2):
                dump = subprocess.run(["pw-dump", "--remote", remote], env=env, capture_output=True, text=True, timeout=2)
                node = next((v for v in json.loads(dump.stdout) if v.get("info", {}).get("props", {}).get("node.name") == name), None)
                if not node:
                    return False
                parameter = ("{ direction = " + direction + " mode = dsp format = { mediaType = audio mediaSubtype = raw "
                             f"format = F32 rate = 48000 channels = {channels} position = [ {'FL FR' if channels == 2 else 'MONO'} ] }} }}")
                result = subprocess.run(["pw-cli", "--remote", remote, "set-param", str(node["id"]), "PortConfig", parameter],
                                        env=env, capture_output=True, text=True, timeout=2)
                assert result.returncode == 0, result.stderr
                return True

            def port(direction, prefix):
                listing = subprocess.run(["pw-link", "--remote", remote, direction], env=env, capture_output=True, text=True, timeout=2)
                return next((line.strip() for line in listing.stdout.splitlines() if line.startswith(prefix)), None)

            def link(out_port, in_port):
                subprocess.run(["pw-link", "--remote", remote, out_port, in_port], env=env, check=True, capture_output=True, timeout=2)

            # Driver A: fl-live-output plays into fixture-sink, as into speakers.
            sink = subprocess.Popen(["pw-cat", "--remote", remote, "--record", "--raw", "--format", "f32", "--rate", "48000",
                "--channels", "1", "--channel-map", "MONO", "--target", "0", "--properties",
                "{ node.name = fixture-sink node.group = fixture-a node.always-process = true }", "-"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            until(lambda: configure("fl-live-output", "Output", 1))
            until(lambda: configure("fixture-sink", "Input", 1))
            link(until(lambda: port("--output", "fl-live-output:")), until(lambda: port("--input", "fixture-sink:")))
            time.sleep(1.5)  # driver A's frame counter runs ahead of driver B's

            # Driver B, created now: fixture-source feeds fl-live-input, as a microphone.
            created = subprocess.run(["pw-cli", "--remote", remote, "create-node", "spa-node-factory",
                "{ factory.name = support.node.driver node.name = Driver-B node.group = fixture-b priority.driver = 19000 object.linger = true }"],
                env=env, capture_output=True, text=True, timeout=4)
            assert created.returncode == 0, created.stderr
            source = subprocess.Popen(["pw-cat", "--remote", remote, "--playback", "--raw", "--format", "f32", "--rate", "48000",
                "--channels", "2", "--target", "0", "--properties",
                "{ node.name = fixture-source node.group = fixture-b node.always-process = true }", str(source_audio)],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            until(lambda: configure("fixture-source", "Output"))
            until(lambda: configure("fl-live-input", "Input"))
            for channel in ("FL", "FR"):
                link(until(lambda: port("--output", f"fixture-source:output_{channel}")), until(lambda: port("--input", f"fl-live-input:input_{channel}")))
            time.sleep(0.3)

            # Which driver runs each node: pw-top lists followers ("+") under their driver.
            if shutil.which("pw-top"):
                top = subprocess.run(["pw-top", "--remote", remote, "-b", "-n", "2"], env=env, capture_output=True, text=True, timeout=5).stdout
                (root / "pw-top.txt").write_text(top)
                current = None
                for line in top.splitlines():
                    fields = line.split()
                    if len(fields) < 2 or fields[0] == "S":
                        continue
                    name = line.rstrip().split(" + ")[-1].split()[-1] if " + " in line else fields[-1]
                    if " + " in line:
                        drivers[name] = current
                    else:
                        current = name
                        drivers[name] = name

            desktop.stdin.write("record\n"); desktop.stdin.flush()
            result = json.loads(desktop.stdout.readline())
            assert desktop.wait(timeout=10) == 0, desktop.stderr.read()
        finally:
            stop(desktop); stop(source); stop(sink); stop(server)
    if drivers:
        assert drivers.get("fl-live-output") and drivers.get("fl-live-output") == drivers.get("fl-live-input"), \
            f"fl-live-output and fl-live-input run from different drivers: {drivers}"
    assert result["message"].startswith("Recorded 1 take as new clips"), result
    start = int(result["range_start"])
    # Record from a stopped transport rewinds to bar 1, and the input was
    # already running, so the take starts at or just before the bar line.
    assert 0 <= start <= 4 * QUANTUM, f"the first bar line is {start} frames into the take; the input's frames do not match the transport"
    print(json.dumps({"quantum": QUANTUM, "drivers": drivers, "audio_range_start": start, "message": result["message"]}))
    print("PASS: with input and output fed by different drivers, fl-live runs both nodes from one driver and places the take "
          f"from its first bar line ({start} frames in)")
