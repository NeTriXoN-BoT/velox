"""Private ADB control with a restricted emulator-registration endpoint."""

from __future__ import annotations

import os
import re
import socket
import struct
import threading
from pathlib import Path

from .core import INSTANCE, VeloxError


class Registration:
    def __init__(self, hub):
        self.hub = hub
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind(("127.0.0.1", 0))
        self.port = self.listener.getsockname()[1]
        self.listener.listen(4)
        self.listener.settimeout(0.5)
        self.stop = threading.Event()
        self.forwarded = 0
        self.thread = threading.Thread(
            target=self._loop, name="Velox ADB registration", daemon=True
        )
        self.thread.start()

    def _same_user(self, peer: tuple) -> bool:
        # TCP does not provide SO_PEERCRED. Match the established loopback client
        # socket's kernel UID before accepting its narrowly defined notification.
        expected_local = f"0100007F:{peer[1]:04X}"
        expected_remote = f"0100007F:{self.port:04X}"
        try:
            for line in Path("/proc/net/tcp").read_text().splitlines()[1:]:
                fields = line.split()
                if fields[1] == expected_local and fields[2] == expected_remote:
                    return int(fields[7]) == os.getuid()
        except (OSError, ValueError, IndexError):
            return False
        return False

    @staticmethod
    def _read(connection: socket.socket, length: int) -> bytes:
        result = bytearray()
        while len(result) < length:
            block = connection.recv(length - len(result))
            if not block:
                raise OSError("Connection ended")
            result.extend(block)
        return bytes(result)

    def _allowed(self, message: bytes) -> bool:
        match = re.fullmatch(rb"host:emulator:([0-9]{4,5})", message)
        if not match:
            return False
        port = int(match[1])
        for folder in self.hub.paths.phones.iterdir():
            if INSTANCE.fullmatch(folder.name) and folder.is_dir() and not folder.is_symlink():
                try:
                    record = self.hub.paths.metadata(folder.name)
                    if port == record["port"] + 1:
                        return True
                except VeloxError:
                    pass
        return False

    def _loop(self):
        while not self.stop.is_set():
            try:
                incoming, peer = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with incoming:
                incoming.settimeout(1)
                try:
                    if peer[0] != "127.0.0.1" or not self._same_user(peer):
                        continue
                    header = self._read(incoming, 4)
                    length = int(header, 16)
                    if length < 1 or length > 64:
                        continue
                    message = self._read(incoming, length)
                    if not self._allowed(message):
                        incoming.sendall(b"FAIL0012Registration only.")
                        continue
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as private:
                        private.settimeout(2)
                        private.connect(str(self.hub.adb_socket))
                        credentials = private.getsockopt(
                            socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i")
                        )
                        if struct.unpack("3i", credentials)[1] != os.getuid():
                            continue
                        private.sendall(header + message)
                        response = self._read(private, 4)
                        if response == b"OKAY":
                            self.forwarded += 1
                            incoming.sendall(response)
                except (OSError, ValueError, VeloxError):
                    continue

    def close(self):
        self.stop.set()
        self.listener.close()
        if self.thread is not threading.current_thread():
            self.thread.join(timeout=2)
