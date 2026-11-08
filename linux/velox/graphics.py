"""Probe the host driver in an isolated process, outside AppImage library paths."""

from __future__ import annotations

import ctypes as C
import ctypes.util
import json
import re
import struct
import sys
from pathlib import Path


def engine_details(log: str) -> dict:
    result = {"mode": "unknown", "renderer": "", "vendor": "", "api": ""}
    for key, marker in (
        ("renderer", "Graphics Adapter"),
        ("vendor", "Graphics Adapter Vendor"),
        ("api", "Graphics API Version"),
    ):
        exact = r"(?!Vendor )" if key == "renderer" else ""
        matches = re.findall(r"(?m)^.*?\| " + marker + " " + exact + r"([^\r\n]{1,500})\r?$", log)
        if matches:
            result[key] = matches[-1].strip()
    if result["renderer"]:
        result["mode"] = (
            "software"
            if any(
                name in result["renderer"].lower()
                for name in (
                    "swiftshader",
                    "llvmpipe",
                    "lavapipe",
                    "softpipe",
                    "software rasterizer",
                )
            )
            else "host"
        )
    return result


def effective_rate(record, preferred):
    if record.get("graphics") == "software" and record.get("settings", {}).get("gpu") == "auto":
        return min(preferred, 30)
    return preferred


def egl_renderer() -> dict:
    library = ctypes_library("EGL")
    graphics = ctypes_library("GLESv2")
    if not library or not graphics:
        return {}

    def function(name, result, arguments):
        value = getattr(library, name)
        value.restype, value.argtypes = result, arguments
        return value

    pointer, integer = C.c_void_p, C.c_int
    initialize = function(
        "eglInitialize", integer, [pointer, C.POINTER(integer), C.POINTER(integer)]
    )
    display_for = function(
        "eglGetPlatformDisplay", pointer, [C.c_uint, pointer, C.POINTER(integer)]
    )
    terminate = function("eglTerminate", integer, [pointer])
    display = display_for(0x31DD, None, None)  # Mesa/NVIDIA surfaceless platform.
    major, minor = integer(), integer()
    if not display or not initialize(display, C.byref(major), C.byref(minor)):
        return {}
    context, surface = None, None
    try:
        bind = function("eglBindAPI", integer, [C.c_uint])
        if not bind(0x30A0):
            return {}
        attributes = (integer * 9)(0x3033, 1, 0x3040, 4, 0x3024, 8, 0x3025, 0, 0x3038)
        config, count = pointer(), integer()
        choose = function(
            "eglChooseConfig",
            integer,
            [pointer, C.POINTER(integer), C.POINTER(pointer), integer, C.POINTER(integer)],
        )
        if not choose(display, attributes, C.byref(config), 1, C.byref(count)) or not count.value:
            return {}
        create = function(
            "eglCreateContext", pointer, [pointer, pointer, pointer, C.POINTER(integer)]
        )
        context = create(display, config, None, (integer * 3)(0x3098, 2, 0x3038))
        pbuffer = function(
            "eglCreatePbufferSurface", pointer, [pointer, pointer, C.POINTER(integer)]
        )
        surface = pbuffer(display, config, (integer * 5)(0x3057, 1, 0x3056, 1, 0x3038))
        current = function("eglMakeCurrent", integer, [pointer, pointer, pointer, pointer])
        if not context or not current(display, surface, surface, context):
            return {}
        graphics.glGetString.restype, graphics.glGetString.argtypes = C.c_char_p, [C.c_uint]
        renderer = (graphics.glGetString(0x1F01) or b"").decode(errors="replace")[:300]
        vendor = (graphics.glGetString(0x1F00) or b"").decode(errors="replace")[:160]
        version = (graphics.glGetString(0x1F02) or b"").decode(errors="replace")[:160]
        lower = renderer.lower()
        return {
            "renderer": renderer,
            "vendor": vendor,
            "version": version,
            "software": any(
                name in lower
                for name in (
                    "llvmpipe",
                    "softpipe",
                    "swiftshader",
                    "software rasterizer",
                    "lavapipe",
                )
            ),
        }
    finally:
        with_cleanup = function("eglMakeCurrent", integer, [pointer, pointer, pointer, pointer])
        with_cleanup(display, None, None, None)
        if surface:
            function("eglDestroySurface", integer, [pointer, pointer])(display, surface)
        if context:
            function("eglDestroyContext", integer, [pointer, pointer])(display, context)
        terminate(display)


def ctypes_library(name):
    try:
        path = C.util.find_library(name)
        return C.CDLL(path) if path else None
    except OSError:
        return None


def vulkan_devices():
    library = ctypes_library("vulkan")
    if not library:
        return []
    pointer, integer, result = C.c_void_p, C.c_uint32, C.c_int32

    class Application(C.Structure):
        _fields_ = [
            ("kind", integer),
            ("next", pointer),
            ("name", C.c_char_p),
            ("version", integer),
            ("engine", C.c_char_p),
            ("engine_version", integer),
            ("api", integer),
        ]

    class Instance(C.Structure):
        _fields_ = [
            ("kind", integer),
            ("next", pointer),
            ("flags", integer),
            ("application", C.POINTER(Application)),
            ("layer_count", integer),
            ("layers", pointer),
            ("extension_count", integer),
            ("extensions", pointer),
        ]

    class MemoryType(C.Structure):
        _fields_ = [("flags", integer), ("heap", integer)]

    class Heap(C.Structure):
        _fields_ = [("bytes", C.c_uint64), ("flags", integer)]

    class Memory(C.Structure):
        _fields_ = [
            ("type_count", integer),
            ("types", MemoryType * 32),
            ("heap_count", integer),
            ("heaps", Heap * 16),
        ]

    for name, returns, arguments in (
        ("vkCreateInstance", result, [C.POINTER(Instance), pointer, C.POINTER(pointer)]),
        ("vkDestroyInstance", None, [pointer, pointer]),
        ("vkEnumeratePhysicalDevices", result, [pointer, C.POINTER(integer), C.POINTER(pointer)]),
        ("vkGetPhysicalDeviceProperties", None, [pointer, pointer]),
        ("vkGetPhysicalDeviceMemoryProperties", None, [pointer, C.POINTER(Memory)]),
    ):
        function = getattr(library, name)
        function.restype, function.argtypes = returns, arguments
    application = Application(0, None, b"Velox hardware probe", 0, None, 0, 1 << 22)
    information = Instance(1, None, 0, C.pointer(application), 0, None, 0, None)
    instance = pointer()
    if library.vkCreateInstance(C.byref(information), None, C.byref(instance)) != 0:
        return []
    try:
        count = integer()
        if (
            library.vkEnumeratePhysicalDevices(instance, C.byref(count), None) != 0
            or not 0 < count.value <= 32
        ):
            return []
        devices = (pointer * count.value)()
        if library.vkEnumeratePhysicalDevices(instance, C.byref(count), devices) != 0:
            return []
        found = []
        for device in devices[: count.value]:
            # Core VkPhysicalDeviceProperties has a stable prefix. The aligned
            # buffer also leaves room for its fixed Vulkan1.0 limits structure.
            properties = (C.c_uint64 * 512)()
            library.vkGetPhysicalDeviceProperties(device, C.byref(properties))
            prefix = C.string_at(C.addressof(properties), 276)
            api, _driver, vendor, _device, kind = struct.unpack_from("=IIIII", prefix)
            memory = Memory()
            library.vkGetPhysicalDeviceMemoryProperties(device, C.byref(memory))
            if memory.type_count > 32 or memory.heap_count > 16:
                continue
            local = sum(heap.bytes for heap in memory.heaps[: memory.heap_count] if heap.flags & 1)
            found.append(
                {
                    "name": prefix[20:].split(b"\0", 1)[0].decode(errors="replace"),
                    "vendor": vendor,
                    "type": kind,
                    "api": api,
                    "dedicated_mb": min(1024 * 1024, local // (1024 * 1024)) if kind == 2 else 0,
                }
            )
        return found
    finally:
        library.vkDestroyInstance(instance, None)


def select_device(devices, renderer=""):
    hardware = [
        device
        for device in devices
        if device.get("type") in (1, 2, 3) and device.get("api", 0) >= (1 << 22 | 1 << 12)
    ]

    def normalized(value):
        value = re.sub(r"\((?:R|TM)\)", "", value, flags=re.I).split("(", 1)[0]
        return re.sub(r"[^a-z0-9]", "", value.casefold())

    render = re.sub(r"[^a-z0-9]", "", re.sub(r"\((?:R|TM)\)", "", renderer, flags=re.I).casefold())
    matching = [
        device
        for device in hardware
        if len(normalized(device["name"])) >= 8 and normalized(device["name"]) in render
    ]
    return max(
        matching or hardware,
        key=lambda item: (item["type"] == 2, item["dedicated_mb"]),
        default=None,
    )


def host_graphics():
    try:
        egl = egl_renderer()
    except (OSError, AttributeError, ValueError):
        egl = {}
    try:
        devices = vulkan_devices()
    except (OSError, AttributeError, ValueError):
        devices = []
    selected = select_device(devices, egl.get("renderer", ""))
    if selected:
        return {
            **egl,
            "emulator_gpu": selected["name"],
            "emulator_software": False,
            "dedicated_mb": selected["dedicated_mb"],
            "devices": devices,
        }
    return {**egl, "devices": devices, "dedicated_mb": 0}


def probe() -> dict:
    from .phone import bounded_process, clean_environment
    from .core import VeloxError

    environment = clean_environment()
    try:
        output = bounded_process(
            [sys.executable, str(Path(__file__).resolve())],
            environment=environment,
            timeout=5,
            maximum=65536,
        )
        value = json.loads(output.splitlines()[-1])
        return value if isinstance(value, dict) else {}
    except (VeloxError, ValueError, IndexError):
        return {}


if __name__ == "__main__":
    try:
        print(json.dumps(host_graphics()))
    except Exception:
        print("{}")
