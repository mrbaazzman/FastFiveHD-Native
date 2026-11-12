"""Run a Python script, from a normal interpreter or from a frozen executable.

The port builder is packaged with PyInstaller into a single executable, and inside a frozen
application `sys.executable` is *that executable*, not a Python interpreter.  A script that
shells out with `sys.executable` would therefore run the builder again - forever.

`pyrun.run()` handles both: in a frozen application it re-executes itself with
`--internal-run <script>`, which runs the script in this very interpreter.
"""

import runpy
import subprocess
import sys
from pathlib import Path

# A console program started from a windowed program opens a console window of its own
# unless it is told not to.  Every child here is a build step, and none of them should
# put a window on the user's screen.
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)


def frozen() -> bool:
    return bool(getattr(sys, "frozen", False))


def command_for(script: Path, args) -> list:
    if frozen():
        return [sys.executable, "--internal-run", str(script), *[str(a) for a in args]]
    return [sys.executable, str(script), *[str(a) for a in args]]


def run(script, args=(), cwd=None, capture=True):
    """Run a script and return a CompletedProcess-like object."""
    script = Path(script)
    cmd = command_for(script, args)
    if capture:
        return subprocess.run(cmd, cwd=str(cwd) if cwd else None,
                              capture_output=True, text=True, encoding="utf-8",
                              errors="replace", creationflags=NO_WINDOW)
    return subprocess.run(cmd, cwd=str(cwd) if cwd else None,
                          creationflags=NO_WINDOW)


def popen(script, args=(), cwd=None, **kwargs):
    """Run a script with its output streamed."""
    script = Path(script)
    cmd = command_for(script, args)
    kwargs.setdefault("creationflags", NO_WINDOW)
    return subprocess.Popen(cmd, cwd=str(cwd) if cwd else None, **kwargs)


def run_here(script: Path, args) -> None:
    """Execute a script in this interpreter (the `--internal-run` path)."""
    sys.argv = [str(script), *[str(a) for a in args]]
    try:
        runpy.run_path(str(script), run_name="__main__")
    except SystemExit as exc:
        code = exc.code
        if isinstance(code, int):
            raise
        if code is not None:
            print(code, file=sys.stderr)
            raise SystemExit(1)
        raise SystemExit(0)
