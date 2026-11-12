#!/usr/bin/env python3
"""Fast Five Port Builder - a single executable that builds the native port.

This is the end-user front end.  It bundles the whole Python side of the build (the
bootstrap pipeline, the Ghidra dumper, the recompiler and the native build script) into
one Windows executable, so the user needs no Python installation.

    FastFivePortBuilder.exe                       (graphical)
    FastFivePortBuilder.exe --apk X --data Y      (command line, same pipeline)
    FastFivePortBuilder.exe /S --apk X --data Y   (silent: no window, no output)

The two things it cannot bundle are the two things that are not ours: **Ghidra** (the
analysis of the library) and a **C compiler**.  The builder finds them, and says exactly
what to install when they are missing.

Everything it produces lives in one place:

    %LOCALAPPDATA%/FastFivePortBuilder/
        app/       the toolchain, materialised on the first run
        build/     the native binary, the captures, the writable game data
        analysis/  the extracted library, the P-code, the Ghidra project
"""

import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pyrun  # noqa: E402

APP_NAME = "FastFivePortBuilder"


def default_install_dir() -> Path:
    """Where the game is installed when the user does not pick a folder."""
    base = os.environ.get("LOCALAPPDATA") or str(Path.home())
    return Path(base) / "Programs" / "Fast Five HD"

# Where the bundled toolchain is unpacked and everything is built.
def work_dir() -> Path:
    """Where the builder unpacks itself and keeps its build state.

    FAST5_WORK moves it: the installer uses that so that everything ends up in the
    temporary folder it removes afterwards.
    """
    override = os.environ.get("FAST5_WORK")
    if override:
        return Path(override)
    base = os.environ.get("LOCALAPPDATA") or str(Path.home())
    return Path(base) / APP_NAME


def bundle_root() -> Path:
    """The directory the bundled data was unpacked into (PyInstaller: sys._MEIPASS)."""
    meipass = getattr(sys, "_MEIPASS", None)
    if meipass:
        return Path(meipass)
    return Path(__file__).resolve().parent.parent


def materialise() -> Path:
    """Copy the bundled toolchain into the work directory and return it.

    The scripts work out their own root from their own location, so they have to live
    somewhere that survives the run: the work directory.  The copy is refreshed every
    time, so a new build of the builder always brings its current scripts.
    """
    work = work_dir()
    # FAST5_WORK means "you were unpacked into a folder and you own it": the toolchain
    # lands next to the payload instead of in the user's application data.
    app = work if os.environ.get("FAST5_WORK") else work / "app"
    src = bundle_root()
    app.mkdir(parents=True, exist_ok=True)
    for name in ("tools", "recomp", "runtime"):
        source = src / name
        if source.is_dir():
            shutil.copytree(source, app / name, dirs_exist_ok=True)
    for name in ("play.bat",):
        if (src / name).exists():
            shutil.copy2(src / name, app / name)
    return app


# --------------------------------------------------------------------------- log

class Logger:
    """Collects the pipeline output for the window, and echoes it to the console.

    It also reports progress: the pipeline marks its steps with "[n/7]", and the
    installer prints "NN%" while it copies the game data.
    """

    def __init__(self, sink=None, progress=None):
        self.sink = sink
        self.progress = progress
        self.lines = []

    def write(self, text: str) -> None:
        for line in text.splitlines():
            self.lines.append(line)
            if self.sink is not None:
                self.sink(line)
            else:
                print(line, flush=True)
            self._progress(line)

    def _progress(self, line: str) -> None:
        if self.progress is None:
            return
        step = re.match(r"\[(\d+)/(\d+)\]", line)
        if step:
            done, total = int(step.group(1)), int(step.group(2))
            self.progress(done / (total + 1.0) * 100.0, line.strip())
            return
        pct = re.match(r"\s+(\d+)%", line)
        if pct:
            base = 100.0 / 8.0 * 7.0
            self.progress(base + int(pct.group(1)) / 100.0 * (100.0 / 8.0),
                          line.strip())

    def flush(self) -> None:
        pass


# ---------------------------------------------------------------------- pipeline

def run_install(build_dir: Path, data: str | None, target: Path, logger: Logger) -> int:
    """Copy the built port and the game data into the chosen folder."""
    log = logger.write
    log("")
    log("=" * 70)
    log("Installing the game")
    log("=" * 70)
    args = ["--build", str(build_dir), "--data", str(data or ""), "--install", str(target)]
    # The installer script lives in the materialised toolchain, not in the frozen bundle.
    script = materialise() / "tools" / "installer.py"
    proc = pyrun.popen(script, args,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       encoding="utf-8", errors="replace", bufsize=1)
    assert proc.stdout is not None
    for line in proc.stdout:
        log(line.rstrip())
    return proc.wait()


def run_silent(apk: str | None, data: str | None, extra: list[str],
               install_to: Path | None = None) -> int:
    """Build and install with no window and no output.

    This is the mode for unattended deployments: the exit code says whether it worked
    and the log says what happened.
    """
    app = materialise()
    log_path = work_dir() / "silent.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [app / "tools" / "bootstrap.py"]
    if apk:
        cmd += ["--apk", apk]
    if data:
        cmd += ["--data", data]
    cmd += extra

    with log_path.open("w", encoding="utf-8", errors="replace") as fh:
        fh.write("Fast Five HD - silent install\n")
        fh.write(" ".join(str(c) for c in cmd) + "\n\n")
        fh.flush()
        # Inside a frozen executable a script cannot be run directly; pyrun re-executes
        # this executable with --internal-run.
        proc = pyrun.popen(cmd[0], cmd[1:], cwd=app, stdout=fh, stderr=subprocess.STDOUT)
        rc = proc.wait()
        if rc == 0 and install_to is not None:
            rc = run_silent_install(app / "build", data, install_to, fh)

    if rc == 0:
        with log_path.open("a", encoding="utf-8", errors="replace") as fh:
            fh.write(f"\ninstalled into {install_to}\n")
    return rc


def run_silent_install(build_dir: Path, data: str | None, target: Path, fh) -> int:
    args = ["--build", str(build_dir), "--data", str(data or ""), "--install", str(target)]
    script = work_dir() / "app" / "tools" / "installer.py"
    proc = pyrun.popen(script, args, cwd=build_dir, stdout=fh, stderr=subprocess.STDOUT)
    return proc.wait()


def run_pipeline(apk: str | None, data: str | None, extra: list[str],
                 logger: Logger, gui_done=None, install_to: Path | None = None) -> int:
    """Run the bootstrap pipeline, streaming its output into the logger."""
    app = materialise()
    log = logger.write

    cmd = [app / "tools" / "bootstrap.py"]
    if apk:
        cmd += ["--apk", apk]
    if data:
        cmd += ["--data", data]
    cmd += extra

    log("")
    log("=" * 70)
    log("Fast Five HD - native port builder")
    log("work directory: " + str(app))
    log("=" * 70)

    proc = pyrun.popen(cmd[0], cmd[1:], cwd=app, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True,
                       encoding="utf-8", errors="replace", bufsize=1)
    assert proc.stdout is not None
    for line in proc.stdout:
        log(line.rstrip())
    rc = proc.wait()

    log("")
    if rc == 0:
        exe = app / "build" / "fast5_win.exe"
        if exe.exists():
            log(f"built: {exe}  ({exe.stat().st_size / 1048576:.0f} MiB)")
        else:
            log("the build reported success but produced no executable - see the log above.")
            rc = 1
    if rc == 0 and install_to is not None:
        rc = run_install(app / "build", data, install_to, logger)
        if rc == 0:
            log("")
            log(f"the game is installed in: {install_to}")
            log("Start it with play.bat in that folder.")
    else:
        log(f"the build failed (exit {rc}) - see the log above.")
    if gui_done is not None:
        gui_done(rc)
    return rc


# -------------------------------------------------------------------- graphical

def attach_parent_console() -> None:
    """Use the terminal the builder was started from, if there is one.

    The executable is built as a windowed application, so it has no console of its own and
    nothing flashes on screen.  When it is run from a command prompt, the parent's console is
    attached so the output lands where the user is looking.
    """
    if os.name != "nt":
        return
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        if kernel32.GetConsoleWindow():
            return
        if kernel32.AttachConsole(-1):        # ATTACH_PARENT_PROCESS
            sys.stdout = open("CONOUT$", "w", encoding="utf-8", errors="replace", buffering=1)
            sys.stderr = open("CONOUT$", "w", encoding="utf-8", errors="replace", buffering=1)
            sys.stdin = open("CONIN$", "r", encoding="utf-8", errors="replace")
    except Exception:
        pass


def run_gui() -> int:
    try:
        import tkinter as tk
        from tkinter import filedialog, messagebox, ttk
    except ImportError:
        print("This build has no graphical interface; use the command line instead:\n"
              "    FastFivePortBuilder.exe --apk <file> --data <folder>", file=sys.stderr)
        return 2

    root = tk.Tk()
    root.title(APP_NAME)
    root.geometry("900x520")
    root.minsize(760, 460)

    state = {"apk": tk.StringVar(), "data": tk.StringVar(),
             "install": tk.StringVar(value=str(default_install_dir())),
             "build": None, "queue": queue.Queue()}

    frm = ttk.Frame(root, padding=10)
    frm.pack(fill="x")

    def pick_apk() -> None:
        path = filedialog.askopenfilename(
            title="Choose the game's APK",
            filetypes=[("Android package", "*.apk"), ("All files", "*.*")])
        if path:
            state["apk"].set(path)

    def pick_data() -> None:
        path = filedialog.askdirectory(title="Choose the GloftF5HP data folder")
        if path:
            state["data"].set(path)

    def pick_install() -> None:
        path = filedialog.askdirectory(title="Where should the game be installed?")
        if path:
            state["install"].set(path)

    ttk.Label(frm, text="APK:").grid(row=0, column=0, sticky="w")
    ttk.Entry(frm, textvariable=state["apk"]).grid(row=0, column=1, sticky="ew", padx=6)
    ttk.Button(frm, text="Browse...", command=pick_apk).grid(row=0, column=2)

    ttk.Label(frm, text="Game data:").grid(row=1, column=0, sticky="w", pady=6)
    ttk.Entry(frm, textvariable=state["data"]).grid(row=1, column=1, sticky="ew", padx=6)
    ttk.Button(frm, text="Browse...", command=pick_data).grid(row=1, column=2)

    ttk.Label(frm, text="Install to:").grid(row=2, column=0, sticky="w", pady=6)
    ttk.Entry(frm, textvariable=state["install"]).grid(row=2, column=1, sticky="ew", padx=6)
    ttk.Button(frm, text="Browse...", command=pick_install).grid(row=2, column=2)

    frm.columnconfigure(1, weight=1)

    buttons = ttk.Frame(root, padding=(10, 0, 10, 6))
    buttons.pack(fill="x")
    build_btn = ttk.Button(buttons, text="Install the game", command=lambda: start())
    build_btn.pack(side="left")
    play_btn = ttk.Button(buttons, text="Play", command=lambda: play(), state="disabled")
    play_btn.pack(side="left", padx=8)
    ttk.Button(buttons, text="Open the work folder",
               command=lambda: os.startfile(str(work_dir()))).pack(side="left")

    status = tk.StringVar(value="Ready.")
    status_label = ttk.Label(root, textvariable=status, padding=(10, 8, 10, 4))
    status_label.pack(fill="x")
    bar = ttk.Progressbar(root, mode="determinate", maximum=100)
    bar.pack(fill="x", padx=10)
    ttk.Separator(root).pack(fill="x", pady=8)

    details = tk.Text(root, wrap="none", height=14, font=("Consolas", 9),
                      state="disabled")
    details.pack(fill="both", expand=True, padx=10)

    def put(line: str) -> None:
        details.configure(state="normal")
        details.insert("end", line + "\n")
        details.see("end")
        details.configure(state="disabled")

    def show_progress(percent: float, text: str) -> None:
        bar["value"] = percent
        status.set(text)

    def done(rc: int) -> None:
        state["queue"].put(("done", rc))

    def start() -> None:
        if state["build"] is not None:
            return
        apk = state["apk"].get().strip()
        data = state["data"].get().strip()
        if not apk:
            messagebox.showwarning("Fast Five Port Builder",
                                   "Choose the game's APK first.")
            return
        target = Path(state["install"].get().strip() or default_install_dir())
        build_btn.configure(state="disabled")
        play_btn.configure(state="disabled")
        status.set("Building - this takes about ten minutes ...")
        state["build"] = threading.Thread(
            target=lambda: run_pipeline(apk or None, data or None, [],
                                        Logger(put, show_progress), done,
                                        install_to=target),
            daemon=True)
        state["build"].start()

    def play() -> None:
        # The installed copy is what the user gets; fall back to the build folder.
        target = Path(state["install"].get().strip() or default_install_dir())
        exe = target / "fast5_win.exe"
        if not exe.exists():
            exe = work_dir() / "app" / "build" / "fast5_win.exe"
            target = exe.parent
        if exe.exists():
            # The working directory matters: the game looks for the data tree and writes
            # its own data relative to it.
            subprocess.Popen([str(exe)], cwd=str(target))

    def poll() -> None:
        try:
            while True:
                kind, value = state["queue"].get_nowait()
                if kind == "done":
                    state["build"] = None
                    build_btn.configure(state="normal")
                    if value == 0:
                        play_btn.configure(state="normal")
                        bar["value"] = 100
                        status.set("Done. The game is installed - press Play.")
                    else:
                        status.set("The build failed - the details are below.")
        except queue.Empty:
            pass
        root.after(200, poll)

    root.after(200, poll)
    put(f"work directory: {work_dir()}")
    put("Choose the APK, the game data folder and where to install, then press "
        "'Install the game'.")
    root.mainloop()
    return 0


def main() -> int:
    args = sys.argv[1:]

    # The frozen path: run a bundled script in this interpreter.  The build steps call each
    # other, and inside a frozen application `sys.executable` is this executable, so they
    # have to go through here rather than through a plain subprocess.
    if args and args[0] == "--internal-run":
        if len(args) < 2:
            print("--internal-run needs a script", file=sys.stderr)
            return 2
        pyrun.run_here(Path(args[1]), args[2:])
        return 0

    gui = True
    silent = False
    apk = data = install = None
    extra = []
    i = 0
    while i < len(args):
        a = args[i]
        # Git Bash (and MSYS in general) turn a leading-slash switch into a path:
        # "/S" arrives as "S:/".  Put it back.
        m = re.match(r"^([A-Za-z]):[/\\\\](.*)$", a)
        if m and not os.path.exists(a):
            a = "/" + m.group(1) + ("/" + m.group(2) if m.group(2) else "")
        if a in ("/S", "--silent", "/silent"):
            silent = True
            i += 1
            continue
        if a in ("--apk", "--data", "--install"):
            if i + 1 >= len(args):
                print(f"{a} needs a value", file=sys.stderr)
                return 2
            if a == "--apk":
                apk = args[i + 1]
            elif a == "--data":
                data = args[i + 1]
            else:
                install = args[i + 1]
            i += 2
            continue
        if a in ("--no-gui", "-c", "--cli"):
            gui = False
            i += 1
            continue
        if a in ("--help", "-h"):
            print(__doc__)
            return 0
        extra.append(a)
        i += 1

    # The pipeline runs in the work directory, so the paths have to be absolute.
    if apk:
        apk = str(Path(apk).expanduser().resolve())
    if data:
        data = str(Path(data).expanduser().resolve())
    if install:
        install = str(Path(install).expanduser().resolve())
    target = Path(install).expanduser() if install else default_install_dir()
    if silent:
        return run_silent(apk, data, extra, target)
    if gui and not (apk or data):
        return run_gui()
    attach_parent_console()
    return run_pipeline(apk, data, extra, Logger(), install_to=target)


if __name__ == "__main__":
    sys.exit(main())
