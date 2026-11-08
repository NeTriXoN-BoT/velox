"""Pinned, bounded source downloads shared by the Linux build tools."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import stat
import tempfile
import urllib.request


class HttpsOnly(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        if not newurl.startswith("https://"):
            raise RuntimeError("Build downloads must remain on HTTPS.")
        return super().redirect_request(request, fp, code, message, headers, newurl)


def verified(file, spec):
    try:
        descriptor = os.open(file, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError:
        return False
    with os.fdopen(descriptor, "rb") as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size != spec["bytes"]:
            return False
        digest = hashlib.sha256()
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
        return digest.hexdigest() == spec["sha256"]


def download(work: Path, name: str, spec: dict):
    work.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = work.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        raise RuntimeError("Use a private build folder owned by your account.")
    os.chmod(work, 0o700)
    target = work / name
    if verified(target, spec):
        return target
    if not spec["url"].startswith("https://"):
        raise RuntimeError("Build downloads require HTTPS.")
    descriptor, temporary = tempfile.mkstemp(prefix=".download-", dir=work)
    try:
        request = urllib.request.Request(spec["url"], headers={"User-Agent": "Velox build"})
        with (
            os.fdopen(descriptor, "wb") as output,
            urllib.request.build_opener(HttpsOnly()).open(request, timeout=45) as response,
        ):
            if not response.geturl().startswith("https://"):
                raise RuntimeError("The source download left HTTPS.")
            total = 0
            while block := response.read(1024 * 1024):
                total += len(block)
                if total > spec["bytes"]:
                    raise RuntimeError("Unexpected source archive size.")
                output.write(block)
            output.flush()
            os.fsync(output.fileno())
        if not verified(temporary, spec):
            raise RuntimeError("The source archive checksum did not match.")
        os.replace(temporary, target)
        return target
    finally:
        Path(temporary).unlink(missing_ok=True)
