"""Read-only host telemetry; unsupported or stale sensors stay unavailable."""

from __future__ import annotations

import ctypes as C
import dataclasses
import math
import os
from pathlib import Path
import re
import time

from .core import bounded_read
from .host import cgroup_limits


def read(path, limit=65536):
    try:
        # Kernel sysfs files normally have size zero, so bounded_read's regular
        # file metadata checks are unsuitable here. Cap the actual read instead.
        with open(path, "rb") as source:
            raw = source.read(limit + 1)
        return raw.decode("ascii") if len(raw) <= limit else ""
    except (OSError, UnicodeError):
        return ""


def number(path, divisor=1, maximum=1e15):
    try:
        value = float(read(path, 128).strip()) / divisor
        return value if math.isfinite(value) and 0 <= value <= maximum else None
    except ValueError:
        return None


@dataclasses.dataclass
class Sample:
    captured: float = 0
    phone: str = ""
    cpu: float | None = None
    gpu: float | None = None
    memory: float | None = None
    memory_used: float | None = None
    memory_total: float | None = None
    vram: float | None = None
    vram_used: float | None = None
    vram_total: float | None = None
    cpu_temperature: float | None = None
    gpu_temperature: float | None = None
    vram_temperature: float | None = None
    cpu_clock: float | None = None
    gpu_clock: float | None = None
    cpu_power: float | None = None
    gpu_power: float | None = None
    cpu_fan: float | None = None
    gpu_fan: float | None = None
    gpu_fan_percent: float | None = None
    phone_cpu: float | None = None
    phone_memory: float | None = None


class Nvidia:
    class Memory(C.Structure):
        _fields_ = [("total", C.c_ulonglong), ("free", C.c_ulonglong), ("used", C.c_ulonglong)]

    class Usage(C.Structure):
        _fields_ = [("gpu", C.c_uint), ("memory", C.c_uint)]

    def __init__(self, gpu):
        self.library, self.device = None, None
        folders = (
            "/usr/lib/x86_64-linux-gnu",
            "/lib/x86_64-linux-gnu",
            "/usr/lib64",
            "/usr/lib",
            "/lib64",
            "/run/opengl-driver/lib",
        )
        for folder in folders:
            try:
                path = (Path(folder) / "libnvidia-ml.so.1").resolve(strict=True)
                info = path.stat()
                if info.st_uid != 0 or info.st_mode & 0o022:
                    continue
                self.library = C.CDLL(str(path), mode=os.RTLD_LOCAL | os.RTLD_NOW)
                break
            except OSError:
                continue
        if not self.library:
            return
        if self.call("nvmlInit_v2", [], []) != 0:
            self.library = None
            return
        count = C.c_uint()
        if (
            self.call("nvmlDeviceGetCount_v2", [C.POINTER(C.c_uint)], [C.byref(count)]) != 0
            or count.value > 32
        ):
            return
        matches = []
        for index in range(count.value):
            device, name = C.c_void_p(), C.create_string_buffer(256)
            if (
                self.call(
                    "nvmlDeviceGetHandleByIndex_v2",
                    [C.c_uint, C.POINTER(C.c_void_p)],
                    [index, C.byref(device)],
                )
                != 0
            ):
                continue
            if (
                self.call(
                    "nvmlDeviceGetName",
                    [C.c_void_p, C.c_void_p, C.c_uint],
                    [device, name, len(name)],
                )
                == 0
            ):
                model = name.value.decode("ascii", errors="replace").lower()
                if gpu.lower() in model or model in gpu.lower():
                    matches.append(device)
        if len(matches) == 1:
            self.device = matches[0]

    def call(self, name, types, args):
        function = getattr(self.library, name, None)
        if function is None:
            return -1
        function.argtypes, function.restype = types, C.c_int
        return function(*args)

    def close(self):
        if self.library:
            self.call("nvmlShutdown", [], [])
            self.library, self.device = None, None

    def sample(self, result):
        if not self.device:
            return
        for symbol, key, maximum, divisor, index in (
            ("nvmlDeviceGetTemperature", "gpu_temperature", 130, 1, 0),
            ("nvmlDeviceGetClockInfo", "gpu_clock", 20000, 1, 0),
            ("nvmlDeviceGetPowerUsage", "gpu_power", 2000, 1000, None),
            ("nvmlDeviceGetFanSpeed", "gpu_fan_percent", 100, 1, None),
        ):
            value = C.c_uint()
            types, args = [C.c_void_p], [self.device]
            if index is not None:
                types.append(C.c_uint)
                args.append(index)
            types.append(C.POINTER(C.c_uint))
            args.append(C.byref(value))
            if self.call(symbol, types, args) == 0 and 0 <= value.value / divisor <= maximum:
                setattr(result, key, value.value / divisor)
        memory, usage = self.Memory(), self.Usage()
        if (
            self.call(
                "nvmlDeviceGetMemoryInfo",
                [C.c_void_p, C.POINTER(self.Memory)],
                [self.device, C.byref(memory)],
            )
            == 0
            and 0 < memory.total <= 2**42
            and memory.used <= memory.total
        ):
            result.vram_total, result.vram_used = memory.total / 2**30, memory.used / 2**30
        if (
            self.call(
                "nvmlDeviceGetUtilizationRates",
                [C.c_void_p, C.POINTER(self.Usage)],
                [self.device, C.byref(usage)],
            )
            == 0
            and usage.gpu <= 100
        ):
            result.gpu = usage.gpu


class Sampler:
    def __init__(self, hardware):
        self.hardware = hardware
        self.previous = None
        self.process = None
        self.nvidia = None
        self.gpu = ""

    def close(self):
        if self.nvidia:
            self.nvidia.close()
            self.nvidia = None

    def sample(self, phone, hardware=None):
        self.hardware = hardware or self.hardware
        result = Sample(phone=phone.identity)
        captured = time.monotonic()
        fields = read("/proc/stat", 1024**2).splitlines()
        try:
            ticks = list(map(int, fields[0].split()[1:9]))
            total, idle = sum(ticks), ticks[3] + ticks[4]
            if self.previous:
                last_total, last_idle = self.previous
                if total > last_total and 0 <= idle - last_idle <= total - last_total:
                    result.cpu = 100 * (1 - (idle - last_idle) / (total - last_total))
            self.previous = total, idle
        except (ValueError, IndexError):
            self.previous = None
        info = dict(
            re.findall(r"^(MemTotal|MemAvailable):\s+(\d+) kB", read("/proc/meminfo"), re.M)
        )
        if len(info) == 2:
            _threads, total, available = cgroup_limits(
                self.hardware.threads,
                int(info["MemTotal"]) * 1024,
                int(info["MemAvailable"]) * 1024,
            )
            if total > 0 and 0 <= available <= total:
                result.memory_total, result.memory_used = total / 2**30, (total - available) / 2**30
                result.memory = 100 * (1 - available / total)
        clocks = [
            float(value)
            for value in re.findall(
                r"^cpu MHz\s*:\s*([0-9.]+)$", read("/proc/cpuinfo", 4 * 1024**2), re.M
            )
        ]
        if clocks and all(math.isfinite(value) and 0 < value <= 20000 for value in clocks):
            result.cpu_clock = sum(clocks) / len(clocks)
        temperatures = []
        for chip in list(Path("/sys/class/hwmon").glob("hwmon*"))[:128]:
            name = read(chip / "name", 128).strip()
            if name in ("coretemp", "k10temp", "zenpower", "cpu_thermal"):
                for path in list(chip.glob("temp*_input"))[:256]:
                    value = number(path, 1000, 130)
                    if value is not None:
                        temperatures.append(value)
            for path in list(chip.glob("fan*_label"))[:32]:
                if read(path, 128).strip().lower() in ("cpu", "cpu fan"):
                    result.cpu_fan = number(
                        path.with_name(path.name.replace("_label", "_input")), maximum=30000
                    )
        result.cpu_temperature = max(temperatures) if temperatures else None
        devices = list(Path("/sys/class/drm").glob("card[0-9]*/device"))[:32]
        vendor = (
            "0x1002"
            if re.search(r"amd|radeon", self.hardware.gpu, re.I)
            else "0x8086"
            if "intel" in self.hardware.gpu.lower()
            else ""
        )
        devices = [
            device
            for device in devices
            if vendor and read(device / "vendor", 128).strip() == vendor
        ]
        if len(devices) == 1:
            device = devices[0]
            result.gpu = number(device / "gpu_busy_percent", maximum=100)
            result.vram_total = number(device / "mem_info_vram_total", 2**30, 4096)
            result.vram_used = number(device / "mem_info_vram_used", 2**30, 4096)
            result.gpu_clock = number(device / "gt_cur_freq_mhz", maximum=20000)
            for chip in list((device / "hwmon").glob("hwmon*"))[:8]:
                result.gpu_temperature = number(chip / "temp1_input", 1000, 130)
                result.gpu_power = number(chip / "power1_average", 1e6, 2000)
                result.gpu_fan = number(chip / "fan1_input", maximum=30000)
                for path in list(chip.glob("temp*_label"))[:32]:
                    if "mem" in read(path, 128).lower():
                        result.vram_temperature = number(
                            path.with_name(path.name.replace("_label", "_input")), 1000, 130
                        )
        if self.gpu != self.hardware.gpu:
            self.close()
            self.gpu = self.hardware.gpu
            self.nvidia = Nvidia(self.gpu)
        if self.nvidia:
            self.nvidia.sample(result)
        if (
            result.vram_total
            and result.vram_used is not None
            and result.vram_used <= result.vram_total
        ):
            result.vram = 100 * result.vram_used / result.vram_total
        if phone.alive():
            runtime = phone.runtime()
            pid, birth = runtime["pid"], runtime["birth"]
            try:
                fields = (
                    bounded_read(Path(f"/proc/{pid}/stat"), 65536)
                    .decode()
                    .rsplit(")", 1)[1]
                    .split()
                )
                process_ticks = int(fields[11]) + int(fields[12])
                if (
                    str(fields[19]) == str(birth)
                    and self.process
                    and self.process[:2] == (pid, birth)
                ):
                    delta = (process_ticks - self.process[2]) / os.sysconf("SC_CLK_TCK")
                    if delta >= 0 and captured > self.process[3]:
                        result.phone_cpu = min(
                            100,
                            delta
                            / (captured - self.process[3])
                            / max(1, self.hardware.threads)
                            * 100,
                        )
                self.process = pid, birth, process_ticks, captured
                if str(fields[19]) == str(birth):
                    result.phone_memory = int(fields[21]) * os.sysconf("SC_PAGE_SIZE") / 2**30
            except (OSError, ValueError, IndexError):
                self.process = None
        else:
            self.process = None
        result.captured = time.monotonic()
        return result
