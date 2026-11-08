"""One launcher per private data root, with a same-account activation socket."""

from __future__ import annotations

import os
import socket
import stat
import struct

from PySide6.QtCore import QElapsedTimer, QObject, QTimer, Signal
from PySide6.QtNetwork import QLocalServer, QLocalSocket

from .core import VeloxError, lease


REQUEST = b"VELOX ACTIVATE 1\n"


def activate_existing(path):
    client = QLocalSocket()
    try:
        client.connectToServer(str(path))
        if not client.waitForConnected(1500):
            return False
        client.write(REQUEST)
        if not client.waitForBytesWritten(1500) and client.bytesToWrite():
            return False
        timer = QElapsedTimer()
        timer.start()
        answer = bytearray()
        while b"\n" not in answer and len(answer) < 16:
            remaining = 1500 - timer.elapsed()
            if (
                remaining <= 0
                or not client.bytesAvailable()
                and not client.waitForReadyRead(remaining)
            ):
                return False
            answer.extend(bytes(client.read(16 - len(answer))))
        return answer == b"OK\n"
    finally:
        client.abort()


class Launcher(QObject):
    activated = Signal()

    def __init__(self, hub):
        super().__init__()
        self.path = hub.runtime / "launcher.sock"
        self._lease = lease(hub.runtime / "launcher.lock")
        self._lease.__enter__()
        self.server = QLocalServer(self)
        self.server.setMaxPendingConnections(8)
        self.clients = set()
        self.server.setSocketOptions(QLocalServer.SocketOption.UserAccessOption)
        try:
            if os.path.lexists(self.path):
                info = self.path.lstat()
                if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
                    raise VeloxError("The launcher socket is not a private Velox socket.")
                # The lifetime lock is held. A live peer still wins over cleanup
                # if an older launcher was started without that lock.
                if activate_existing(self.path):
                    raise VeloxError("Velox is already open for this data folder.")
                self.path.unlink()
            if not self.server.listen(str(self.path)):
                raise VeloxError("The private launcher connection could not be opened.")
            self.server.newConnection.connect(self.accept)
        except BaseException:
            self._lease.__exit__(None, None, None)
            self._lease = None
            raise

    def accept(self):
        while self.server.hasPendingConnections():
            client = self.server.nextPendingConnection()
            if len(self.clients) >= 16:
                client.abort()
                client.deleteLater()
                continue
            try:
                with socket.socket(fileno=os.dup(client.socketDescriptor())) as peer:
                    _pid, uid, _gid = struct.unpack(
                        "3i", peer.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
                    )
                if uid != os.getuid():
                    raise PermissionError("Different account")
            except OSError:
                client.abort()
                client.deleteLater()
                continue
            deadline = QTimer(client)
            deadline.setSingleShot(True)
            self.clients.add(client)
            client.disconnected.connect(lambda client=client: self.clients.discard(client))
            deadline.timeout.connect(lambda client=client: client.abort())
            deadline.start(2000)
            client.disconnected.connect(deadline.stop)
            buffer = bytearray()

            def received(client=client, buffer=buffer, deadline=deadline):
                if client.bytesAvailable() > 64 - len(buffer):
                    client.abort()
                    return
                buffer.extend(bytes(client.readAll()))
                if b"\n" not in buffer:
                    return
                if bytes(buffer) != REQUEST:
                    client.abort()
                    return
                deadline.stop()
                client.write(b"OK\n")
                client.flush()
                client.disconnectFromServer()
                self.activated.emit()

            client.readyRead.connect(received)
            client.disconnected.connect(client.deleteLater)
            received()

    def close(self):
        self.server.close()
        for client in list(self.clients):
            client.abort()
        self.clients.clear()
        if self._lease is not None:
            self._lease.__exit__(None, None, None)
            self._lease = None
