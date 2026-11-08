"""Exercise signed updates through real HTTPS without external services or SDKs."""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
import functools
import hashlib
import http.server
import ipaddress
import os
from pathlib import Path
import ssl
import sys
import tempfile
import threading
from unittest.mock import patch

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils
from cryptography.x509.oid import NameOID

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from velox.core import Paths
from velox import updates


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_args):
        pass


def main():
    with tempfile.TemporaryDirectory(prefix="velox-update-test-") as directory:
        root = Path(directory)
        server_root = root / "server"
        server_root.mkdir()
        payload = b"\x7fELF\x02\x01\x01\x00AI\x02" + bytes(1024 * 1024)
        (server_root / "release.AppImage").write_bytes(payload)
        key = ec.generate_private_key(ec.SECP256R1())
        subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Velox local test")])
        now = datetime.now(timezone.utc)
        certificate = (
            x509.CertificateBuilder()
            .subject_name(subject)
            .issuer_name(subject)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - timedelta(minutes=1))
            .not_valid_after(now + timedelta(days=1))
            .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
            .add_extension(
                x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]),
                critical=False,
            )
            .sign(key, hashes.SHA256())
        )
        certificate_path, key_path = root / "certificate.pem", root / "test-key.pem"
        certificate_path.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
        key_path.write_bytes(
            key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            )
        )
        key_path.chmod(0o600)
        server = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), functools.partial(Handler, directory=server_root)
        )
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate_path, key_path)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            digest = hashlib.sha256(payload).hexdigest()
            body = (
                "velox-linux-update 1\nversion=2.0.0\n"
                f"url=https://127.0.0.1:{server.server_port}/release.AppImage\n"
                f"size={len(payload)}\nsha256={digest}\nnotes=Local integration test\n"
            ).encode()
            r, s = utils.decode_dss_signature(key.sign(body, ec.ECDSA(hashes.SHA256())))
            signature = (r.to_bytes(32, "big") + s.to_bytes(32, "big")).hex().encode()
            manifest = body + b"signature=" + signature + b"\n"
            numbers = key.public_key().public_numbers()
            (root / "version.txt").write_text("1.0.0\n")
            installed = root / "Installed.AppImage"
            original = b"Previous installed image"
            installed.write_bytes(original)
            installed.chmod(0o755)
            environment = dict(
                APPIMAGE=str(installed),
                SSL_CERT_FILE=str(certificate_path),
                VELOX_RESOURCES=str(root),
            )
            progress = []
            with (
                patch.dict(os.environ, environment),
                patch.object(updates, "PUBLIC_X", numbers.x),
                patch.object(updates, "PUBLIC_Y", numbers.y),
            ):
                release = updates.verify(manifest)
                current, previous = updates.Updater(Paths(root / "data")).apply(
                    release, lambda message, fraction: progress.append((message, fraction))
                )
            assert current.read_bytes() == payload
            assert previous.read_bytes() == original
            assert any(0 < fraction < 1 for _message, fraction in progress)
            assert all(isinstance(message, str) and message for message, _fraction in progress)
            assert progress[-1][1] == 1
            print(
                "PASS signed HTTPS download, progress, atomic publication and retained previous image"
            )
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)


if __name__ == "__main__":
    main()
