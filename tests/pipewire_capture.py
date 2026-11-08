"""Records an independently authored stereo signal through fl-live's PipeWire
capture stream on an owned private graph and checks the take frame by frame.
No desktop remote, devices or session manager are used.

    pipewire_capture.py CAPTURE_PROBE                        # the capture stream alone
    pipewire_capture.py --desktop DESKTOP_PROBE              # the desktop Session: arm, record, stop
    pipewire_capture.py --desktop --loss server DESKTOP_PROBE  # kill the server mid-take, restart, Retry audio
    pipewire_capture.py --desktop --loss input DESKTOP_PROBE   # destroy only the input node mid-take
    pipewire_capture.py --desktop --route-change DESKTOP_PROBE # unlink and relink the input mid-take
    pipewire_capture.py --desktop --monitor DESKTOP_PROBE      # input monitoring heard in the output

In desktop mode arming the track opens the input node, so the fixture links
it before recording starts, exactly as for the capture stream alone."""
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import time

desktop = "--desktop" in sys.argv
loss = sys.argv[sys.argv.index("--loss") + 1] if "--loss" in sys.argv else None
route_change = "--route-change" in sys.argv
monitor = "--monitor" in sys.argv
assert loss in (None, "server", "input") and (desktop or not loss) and (desktop or not route_change) and not (loss and route_change), sys.argv
probe = str(pathlib.Path(sys.argv[-1]).resolve())


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
            if desktop:
                (root / "project").mkdir()
            mode = "--monitor" if monitor else "--record-loss" if loss else "--record"
            recorder = subprocess.Popen([probe, remote, mode, str(root / "project")] if desktop else [probe, remote, str(output), "2"],
                                        env=env, stdin=subprocess.PIPE,
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

            if monitor:
                # The desktop output goes to a recorder; the armed, monitored
                # track at level 50% and pan -50 plays the input into it.
                heard = root / "monitored.f32"
                sink = subprocess.Popen(["pw-cat", "--remote", remote, "--record", "--raw", "--format", "f32", "--rate", "48000",
                    "--channels", "1", "--channel-map", "MONO", "--target", "0", "--sample-count", "72000", "--properties",
                    "{ node.name = fixture-sink node.group = pipewire.test node.always-process = true }", "-"],
                    env=env, stdout=heard.open("wb"), stderr=subprocess.PIPE)

                def configure_mono(name, direction):
                    dump = subprocess.run(["pw-dump", "--remote", remote], env=env, capture_output=True, text=True, timeout=2)
                    node = next((v for v in json.loads(dump.stdout) if v.get("info", {}).get("props", {}).get("node.name") == name), None)
                    if not node:
                        return False
                    parameter = ("{ direction = " + direction + " mode = dsp format = { mediaType = audio mediaSubtype = raw "
                                 "format = F32 rate = 48000 channels = 1 position = [ MONO ] } }")
                    return subprocess.run(["pw-cli", "--remote", remote, "set-param", str(node["id"]), "PortConfig", parameter],
                                          env=env, capture_output=True, text=True, timeout=2).returncode == 0
                # The input chosen in the app reaches the stream; with no session
                # manager here, the fixture still makes the links itself.
                dump = json.loads(subprocess.run(["pw-dump", "--remote", remote], env=env, capture_output=True, text=True, timeout=2).stdout)
                chosen = next(v["info"]["props"].get("target.object") for v in dump if v.get("info", {}).get("props", {}).get("node.name") == "fl-live-input")
                assert chosen == "fixture-source", chosen
                until(lambda: configure("fixture-source", "Output"))
                until(lambda: configure("fl-live-input", "Input"))
                until(lambda: configure_mono("fl-live-output", "Output"))
                until(lambda: configure_mono("fixture-sink", "Input"))
                for channel in ("FL", "FR"):
                    subprocess.run(["pw-link", "--remote", remote, until(lambda: port("--output", f"fixture-source:output_{channel}")),
                                    until(lambda: port("--input", f"fl-live-input:input_{channel}"))], env=env, check=True, capture_output=True, timeout=2)
                subprocess.run(["pw-link", "--remote", remote, until(lambda: port("--output", "fl-live-output:")),
                                until(lambda: port("--input", "fixture-sink:"))], env=env, check=True, capture_output=True, timeout=2)
                sink.wait(timeout=15)  # pw-cat ends with status 1 when --sample-count is reached; the size is checked below
                recorder.stdin.write("stop\n"); recorder.stdin.flush()
                assert json.loads(recorder.stdout.readline())["done"]
                assert recorder.wait(timeout=5) == 0, recorder.stderr.read()
                raw = heard.read_bytes()
                assert len(raw) == 72000 * 4, (len(raw), sink.stderr.read())
                mono = struct.unpack(f"<{len(raw) // 4}f", raw)
                raise StopIteration
            until(lambda: configure("fixture-source", "Output"))
            until(lambda: configure("fl-live-input", "Input"))
            pairs = []
            for channel in ("FL", "FR"):
                out_port = until(lambda: port("--output", f"fixture-source:output_{channel}"))
                in_port = until(lambda: port("--input", f"fl-live-input:input_{channel}"))
                subprocess.run(["pw-link", "--remote", remote, out_port, in_port], env=env, check=True, capture_output=True, timeout=2)
                pairs.append((out_port, in_port))
            time.sleep(0.3)  # let the link settle before the take starts
            recorder.stdin.write("record\n"); recorder.stdin.flush()
            if loss == "server":
                time.sleep(1.0)
                server.terminate(); server.wait(timeout=5)
                lost = json.loads(recorder.stdout.readline())
                until(lambda: not (root / remote).exists(), 2)
                server = subprocess.Popen(["pipewire", "-c", str(configuration)], env=env, stdout=log, stderr=log)
                until(lambda: (root / remote).exists() if server.poll() is None else False)
                recorder.stdin.write("retry\n"); recorder.stdin.flush()
                retried = json.loads(recorder.stdout.readline())
                dump = subprocess.run(["pw-dump", "--remote", remote], env=env, capture_output=True, text=True, timeout=2)
                reopened = any(v.get("info", {}).get("props", {}).get("node.name") == "fl-live-input" for v in json.loads(dump.stdout))
                recorder.stdin.write("done\n"); recorder.stdin.flush()
            elif loss == "input":
                time.sleep(1.0)
                dump = subprocess.run(["pw-dump", "--remote", remote], env=env, capture_output=True, text=True, timeout=2)
                node = next(v for v in json.loads(dump.stdout) if v.get("info", {}).get("props", {}).get("node.name") == "fl-live-input")
                subprocess.run(["pw-cli", "--remote", remote, "destroy", str(node["id"])], env=env, check=True, capture_output=True, timeout=2)
                lost = json.loads(recorder.stdout.readline())
            elif route_change:
                time.sleep(0.6)
                for out_port, in_port in pairs:
                    subprocess.run(["pw-link", "--remote", remote, "-d", out_port, in_port], env=env, check=True, capture_output=True, timeout=2)
                time.sleep(0.3)
                for out_port, in_port in pairs:
                    subprocess.run(["pw-link", "--remote", remote, out_port, in_port], env=env, check=True, capture_output=True, timeout=2)
                time.sleep(0.9)
                recorder.stdin.write("stop\n"); recorder.stdin.flush()
                status = json.loads(recorder.stdout.readline())
            else:
                time.sleep(1.5)
                recorder.stdin.write("stop\n"); recorder.stdin.flush()
                status = json.loads(recorder.stdout.readline())
            assert recorder.wait(timeout=5) == 0, recorder.stderr.read()
        except StopIteration:
            pass
        finally:
            stop(recorder); stop(player); stop(server)
            if monitor:
                stop(sink)
    def check(samples, minimum, allowance=10, partial=False):
        frames = len(samples) // 2
        # Locate the first captured frame in the reference, then walk: every frame
        # must equal the next reference frame, except where the private graph
        # dropped quanta under host load, which shows as a forward jump, or
        # filled a late cycle with silence, which shows as a run of (0, 0) after
        # which the reference resumes at or after where it stopped. Both are
        # counted and limited together to a tenth of the take.
        index = {}
        for i in range(FRAMES):
            index.setdefault((reference[2 * i], reference[2 * i + 1]), i)
        lefts, rights = set(reference[0::2]), set(reference[1::2])
        position = None
        matched = jumps = skipped = silent = gaps = 0
        for f in range(frames):
            pair = (samples[2 * f], samples[2 * f + 1])
            # Links change one channel at a time: with `partial`, a frame with
            # one silent channel counts as silence if the other channel holds
            # a value of its own reference sequence.
            if partial and pair != (0.0, 0.0) and 0.0 in pair and (pair[0] in lefts if pair[1] == 0.0 else pair[1] in rights):
                pair = (0.0, 0.0)
            if pair == (0.0, 0.0):
                if position is not None:  # before the reference reached the link otherwise
                    gaps += f == 0 or (samples[2 * f - 2], samples[2 * f - 1]) != (0.0, 0.0)
                    silent += 1
                continue
            at = index.get(pair)
            if at is None:
                context = [(samples[2 * g], samples[2 * g + 1]) for g in range(max(0, f - 3), min(frames, f + 4))]
                raise AssertionError(f"captured frame {f} is not a reference frame (swapped, corrupted or invented); position {position}; {context}")
            if position is not None and at != position:
                assert at > position, f"frame {f} goes back in the reference ({at} after {position})"
                jumps += 1
                skipped += at - position
            position = at + 1
            matched += 1
        assert matched > minimum, (matched, frames)
        assert skipped + silent <= matched * allowance // 100, f"too many frames missing: {skipped} over {jumps} jumps, {silent} silent over {gaps} gaps"
        return {"captured_frames": frames, "matched_frames": matched, "jumps": jumps, "skipped_frames": skipped,
                "silent_gaps": gaps, "silent_frames": silent}

    if loss == "server":
        # The take is finished at the loss, held while audio is stopped, and
        # becomes a clip once Retry audio reconnects to the restarted server.
        assert lost["lost"] and not lost["audio"] and not lost["recording"] and lost["clips"] == 1, lost
        assert "Audio is stopped" in lost["message"] and "1 take will be added as clips after Retry audio" in lost["message"], lost
        assert len(lost["files"]) == 1 and lost["files"][0].endswith(" take 1.wav"), lost
        assert retried["reconnected"] and retried["audio"] and retried["clips"] == 2 and retried["selected_last"], retried
        assert "Added 1 take as new clips" in retried["message"] and retried["armed"] and reopened, (retried, reopened)
        result = check(wav_floats(lost["files"][0]), 48000 * 0.6)
        print(json.dumps(result))
        print("PASS: server killed mid-take: the take is finished, matches the reference frame by frame up to the loss, "
              "is held while audio is stopped and becomes the selected clip after Retry audio; the input reopens")
        sys.exit(0)
    if loss == "input":
        assert lost["lost"] and lost["audio"] and not lost["recording"] and lost["clips"] == 2 and lost["selected_last"], lost
        assert "recording input was lost" in lost["message"] and "Recorded 1 take as new clips" in lost["message"] and not lost["armed"], lost
        result = check(wav_floats(lost["files"][0]), 48000 * 0.6)
        print(json.dumps(result))
        print("PASS: input node destroyed mid-take: output keeps running, the take is finished as the selected clip and "
              "matches the reference frame by frame up to the loss; the track is disarmed")
        sys.exit(0)
    if monitor:
        # Expected output, computed in 32-bit float as the monitor does:
        # (0.5 * L + 0.25 * R) / 2. From the first full match on, every sample
        # must be the next expected one, or a counted forward jump where the
        # graph slipped.
        f32 = lambda x: struct.unpack("<f", struct.pack("<f", x))[0]
        sequence = [f32(f32(0.5 * reference[2 * i]) + f32(0.25 * reference[2 * i + 1])) / 2 for i in range(FRAMES)]
        expected = {}
        for i, value in enumerate(sequence):
            expected.setdefault(value, i)
        # Start at eight samples in a row that follow the input; before that
        # the two input channels are still being linked one at a time, and a
        # half-linked value can equal some expected value by chance.
        begin = next(f for f in range(len(mono) - 8)
                     if expected.get(mono[f]) is not None and all(expected.get(mono[f + k]) == expected[mono[f]] + k for k in range(8)))
        position, matched, jumps, skipped, silent = None, 0, 0, 0, 0
        for f, value in enumerate(mono[begin:], begin):
            # Different input pairs can mix to the same value, so the next
            # expected sample is tried first and the lookup only finds jumps.
            at = position if position is not None and position < FRAMES and sequence[position] == value else expected.get(value)
            if at is None and value == 0.0:
                # A cycle whose input arrived late plays silence, as the
                # monitor is designed to; counted like a slipped quantum.
                silent += 1
                continue
            assert at is not None, f"output sample {f} ({value}) is not the monitored input at level 50% and pan -50"
            if position is not None and at != position:
                assert at > position, f"sample {f} goes back in the input"
                jumps += 1
                skipped += at - position
            position = at + 1
            matched += 1
        assert matched > 48000 and skipped + silent <= matched // 10, (matched, jumps, skipped, silent)
        print(json.dumps({"output_samples": len(mono), "first_followed_sample": begin, "matched_samples": matched, "jumps": jumps, "skipped": skipped, "silent": silent}))
        print("PASS: input monitoring: the desktop output carries the input at the armed track's level and pan, (0.5 L + 0.25 R) / 2 "
              "in mono, sample for sample")
        sys.exit(0)
    assert not status["failed"] and status["dropped"] == 0 and status["error"] == 0, status
    if desktop:
        assert status["file"].startswith(str(root / "project" / "recordings")) and status["file"].endswith(" take 1.wav"), status
        output = pathlib.Path(status["file"])
    else:
        assert status["file"] == str(output), status
    samples = wav_floats(output)
    frames = len(samples) // 2
    assert (status["frames"] < 0 or frames == status["frames"]) and 48000 * 1.2 < frames < 48000 * 3.0, (frames, status)
    if route_change:
        # While unlinked the input reads silence; after relinking the
        # reference resumes further on. Every non-silent frame is still a
        # reference frame in order; the gap is reported, not hidden.
        result = check(samples, 48000 * 1.0, allowance=80, partial=True)
        assert result["silent_gaps"] >= 1 and result["silent_frames"] >= 48000 * 0.15, result
        assert result["jumps"] >= 1, result
        print(json.dumps(result))
        print("PASS: route change mid-take: the take keeps running through an unlink and relink, records silence while unlinked, "
              "resumes on the reference afterwards and still becomes the selected clip")
        sys.exit(0)
    result = check(samples, 48000 * 1.2)
    print(json.dumps({**result, "callbacks": status["callbacks"], "bad_buffers": status["bad_buffers"]}))
    print("PASS: " + ("desktop recording: armed track, take saved in recordings/ and added as the selected clip; " if desktop else "")
          + "stereo capture through fl-live-input matches the independent reference frame by frame, channels in place")
