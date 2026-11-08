"""Only explicit local file drops enter the reviewed Android import flow."""

from __future__ import annotations

from pathlib import Path


def local_files(mime):
    if not mime.hasUrls():
        return []
    urls = mime.urls()
    if not 1 <= len(urls) <= 64 or any(not url.isLocalFile() or url.host() for url in urls):
        return []
    files = [Path(url.toLocalFile()) for url in urls]
    # Filesystem checks run in the import worker; a slow mount must not block a
    # drag event on the desktop thread. Directories/links are rejected there.
    return files if all(file.is_absolute() for file in files) else []
