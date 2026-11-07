"""Plays an independently authored Standard MIDI File with pw-midiplay into
fl-live-midi-input on an owned private PipeWire graph and checks the recorded
notes against the file's own timeline. No devices or session manager.

    pipewire_midi_capture.py MIDI_CAPTURE_PROBE          # the MIDI input and MidiTake alone
    pipewire_midi_capture.py --desktop DESKTOP_PROBE     # the desktop Session: arm, record, piano-roll clip

At 120 BPM and 96 ticks per quarter note one tick is exactly 250 frames at
48 kHz, so the expected frames follow from the file alone. Recorded starts
are compared relative to the first note, because the take starts before the
file does; lengths, pitches and velocities are compared directly."""
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import time

desktop = "--desktop" in sys.argv
probe = str(pathlib.Path(sys.argv[-1]).resolve())
TICK = 250  # frames per tick
LEAD = 384  # ticks of silence before the first note, time to link
# (start tick, length ticks, pitch, velocity): a chord, overlapping notes,
# adjacent repeated pitches and the velocity extremes.
NOTES = [(0, 48, 60, 100), (96, 48, 64, 90), (192, 96, 67, 80), (192, 96, 48, 70), (240, 24, 72, 127),
         (300, 30, 72, 1), (330, 6, 72, 64), (400, 200, 36, 55)]


def smf():
    def vlq(n):
        out = [n & 0x7F]
        n >>= 7
        while n:
            out.insert(0, (n & 0x7F) | 0x80)
            n >>= 7
        return bytes(out)
    events = [(0, b"\xff\x51\x03\x07\xa1\x20")]  # 500000 us per quarter note
    for start, length, pitch, velocity in NOTES:
        events.append((LEAD + start, bytes([0x90, pitch, velocity])))
        events.append((LEAD + start + length, bytes([0x80, pitch, 0])))
    events.sort(key=lambda e: (e[0], e[1][0] == 0x90))  # offs before ons at the same tick
    track, last = b"", 0
    for tick, data in events:
        track += vlq(tick - last) + data
        last = tick
    track += vlq(96) + b"\xff\x2f\x00"
    return b"MThd" + struct.pack(">IHHH", 6, 0, 1, 96) + b"MTrk" + struct.pack(">I", len(track)) + track


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


with tempfile.TemporaryDirectory(prefix="fl-live-midi-") as temporary:
    root = pathlib.Path(temporary)
    root.chmod(0o700)
    remote = "fl-live-midi"
    env = dict(os.environ, XDG_RUNTIME_DIR=str(root), PIPEWIRE_RUNTIME_DIR=str(root), PIPEWIRE_REMOTE=remote,
               XDG_CONFIG_HOME=str(root / "config"), DBUS_SESSION_BUS_ADDRESS="unix:path=/nonexistent", PIPEWIRE_DEBUG="0")
    (root / "config" / "pipewire").mkdir(parents=True)
    (root / "config" / "pipewire" / "client.conf").write_text('''
context.properties = { support.dbus = false }
context.spa-libs = { audio.convert.* = audioconvert/libspa-audioconvert support.* = support/libspa-support }
context.modules = [ { name = libpipewire-module-protocol-native } { name = libpipewire-module-client-node }
                    { name = libpipewire-module-adapter } { name = libpipewire-module-metadata } ]
''')
    configuration = root / "private.conf"
    configuration.write_text('''
context.properties = { core.daemon = true core.name = fl-live-midi support.dbus = false
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
    song = root / "reference.mid"
    song.write_bytes(smf())
    server = player = recorder = None
    with (root / "server.log").open("wb") as log:
        try:
            server = subprocess.Popen(["pipewire", "-c", str(configuration)], env=env, stdout=log, stderr=log)
            until(lambda: (root / remote).exists() if server.poll() is None else False)
            (root / "project").mkdir()
            recorder = subprocess.Popen([probe, remote, "--record-midi", str(root / "project")] if desktop else [probe, remote], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            assert json.loads(recorder.stdout.readline())["ready"]
            recorder.stdin.write("record\n"); recorder.stdin.flush()
            assert json.loads(recorder.stdout.readline())["recording"]

            def ports(direction):
                return subprocess.run(["pw-link", "--remote", remote, direction], env=env, capture_output=True, text=True, timeout=2).stdout.split()
            player = subprocess.Popen(["pw-midiplay", "--remote", remote, "--target", "0", "--properties",
                "{ node.name = fixture-midi node.group = pipewire.test node.always-process = true }", str(song)],
                env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            out_port = until(lambda: next((p for p in ports("--output") if p.startswith("fixture-midi:")), None))
            in_port = until(lambda: next((p for p in ports("--input") if p.startswith("fl-live-midi-input:")), None))
            subprocess.run(["pw-link", "--remote", remote, out_port, in_port], env=env, check=True, capture_output=True, timeout=2)
            player.wait(timeout=15)  # the file is about 4.5 s long
            time.sleep(0.2)
            recorder.stdin.write("stop\n"); recorder.stdin.flush()
            take = json.loads(recorder.stdout.readline())
            assert recorder.wait(timeout=5) == 0, recorder.stderr.read()
        finally:
            stop(recorder); stop(player); stop(server)
    if desktop:
        # The clip holds the file's notes in ticks: same tempo, same 96 ticks
        # per beat, so positions relative to the first note and lengths match
        # exactly; velocity 1..127 maps to percent as (v * 100 + 63) // 127.
        assert take["clips"] == take["before"] + 1 and take["selected_last"], take
        assert take["message"].startswith("Recorded 1 take as new clips"), take
        notes = [tuple(n) for n in take["notes"]]
        origin = notes[0][0]
        expected = sorted(((s, l, p, max(1, (v * 100 + 63) // 127)) for s, l, p, v in NOTES), key=lambda n: (n[0], n[2]))
        recorded = sorted(((s - origin, l, p, v) for s, l, p, v in notes), key=lambda n: (n[0], n[2]))
        assert recorded == expected, list(zip(recorded, expected))
        print(json.dumps({"notes": len(notes), "first_note_tick": origin, "clip_length_ticks": take["length"]}))
        print("PASS: desktop MIDI recording: the armed MIDI track's take becomes the selected piano-roll clip holding every note "
              "of the independently authored file at its exact tick, length, pitch and mapped velocity")
        sys.exit(0)
    assert take["error"] == 0 and take["bad_buffers"] == 0 and take["dropped"] == 0 and take["closed_at_end"] == 0, take
    assert take["events"] == 2 * len(NOTES), take
    notes = [tuple(n) for n in take["notes"]]
    assert len(notes) == len(NOTES), notes
    origin = notes[0][0]
    expected = sorted(((s * TICK, l * TICK, p, v) for s, l, p, v in NOTES), key=lambda n: (n[0], n[2]))
    recorded = [(start - origin, length, pitch, velocity) for start, length, pitch, velocity in notes]
    worst = max(max(abs(r[0] - e[0]), abs(r[1] - e[1])) for r, e in zip(recorded, expected))
    assert all(r[2:] == e[2:] for r, e in zip(recorded, expected)), (recorded, expected)
    assert worst == 0, f"timing differs from the file by up to {worst} frames: {list(zip(recorded, expected))}"
    assert origin >= LEAD * TICK // 4, f"the first note came {origin} frames into the take; the link may have missed notes"
    print(json.dumps({"notes": len(notes), "events": take["events"], "first_note_frame": origin, "take_frames": take["frames"], "worst_offset_frames": worst}))
    print("PASS: every note of an independently authored MIDI file arrives in fl-live-midi-input with its pitch and velocity "
          "and the file's exact frame timing relative to the first note")
