"""Per-phone stream mute through PulseAudio or PipeWire's Pulse server."""

from __future__ import annotations

import ctypes as C
import time

from .core import VeloxError


class SampleSpec(C.Structure):
    _fields_ = [("format", C.c_int), ("rate", C.c_uint32), ("channels", C.c_uint8)]


class ChannelMap(C.Structure):
    _fields_ = [("channels", C.c_uint8), ("map", C.c_int * 32)]


class Volume(C.Structure):
    _fields_ = [("channels", C.c_uint8), ("values", C.c_uint32 * 32)]


class SinkInput(C.Structure):
    _fields_ = [
        ("index", C.c_uint32),
        ("name", C.c_char_p),
        ("module", C.c_uint32),
        ("client", C.c_uint32),
        ("sink", C.c_uint32),
        ("sample", SampleSpec),
        ("channels", ChannelMap),
        ("volume", Volume),
        ("buffer_usec", C.c_uint64),
        ("sink_usec", C.c_uint64),
        ("resample", C.c_char_p),
        ("driver", C.c_char_p),
        ("mute", C.c_int),
        ("properties", C.c_void_p),
    ]


InfoCallback = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(SinkInput), C.c_int, C.c_void_p)
SuccessCallback = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_void_p)


class Pulse:
    def __init__(self):
        self.loop = self.context = None
        self.library = C.CDLL("libpulse.so.0")
        pointer, integer, unsigned = C.c_void_p, C.c_int, C.c_uint32
        declarations = (
            ("pa_mainloop_new", pointer, []),
            ("pa_mainloop_get_api", pointer, [pointer]),
            ("pa_mainloop_iterate", integer, [pointer, integer, C.POINTER(integer)]),
            ("pa_mainloop_free", None, [pointer]),
            ("pa_context_new", pointer, [pointer, C.c_char_p]),
            ("pa_context_connect", integer, [pointer, C.c_char_p, integer, pointer]),
            ("pa_context_get_state", integer, [pointer]),
            ("pa_context_disconnect", None, [pointer]),
            ("pa_context_unref", None, [pointer]),
            ("pa_operation_get_state", integer, [pointer]),
            ("pa_operation_cancel", None, [pointer]),
            ("pa_operation_unref", None, [pointer]),
            ("pa_context_get_sink_input_info_list", pointer, [pointer, InfoCallback, pointer]),
            (
                "pa_context_set_sink_input_mute",
                pointer,
                [pointer, unsigned, integer, SuccessCallback, pointer],
            ),
            ("pa_proplist_gets", C.c_char_p, [pointer, C.c_char_p]),
        )
        for name, result, arguments in declarations:
            function = getattr(self.library, name)
            function.restype, function.argtypes = result, arguments

    def __enter__(self):
        self.loop = self.library.pa_mainloop_new()
        if not self.loop:
            raise VeloxError("The desktop audio connection could not be opened.")
        try:
            self.context = self.library.pa_context_new(
                self.library.pa_mainloop_get_api(self.loop), b"Velox"
            )
            # Never spawn or reconfigure the user's audio service.
            if not self.context or self.library.pa_context_connect(self.context, None, 1, None) < 0:
                raise VeloxError("The desktop audio service is unavailable.")
            self.wait(lambda: self.library.pa_context_get_state(self.context) == 4)
            return self
        except Exception:
            self.__exit__(None, None, None)
            raise

    def wait(self, finished):
        deadline = time.monotonic() + 2
        while not finished():
            if time.monotonic() > deadline or self.library.pa_context_get_state(self.context) in (
                5,
                6,
            ):
                raise VeloxError("The desktop audio service did not respond.")
            result = C.c_int()
            if self.library.pa_mainloop_iterate(self.loop, 0, C.byref(result)) < 0:
                raise VeloxError("The desktop audio service disconnected.")
            time.sleep(0.005)

    def operation(self, operation):
        if not operation:
            raise VeloxError("The audio service refused this operation.")
        try:
            self.wait(lambda: self.library.pa_operation_get_state(operation) != 0)
        finally:
            self.library.pa_operation_cancel(operation)
            self.library.pa_operation_unref(operation)

    def streams(self):
        result, complete = [], []

        @InfoCallback
        def received(_context, raw, end, _user):
            if end:
                complete.append(end > 0)
            elif raw and len(result) < 4096:
                value = raw.contents

                def prop(name):
                    return self.library.pa_proplist_gets(value.properties, name) or b""

                result.append(
                    {
                        "index": value.index,
                        "client": value.client,
                        "name": value.name or b"",
                        "id": prop(b"application.id"),
                        "pid": prop(b"application.process.id"),
                        "serial": prop(b"object.serial"),
                        "mute": bool(value.mute),
                    }
                )

        self.operation(
            self.library.pa_context_get_sink_input_info_list(self.context, received, None)
        )
        if complete != [True]:
            raise VeloxError("The audio service returned an incomplete stream list.")
        return result

    def mute(self, index, value):
        accepted = []

        @SuccessCallback
        def finished(_context, success, _user):
            accepted.append(bool(success))

        self.operation(
            self.library.pa_context_set_sink_input_mute(
                self.context, index, int(value), finished, None
            )
        )
        return accepted == [True]

    def __exit__(self, *_error):
        if self.context:
            self.library.pa_context_disconnect(self.context)
            self.library.pa_context_unref(self.context)
            self.context = None
        if self.loop:
            self.library.pa_mainloop_free(self.loop)
            self.loop = None


class PhoneMute:
    def __init__(self):
        self.saved = {}

    @staticmethod
    def identity(stream):
        return tuple(stream[field] for field in ("index", "client", "name", "id", "pid", "serial"))

    def update(self, phone, wanted):
        if not wanted and not self.saved:
            return
        if not phone.alive():
            self.saved.clear()
            return
        record = phone.runtime()
        audio_id = record.get("audio_id", "")
        if not audio_id:
            return
        with Pulse() as pulse:
            streams = pulse.streams()
            present = set()
            for stream in streams:
                identity = self.identity(stream)
                present.add(identity)
                own = (
                    stream["id"] == audio_id.encode()
                    and stream["pid"] == str(record["pid"]).encode()
                    and phone._owned(record)
                )
                if wanted and own:
                    if identity not in self.saved and pulse.mute(stream["index"], True):
                        self.saved[identity] = stream["mute"]
                elif identity in self.saved:
                    if own and pulse.mute(stream["index"], self.saved[identity]):
                        del self.saved[identity]
                    elif not own:
                        del self.saved[identity]
            self.saved = {key: value for key, value in self.saved.items() if key in present}
