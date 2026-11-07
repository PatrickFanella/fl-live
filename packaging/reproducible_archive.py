"""Builds a byte-reproducible .tar.gz of an fl-live build (docs/install.md).

    SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) python3 packaging/reproducible_archive.py BUILD_DIR OUTPUT_DIR

It installs BUILD_DIR into a temporary prefix with `cmake --install` and
writes the files in sorted order, owned by uid/gid 0 with no owner names,
with every timestamp set to SOURCE_DATE_EPOCH, gzip-compressed without a
timestamp or file name. The archive is named like the CPack package. Two
builds of the same source with the same toolchain and build directory give
the same bytes."""
import gzip
import hashlib
import io
import os
import pathlib
import re
import subprocess
import sys
import tarfile
import tempfile

if len(sys.argv) != 3:
    sys.exit(__doc__)
build, output = pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve()
epoch = os.environ.get("SOURCE_DATE_EPOCH", "")
if not epoch.isdigit():
    sys.exit("Set SOURCE_DATE_EPOCH to the commit time, for example SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)")
config = (build / "CPackConfig.cmake").read_text()
name = re.search(r'set\(CPACK_PACKAGE_FILE_NAME "([^"]+)"\)', config).group(1)
output.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="fl-live-archive-") as temporary:
    root = pathlib.Path(temporary) / name
    subprocess.run(["cmake", "--install", str(build), "--prefix", str(root)], check=True, capture_output=True, text=True)
    paths = sorted(p for p in root.rglob("*"))
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.GNU_FORMAT) as archive:
        for path in [root] + paths:
            info = tarfile.TarInfo(str(path.relative_to(root.parent)))
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mtime = int(epoch)
            if path.is_symlink():
                info.type, info.linkname, info.mode = tarfile.SYMTYPE, os.readlink(path), 0o777
                archive.addfile(info)
            elif path.is_dir():
                info.type, info.mode = tarfile.DIRTYPE, 0o755
                archive.addfile(info)
            else:
                info.mode = 0o755 if os.access(path, os.X_OK) else 0o644
                info.size = path.stat().st_size
                with path.open("rb") as data:
                    archive.addfile(info, data)
    target = output / f"{name}.tar.gz"
    with target.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as packed:
        packed.write(buffer.getvalue())
print(f"{hashlib.sha256(target.read_bytes()).hexdigest()}  {target}")
