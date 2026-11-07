"""The reproducible archive (#85): built twice from one build it has the same
bytes, every entry is owned by 0/0 with no names, dated SOURCE_DATE_EPOCH and
sorted, it holds the staged install, and an unset SOURCE_DATE_EPOCH is
refused with an actionable message.

    reproducible_archive.py SCRIPT BUILD_DIR"""
import hashlib
import os
import pathlib
import subprocess
import sys
import tarfile
import tempfile

script, build = sys.argv[1], sys.argv[2]
epoch = "1778112000"
with tempfile.TemporaryDirectory(prefix="fl-live-repro-") as temporary:
    root = pathlib.Path(temporary)
    env = {k: v for k, v in os.environ.items() if k != "SOURCE_DATE_EPOCH"}
    refused = subprocess.run([sys.executable, script, build, str(root / "none")], env=env, capture_output=True, text=True)
    assert refused.returncode != 0 and "SOURCE_DATE_EPOCH" in refused.stderr, refused
    digests = []
    for run in ("a", "b"):
        made = subprocess.run([sys.executable, script, build, str(root / run)], env=dict(env, SOURCE_DATE_EPOCH=epoch),
                              capture_output=True, text=True, check=True)
        digests.append(made.stdout.split()[0])
        archive = pathlib.Path(made.stdout.split()[1])
        assert hashlib.sha256(archive.read_bytes()).hexdigest() == digests[-1]
    assert digests[0] == digests[1], digests
    with tarfile.open(archive) as tar:
        members = tar.getmembers()
    names = [m.name for m in members]
    assert names[1:] == sorted(names[1:]), "entries are sorted"
    assert all(m.uid == 0 and m.gid == 0 and m.uname == "" and m.gname == "" and m.mtime == int(epoch) for m in members), "normalized metadata"
    top = names[0]
    for required in ("bin/fl-live", "bin/fl-live-render", "share/doc/fl_live/THIRD-PARTY-NOTICES.md", "share/doc/fl_live/install.md"):
        assert f"{top}/{required}" in names, required
    assert next(m for m in members if m.name == f"{top}/bin/fl-live").mode == 0o755
    raw = archive.read_bytes()
    assert raw[4:8] == b"\x00\x00\x00\x00", "the gzip header carries no timestamp"
    print(f"PASS: two archives of one build are byte-identical ({digests[0][:16]}); {len(members)} entries sorted, owned 0/0 "
          "without names, dated SOURCE_DATE_EPOCH; an unset SOURCE_DATE_EPOCH is refused")
