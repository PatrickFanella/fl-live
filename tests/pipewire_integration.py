"""Owned private PipeWire graph; no ALSA, desktop remote or service changes."""
import json
import math
import statistics
import os
import pathlib
import selectors
import struct
import subprocess
import sys
import tempfile
import time


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)


def until(predicate, timeout=4):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.01)
    raise AssertionError("private graph observation timed out")


def ready(process):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        if not selector.select(timeout=4):
            raise AssertionError("playback ready response timed out")
        line = process.stdout.readline()
    assert line and json.loads(line)["ready"], (line, process.poll(), process.stderr.read() if process.poll() is not None else "still running")


def verify_a4(samples):
    """Pitch qualification tolerates bounded private-graph capture interruptions.

    This is not an xrun or lossless-continuity assertion. Verify individual
    periods so a few missing cycles cannot masquerade as a different pitch.
    """
    edges = [i for i, (a, b) in enumerate(zip(samples, samples[1:])) if a <= 0 < b]
    periods = [b - a for a, b in zip(edges, edges[1:])]
    stable = sum(period in (109, 110) for period in periods)  # 48000 / 440
    assert len(samples) == 24000 and stable >= 200, (len(samples), len(edges), stable)
    median = statistics.median(periods)
    assert 109 <= median <= 110, median
    return dict(positive_crossings=len(edges), stable_periods=stable,
                discontinuous_periods=len(periods)-stable, median_period_frames=median)


def pitch_check_regression():
    tone = [0.02 * math.sin(2 * math.pi * 440 * i / 48000) for i in range(24000)]
    verify_a4(tone)
    interrupted = tone.copy()
    interrupted[8000:8256] = [0.0] * 256
    result = verify_a4(interrupted)
    assert result["positive_crossings"] == 218, result
    for invalid in ([0.02 * math.sin(2 * math.pi * 432 * i / 48000) for i in range(24000)],
                    [0.0] * 24000, tone[:12000] + [0.0]*12000):
        try:
            verify_a4(invalid)
        except AssertionError:
            continue
        raise AssertionError("pitch check accepted an incorrect or insufficient tone")


pitch_check_regression()

binary = str(pathlib.Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="fl-live-pw-") as temporary:
    root = pathlib.Path(temporary)
    root.chmod(0o700)
    remote = "fl-live-private"
    env = dict(os.environ, XDG_RUNTIME_DIR=str(root), PIPEWIRE_RUNTIME_DIR=str(root),
               PIPEWIRE_REMOTE=remote, XDG_CONFIG_HOME=str(root / "config"),
               DBUS_SESSION_BUS_ADDRESS="unix:path=/nonexistent", PIPEWIRE_DEBUG="0")
    # Own the client configuration as well as the server. This software-only
    # test does not use desktop RTKit/portal scheduling or a session manager.
    client_dir = root / "config" / "pipewire"
    client_dir.mkdir(parents=True)
    (client_dir / "client.conf").write_text('''
context.properties = { support.dbus = false }
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.* = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-metadata }
]
''')
    configuration = root / "private.conf"
    configuration.write_text('''
context.properties = {
    core.daemon = true
    core.name = fl-live-private
    support.dbus = false
    default.clock.rate = 48000
    default.clock.allowed-rates = [ 48000 ]
    default.clock.quantum = 128
    default.clock.min-quantum = 128
    default.clock.max-quantum = 4096
}
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.* = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-access args = { access.force = unrestricted } }
]
context.objects = [
    { factory = spa-node-factory
      args = { factory.name = support.node.driver node.name = Test-Driver
               node.group = pipewire.test priority.driver = 20000 } }
]
''')
    server = player = capture = None
    with (root / "server.log").open("wb") as server_log:
        try:
            server = subprocess.Popen(["pipewire", "-c", str(configuration)], env=env,
                                      stdout=server_log, stderr=server_log)
            until(lambda: (root / remote).exists() if server.poll() is None else False)
            player = subprocess.Popen([binary, "--remote", remote, "--group", "pipewire.test", "--duration-ms", "3000"],
                                      env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            ready(player)
            capture = subprocess.Popen(["pw-cat", "--remote", remote, "--record", "--raw", "--format", "f32",
                "--rate", "48000", "--channels", "1", "--channel-map", "MONO", "--target", "0",
                "--sample-count", "24000", "--properties",
                "{ node.name = fixture-capture node.group = pipewire.test node.always-process = true }", "-"],
                env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            def configure_node(name, direction):
                snapshot = subprocess.run(["pw-dump", "--remote", remote], env=env,
                                          capture_output=True, text=True, timeout=2)
                assert snapshot.returncode == 0, snapshot.stderr
                node = next((v for v in json.loads(snapshot.stdout)
                             if v.get("info", {}).get("props", {}).get("node.name") == name), None)
                if not node:
                    return False
                # Explicitly perform the port setup normally supplied by a
                # session manager. Only nodes in this private daemon are touched.
                parameter = ("{ direction = " + direction + " mode = dsp format = { mediaType = audio "
                             "mediaSubtype = raw format = F32 rate = 48000 channels = 1 position = [ MONO ] } }")
                result = subprocess.run(["pw-cli", "--remote", remote, "set-param", str(node["id"]),
                                         "PortConfig", parameter], env=env, capture_output=True, text=True, timeout=2)
                assert result.returncode == 0, result.stderr
                return True

            until(lambda: configure_node("fl-live-output", "Output"))
            until(lambda: configure_node("fixture-capture", "Input"))

            def port(direction, prefix):
                command = subprocess.run(["pw-link", "--remote", remote, direction], env=env,
                                         capture_output=True, text=True, timeout=2)
                assert command.returncode == 0, command.stderr
                return next((line.strip() for line in command.stdout.splitlines() if line.startswith(prefix)), None)

            output_port = until(lambda: port("--output", "fl-live-output:"))
            input_port = until(lambda: port("--input", "fixture-capture:"))
            subprocess.run(["pw-link", "--remote", remote, output_port, input_port], env=env,
                           check=True, capture_output=True, timeout=2)
            data, errors = capture.communicate(timeout=5)
            # PipeWire 1.6.9 pw-cat's finite capture limit quits without setting
            # `drained`, while main returns success only for that flag. Preserve
            # the observed code and require exact payload length/no diagnostic.
            # https://github.com/PipeWire/pipewire/blob/1.6.9/src/tools/pw-cat.c
            assert capture.returncode in (0, 1) and not errors, (capture.returncode, len(data), errors.decode())
            capture_code = capture.returncode
            assert len(data) == 24000 * 4, (len(data), errors.decode())
            values = struct.unpack("<24000f", data)
            expected = (0.0625, 0.125, 0.1875, 0.25, -0.0625, -0.125, -0.1875, -0.25)
            first = next(i for i, value in enumerate(values) if value != 0)
            # Ignore only a possible graph-start silent prefix; the captured
            # non-silent span must be the engine waveform with no missing samples.
            offset = expected.index(values[first])
            assert len(values) - first >= 20000, "insufficient engine audio captured"
            assert all(value == expected[(i + offset) % 8] for i, value in enumerate(values[first:])), "capture differs from authored engine output"
            remaining, errors = player.communicate(timeout=4)
            assert player.returncode == 0, (remaining, errors)
            stats = json.loads(remaining.strip())
            assert stats["callbacks"] > 0 and stats["frames"] >= 24000 and stats["error"] == 0, stats
            assert stats["bad_buffers"] == stats["rejected"] == 0 and stats["handled_serial"] == 2 and not stats["running"], stats

            project_rebuilds = None
            if len(sys.argv) > 2:
                probe = subprocess.run([str(pathlib.Path(sys.argv[2]).resolve()), remote],
                                       env=dict(env, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software"),
                                       capture_output=True, text=True, timeout=15)
                assert probe.returncode == 0, (probe.stdout, probe.stderr)
                assert "five stopped project rebuilds including Undo/Redo" in probe.stdout, probe.stdout
                project_rebuilds = probe.stdout.strip()

            desktop_workflow = None
            if len(sys.argv) > 2:
                player = subprocess.Popen([str(pathlib.Path(sys.argv[2]).resolve()), remote, "--workflow"],
                    env=dict(env, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software"),
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                ready(player)
                capture = subprocess.Popen(["pw-cat", "--remote", remote, "--record", "--raw", "--format", "f32",
                    "--rate", "48000", "--channels", "1", "--channel-map", "MONO", "--target", "0",
                    "--sample-count", "144000", "--properties",
                    "{ node.name = fixture-desktop-capture node.group = pipewire.test node.always-process = true }", "-"],
                    env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                until(lambda: configure_node("fl-live-output", "Output"))
                until(lambda: configure_node("fixture-desktop-capture", "Input"))
                subprocess.run(["pw-link", "--remote", remote,
                    until(lambda: port("--output", "fl-live-output:")),
                    until(lambda: port("--input", "fixture-desktop-capture:"))],
                    env=env, check=True, capture_output=True, timeout=2)
                player.stdin.write("g"); player.stdin.flush()
                audio, errors = capture.communicate(timeout=5)
                assert capture.returncode in (0, 1) and not errors and len(audio)==144000*4
                samples = struct.unpack("<144000f", audio)
                # Independent pitch/gain check, away from the quantized launch.
                # One 200ms step repeats a 440Hz sine at 0.1*0.5*0.4 gain.
                tail = samples[72000:96000]
                peak = max(abs(v) for v in tail)
                pitch = verify_a4(tail)
                assert 0.0199 < peak < 0.0201, peak
                assert all(v == 0 for v in samples[132000:]), "audio after acknowledged stop"
                output, errors = player.communicate(timeout=5)
                assert player.returncode == 0, (output, errors)
                assert "QML edits, play, MIDI launch and acknowledged stop" in output
                desktop_workflow = dict(frames=144000, peak=peak, pitch=pitch, stopped_silence_frames=12000)

            # A second actual client must report the loss of this owned server
            # and exit; this does not touch any pre-existing server or stream.
            player = subprocess.Popen([binary, "--remote", remote, "--group", "pipewire.test", "--duration-ms", "5000"],
                                      env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            ready(player)
            capture = subprocess.Popen(["pw-cat", "--remote", remote, "--record", "--raw", "--format", "f32",
                "--rate", "48000", "--channels", "1", "--channel-map", "MONO", "--target", "0",
                "--sample-count", "1024", "--properties",
                "{ node.name = fixture-loss-capture node.group = pipewire.test node.always-process = true }", "-"],
                env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            until(lambda: configure_node("fl-live-output", "Output"))
            until(lambda: configure_node("fixture-loss-capture", "Input"))
            loss_input = until(lambda: port("--input", "fixture-loss-capture:"))
            loss_output = until(lambda: port("--output", "fl-live-output:"))
            subprocess.run(["pw-link", "--remote", remote, loss_output, loss_input], env=env,
                           check=True, capture_output=True, timeout=2)
            loss_data, loss_errors = capture.communicate(timeout=3)
            assert len(loss_data) == 4096 and not loss_errors, (len(loss_data), loss_errors)
            stop(server)
            tail, errors = player.communicate(timeout=4)
            assert player.returncode != 0, (tail, errors)
            lost = json.loads(tail.strip())
            assert lost["error"] != 0 and lost["callbacks"] > 0 and not lost["running"], lost
            print(json.dumps(dict(passed=True,scope="private software graph; no hardware/audio-device qualification",
                                  captured_frames=len(values), capture_returncode=capture_code,
                                  silent_prefix=first, playback=stats, server_loss=lost, project_rebuilds=project_rebuilds, desktop_workflow=desktop_workflow)))
        except Exception:
            print((root / "server.log").read_text(errors="replace")[-2000:], file=sys.stderr)
            raise
        finally:
            stop(capture)
            stop(player)
            stop(server)
    absent = subprocess.run([binary, "--remote", remote, "--duration-ms", "100"], env=env,
                            capture_output=True, text=True, timeout=5)
    assert absent.returncode != 0 and "ready" not in absent.stdout
