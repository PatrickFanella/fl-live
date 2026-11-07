"""Recording placement against an independent timeline (#45). On an owned
private PipeWire graph, graph-source emits clicks and MIDI notes at exact
graph-clock frames chosen from the desktop's transport anchor, so they fall
on known bar and beat lines. The desktop records both from the start of the
transport; the recorded clips must hold them on those lines.

    pipewire_placement.py DESKTOP_PROBE GRAPH_SOURCE             # events scheduled from the anchor
    pipewire_placement.py --loopback DESKTOP_PROBE GRAPH_SOURCE  # fl-live's own output fed back

The first mode checks MIDI and audio against events placed on the graph
clock through fl-live's transport anchor, so an anchor error would cancel
out. The loopback mode removes that dependence: the audio track plays a
one-bar clip with a click on frame 0, so the output clicks on every bar line
of the transport, and the output is linked back into fl-live-input. Any
anchor error or graph delay then shows as an offset of the clicks from the
recorded clip's bar lines.

Policy (docs/recording.md): no input latency compensation. The measured
offset between where an event was emitted and where it lands in the clip is
reported; the tolerance is one quantum (256 frames) for audio and zero for
MIDI, whose timestamps carry their offset in the cycle."""
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import time

loopback = "--loopback" in sys.argv
probe = str(pathlib.Path(sys.argv[-2]).resolve())
source_binary = str(pathlib.Path(sys.argv[-1]).resolve())
BAR, TICK, QUANTUM = 96000, 250, 256  # 120 BPM at 48 kHz; 96 ticks per beat
CLICKS = [BAR, 2 * BAR, 3 * BAR]  # bars 2, 3 and 4
NOTES = [(BAR, 60, 100, 12000), (BAR + 2 * 24000 + 12000, 64, 80, 6000), (2 * BAR + 24000, 67, 127, 24000), (3 * BAR + 3 * 24000, 72, 64, 12000)]


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
    at = 12
    while at + 8 <= len(data):
        tag, size = data[at:at + 4], struct.unpack("<I", data[at + 4:at + 8])[0]
        if tag == b"data":
            payload = data[at + 8:at + 8 + size]
            return struct.unpack(f"<{len(payload) // 4}f", payload)
        at += 8 + size + (size & 1)
    raise AssertionError("no data chunk")


with tempfile.TemporaryDirectory(prefix="fl-live-placement-") as temporary:
    root = pathlib.Path(temporary)
    root.chmod(0o700)
    remote = "fl-live-placement"
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
    {{ name = libpipewire-module-link-factory }} {{ name = libpipewire-module-metadata }}
    {{ name = libpipewire-module-access args = {{ access.force = unrestricted }} }} ]
context.objects = [ {{ factory = spa-node-factory args = {{ factory.name = support.node.driver node.name = Test-Driver
    node.group = pipewire.test priority.driver = 20000 }} }} ]
''')
    (root / "project").mkdir()
    click = root / "click.wav"  # one bar, mono 32-bit float, a click of 0.5 on frame 0
    payload = struct.pack("<f", 0.5) + bytes(4 * (BAR - 1))
    click.write_bytes(b"RIFF" + struct.pack("<I", 36 + len(payload)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, 1, 48000, 48000 * 4, 4, 32)
                      + b"data" + struct.pack("<I", len(payload)) + payload)
    server = desktop = source = sink = None
    with (root / "server.log").open("wb") as log:
        try:
            server = subprocess.Popen(["pipewire", "-c", str(configuration)], env=env, stdout=log, stderr=log)
            until(lambda: (root / remote).exists() if server.poll() is None else False)
            desktop = subprocess.Popen([probe, remote, "--record-loopback", str(root / "project"), str(click)] if loopback else
                                       [probe, remote, "--record-placement", str(root / "project")], env=env, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            assert json.loads(desktop.stdout.readline())["ready"]
            if not loopback:
                source = subprocess.Popen([source_binary, remote], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                assert json.loads(source.stdout.readline())["ready"]

            # The desktop output must be linked, as to a device, or the graph
            # never schedules it and the transport does not run.
            sink = subprocess.Popen(["pw-cat", "--remote", remote, "--record", "--raw", "--format", "f32", "--rate", "48000",
                "--channels", "1", "--channel-map", "MONO", "--target", "0", "--properties",
                "{ node.name = fixture-sink node.group = pipewire.test node.always-process = true }", "-"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

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
            if loopback:
                until(lambda: configure("fl-live-input", "Input"))
                until(lambda: configure("fl-live-output", "Output", 1))
                out_port = until(lambda: port("--output", "fl-live-output:"))
                for channel in ("FL", "FR"):
                    link(out_port, until(lambda: port("--input", f"fl-live-input:input_{channel}")))
                time.sleep(0.3)
                desktop.stdin.write("record\n"); desktop.stdin.flush()
                result = json.loads(desktop.stdout.readline())
                assert desktop.wait(timeout=10) == 0, desktop.stderr.read()
                raise StopIteration
            until(lambda: configure("fixture-clicks", "Output"))
            until(lambda: configure("fl-live-input", "Input"))
            for channel in ("FL", "FR"):
                link(until(lambda: port("--output", f"fixture-clicks:output_{channel}")), until(lambda: port("--input", f"fl-live-input:input_{channel}")))
            link(until(lambda: port("--output", "fixture-notes:")), until(lambda: port("--input", "fl-live-midi-input:")))
            until(lambda: configure("fl-live-output", "Output", 1))
            until(lambda: configure("fixture-sink", "Input", 1))
            link(until(lambda: port("--output", "fl-live-output:")), until(lambda: port("--input", "fixture-sink:")))
            time.sleep(0.3)
            desktop.stdin.write("record\n"); desktop.stdin.flush()
            anchor = json.loads(desktop.stdout.readline())
            assert anchor["valid"], (anchor, (stop(desktop), desktop.stderr.read()[-3000:])[1])
            zero = int(anchor["graph"]) - int(anchor["transport"])  # graph frame of transport frame 0
            for frame in CLICKS:
                source.stdin.write(f"click {zero + frame}\n")
            for frame, pitch, velocity, length in NOTES:
                source.stdin.write(f"note {zero + frame} {pitch} {velocity} {length}\n")
            source.stdin.flush()
            for _ in CLICKS + NOTES:
                assert json.loads(source.stdout.readline())["ok"]
            result = json.loads(desktop.stdout.readline())
            assert desktop.wait(timeout=10) == 0, desktop.stderr.read()
        except StopIteration:
            pass
        finally:
            stop(desktop); stop(source); stop(sink); stop(server)
    if loopback:
        assert result["message"].startswith("Recorded 1 take as new clips") and result["selected_is_take"], result
        samples = wav_floats(result["file"])
        start = int(result["range_start"])
        found = [f - start for f in range(len(samples) // 2) if abs(samples[2 * f]) > 1e-4]
        assert len(found) >= 4, (found, start)
        assert all(abs(samples[2 * (f + start)] - samples[2 * (f + start) + 1]) == 0 for f in found), "the mono output reached both channels"
        offsets = [f - round(f / BAR) * BAR for f in found]
        assert len(set(offsets)) == 1, f"clicks land at different offsets from their bar lines: {list(zip(found, offsets))}"
        assert [round(f / BAR) for f in found][:4] == [0, 1, 2, 3], found
        loop_offset = offsets[0]
        assert abs(loop_offset) <= QUANTUM, f"loopback offset {loop_offset} frames exceeds one quantum"
        print(json.dumps({"quantum": QUANTUM, "rate": 48000, "audio_range_start": start, "clicks": len(found), "loopback_offset_frames": loop_offset}))
        print(f"PASS: loopback: fl-live's own output, clicking on every transport bar line, lands {loop_offset} frames from the bar lines "
              "of the recorded clip, the same for every bar; no fixture scheduling depends on the transport anchor")
        sys.exit(0)
    assert result["message"].startswith("Recorded 2 takes as new clips"), result
    samples = wav_floats(result["file"])
    found = [f for f in range(len(samples) // 2) if samples[2 * f] == 0.5 and samples[2 * f + 1] == 0.25]
    assert len(found) == len(CLICKS), (found, len(samples) // 2)
    start = int(result["range_start"])
    audio_offsets = [(f - start) - expected for f, expected in zip(found, CLICKS)]
    assert len(set(audio_offsets)) == 1, f"clicks land at different offsets from their bar lines: {audio_offsets}"
    audio_offset = audio_offsets[0]
    assert abs(audio_offset) <= QUANTUM, f"audio offset {audio_offset} frames exceeds one quantum"
    notes = sorted(tuple(n) for n in result["notes"])
    expected = sorted((frame // TICK, length // TICK, pitch, (velocity * 100 + 63) // 127) for frame, pitch, velocity, length in NOTES)
    midi_offsets = [n[0] - e[0] for n, e in zip(notes, expected)]
    assert len(notes) == len(expected) and [n[1:] for n in notes] == [e[1:] for e in expected], (notes, expected)
    assert midi_offsets == [0] * len(expected), f"MIDI notes are off their beat lines by {midi_offsets} ticks: {notes}"
    print(json.dumps({"quantum": QUANTUM, "rate": 48000, "transport_zero_graph_frame": zero, "audio_range_start": start,
                      "audio_offset_frames": audio_offset, "midi_offset_ticks": 0, "clicks": len(found), "notes": len(notes)}))
    print(f"PASS: clicks emitted on bar lines land {audio_offset} frames from the bar lines of the recorded audio clip, the same for "
          "every bar; MIDI notes emitted on beat lines land on their exact ticks in the recorded piano-roll clip")
