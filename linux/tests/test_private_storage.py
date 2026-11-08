"""Private storage must stay private even when a filesystem ignores chmod."""

from pathlib import Path
import stat
import sys
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from velox.core import Paths, VeloxError


def main():
    with tempfile.TemporaryDirectory(prefix="velox-storage-test-") as directory:
        root = Path(directory)
        writable = root / "normal"
        writable.mkdir(mode=0o755)
        paths = Paths(writable)
        assert stat.S_IMODE(paths.root.stat().st_mode) == 0o700
        unsupported = root / "unsupported"
        unsupported.mkdir(mode=0o755)
        with patch("velox.core.os.chmod", lambda *_args, **_kwargs: None):
            try:
                Paths(unsupported)
            except VeloxError:
                pass
            else:
                raise AssertionError("Filesystem without private permissions was accepted")
        assert not list(unsupported.iterdir())
        outside = root / "outside"
        outside.mkdir(mode=0o755)
        linked = root / "linked"
        linked.symlink_to(outside, target_is_directory=True)
        try:
            Paths(linked)
        except VeloxError:
            pass
        else:
            raise AssertionError("Linked private storage was accepted")
        assert stat.S_IMODE(outside.stat().st_mode) == 0o755
        assert not list(outside.iterdir())
    print(
        "PASS private storage permissions, unsupported-filesystem rejection and linked-folder safety"
    )


if __name__ == "__main__":
    main()
