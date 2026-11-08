from __future__ import annotations

import dataclasses
import fcntl
import os
import platform
import re
import shutil
from pathlib import Path, PurePosixPath

from .core import CPU_CHOICES, RAM_CHOICES, RATES, Settings
from .capabilities import cpu_limit as guest_cpu_limit


def _number(path: str, fallback: int = 0) -> int:
    try:
        return int(Path(path).read_text().strip())
    except (OSError, ValueError):
        return fallback


def kvm_available() -> bool:
    try:
        descriptor = os.open("/dev/kvm", os.O_RDWR | os.O_CLOEXEC)
        try:
            return fcntl.ioctl(descriptor, 0xAE00) == 12
        finally:
            os.close(descriptor)
    except OSError:
        return False


def cgroup_limits(
    threads: int,
    memory: int,
    available: int,
    root=Path("/sys/fs/cgroup"),
    membership=Path("/proc/self/cgroup"),
):
    try:
        groups = membership.read_text().splitlines()
    except OSError:
        return threads, memory, available
    for line in groups:
        fields = line.split(":", 2)
        if len(fields) != 3:
            continue
        relative = PurePosixPath(fields[2])
        if not relative.is_absolute() or ".." in relative.parts:
            continue
        unified = fields[0] == "0" and not fields[1]
        controllers = fields[1].split(",")
        candidates = (
            [(root, "v2")]
            if unified
            else (
                [(root / "cpu", "cpu"), (root / "cpu,cpuacct", "cpu")]
                if "cpu" in controllers
                else [(root / "memory", "memory")]
                if "memory" in controllers
                else []
            )
        )
        for base, kind in candidates:
            current = base.joinpath(*relative.parts[1:])
            while current == base or base in current.parents:
                try:
                    if kind == "v2":
                        quota = (current / "cpu.max").read_text().split()
                        if len(quota) == 2 and quota[0] != "max" and int(quota[1]) > 0:
                            threads = min(threads, max(1, int(quota[0]) // int(quota[1])))
                    elif kind == "cpu":
                        quota, period = (
                            _number(str(current / "cpu.cfs_quota_us")),
                            _number(str(current / "cpu.cfs_period_us")),
                        )
                        if quota > 0 and period > 0:
                            threads = min(threads, max(1, quota // period))
                except (OSError, ValueError):
                    pass
                for name in (
                    ("memory.max", "memory.high")
                    if kind == "v2"
                    else ("memory.limit_in_bytes",)
                    if kind == "memory"
                    else ()
                ):
                    maximum = _number(str(current / name))
                    if maximum > 0:
                        used = _number(
                            str(
                                current
                                / ("memory.current" if kind == "v2" else "memory.usage_in_bytes")
                            )
                        )
                        memory, available = (
                            min(memory, maximum),
                            min(available, max(0, maximum - used)),
                        )
                if current == base:
                    break
                current = current.parent
    return threads, memory, available


@dataclasses.dataclass(frozen=True)
class Hardware:
    cpu: str
    threads: int
    cores: int
    memory: int
    available: int
    gpu: str
    dedicated_mb: int
    software: bool
    kvm: bool
    kvm_exists: bool
    architecture: str
    distro: str
    width: int = 1920
    height: int = 1080
    refresh: int = 60
    guest_limit: int = 64

    @property
    def memory_limit(self) -> int:
        return min(64, max(2, self.memory - max(3, self.memory // 4)))

    @property
    def cpu_limit(self) -> int:
        return min(64, max(1, self.threads), max(1, self.guest_limit))


def detect(root: Path, width=1920, height=1080, refresh=60) -> Hardware:
    cpu, physical = "Processor", set()
    affinity = (
        os.sched_getaffinity(0)
        if hasattr(os, "sched_getaffinity")
        else set(range(os.cpu_count() or 1))
    )
    try:
        for block in Path("/proc/cpuinfo").read_text().split("\n\n"):
            fields = dict(line.split(":", 1) for line in block.splitlines() if ":" in line)
            fields = {key.strip(): value.strip() for key, value in fields.items()}
            cpu = fields.get("model name", cpu)
            if (
                "physical id" in fields
                and "core id" in fields
                and fields.get("processor", "").isdigit()
                and int(fields["processor"]) in affinity
            ):
                physical.add((fields["physical id"], fields["core id"]))
    except OSError:
        pass
    threads = max(1, len(affinity))
    memory, available = 0, 0
    try:
        info = dict(
            re.findall(
                r"^(MemTotal|MemAvailable):\s+(\d+) kB", Path("/proc/meminfo").read_text(), re.M
            )
        )
        memory, available = int(info["MemTotal"]) * 1024, int(info["MemAvailable"]) * 1024
    except (OSError, KeyError, ValueError):
        memory = os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")
        available = memory // 2
    threads, memory, available = cgroup_limits(threads, memory, available)
    gpu, vram, software = "Software graphics", 0, True
    for card in sorted(Path("/sys/class/drm").glob("card[0-9]*/device")):
        try:
            vendor = (card / "vendor").read_text().strip()
            device = (card / "device").read_text().strip()
            gpu = (
                {
                    "0x10de": "NVIDIA",
                    "0x1002": "AMD",
                    "0x8086": "Intel",
                    "0x1414": "Virtual GPU",
                }.get(vendor, "GPU")
                + " "
                + device
            )
            software = not any(Path("/dev/dri").glob("renderD*"))
            vram = max(vram, _number(str(card / "mem_info_vram_total")) // (1024 * 1024))
        except OSError:
            pass
    if gpu == "Software graphics" and any(Path("/dev/dri").glob("renderD*")):
        gpu, software = "Graphics acceleration", False
    if software and any(
        os.access(path, os.R_OK | os.W_OK) for path in ("/dev/nvidia0", "/dev/dxg")
    ):
        gpu, software = "Graphics acceleration", False
    try:
        distro = platform.freedesktop_os_release().get("PRETTY_NAME", "Linux")
    except OSError:
        distro = "Linux"
    return Hardware(
        cpu,
        threads,
        min(threads, len(physical) or max(1, threads // 2)),
        max(1, round(memory / 1024**3)),
        max(0, available // 1024**3),
        gpu,
        vram,
        software,
        kvm_available(),
        Path("/dev/kvm").exists(),
        platform.machine(),
        distro,
        max(1, width),
        max(1, height),
        max(30, refresh),
        guest_cpu_limit(root),
    )


def allocations(settings: Settings, hardware: Hardware) -> tuple[int, int]:
    cpu = min(6, max(1, hardware.threads // 2)) if not settings.cores else settings.cores
    ram = (
        (
            2
            if hardware.memory <= 6
            else 3
            if hardware.memory <= 8
            else 4
            if hardware.memory <= 12
            else 6
            if hardware.memory <= 16
            else 8
        )
        if not settings.memory
        else settings.memory
    )
    cpu = min(cpu, hardware.cpu_limit)
    cpu = max((value for value in CPU_CHOICES if 0 < value <= cpu), default=1)
    ram = max(
        (value for value in RAM_CHOICES if 0 < value <= min(ram, hardware.memory_limit)), default=2
    )
    return cpu, ram


def recommended(hardware: Hardware, occupied_cpu=0, occupied_ram=0) -> Settings:
    settings = Settings()
    cpu, memory = allocations(settings, hardware)
    cpu = min(cpu, max(1, hardware.threads - max(1, hardware.threads // 4) - occupied_cpu))
    memory = min(memory, max(2, min(hardware.memory_limit - occupied_ram, hardware.available - 2)))
    if hardware.dedicated_mb < 2048:
        memory = min(memory, 3 if hardware.memory <= 12 else 4)
    settings.cores = max((n for n in CPU_CHOICES if 0 < n <= cpu), default=0)
    settings.memory = max((n for n in RAM_CHOICES if 0 < n <= memory), default=2)
    constrained = (
        hardware.software
        or hardware.threads <= 4
        or hardware.memory <= 8
        or cpu <= 2
        or memory <= 3
    )
    settings.display = 0 if constrained or min(hardware.width, hardware.height) < 900 else 2
    ceiling = (
        30
        if hardware.software
        else 60
        if constrained
        else 90
        if hardware.dedicated_mb < 2048
        else (144 if abs(hardware.refresh - 144) <= 2 else 120)
    )
    candidates = [rate for rate in RATES if rate <= min(ceiling, hardware.refresh + 2)]
    divisors = [
        rate
        for rate in candidates
        if abs(hardware.refresh / rate - round(hardware.refresh / rate)) < 0.025
    ]
    settings.rate = max(divisors or candidates or [30])
    settings.gpu = "software" if hardware.software else "auto"
    return settings


def requirements(hardware: Hardware, root: Path) -> list[tuple[str, str]]:
    result = []
    if hardware.architecture != "x86_64":
        result.append(("architecture", "This build needs an x86-64 Linux computer."))
    if not hardware.kvm:
        result.append(
            (
                "kvm",
                "Enable CPU virtualization and grant your account access to /dev/kvm."
                if hardware.kvm_exists
                else "KVM is unavailable. Enable CPU virtualization and your distribution's KVM support.",
            )
        )
    if hardware.memory < 6:
        result.append(
            ("memory", "At least 6 GB of host memory is required; 8 GB or more is recommended.")
        )
    if shutil.disk_usage(root).free < 10 * 1024**3:
        result.append(("storage", "Keep at least 10 GB free for Android setup and phone data."))
    return result
