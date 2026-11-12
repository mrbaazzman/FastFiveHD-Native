"""What this machine can afford: CPU count and free memory, without dependencies."""

import ctypes
import os


def cpu_count() -> int:
    try:
        return len(os.sched_getaffinity(0))     # respects the CPU limit
    except AttributeError:
        return os.cpu_count() or 4


def memory_gb() -> tuple:
    """(total, available) memory in GiB."""
    try:
        class MEMORYSTATUSEX(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong),
                        ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong),
                        ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong),
                        ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong),
                        ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

        stat = MEMORYSTATUSEX()
        stat.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(stat)):
            return (stat.ullTotalPhys / 1073741824.0,
                    stat.ullAvailPhys / 1073741824.0)
    except Exception:
        pass
    try:
        page = os.sysconf("SC_PAGE_SIZE")
        pages = os.sysconf("SC_PHYS_PAGES")
        free = os.sysconf("SC_AVPHYS_PAGES")
        return (pages * page / 1073741824.0, free * page / 1073741824.0)
    except (ValueError, OSError, AttributeError):
        return (4.0, 4.0)


def jobs(per_job_gb: float = 1.0, hard_max: int = 8) -> int:
    """How many parallel jobs this machine can afford.

    Compiling the generated chunks is memory hungry: measured, twelve parallel jobs on the
    largest translation units (8.5 MiB of C each) peak at 5 GiB, so a job costs roughly
    half a gigabyte.  The parallelism therefore follows the free memory, not just the core
    count - otherwise the build would swap on a small machine.
    """
    cpus = cpu_count()
    total, available = memory_gb()
    by_memory = int(available // per_job_gb)
    n = max(1, min(cpus, by_memory, hard_max))
    return n


def describe(per_job_gb: float = 1.0) -> str:
    total, available = memory_gb()
    return (f"{cpu_count()} CPUs, {total:.0f} GiB RAM ({available:.0f} GiB free) "
            f"-> {jobs(per_job_gb)} job(s)")
