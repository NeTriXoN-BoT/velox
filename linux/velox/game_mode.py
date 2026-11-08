"""Optional, focus-scoped GameMode requests with process-safe recovery."""

from __future__ import annotations

import ctypes as C
import functools
import os
import re
import secrets
import time

from .core import VeloxError, atomic_json, lease, read_json


SERVICE = b"com.feralinteractive.GameMode"


class _Error(C.Structure):
    _fields_ = [
        ("name", C.c_char_p),
        ("message", C.c_char_p),
        ("flags", C.c_uint),
        ("padding", C.c_void_p),
    ]


@functools.lru_cache(maxsize=1)
def _library():
    try:
        library = C.CDLL("libdbus-1.so.3")
    except OSError:
        return None
    pointer, integer = C.c_void_p, C.c_int
    declarations = (
        ("dbus_threads_init_default", integer, []),
        ("dbus_error_init", None, [C.POINTER(_Error)]),
        ("dbus_error_free", None, [C.POINTER(_Error)]),
        ("dbus_bus_get_private", pointer, [integer, C.POINTER(_Error)]),
        ("dbus_connection_set_exit_on_disconnect", None, [pointer, integer]),
        ("dbus_connection_close", None, [pointer]),
        ("dbus_connection_unref", None, [pointer]),
        ("dbus_message_new_method_call", pointer, [C.c_char_p] * 4),
        ("dbus_message_unref", None, [pointer]),
        (
            "dbus_connection_send_with_reply_and_block",
            pointer,
            [pointer, pointer, integer, C.POINTER(_Error)],
        ),
        ("dbus_message_get_type", integer, [pointer]),
        ("dbus_message_append_args", integer, [pointer, integer]),
        ("dbus_message_get_args", integer, [pointer, C.POINTER(_Error), integer]),
    )
    for name, result, arguments in declarations:
        function = getattr(library, name)
        function.restype, function.argtypes = result, arguments
    return library if library.dbus_threads_init_default() else None


def _exchange(library, bus, method, descriptor=None, *, owner=False, destination=None):
    error = _Error()
    library.dbus_error_init(C.byref(error))
    message = reply = requester = None
    try:
        message = library.dbus_message_new_method_call(
            b"org.freedesktop.DBus" if owner else destination.encode() if destination else SERVICE,
            b"/org/freedesktop/DBus" if owner else b"/com/feralinteractive/GameMode",
            b"org.freedesktop.DBus" if owner else SERVICE,
            method,
        )
        if not message:
            return None
        if owner:
            name = C.c_char_p(SERVICE)
            if method == b"StartServiceByName":
                flags = C.c_uint(0)
                accepted = library.dbus_message_append_args(
                    message, 115, C.byref(name), C.c_int(117), C.byref(flags), C.c_int(0)
                )
            elif method == b"GetId":
                accepted = True
            else:
                accepted = library.dbus_message_append_args(message, 115, C.byref(name), C.c_int(0))
        else:
            requester = os.pidfd_open(os.getpid())
            target_fd, requester_fd = C.c_int(descriptor), C.c_int(requester)
            accepted = library.dbus_message_append_args(
                message,
                104,
                C.byref(target_fd),
                C.c_int(104),
                C.byref(requester_fd),
                C.c_int(0),
            )
        if not accepted:
            return None
        reply = library.dbus_connection_send_with_reply_and_block(
            bus, message, 1500, C.byref(error)
        )
        if not reply or library.dbus_message_get_type(reply) != 2:
            # An absent service is a definitive end to an old registration.
            # Other D-Bus failures keep its recovery journal for a later retry.
            return (
                "" if owner and error.name == b"org.freedesktop.DBus.Error.NameHasNoOwner" else None
            )
        activation = method == b"StartServiceByName"
        value = (
            C.c_char_p() if owner and not activation else C.c_uint(0) if activation else C.c_int(-1)
        )
        kind = 117 if activation else 115 if owner else 105
        if not library.dbus_message_get_args(
            reply, C.byref(error), kind, C.byref(value), C.c_int(0)
        ):
            return None
        if owner and not activation:
            result = (value.value or b"").decode("ascii", errors="replace")
            pattern = r"[a-f0-9]{32}" if method == b"GetId" else r":[0-9]+\.[0-9]+"
            return result if re.fullmatch(pattern, result) else None
        return value.value
    finally:
        if requester is not None:
            os.close(requester)
        if reply:
            library.dbus_message_unref(reply)
        if message:
            library.dbus_message_unref(message)
        library.dbus_error_free(C.byref(error))


def _invoke(method, descriptor=None, *, owner=False, destination=None, expected_bus=None):
    # ctypes releases the Python lock during these bounded native calls. A
    # stalled optional host service cannot hold Qt's event loop or input thread.
    library = _library()
    if library is None:
        return None
    error = _Error()
    library.dbus_error_init(C.byref(error))
    bus = None
    try:
        bus = library.dbus_bus_get_private(0, C.byref(error))
        if not bus:
            return None
        library.dbus_connection_set_exit_on_disconnect(bus, 0)
        if (
            expected_bus is not None
            and _exchange(library, bus, b"GetId", owner=True) != expected_bus
        ):
            return None
        return _exchange(library, bus, method, descriptor, owner=owner, destination=destination)
    finally:
        if bus:
            library.dbus_connection_close(bus)
            library.dbus_connection_unref(bus)
        library.dbus_error_free(C.byref(error))


class GameMode:
    def __init__(self):
        self.record = self.descriptor = self.saved = None
        self.status = "idle"
        self.retry_at = 0.0

    def _unavailable(self):
        self.status = "unavailable"
        self.retry_at = time.monotonic() + 30

    @staticmethod
    def call(action, descriptor, owner=None):
        method = "QueryStatusByPIDFd" if action == "QueryStatus" else action + "GameByPIDFd"
        return _invoke(
            method.encode(),
            descriptor,
            destination=owner[1] if owner else None,
            expected_bus=owner[0] if owner else None,
        )

    @staticmethod
    def owner(activate=False):
        identifier = _invoke(b"GetId", owner=True)
        if identifier is None:
            return None
        name = _invoke(b"GetNameOwner", owner=True, expected_bus=identifier)
        if name == "" and activate:
            if _invoke(b"StartServiceByName", owner=True, expected_bus=identifier) in (1, 2):
                name = _invoke(b"GetNameOwner", owner=True, expected_bus=identifier)
        return (identifier, name) if name is not None else None

    @staticmethod
    def _path(phone):
        return phone.folder / "performance.json"

    def _load(self, phone):
        value = read_json(self._path(phone), None, 2048)
        if value is not None and (
            not isinstance(value, dict)
            or set(value) != {"format", "pid", "birth", "bus", "owner", "token"}
            or type(value["format"]) is not int
            or value["format"] != 1
            or type(value["pid"]) is not int
            or value["pid"] <= 1
            or type(value["birth"]) is not int
            or value["birth"] < 0
            or not isinstance(value["bus"], str)
            or not re.fullmatch(r"[a-f0-9]{32}", value["bus"])
            or not isinstance(value["owner"], str)
            or not re.fullmatch(r":[0-9]+\.[0-9]+", value["owner"])
            or not isinstance(value["token"], str)
            or not re.fullmatch(r"[a-f0-9]{32}", value["token"])
        ):
            raise VeloxError("The saved performance recovery record is damaged.")
        return value

    def _forget(self, phone, saved):
        if self._load(phone) == saved:
            self._path(phone).unlink()
            directory = os.open(phone.folder, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        self.abandon()
        self.status = "idle"

    def _recover(self, phone):
        saved = self.saved or self._load(phone)
        if saved is None:
            return True
        self.saved = saved
        if not phone._owned(saved):
            self._forget(phone, saved)
            return True
        owner = self.owner()
        if owner is None:
            self.status = "releasing"
            return False
        if owner != (saved["bus"], saved["owner"]):
            self._forget(phone, saved)
            return True
        descriptor = self.descriptor
        temporary = descriptor is None
        if temporary:
            descriptor = os.pidfd_open(saved["pid"])
        try:
            # Recheck after opening a PID fd, so reused numeric PIDs and other
            # emulator instances can never receive a recovery request.
            if (
                not phone._owned(saved)
                or self.call("Unregister", descriptor, owner) == 0
                or self.call("QueryStatus", descriptor, owner) in (0, 1)
            ):
                self._forget(phone, saved)
                return True
            self.status = "releasing"
            return False
        finally:
            if temporary:
                os.close(descriptor)

    def update(self, phone, wanted):
        with lease(phone.paths.locks / (phone.identity + ".performance")):
            if self.record and (not wanted or not phone._owned(self.record)):
                if not self._recover(phone):
                    return
            if self.record:
                self.status = "active"
                return
            if not self._recover(phone) or not wanted:
                return
            owner = self.owner(activate=True)
            if not owner or not owner[1]:
                self._unavailable()
                return
            record = phone.runtime()
            descriptor = os.pidfd_open(record["pid"])
            try:
                if not phone._owned(record):
                    return
                existing = self.call("QueryStatus", descriptor, owner)
                if existing == 2:
                    self.status = "external"
                    return
                if existing not in (0, 1):
                    self._unavailable()
                    return
                saved = {
                    "format": 1,
                    "pid": record["pid"],
                    "birth": record["birth"],
                    "bus": owner[0],
                    "owner": owner[1],
                    "token": secrets.token_hex(16),
                }
                # Write before the request: a crash or a lost D-Bus reply must
                # not leave an unrecorded change while Android keeps running.
                atomic_json(self._path(phone), saved)
                self.saved = saved
                result = self.call("Register", descriptor, owner)
                if result == 0 and self.owner() == owner:
                    self.record, self.descriptor = record, descriptor
                    descriptor = None
                    self.status = "active"
                elif result is not None and result < 0:
                    # A declined request owns no existing registration.
                    self._forget(phone, saved)
                    self._unavailable()
                else:
                    self._recover(phone)
            finally:
                if descriptor is not None:
                    os.close(descriptor)

    def close(self, phone):
        with lease(phone.paths.locks / (phone.identity + ".performance")):
            return self._recover(phone)

    def abandon(self):
        """Release our fd after shutdown retries; retain any pending disk journal."""
        if self.descriptor is not None:
            os.close(self.descriptor)
        self.descriptor = self.record = self.saved = None
