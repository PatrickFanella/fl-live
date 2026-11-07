"""Owned private PipeWire graph; no ALSA, desktop remote or service changes."""
import json
import argparse
import concurrent.futures
import errno
import hashlib
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
    return json.loads(line)


def capture_prefix(process, count):
    data = bytearray()
    deadline = time.monotonic() + 3
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while len(data) < count:
            remaining = deadline - time.monotonic()
            assert remaining > 0 and selector.select(timeout=remaining), "capture startup timed out"
            chunk = os.read(process.stdout.fileno(), count - len(data))
            assert chunk, "capture exited before startup samples"
            data.extend(chunk)
    return bytes(data)


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

DROPPED_EXIT = 3
MAX_ATTEMPTS = 5


def authored_segments(frames, loop):
    """Split a capture into runs that each continue the authored loop exactly.

    frames are samples (mono) or (left, right) tuples; loop is the authored
    period in the same form. A frame that is exact silence between runs is a
    gap. Every other frame must be authored content at the current offset or
    start a new run at its own offset. Returns (segments, anomalies) where an
    anomaly is a "drop" (silence, then the source continued at the expected
    offset) or a "slip" (the source resumed at a different offset). Raises
    AssertionError for any frame that is neither silence nor authored content.
    """
    silent = (lambda f: f == 0) if not isinstance(loop[0], tuple) else (lambda f: f[0] == 0 and f[1] == 0)
    index = {}
    for i, value in enumerate(loop):
        index.setdefault(value, i)
    segments, anomalies = [], []
    i, n, period = 0, len(frames), len(loop)
    while i < n:
        if silent(frames[i]) and frames[i] not in index:
            gap = i
            while i < n and silent(frames[i]) and frames[i] not in index:
                i += 1
            if segments and i < n:
                anomalies.append(dict(frame=gap, silent_frames=i - gap))
            continue
        assert frames[i] in index, ("captured frame is not authored content", i, frames[i])
        start, offset = i, index[frames[i]]
        while i < n and frames[i] == loop[offset]:
            offset = (offset + 1) % period
            i += 1
        if segments:
            previous = segments[-1]
            gap = start - (previous[0] + previous[1])
            continued = (previous[2] + previous[1] + gap) % period == index[frames[start]]
            if anomalies and anomalies[-1]["frame"] == previous[0] + previous[1]:
                anomalies[-1]["kind"] = "drop" if continued else "slip"
            else:
                anomalies.append(dict(frame=start, silent_frames=0, kind="drop" if continued else "slip"))
        segments.append((start, i - start, index[frames[start]]))
    return segments, anomalies


def segmentation_regression():
    loop = (0.25, -0.5, 0.75, -1.0)
    clean = [loop[i % 4] for i in range(400)]
    assert authored_segments(clean, loop) == ([(0, 400, 0)], [])
    dropped = clean.copy(); dropped[100:228] = [0.0] * 128
    segments, anomalies = authored_segments(dropped, loop)
    assert len(segments) == 2 and anomalies == [dict(frame=100, silent_frames=128, kind="drop")], anomalies
    slipped = clean[:150] + clean[151:] + [loop[0]]
    assert authored_segments(slipped, loop)[1][0]["kind"] == "slip"
    for corrupt in (clean[:50] + [0.3] + clean[51:], clean[:50] + [0.25 * 1.0001] + clean[51:]):
        try:
            authored_segments(corrupt, loop)
        except AssertionError:
            continue
        raise AssertionError("segmentation accepted corrupted content")
    stereo = ((0.1, -0.1), (0.2, -0.2), (0.3, -0.3))
    swapped = [stereo[i % 3] for i in range(30)]; swapped[10] = (-0.2, 0.2)
    try:
        authored_segments(swapped, stereo)
    except AssertionError:
        pass
    else:
        raise AssertionError("segmentation accepted a channel swap")


segmentation_regression()


def fail_dropped(runs, attempt_scope):
    """Exit for the parent to retry a continuity control whose only failures
    were transport drops or slips; the full JSON was already printed."""
    print("DROPPED_CYCLES " + json.dumps(dict(scope=attempt_scope, runs=runs)), file=sys.stderr, flush=True)
    raise SystemExit(DROPPED_EXIT)


if os.environ.get("FL_LIVE_PRIVATE_GRAPH_ATTEMPT") is None:
    # Each attempt is a fresh private server. Only a run whose sole failures
    # are dropped whole cycles is repeated; everything else fails at once.
    dropped = []
    for attempt in range(1, MAX_ATTEMPTS + 1):
        child = subprocess.run([sys.executable, *sys.argv], capture_output=True, text=True,
                               env=dict(os.environ, FL_LIVE_PRIVATE_GRAPH_ATTEMPT=str(attempt)))
        sys.stdout.write(child.stdout)
        sys.stderr.write(child.stderr)
        if child.returncode != DROPPED_EXIT:
            if dropped:
                print(json.dumps(dict(retried_after_transport_anomalies=dropped, attempts=attempt,
                                      final_returncode=child.returncode)), flush=True)
            sys.exit(child.returncode)
        dropped.append(next(json.loads(line[len("DROPPED_CYCLES "):]) for line in child.stderr.splitlines()
                            if line.startswith("DROPPED_CYCLES ")))
    print(json.dumps(dict(passed=False, retried_after_transport_anomalies=dropped, attempts=MAX_ATTEMPTS)), flush=True)
    sys.exit(1)

parser = argparse.ArgumentParser()
parser.add_argument("binary")
parser.add_argument("desktop_probe", nargs="?")
parser.add_argument("--quantum", type=int, default=128, choices=[128, 257, 1024, 16384])
parser.add_argument("--live-probe")
parser.add_argument("--no-quantum-changes", action="store_true")
parser.add_argument("--reference-producer", action="store_true")
parser.add_argument("--pause-capture-during-changes", action="store_true")
parser.add_argument("--channels", type=int, default=1, choices=[1, 2])
args = parser.parse_args()
if args.channels == 2 and (args.live_probe or args.desktop_probe or args.quantum > 8192):
    parser.error("stereo routing check uses fl-live-play only, at a renderable quantum")
CHANNEL_MAP = "FL,FR" if args.channels == 2 else "MONO"
if args.live_probe and (args.quantum != 257 or args.desktop_probe):
    parser.error("live probe requires --quantum 257 and no desktop probe")
if args.no_quantum_changes and not args.live_probe:
    parser.error("--no-quantum-changes requires --live-probe")
if args.reference_producer and not args.live_probe:
    parser.error("--reference-producer requires --live-probe")
if args.pause_capture_during_changes and not args.live_probe:
    parser.error("--pause-capture-during-changes requires --live-probe")
if args.reference_producer and args.pause_capture_during_changes:
    parser.error("reference producer requires continuous capture draining")
binary = str(pathlib.Path(args.binary).resolve())
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
    default.clock.quantum = QUANTUM
    default.clock.min-quantum = MIN_QUANTUM
    default.clock.max-quantum = MAX_QUANTUM
    default.clock.quantum-limit = 16384
    clock.power-of-two-quantum = false
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
'''.replace("MIN_QUANTUM", "128" if args.live_probe else str(args.quantum))
   .replace("MAX_QUANTUM", "1024" if args.live_probe else str(args.quantum))
   .replace("QUANTUM", str(args.quantum)))
    server = player = capture = recovery = reference = None
    capture_reader = None
    with (root / "server.log").open("wb") as server_log:
        try:
            server = subprocess.Popen(["pipewire", "-c", str(configuration)], env=env,
                                      stdout=server_log, stderr=server_log)
            until(lambda: (root / remote).exists() if server.poll() is None else False)
            command = [args.live_probe, remote] if args.live_probe else [binary, "--remote", remote, "--group", "pipewire.test", "--duration-ms", "3000",
                                                                       "--channels", str(args.channels)]
            if args.no_quantum_changes:
                command.append("--fixed")
            player = subprocess.Popen(command, env=env, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            ready(player)
            if args.reference_producer:
                # Independently authored raw PCM is supplied to PipeWire's own
                # playback client; no fl-live engine/backend output is linked
                # into the capture path. The unlinked probe observes driver
                # quantum changes only, not the reference client's callbacks.
                source = root / "reference.f32"
                source.write_bytes(b"".join(struct.pack("<f", (i % 65521 - 32760) / 131072)
                                            for i in range(480000)))
                reference = subprocess.Popen(["pw-cat", "--remote", remote, "--playback", "--raw",
                    "--format", "f32", "--rate", "48000", "--channels", "1", "--channel-map", "MONO",
                    "--latency", "257", "--target", "0", "--properties",
                    "{ node.name = fixture-reference node.group = pipewire.test node.always-process = true }",
                    str(source)], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            capture = subprocess.Popen(["pw-cat", "--remote", remote, "--record", "--raw", "--format", "f32",
                "--rate", "48000", "--channels", str(args.channels), "--channel-map", CHANNEL_MAP, "--target", "0",
                "--sample-count", "72000" if args.live_probe else "24000", "--properties",
                "{ node.name = fixture-capture node.group = pipewire.test node.always-process = true }", "-"],
                env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            def configure_node(name, direction, channels=1):
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
                             f"mediaSubtype = raw format = F32 rate = 48000 channels = {channels} "
                             f"position = [ {'FL FR' if channels == 2 else 'MONO'} ] }} }}")
                result = subprocess.run(["pw-cli", "--remote", remote, "set-param", str(node["id"]),
                                         "PortConfig", parameter], env=env, capture_output=True, text=True, timeout=2)
                assert result.returncode == 0, result.stderr
                return True

            until(lambda: configure_node("fl-live-output", "Output", args.channels))
            if args.reference_producer:
                until(lambda: configure_node("fixture-reference", "Output"))
            until(lambda: configure_node("fixture-capture", "Input", args.channels))

            def port(direction, prefix):
                command = subprocess.run(["pw-link", "--remote", remote, direction], env=env,
                                         capture_output=True, text=True, timeout=2)
                assert command.returncode == 0, command.stderr
                return next((line.strip() for line in command.stdout.splitlines() if line.startswith(prefix)), None)

            if args.channels == 2:
                # Link each named channel to the same capture channel; a swap
                # or a missing link changes the independently checked samples.
                stereo_links = []
                for channel in ("FL", "FR"):
                    output_port = until(lambda: port("--output", f"fl-live-output:output_{channel}"))
                    input_port = until(lambda: port("--input", f"fixture-capture:input_{channel}"))
                    subprocess.run(["pw-link", "--remote", remote, output_port, input_port], env=env,
                                   check=True, capture_output=True, timeout=2)
                    stereo_links.append([output_port, input_port])
            else:
                output_port = until(lambda: port("--output", "fixture-reference:" if args.reference_producer else "fl-live-output:"))
                input_port = until(lambda: port("--input", "fixture-capture:"))
                subprocess.run(["pw-link", "--remote", remote, output_port, input_port], env=env,
                               check=True, capture_output=True, timeout=2)
            transitions = []
            prefix = b""
            capture_result = None
            if args.live_probe:
                transitions.append(ready(player))
                # Wait for real recorded source samples before changing the
                # driver. A connected port alone does not prove capture started.
                prefix = capture_prefix(capture, 4096 * 4)
                assert sum(value != 0 for value in struct.unpack("<4096f", prefix)) >= 1024
                if not args.pause_capture_during_changes:
                    # pw-cat writes capture to a bounded stdout pipe. Keep its
                    # reader running while metadata subprocesses execute; a
                    # stalled reader can interrupt the measured audio graph.
                    capture_reader = concurrent.futures.ThreadPoolExecutor(max_workers=1)
                    capture_result = capture_reader.submit(capture.communicate, timeout=5)
                for quantum in (() if args.no_quantum_changes else (1024, 128)):
                    subprocess.run(["pw-metadata", "--remote", remote, "--name", "settings",
                                    "0", "clock.force-quantum", str(quantum)],
                                   env=env, check=True, capture_output=True, text=True, timeout=2)
                    transitions.append(ready(player))
                assert [v["quantum"] for v in transitions] == ([257] if args.no_quantum_changes else [257, 1024, 128]), transitions
                if args.reference_producer and not args.no_quantum_changes:
                    # Observer counters are not reference-source frame indices.
                    # With continuous draining, require capture to remain active
                    # after the last change instead of equating those clocks.
                    assert capture_result is not None and not capture_result.done(), "reference capture ended before transitions"
            if args.quantum > 8192:
                remaining, errors = player.communicate(timeout=4)
                stats = json.loads(remaining.strip())
                assert player.returncode != 0, (remaining, errors)
                assert stats["error"] == -errno.EMSGSIZE and stats["bad_buffers"] > 0, stats
                # Once the oversized callback fails, PipeWire may offer a
                # smaller buffer while teardown is pending. It must stay silent
                # and never consume the queued start/launch commands.
                assert stats["min_render_frames"] == stats["max_render_frames"] == stats["position"] == 0, stats
                assert not stats["running"] and stats["handled_serial"] == 2, stats
                print(json.dumps(dict(passed=True, scope="private oversized quantum rejection; no hardware",
                                      requested_quantum=args.quantum, playback=stats)))
                sys.exit(0)  # finally reaps the owned capture/player/server
            data, errors = capture_result.result(timeout=6) if capture_result else capture.communicate(timeout=5)
            data = prefix + data
            # PipeWire 1.6.9 pw-cat's finite capture limit quits without setting
            # `drained`, while main returns success only for that flag. Preserve
            # the observed code and require exact payload length/no diagnostic.
            # https://github.com/PipeWire/pipewire/blob/1.6.9/src/tools/pw-cat.c
            assert capture.returncode in (0, 1) and not errors, (capture.returncode, len(data), errors.decode())
            capture_code = capture.returncode
            if args.channels == 2:
                # pw-cat counts frames; each stereo frame is FL then FR.
                assert len(data) == 24000 * 2 * 4, (len(data), errors.decode())
                frames = struct.unpack("<48000f", data)
                left, right = frames[0::2], frames[1::2]
                expected_left = (0.0625, 0.125, 0.1875, 0.25, -0.0625, -0.125, -0.1875, -0.25)
                expected_right = (0.03125, -0.03125, 0.09375, -0.09375, 0.15625, -0.15625, 0.21875, -0.21875)
                # The FL and FR links are created one after the other, so the
                # later link may start a few quanta late. Before its start that
                # channel must be exactly silent; afterwards both channels must
                # match the authored frames at one shared offset.
                left_start = next(i for i, value in enumerate(left) if value != 0)
                right_start = next(i for i, value in enumerate(right) if value != 0)
                link_lag = abs(right_start - left_start)
                assert link_lag <= 4 * args.quantum, (left_start, right_start)
                first = max(left_start, right_start)
                offset = expected_left.index(left[first])
                assert len(left) - first >= 20000, "insufficient engine audio captured"
                assert all(value == 0 for value in left[:left_start] + right[:right_start]), "unexpected pre-link samples"
                # Every frame must be the authored left/right pair in order;
                # transport drops and slips in this unprivileged graph start
                # new segments and are reported, not counted as engine output.
                segments, anomalies = authored_segments(list(zip(left[first:], right[first:])),
                                                        tuple(zip(expected_left, expected_right)))
                authored_frames = sum(length for _, length, _ in segments)
                assert authored_frames >= 20000 and len(anomalies) <= 32, (authored_frames, anomalies[:8])
                remaining, errors = player.communicate(timeout=4)
                assert player.returncode == 0, (remaining, errors)
                stats = json.loads(remaining.strip())
                assert stats["callbacks"] > 0 and stats["frames"] >= 24000 and stats["error"] == 0, stats
                assert stats["bad_buffers"] == stats["rejected"] == 0 and stats["handled_serial"] == 2 and not stats["running"], stats
                assert stats["min_render_frames"] == stats["max_render_frames"] == args.quantum, stats
                refused = subprocess.run([binary, "--remote", remote, "--channels", "3", "--duration-ms", "100"], env=env,
                                         capture_output=True, text=True, timeout=5)
                assert refused.returncode != 0 and "ready" not in refused.stdout, (refused.stdout, refused.stderr)
                assert "one (MONO) or two (FL/FR)" in refused.stderr, refused.stderr
                print(json.dumps(dict(passed=True, scope="private software graph stereo routing; no hardware",
                                      requested_quantum=args.quantum, channels=2, links=stereo_links,
                                      captured_frames=len(left), capture_returncode=capture_code, silent_prefix=first, link_lag_frames=link_lag,
                                      first_source_frame=offset, playback=stats, unsupported_channels_refused=3,
                                      authored_frames=authored_frames, transport_anomalies=anomalies,
                                      capture_sha256=hashlib.sha256(data).hexdigest())))
                sys.exit(0)  # finally reaps the owned capture/player/server
            sample_count = 72000 if args.live_probe else 24000
            assert len(data) == sample_count * 4, (len(data), errors.decode())
            values = struct.unpack(f"<{sample_count}f", data)
            expected = ([(i - 32760) / 131072 for i in range(65521)] if args.live_probe else
                        (0.0625, 0.125, 0.1875, 0.25, -0.0625, -0.125, -0.1875, -0.25))
            first = next(i for i, value in enumerate(values) if value != 0)
            # Ignore only a possible graph-start silent prefix; the captured
            # non-silent span must be the engine waveform with no missing samples.
            offset = expected.index(values[first])
            assert len(values) - first >= 20000, "insufficient engine audio captured"
            mismatches = [(i + first, value, expected[(i + offset) % len(expected)])
                          for i, value in enumerate(values[first:]) if value != expected[(i + offset) % len(expected)]]
            if args.live_probe:
                # Require the final transition to occur well within the capture,
                # not merely after its finite sample budget has been exhausted.
                assert transitions[-1]["frames"] < sample_count - 4800, transitions
                player.stdin.write("g"); player.stdin.flush()
                output, errors = player.communicate(timeout=4)
                assert player.returncode == 0 and not errors, (output, errors)
                stats = json.loads(output.strip())
                assert stats["passed"] and not stats["running"], stats
                if reference is not None:
                    assert reference.poll() is None, "reference source ended before capture"
                    stop(reference)
                    _, reference_errors = reference.communicate(timeout=2)
                    assert not reference_errors, reference_errors.decode()
                # Frame codes locate the captured span independently of a
                # requested server setting or the engine's own expected buffer.
                if not args.no_quantum_changes and not args.reference_producer:
                    assert offset <= transitions[1]["frames"] < transitions[2]["frames"] < offset + sample_count - first, (offset, transitions)
                print(json.dumps(dict(passed=not mismatches, scope="frame-coded private software capture; no hardware",
                                      producer="pw-cat" if args.reference_producer else "fl-live",
                                      capture_drained_during_changes=not args.pause_capture_during_changes,
                                      quantum_observer="unlinked fl-live probe" if args.reference_producer else "producer backend",
                                      linked_output=output_port, linked_input=input_port,
                                      quantum_changes=not args.no_quantum_changes,
                                      transitions=transitions, captured_frames=sample_count, silent_prefix=first,
                                      first_source_frame=offset, playback=stats, capture_sha256=hashlib.sha256(data).hexdigest(),
                                      mismatched_frames=len(mismatches), first_mismatches=mismatches[:8],
                                      unexpected_zero_frames=sum(value == 0 for _, value, _ in mismatches))), flush=True)
                if mismatches:
                    # Continuity controls stay strict: a pass needs one fully
                    # continuous capture. A failure made only of transport
                    # drops/slips (every frame silence or authored content)
                    # is retried on a fresh server; anything else fails now.
                    try:
                        _, anomalies = authored_segments(list(values[first:]), tuple(expected))
                    except AssertionError:
                        anomalies = None
                    if anomalies:
                        fail_dropped(anomalies[:16], "frame-coded capture")
                assert not mismatches, "frame-coded capture continuity unqualified; see measured JSON above"
                sys.exit(0)
            # Authored content in order, with transport drops/slips reported.
            segments, anomalies = authored_segments(list(values[first:]), tuple(expected))
            authored_frames = sum(length for _, length, _ in segments)
            assert authored_frames >= 20000 and len(anomalies) <= 32, ("capture is not authored engine output", authored_frames, anomalies[:8])
            remaining, errors = player.communicate(timeout=4)
            assert player.returncode == 0, (remaining, errors)
            stats = json.loads(remaining.strip())
            assert stats["callbacks"] > 0 and stats["frames"] >= 24000 and stats["error"] == 0, stats
            assert stats["bad_buffers"] == stats["rejected"] == 0 and stats["handled_serial"] == 2 and not stats["running"], stats
            assert stats["min_render_frames"] == stats["max_render_frames"] == args.quantum, stats

            project_rebuilds = None
            if args.desktop_probe:
                probe = subprocess.run([str(pathlib.Path(args.desktop_probe).resolve()), remote],
                                       env=dict(env, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software"),
                                       capture_output=True, text=True, timeout=15)
                assert probe.returncode == 0, (probe.stdout, probe.stderr)
                assert "five stopped project rebuilds including Undo/Redo" in probe.stdout, probe.stdout
                project_rebuilds = probe.stdout.strip()

            desktop_workflow = None
            if args.desktop_probe:
                player = subprocess.Popen([str(pathlib.Path(args.desktop_probe).resolve()), remote, "--workflow"],
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
                assert "QML edits, atomic scene launch, scene stop while transport runs and acknowledged transport stop" in output
                desktop_workflow = dict(frames=144000, peak=peak, pitch=pitch, stopped_silence_frames=12000,
                                        scene_active_tracks=1, excluded_audio_track_stopped=True, scene_stop_keeps_transport=True,
                                        scene_arrangement_copy=True, scene_order_checked=True)

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
            if args.desktop_probe:
                recovery = subprocess.Popen([str(pathlib.Path(args.desktop_probe).resolve()), remote, "--recovery"],
                    env=dict(env, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software"),
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                ready(recovery)
            stop(server)
            tail, errors = player.communicate(timeout=4)
            assert player.returncode != 0, (tail, errors)
            lost = json.loads(tail.strip())
            assert lost["error"] != 0 and lost["callbacks"] > 0 and not lost["running"], lost
            desktop_recovery = None
            if recovery is not None:
                ready(recovery)  # stopped, saved, and failed retry against absent server
                server = subprocess.Popen(["pipewire", "-c", str(configuration)], env=env,
                                          stdout=server_log, stderr=server_log)
                until(lambda: (root / remote).exists() if server.poll() is None else False)
                recovery.stdin.write("g"); recovery.stdin.flush()
                output, errors = recovery.communicate(timeout=8)
                assert recovery.returncode == 0, (output, errors)
                assert "explicit stopped reconnect and retained history" in output
                desktop_recovery = output.strip()
                stop(server)
            print(json.dumps(dict(passed=True,scope="private software graph; no hardware/audio-device qualification",
                                  requested_quantum=args.quantum,
                                  captured_frames=len(values), capture_returncode=capture_code, authored_frames=authored_frames, transport_anomalies=anomalies[:16],
                                  silent_prefix=first, playback=stats, server_loss=lost, project_rebuilds=project_rebuilds,
                                  desktop_workflow=desktop_workflow, desktop_recovery=desktop_recovery)))
        except Exception:
            print((root / "server.log").read_text(errors="replace")[-2000:], file=sys.stderr)
            raise
        finally:
            stop(capture)
            if capture_reader is not None:
                capture_reader.shutdown(wait=True)
            stop(recovery)
            stop(reference)
            stop(player)
            stop(server)
    absent = subprocess.run([binary, "--remote", remote, "--duration-ms", "100"], env=env,
                            capture_output=True, text=True, timeout=5)
    assert absent.returncode != 0 and "ready" not in absent.stdout
