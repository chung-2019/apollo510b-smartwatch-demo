"""Ambiq Apollo510 nemagfx_watchface build / flash / RTT GUI.

Dependency-free: standard library and Tkinter only.  On Windows the project
Makefile is driven through MSYS2 Bash, matching the commands used by this
project's VS Code tasks (.vscode/tasks.json).

RTT logging is implemented with JLinkGDBServer plus a socket client on its RTT
telnet port, rather than JLinkRTTLogger.  The GDB server is a long-running
service that streams RTT without an intermediate capture file, and unlike
JLinkRTTLogger it does not exit immediately when launched without a console.
"""

from __future__ import annotations

import codecs
import glob
import json
import mmap
import os
import queue
import re
import shlex
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from tkinter import BOTH, END, LEFT, RIGHT, X, BooleanVar, IntVar, StringVar, TclError, Tk
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText


APP_NAME = "Ambiq Apollo510 Watchface Builder"
PROJECT_ROOT = Path(__file__).resolve().parent
TARGET_NAME = "nemagfx_watchface"
DEFAULT_DEVICE = "AP510NFA-CBR"
GDB_PORT = 2331          # JLinkGDBServer default; +1 SWO, +2 telnet
DEFAULT_RTT_PORT = 19021  # JLinkGDBServer default RTT telnet port

# Apollo510 RSTGEN software Power-On-Init.  Shutting down JLinkGDBServer halts
# the core, and on this part that can leave the GPU power domain wedged: the
# firmware then hard-faults inside nema_sys_init() on the next run, and neither
# `r` nor `rnh` clears it.  A POI resets every block except clock-gen/RTC/STIMER
# and does recover it, so the GUI issues one after each RTT session.
RSTGEN_SWPOI_ADDR = "0x40000004"
RSTGEN_SWPOI_KEY = "0x1B"

ANSI_ESCAPE = re.compile(r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])")

ERROR_MARKERS = (
    "could not connect",
    "failed to connect",
    "cannot connect",
    "no valid device has been selected",
    "error while programming",
    "programming failed",
    "failed to download",
    "no emulator connected",
    "no j-link connected",
)

# Windows: keep helper consoles hidden.
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0


# --------------------------------------------------------------------------
# Host discovery helpers
# --------------------------------------------------------------------------
def windows_to_msys(path: Path | str) -> str:
    """Convert a Windows path to the form understood by MSYS2 Bash."""
    text = str(path).replace("\\", "/")
    match = re.match(r"^([A-Za-z]):/(.*)$", text)
    if match:
        return f"/{match.group(1).lower()}/{match.group(2)}"
    return text


def _newest(matches: list[Path]) -> Path | None:
    """Pick the highest-sorting path, so JLink_V840 wins over JLink_V794."""
    if not matches:
        return None
    return sorted(matches, key=lambda item: str(item).lower())[-1].resolve()


def find_bash() -> str:
    for candidate in (r"C:\msys64\usr\bin\bash.exe", r"C:\msys32\usr\bin\bash.exe"):
        if Path(candidate).is_file():
            return candidate
    found = shutil.which("bash")
    return found or r"C:\msys64\usr\bin\bash.exe"


def find_toolchain_bin() -> str:
    found = shutil.which("arm-none-eabi-gcc")
    if found:
        return str(Path(found).parent)
    if os.name == "nt":
        patterns = (
            r"C:\Program Files\Arm\GNU Toolchain*\bin\arm-none-eabi-gcc.exe",
            r"C:\Program Files (x86)\Arm GNU Toolchain*\bin\arm-none-eabi-gcc.exe",
            r"C:\Program Files (x86)\GNU Arm Embedded Toolchain\*\bin\arm-none-eabi-gcc.exe",
        )
        best = _newest([Path(p) for pat in patterns for p in glob.glob(pat)])
        if best:
            return str(best.parent)
    return ""


def find_jlink_dir() -> str:
    """Locate the SEGGER install directory (not just JLink.exe)."""
    found = shutil.which("JLink.exe") or shutil.which("JLink")
    if found:
        return str(Path(found).resolve().parent)
    if os.name == "nt":
        patterns = (
            r"C:\Program Files\SEGGER\JLink*\JLink.exe",
            r"C:\Program Files (x86)\SEGGER\JLink*\JLink.exe",
        )
        best = _newest([Path(p) for pat in patterns for p in glob.glob(pat)])
        if best:
            return str(best.parent)
    return ""


def read_make_variable(makefile: Path, variable: str, default: str) -> str:
    try:
        pattern = re.compile(rf"^\s*{re.escape(variable)}\s*:?=\s*(.+?)\s*$")
        for line in makefile.read_text(encoding="utf-8", errors="replace").splitlines():
            match = pattern.match(line)
            if match:
                return match.group(1).strip()
    except OSError:
        pass
    return default


def read_jlink_directive(script: Path, directive: str, default: str = "") -> str:
    """Read a one-line directive such as `device` or `loadbin` from a J-Link script."""
    try:
        pattern = re.compile(rf"^\s*{re.escape(directive)}\s+(.+?)\s*$", re.IGNORECASE)
        for line in script.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.lstrip().startswith("//"):
                continue
            match = pattern.match(line)
            if match:
                return match.group(1).strip().strip('"')
    except OSError:
        pass
    return default


# --------------------------------------------------------------------------
# Application
# --------------------------------------------------------------------------
class BuildApplication:
    def __init__(self, root: Tk) -> None:
        self.root = root
        self.root.title(APP_NAME)
        self.root.geometry("1080x760")
        self.root.minsize(900, 620)

        self.events: queue.Queue[tuple[str, object]] = queue.Queue()

        # Build state
        self.process: subprocess.Popen[str] | None = None
        self.is_running = False
        self.running_action = ""
        self.start_time = 0.0

        # RTT state
        self.rtt_process: subprocess.Popen[str] | None = None
        self.rtt_socket: socket.socket | None = None
        self.rtt_stop_event = threading.Event()
        self.rtt_running = False
        self.rtt_user_stopped = False

        # Config-backed variables
        self.project_var = StringVar(value=str(PROJECT_ROOT))
        self.bash_var = StringVar(value=find_bash())
        self.toolchain_var = StringVar(value=find_toolchain_bin())
        self.jlink_dir_var = StringVar(value=find_jlink_dir())
        self.jobs_var = IntVar(value=8)
        self.verbose_var = BooleanVar(value=False)
        self.hex_var = BooleanVar(value=False)

        self.rtt_device_var = StringVar(value=DEFAULT_DEVICE)
        self.rtt_interface_var = StringVar(value="SWD")
        self.rtt_speed_var = IntVar(value=4000)
        self.rtt_channel_var = IntVar(value=0)
        self.rtt_auto_addr_var = BooleanVar(value=True)
        self.rtt_address_var = StringVar(value="")
        self.rtt_port_var = IntVar(value=DEFAULT_RTT_PORT)
        self.rtt_recover_var = BooleanVar(value=True)

        self.status_var = StringVar(value="Ready")
        self.rtt_status_var = StringVar(value="RTT not started")

        self.config_path = PROJECT_ROOT / ".build_gui.json"
        self.stale_config_keys: list[str] = []
        self._load_config()
        self._build_layout()

        if self.stale_config_keys:
            self._append_log(
                "The following configured paths are no longer valid and auto-detected ones are used instead: "
                + ", ".join(self.stale_config_keys)
                + "\n",
                "status",
            )

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.after(60, self._poll_events)

    # ---------------------------------------------------------------- config
    def _load_config(self) -> None:
        try:
            data = json.loads(self.config_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return
        # Paths are validated before being restored.  A stale or mis-picked
        # directory in the config must not override the auto-detected value
        # that __init__ already put in the variable -- otherwise the GUI stays
        # broken until the user hand-edits the JSON.
        validators = {
            "toolchain_bin": lambda p: (Path(p) / "arm-none-eabi-gcc.exe").is_file(),
            "jlink_dir": lambda p: (Path(p) / "JLink.exe").is_file(),
            "bash": lambda p: Path(p).is_file(),
            "project": lambda p: (Path(p) / "gcc" / "Makefile").is_file(),
        }
        mapping = {
            "project": self.project_var,
            "bash": self.bash_var,
            "toolchain_bin": self.toolchain_var,
            "jlink_dir": self.jlink_dir_var,
            "rtt_device": self.rtt_device_var,
            "rtt_interface": self.rtt_interface_var,
            "rtt_address": self.rtt_address_var,
        }
        for key, var in mapping.items():
            value = data.get(key)
            if not isinstance(value, str) or not value:
                continue
            check = validators.get(key)
            if check is not None and not check(value):
                self.stale_config_keys.append(key)
                continue
            var.set(value)
        for key, var in (
            ("jobs", self.jobs_var),
            ("rtt_speed", self.rtt_speed_var),
            ("rtt_channel", self.rtt_channel_var),
            ("rtt_port", self.rtt_port_var),
        ):
            value = data.get(key)
            if isinstance(value, int):
                var.set(value)
        for key, var in (
            ("verbose", self.verbose_var),
            ("hex", self.hex_var),
            ("rtt_auto_addr", self.rtt_auto_addr_var),
            ("rtt_recover", self.rtt_recover_var),
        ):
            value = data.get(key)
            if isinstance(value, bool):
                var.set(value)

    def _save_config(self) -> None:
        payload = {
            "project": self.project_var.get(),
            "bash": self.bash_var.get(),
            "toolchain_bin": self.toolchain_var.get(),
            "jlink_dir": self.jlink_dir_var.get(),
            "jobs": self._safe_int(self.jobs_var, 8),
            "verbose": bool(self.verbose_var.get()),
            "hex": bool(self.hex_var.get()),
            "rtt_device": self.rtt_device_var.get(),
            "rtt_interface": self.rtt_interface_var.get(),
            "rtt_speed": self._safe_int(self.rtt_speed_var, 4000),
            "rtt_channel": self._safe_int(self.rtt_channel_var, 0),
            "rtt_auto_addr": bool(self.rtt_auto_addr_var.get()),
            "rtt_recover": bool(self.rtt_recover_var.get()),
            "rtt_address": self.rtt_address_var.get(),
            "rtt_port": self._safe_int(self.rtt_port_var, DEFAULT_RTT_PORT),
        }
        try:
            self.config_path.write_text(
                json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8"
            )
        except OSError:
            pass

    @staticmethod
    def _safe_int(var: IntVar, default: int) -> int:
        try:
            return int(var.get())
        except (TclError, TypeError, ValueError):
            return default

    # ---------------------------------------------------------------- layout
    def _build_layout(self) -> None:
        outer = ttk.Frame(self.root, padding=10)
        outer.pack(fill=BOTH, expand=True)

        # ---- paths ----
        paths = ttk.LabelFrame(outer, text="Paths", padding=8)
        paths.pack(fill=X)
        paths.columnconfigure(1, weight=1)

        rows = (
            ("Project directory", self.project_var, self._browse_project, "dir"),
            ("MSYS2 Bash", self.bash_var, self._browse_bash, "file"),
            ("Arm Toolchain bin", self.toolchain_var, self._browse_toolchain, "dir"),
            ("SEGGER J-Link directory", self.jlink_dir_var, self._browse_jlink, "dir"),
        )
        for index, (label, var, handler, _kind) in enumerate(rows):
            ttk.Label(paths, text=label).grid(row=index, column=0, sticky="w", pady=2)
            ttk.Entry(paths, textvariable=var).grid(
                row=index, column=1, sticky="ew", padx=6, pady=2
            )
            ttk.Button(paths, text="Browse...", width=8, command=handler).grid(
                row=index, column=2, pady=2
            )
        ttk.Button(paths, text="Auto-detect", command=self._auto_detect).grid(
            row=0, column=3, padx=(6, 0), pady=2
        )
        ttk.Button(paths, text="Check environment", command=self._check_environment).grid(
            row=1, column=3, padx=(6, 0), pady=2
        )

        # ---- build options ----
        options = ttk.LabelFrame(outer, text="Build options", padding=8)
        options.pack(fill=X, pady=(8, 0))
        ttk.Label(options, text="Parallel jobs -j").pack(side=LEFT)
        ttk.Spinbox(
            options, from_=1, to=64, width=5, textvariable=self.jobs_var
        ).pack(side=LEFT, padx=(4, 14))
        ttk.Checkbutton(
            options, text="Show full compile commands (VERBOSE=1)", variable=self.verbose_var
        ).pack(side=LEFT, padx=(0, 14))
        ttk.Checkbutton(
            options, text="Also produce Intel HEX", variable=self.hex_var
        ).pack(side=LEFT)

        # ---- actions ----
        actions = ttk.Frame(outer)
        actions.pack(fill=X, pady=(8, 0))
        self.action_buttons: dict[str, ttk.Button] = {}
        for key, text in (
            ("build", "Build"),
            ("rebuild", "Rebuild"),
            ("clean", "Clean"),
            ("flash", "Flash"),
            ("build_flash", "Build and flash"),
        ):
            button = ttk.Button(
                actions, text=text, width=12, command=lambda k=key: self._start(k)
            )
            button.pack(side=LEFT, padx=(0, 6))
            self.action_buttons[key] = button
        self.stop_button = ttk.Button(
            actions, text="Stop", width=10, command=self._stop, state="disabled"
        )
        self.stop_button.pack(side=LEFT, padx=(6, 0))
        ttk.Button(actions, text="Open output directory", command=self._open_output).pack(
            side=RIGHT
        )

        # ---- notebook ----
        self.notebook = ttk.Notebook(outer)
        self.notebook.pack(fill=BOTH, expand=True, pady=(10, 0))

        build_frame = ttk.Frame(self.notebook, padding=6)
        self.notebook.add(build_frame, text="Build log")
        self.log = ScrolledText(
            build_frame, wrap="none", height=18, font=("Consolas", 9)
        )
        self.log.pack(fill=BOTH, expand=True)
        self.log.configure(state="disabled")
        self.log.tag_configure("command", foreground="#0a58ca")
        self.log.tag_configure("error", foreground="#b02a37")
        self.log.tag_configure("status", foreground="#0f5132")

        build_tools = ttk.Frame(build_frame)
        build_tools.pack(fill=X, pady=(6, 0))
        ttk.Button(build_tools, text="Clear", command=self._clear_log).pack(side=LEFT)
        ttk.Button(build_tools, text="Copy", command=self._copy_log).pack(
            side=LEFT, padx=(6, 0)
        )

        self.rtt_frame = ttk.Frame(self.notebook, padding=6)
        self.notebook.add(self.rtt_frame, text="RTT log")
        self._build_rtt_tab(self.rtt_frame)

        # ---- status bar ----
        status = ttk.Frame(outer)
        status.pack(fill=X, pady=(8, 0))
        ttk.Label(status, textvariable=self.status_var).pack(side=LEFT)
        ttk.Label(status, textvariable=self.rtt_status_var).pack(side=RIGHT)

    def _build_rtt_tab(self, parent: ttk.Frame) -> None:
        controls = ttk.LabelFrame(parent, text="RTT settings", padding=8)
        controls.pack(fill=X)

        line1 = ttk.Frame(controls)
        line1.pack(fill=X)
        ttk.Label(line1, text="Device").pack(side=LEFT)
        ttk.Entry(line1, textvariable=self.rtt_device_var, width=18).pack(
            side=LEFT, padx=(4, 12)
        )
        ttk.Label(line1, text="Interface").pack(side=LEFT)
        ttk.Combobox(
            line1,
            textvariable=self.rtt_interface_var,
            values=("SWD", "JTAG"),
            width=6,
            state="readonly",
        ).pack(side=LEFT, padx=(4, 12))
        ttk.Label(line1, text="Speed kHz").pack(side=LEFT)
        ttk.Spinbox(
            line1, from_=100, to=50000, increment=100,
            width=8, textvariable=self.rtt_speed_var,
        ).pack(side=LEFT, padx=(4, 12))
        ttk.Label(line1, text="Channel").pack(side=LEFT)
        ttk.Spinbox(
            line1, from_=0, to=15, width=4, textvariable=self.rtt_channel_var
        ).pack(side=LEFT, padx=(4, 12))
        ttk.Label(line1, text="Telnet Port").pack(side=LEFT)
        ttk.Spinbox(
            line1, from_=1024, to=65535, width=8, textvariable=self.rtt_port_var
        ).pack(side=LEFT, padx=(4, 0))

        line2 = ttk.Frame(controls)
        line2.pack(fill=X, pady=(6, 0))
        ttk.Checkbutton(
            line2,
            text="Take the _SEGGER_RTT address from the .axf automatically",
            variable=self.rtt_auto_addr_var,
            command=self._toggle_rtt_address,
        ).pack(side=LEFT)
        ttk.Label(line2, text="Control block address").pack(side=LEFT, padx=(12, 0))
        self.rtt_address_entry = ttk.Entry(
            line2, textvariable=self.rtt_address_var, width=14
        )
        self.rtt_address_entry.pack(side=LEFT, padx=(4, 12))
        ttk.Button(line2, text="Detect address", command=self._detect_rtt_address_action).pack(
            side=LEFT
        )
        self._toggle_rtt_address()

        line3 = ttk.Frame(controls)
        line3.pack(fill=X, pady=(6, 0))
        ttk.Checkbutton(
            line3,
            text="Send a software POI reset after stopping RTT (recommended: closing the GDB Server leaves the GPU power domain stuck)",
            variable=self.rtt_recover_var,
        ).pack(side=LEFT)

        buttons = ttk.Frame(parent)
        buttons.pack(fill=X, pady=(8, 0))
        self.rtt_start_button = ttk.Button(
            buttons, text="Start RTT", width=12, command=self._start_rtt
        )
        self.rtt_start_button.pack(side=LEFT)
        self.rtt_stop_button = ttk.Button(
            buttons, text="Stop RTT", width=12, command=self._stop_rtt, state="disabled"
        )
        self.rtt_stop_button.pack(side=LEFT, padx=(6, 0))
        ttk.Button(buttons, text="Clear", command=self._clear_rtt_log).pack(
            side=LEFT, padx=(18, 0)
        )
        ttk.Button(buttons, text="Save as...", command=self._save_rtt_log).pack(
            side=LEFT, padx=(6, 0)
        )

        self.rtt_log = ScrolledText(
            parent,
            wrap="word",
            height=16,
            font=("Consolas", 9),
            background="#000000",
            foreground="#ffffff",
            insertbackground="#ffffff",
            selectbackground="#264f78",
            selectforeground="#ffffff",
            borderwidth=0,
        )
        self.rtt_log.pack(fill=BOTH, expand=True, pady=(8, 0))
        self.rtt_log.configure(state="disabled")
        #
        # Brighter tag colours so status / error lines stay readable on black.
        #
        self.rtt_log.tag_configure("status", foreground="#4ec9b0")
        self.rtt_log.tag_configure("error", foreground="#f14c4c")

    def _toggle_rtt_address(self) -> None:
        state = "disabled" if self.rtt_auto_addr_var.get() else "normal"
        self.rtt_address_entry.configure(state=state)

    # -------------------------------------------------------------- browsing
    def _browse_project(self) -> None:
        chosen = filedialog.askdirectory(initialdir=self.project_var.get() or ".")
        if chosen:
            self.project_var.set(chosen)

    def _browse_bash(self) -> None:
        chosen = filedialog.askopenfilename(
            title="Select MSYS2 bash.exe",
            filetypes=[("bash", "bash.exe"), ("All files", "*.*")],
        )
        if chosen:
            self.bash_var.set(chosen)

    def _browse_toolchain(self) -> None:
        chosen = filedialog.askdirectory(
            title="Select the Arm Toolchain bin directory (containing arm-none-eabi-gcc.exe)",
            initialdir=self.toolchain_var.get() or ".",
        )
        if not chosen:
            return
        # Note this is the compiler's bin directory, not the project's gcc/bin
        # output directory -- an easy and previously silent mistake.
        if not (Path(chosen) / "arm-none-eabi-gcc.exe").is_file():
            if not messagebox.askyesno(
                "That directory does not look right",
                f"There is no arm-none-eabi-gcc.exe in this directory:\n{chosen}\n\n"
                "Please select the bin directory of the Arm GNU Toolchain, for example\n"
                r"C:\Program Files\Arm\GNU Toolchain ...\bin"
                "\n\n(Note: not the project's gcc\\bin build output directory.)\n\n"
                "Use this directory anyway?",
                icon="warning",
            ):
                return
        self.toolchain_var.set(chosen)

    def _browse_jlink(self) -> None:
        chosen = filedialog.askdirectory(
            title="Select the SEGGER J-Link installation directory (containing JLink.exe)",
            initialdir=self.jlink_dir_var.get() or ".",
        )
        if not chosen:
            return
        if not (Path(chosen) / "JLink.exe").is_file():
            if not messagebox.askyesno(
                "That directory does not look right",
                f"There is no JLink.exe in this directory:\n{chosen}\n\n"
                "Use this directory anyway?",
                icon="warning",
            ):
                return
        self.jlink_dir_var.set(chosen)

    def _auto_detect(self) -> None:
        self.bash_var.set(find_bash())
        self.toolchain_var.set(find_toolchain_bin())
        self.jlink_dir_var.set(find_jlink_dir())
        self._append_log("Toolchain paths re-detected.\n", "status")
        self._save_config()

    # ----------------------------------------------------------- validation
    def _project(self) -> Path:
        return Path(self.project_var.get().strip()).expanduser().resolve()

    def _gcc_dir(self) -> Path:
        return self._project() / "gcc"

    def _validate_build(self, *, require_compiler: bool) -> tuple[Path, Path, Path]:
        errors: list[str] = []
        project = self._project()
        if not (project / "gcc" / "Makefile").is_file():
            errors.append(f"gcc/Makefile not found under the project directory: {project}")

        bash = Path(self.bash_var.get().strip()).expanduser()
        if not bash.is_file():
            errors.append(f"MSYS2 bash not found: {bash}")

        tool_dir = Path(self.toolchain_var.get().strip()).expanduser()
        if require_compiler:
            gcc = tool_dir / "arm-none-eabi-gcc.exe"
            if not tool_dir.is_dir() or not (gcc.is_file() or shutil.which("arm-none-eabi-gcc")):
                hint = find_toolchain_bin()
                message = f"arm-none-eabi-gcc.exe not found in the \"Arm Toolchain bin\" directory:\n{tool_dir}"
                if hint and hint != str(tool_dir):
                    message += f"\n\nThe detected correct path should be:\n{hint}\nPress \"Auto-detect\" to fix it."
                else:
                    message += (
                        "\n\nThis field needs the bin directory of the Arm GNU Toolchain, "
                        "not the project's gcc\\bin build output directory."
                    )
                errors.append(message)

        if errors:
            raise ValueError("\n".join(errors))
        return project, bash, tool_dir

    def _jlink_exe(self) -> Path:
        directory = Path(self.jlink_dir_var.get().strip()).expanduser()
        candidate = directory / ("JLink.exe" if os.name == "nt" else "JLinkExe")
        if candidate.is_file():
            return candidate.resolve()
        raise ValueError(f"JLink executable not found: {candidate}")

    def _gdb_server_exe(self) -> Path:
        directory = Path(self.jlink_dir_var.get().strip()).expanduser()
        for name in ("JLinkGDBServerCL.exe", "JLinkGDBServerCLExe", "JLinkGDBServerCL"):
            candidate = directory / name
            if candidate.is_file():
                return candidate.resolve()
        raise ValueError(f"JLinkGDBServerCL not found: {directory}")

    # ------------------------------------------------------- command compose
    def _compose_command(self, action: str, project: Path, tool_dir: Path) -> str:
        path_parts: list[str] = []
        if tool_dir and str(tool_dir) not in ("", ".") and tool_dir.is_dir():
            path_parts.append(shlex.quote(windows_to_msys(tool_dir)))
        path_parts.extend(("/usr/bin", "/bin", '"$PATH"'))
        path_value = ":".join(path_parts)

        work_dir = shlex.quote(windows_to_msys(project / "gcc"))
        jobs = max(1, min(64, self._safe_int(self.jobs_var, 8)))
        verbose = " VERBOSE=1" if self.verbose_var.get() else ""
        # `make directories` first so the parallel compile never races on mkdir.
        build = f"make directories && make -j{jobs}{verbose} all"

        if action == "clean":
            operation = "make clean"
        elif action == "rebuild":
            operation = f"make clean && {build}"
        elif action == "flash":
            operation = ""
        else:
            operation = build

        wants_hex = action not in ("clean", "flash") and self.hex_var.get()
        if wants_hex:
            operation += " && make hex"

        if action in ("flash", "build_flash"):
            operation = f"{operation} && make flash" if operation else "make flash"

        # TMP fallback: without it GCC drops temporaries in an unwritable C:\Windows\.
        return (
            f"export PATH={path_value}; test -n \"$TMP\" || export TMP=/tmp; "
            f"cd {work_dir} && {operation}"
        )

    # ------------------------------------------------------------ build flow
    def _start(self, action: str) -> None:
        if self.is_running:
            return
        if action in ("flash", "build_flash") and self.rtt_running:
            messagebox.showwarning(
                "RTT is using the J-Link",
                "RTT is currently using the J-Link; stop RTT before flashing.",
            )
            return
        try:
            project, bash, tool_dir = self._validate_build(
                require_compiler=action != "flash"
            )
            if action in ("flash", "build_flash"):
                self._jlink_exe()  # raise early if missing
                script = project / "gcc" / "flash.jlink"
                if not script.is_file():
                    raise ValueError(f"Flashing script not found: {script}")
                device = read_jlink_directive(script, "device", "unspecified")
                image = read_jlink_directive(script, "loadbin", "unspecified")
                if not messagebox.askyesno(
                    "Confirm J-Link flashing",
                    "This will write the target board's MRAM and reset it to run.\n\n"
                    f"Device: {device}\n"
                    f"Image:  {image}\n"
                    f"Script: {script}\n\n"
                    "Confirm the board is powered and the J-Link is connected. Continue?",
                    icon="warning",
                ):
                    return
            command = self._compose_command(action, project, tool_dir)
        except (OSError, ValueError) as exc:
            messagebox.showerror("Cannot start", str(exc))
            return

        labels = {
            "build": "Build",
            "rebuild": "Rebuild",
            "clean": "Clean",
            "flash": "Flash",
            "build_flash": "Build and flash",
        }
        self.running_action = labels[action]
        self.start_time = time.monotonic()
        self.status_var.set(f"{self.running_action} in progress...")
        self._set_running(True)
        self.notebook.select(0)
        self._append_log(f"\n=== {self.running_action} ===\n$ {command}\n\n", "command")
        self._save_config()

        threading.Thread(
            target=self._run_process,
            args=(bash, command, project),
            name="ambiq-build",
            daemon=True,
        ).start()

    def _run_process(self, bash: Path, command: str, project: Path) -> None:
        return_code = -1
        error_detected = False
        try:
            self.process = subprocess.Popen(
                [str(bash), "-c", command],
                cwd=str(project),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                creationflags=CREATE_NO_WINDOW,
            )
            assert self.process.stdout is not None
            for line in self.process.stdout:
                clean = ANSI_ESCAPE.sub("", line)
                lowered = clean.lower()
                if any(marker in lowered for marker in ERROR_MARKERS):
                    error_detected = True
                self.events.put(("log", clean))
            return_code = self.process.wait()
            # J-Link Commander can report failures while still exiting 0.
            if return_code == 0 and error_detected:
                return_code = 2
        except OSError as exc:
            self.events.put(("log_error", f"Cannot execute: {exc}\n"))
        finally:
            self.process = None
        self.events.put(("done", return_code))

    def _stop(self) -> None:
        process = self.process
        if process is None:
            return
        self.status_var.set("Stopping...")
        try:
            process.terminate()
        except OSError:
            pass

    def _finish(self, return_code: int) -> None:
        elapsed = time.monotonic() - self.start_time
        self.is_running = False
        self._set_running(False)
        if return_code == 0:
            self.status_var.set(f"{self.running_action} complete ({elapsed:.1f} s)")
            self._append_log(
                f"\n=== {self.running_action} complete, took {elapsed:.1f} s ===\n", "status"
            )
            self._append_artifacts()
        else:
            self.status_var.set(f"{self.running_action} failed (code {return_code})")
            self._append_log(
                f"\n=== {self.running_action} failed, exit code {return_code} ===\n", "error"
            )

    def _append_artifacts(self) -> None:
        config = read_make_variable(self._gcc_dir() / "Makefile", "CONFIG", "bin")
        target = read_make_variable(self._gcc_dir() / "Makefile", "TARGET", TARGET_NAME)
        out_dir = self._gcc_dir() / config
        lines = []
        for suffix in (".axf", ".bin", ".hex"):
            artifact = out_dir / f"{target}{suffix}"
            if artifact.is_file():
                lines.append(f"  {artifact.name}  {artifact.stat().st_size:,} bytes")
        if lines:
            self._append_log("Output files:\n" + "\n".join(lines) + "\n", "status")

    def _set_running(self, running: bool) -> None:
        self.is_running = running
        for button in self.action_buttons.values():
            button.configure(state="disabled" if running else "normal")
        self.stop_button.configure(state="normal" if running else "disabled")

    # -------------------------------------------------------------- RTT flow
    def _detect_rtt_address(self) -> str:
        """Read the _SEGGER_RTT symbol address out of the linked .axf."""
        config = read_make_variable(self._gcc_dir() / "Makefile", "CONFIG", "bin")
        target = read_make_variable(self._gcc_dir() / "Makefile", "TARGET", TARGET_NAME)
        axf = self._gcc_dir() / config / f"{target}.axf"
        if not axf.is_file():
            raise ValueError(f"{axf.name} not found; build the project first.")

        tool_dir = Path(self.toolchain_var.get().strip()).expanduser()
        nm = tool_dir / "arm-none-eabi-nm.exe"
        nm_cmd = str(nm) if nm.is_file() else (shutil.which("arm-none-eabi-nm") or "")
        if not nm_cmd:
            raise ValueError("arm-none-eabi-nm not found; cannot auto-detect the RTT address.")

        try:
            output = subprocess.run(
                [nm_cmd, str(axf)],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
                timeout=60, creationflags=CREATE_NO_WINDOW,
            ).stdout
        except (OSError, subprocess.SubprocessError) as exc:
            raise ValueError(f"Running arm-none-eabi-nm failed: {exc}") from exc

        for line in output.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2] == "_SEGGER_RTT":
                return f"0x{parts[0].upper()}"
        raise ValueError(
            "The _SEGGER_RTT symbol was not found in the .axf.\n"
            "Check that gcc/Makefile defines AM_USE_RTT_LOG and includes SEGGER_RTT.c."
        )

    def _detect_rtt_address_action(self) -> None:
        try:
            address = self._detect_rtt_address()
        except ValueError as exc:
            messagebox.showerror("Detection failed", str(exc))
            return
        self.rtt_address_var.set(address)
        self._append_rtt_log(f"Detected _SEGGER_RTT = {address}\n", "status")

    def _start_rtt(self) -> None:
        if self.rtt_running:
            return
        if self.is_running:
            messagebox.showwarning(
                "Build in progress", "Wait for the current build / flash to finish before starting RTT."
            )
            return
        try:
            gdb = self._gdb_server_exe()
            device = self.rtt_device_var.get().strip() or DEFAULT_DEVICE
            interface = self.rtt_interface_var.get().strip().upper()
            speed = self._safe_int(self.rtt_speed_var, 4000)
            channel = self._safe_int(self.rtt_channel_var, 0)
            port = self._safe_int(self.rtt_port_var, DEFAULT_RTT_PORT)

            if self.rtt_auto_addr_var.get():
                address = self._detect_rtt_address()
                self.rtt_address_var.set(address)
            else:
                address = self.rtt_address_var.get().strip()
                if address and not re.fullmatch(r"0[xX][0-9a-fA-F]+", address):
                    raise ValueError("The RTT address must be a hexadecimal value starting with 0x.")
        except (OSError, ValueError) as exc:
            messagebox.showerror("Cannot start RTT", str(exc))
            return

        # The GDB/SWO/telnet ports are not used by this GUI (only the RTT port
        # is), but the server insists on binding them, so keep them on their
        # documented defaults and let the user retune only the RTT port.
        command = [
            str(gdb),
            "-device", device,
            "-if", interface,
            "-speed", str(speed),
            "-nogui", "-silent",
            "-port", str(GDB_PORT),
            "-swoport", str(GDB_PORT + 1),
            "-telnetport", str(GDB_PORT + 2),
            "-rtttelnetport", str(port),
        ]
        if address:
            command.extend(("-rttaddress", address))

        self.rtt_stop_event = threading.Event()
        self.rtt_user_stopped = False
        self.rtt_running = True
        self.rtt_status_var.set("RTT connecting...")
        self._set_rtt_running(True)
        self.notebook.select(self.rtt_frame)
        self._append_rtt_log(
            "\n=== Starting SEGGER RTT ===\n"
            f"Device={device}, If={interface}, Speed={speed} kHz, "
            f"Channel={channel}, Addr={address or '(auto search)'}\n"
            f"$ {subprocess.list2cmdline(command)}\n\n",
            "status",
        )
        self._save_config()

        # Resolve everything the worker needs here, on the main thread:
        # Tkinter variables must never be read from a worker thread.
        work_dir = str(self._gcc_dir())
        recover = (
            (str(self._jlink_exe()), device) if self.rtt_recover_var.get() else None
        )

        threading.Thread(
            target=self._run_rtt,
            args=(command, port, work_dir, recover, self.rtt_stop_event),
            name="ambiq-rtt",
            daemon=True,
        ).start()

    def _poi_recover(self, jlink_exe: str, device: str) -> None:
        """Issue a software POI so the core resumes after the GDB server exits."""
        script = Path(os.environ.get("TEMP", ".")) / "_ambiq_poi.jlink"
        try:
            script.write_text(
                f"device {device}\n"
                "si SWD\n"
                "speed 4000\n"
                "connect\n"
                f"w4 {RSTGEN_SWPOI_ADDR} {RSTGEN_SWPOI_KEY}\n"
                "q\n",
                encoding="ascii",
            )
            subprocess.run(
                [jlink_exe, "-nogui", "1", "-CommanderScript", str(script)],
                stdin=subprocess.DEVNULL, capture_output=True, text=True,
                timeout=60, creationflags=CREATE_NO_WINDOW,
            )
            self.events.put(
                ("rtt_status", "Software POI sent; the target has restarted.\n")
            )
        except (OSError, subprocess.SubprocessError) as exc:
            self.events.put(("rtt_error", f"POI reset failed: {exc}\n"))

    def _run_rtt(
        self,
        command: list[str],
        port: int,
        work_dir: str,
        recover: tuple[str, str] | None,
        stop_event: threading.Event,
    ) -> None:
        return_code = -1
        try:
            self.rtt_process = subprocess.Popen(
                command,
                cwd=work_dir,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                creationflags=CREATE_NO_WINDOW,
            )
            threading.Thread(
                target=self._drain_rtt_server_output,
                args=(self.rtt_process, stop_event),
                name="ambiq-rtt-server-log",
                daemon=True,
            ).start()

            # Wait for the server to accept connections on the RTT telnet port.
            sock = None
            deadline = time.time() + 20
            while time.time() < deadline and not stop_event.is_set():
                if self.rtt_process.poll() is not None:
                    break
                try:
                    sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                    break
                except OSError:
                    time.sleep(0.4)

            if sock is None:
                if not stop_event.is_set():
                    self.events.put(
                        ("rtt_error", f"Cannot connect to RTT telnet port {port}.\n")
                    )
            else:
                self.rtt_socket = sock
                sock.settimeout(0.5)
                self.events.put(("rtt_connected", port))
                # Incremental decoder: a multi-byte sequence split across two
                # recv() calls is held back rather than mangled into U+FFFD.
                decoder = codecs.getincrementaldecoder("utf-8")("replace")
                while not stop_event.is_set():
                    try:
                        chunk = sock.recv(4096)
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    if not chunk:
                        break
                    text = decoder.decode(chunk)
                    if text:
                        self.events.put(("rtt_data", text))
                tail = decoder.decode(b"", final=True)
                if tail:
                    self.events.put(("rtt_data", tail))

            if self.rtt_process is not None:
                if self.rtt_process.poll() is None:
                    self.rtt_process.terminate()
                try:
                    return_code = self.rtt_process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    self.rtt_process.kill()
                    return_code = self.rtt_process.wait()
        except OSError as exc:
            self.events.put(("rtt_error", f"RTT execution failed: {exc}\n"))
        finally:
            if self.rtt_socket is not None:
                try:
                    self.rtt_socket.close()
                except OSError:
                    pass
                self.rtt_socket = None
            self.rtt_process = None
            stop_event.set()

        # The GDB server has fully exited by now, so the J-Link is free again.
        if recover is not None:
            self._poi_recover(*recover)

        self.events.put(("rtt_done", return_code))

    def _drain_rtt_server_output(
        self, process: subprocess.Popen[str], stop_event: threading.Event
    ) -> None:
        """Surface GDB-server diagnostics (connection errors, device mismatch)."""
        if process.stdout is None:
            return
        try:
            for line in process.stdout:
                clean = ANSI_ESCAPE.sub("", line)
                lowered = clean.lower()
                if any(marker in lowered for marker in ERROR_MARKERS):
                    self.events.put(("rtt_error", clean))
                elif stop_event.is_set():
                    break
        except (OSError, ValueError):
            pass

    def _stop_rtt(self) -> None:
        if not self.rtt_running:
            return
        self.rtt_user_stopped = True
        self.rtt_status_var.set("RTT stopping...")
        self.rtt_stop_event.set()
        if self.rtt_socket is not None:
            try:
                self.rtt_socket.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        process = self.rtt_process
        if process is not None:
            try:
                process.terminate()
            except OSError:
                pass

    def _finish_rtt(self, return_code: int) -> None:
        self.rtt_running = False
        self._set_rtt_running(False)
        if self.rtt_user_stopped:
            self.rtt_status_var.set("RTT stopped")
            self._append_rtt_log("\n=== RTT stopped ===\n", "status")
        elif return_code == 0:
            self.rtt_status_var.set("RTT ended")
            self._append_rtt_log("\n=== RTT ended ===\n", "status")
        else:
            self.rtt_status_var.set(f"RTT failed (code {return_code})")
            self._append_rtt_log(
                f"\n=== RTT ended, code {return_code} ===\n", "error"
            )

    def _set_rtt_running(self, running: bool) -> None:
        self.rtt_start_button.configure(state="disabled" if running else "normal")
        self.rtt_stop_button.configure(state="normal" if running else "disabled")

    # ------------------------------------------------------------- log utils
    def _append_log(self, text: str, tag: str = "") -> None:
        self.log.configure(state="normal")
        self.log.insert(END, text, tag or ())
        self.log.see(END)
        self.log.configure(state="disabled")

    def _clear_log(self) -> None:
        self.log.configure(state="normal")
        self.log.delete("1.0", END)
        self.log.configure(state="disabled")

    def _copy_log(self) -> None:
        self.root.clipboard_clear()
        self.root.clipboard_append(self.log.get("1.0", END))
        self.status_var.set("Build log copied to the clipboard")

    def _append_rtt_log(self, text: str, tag: str = "") -> None:
        self.rtt_log.configure(state="normal")
        self.rtt_log.insert(END, text, tag or ())
        self.rtt_log.see(END)
        self.rtt_log.configure(state="disabled")

    def _clear_rtt_log(self) -> None:
        self.rtt_log.configure(state="normal")
        self.rtt_log.delete("1.0", END)
        self.rtt_log.configure(state="disabled")

    def _save_rtt_log(self) -> None:
        target = filedialog.asksaveasfilename(
            title="Save the RTT log",
            defaultextension=".log",
            initialfile=f"{TARGET_NAME}_rtt_{time.strftime('%Y%m%d_%H%M%S')}.log",
            filetypes=[("Log", "*.log"), ("Text file", "*.txt"), ("All files", "*.*")],
        )
        if not target:
            return
        try:
            Path(target).write_text(self.rtt_log.get("1.0", END), encoding="utf-8")
        except OSError as exc:
            messagebox.showerror("Save failed", str(exc))
            return
        self.rtt_status_var.set(f"RTT log saved: {Path(target).name}")

    def _open_output(self) -> None:
        config = read_make_variable(self._gcc_dir() / "Makefile", "CONFIG", "bin")
        out_dir = self._gcc_dir() / config
        if not out_dir.is_dir():
            messagebox.showinfo("No output yet", f"Output directory not found: {out_dir}")
            return
        try:
            if os.name == "nt":
                os.startfile(out_dir)  # noqa: S606
            elif sys.platform == "darwin":
                subprocess.Popen(["open", str(out_dir)])
            else:
                subprocess.Popen(["xdg-open", str(out_dir)])
        except OSError as exc:
            messagebox.showerror("Cannot open", str(exc))

    # ---------------------------------------------------- environment check
    @staticmethod
    def _dll_has_device(dll: Path, device: str) -> bool | None:
        """Search JLinkARM.dll for a device name without loading it into RAM.

        Returns True/False, or None when the DLL cannot be inspected.
        """
        if not dll.is_file():
            return None
        needle = device.encode("ascii", "ignore")
        try:
            with dll.open("rb") as stream:
                with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as view:
                    return view.find(needle) != -1
        except (OSError, ValueError):
            return None

    def _check_environment(self) -> None:
        self.notebook.select(0)
        self._append_log("\n=== Environment check ===\n", "command")
        ok = True

        project = self._project()
        makefile = project / "gcc" / "Makefile"
        if makefile.is_file():
            self._append_log(f"  [ OK ] project Makefile: {makefile}\n")
        else:
            ok = False
            self._append_log(f"  [FAIL] {makefile} not found\n", "error")

        bash = Path(self.bash_var.get().strip())
        if bash.is_file():
            self._append_log(f"  [ OK ] MSYS2 Bash: {bash}\n")
        else:
            ok = False
            self._append_log(f"  [FAIL] MSYS2 Bash not found: {bash}\n", "error")

        tool_dir = Path(self.toolchain_var.get().strip())
        gcc = tool_dir / "arm-none-eabi-gcc.exe"
        if gcc.is_file():
            try:
                version = subprocess.run(
                    [str(gcc), "--version"],
                    capture_output=True, text=True, timeout=30,
                    creationflags=CREATE_NO_WINDOW,
                ).stdout.splitlines()[0]
            except (OSError, subprocess.SubprocessError, IndexError):
                version = str(gcc)
            self._append_log(f"  [ OK ] {version}\n")
        else:
            ok = False
            self._append_log(f"  [FAIL] arm-none-eabi-gcc not found: {tool_dir}\n", "error")

        jlink_dir = Path(self.jlink_dir_var.get().strip())
        jlink = jlink_dir / "JLink.exe"
        if jlink.is_file():
            self._append_log(f"  [ OK ] J-Link: {jlink}\n")
            # The Apollo510 flash loader lives inside JLinkARM.dll; V7.x lacks it.
            device = self.rtt_device_var.get().strip() or DEFAULT_DEVICE
            supported = self._dll_has_device(jlink_dir / "JLinkARM.dll", device)
            if supported is True:
                self._append_log(f"  [ OK ] J-Link supports {device}\n")
            elif supported is False:
                ok = False
                self._append_log(
                    f"  [FAIL] this J-Link does not support {device}; install V8.40 or newer\n",
                    "error",
                )
        else:
            ok = False
            self._append_log(f"  [FAIL] JLink.exe not found: {jlink_dir}\n", "error")

        gdb = jlink_dir / "JLinkGDBServerCL.exe"
        if gdb.is_file():
            self._append_log(f"  [ OK ] GDB Server (for RTT): {gdb.name}\n")
        else:
            self._append_log(
                "  [WARN] JLinkGDBServerCL.exe not found; RTT is unavailable\n", "error"
            )

        try:
            address = self._detect_rtt_address()
            self._append_log(f"  [ OK ] _SEGGER_RTT = {address}\n")
        except ValueError as exc:
            self._append_log(f"  [WARN] {exc}\n")

        self._append_log(
            "Environment check result: " + ("all passed\n" if ok else "something did not pass\n"),
            "status" if ok else "error",
        )

    # ------------------------------------------------------------ event loop
    def _poll_events(self) -> None:
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "log":
                    self._append_log(str(payload))
                elif kind == "log_error":
                    self._append_log(str(payload), "error")
                elif kind == "done":
                    self._finish(int(payload))  # type: ignore[arg-type]
                elif kind == "rtt_data":
                    self._append_rtt_log(str(payload))
                elif kind == "rtt_error":
                    self._append_rtt_log(str(payload), "error")
                elif kind == "rtt_status":
                    self._append_rtt_log(str(payload), "status")
                elif kind == "rtt_connected":
                    self.rtt_status_var.set(f"RTT connected (port {payload})")
                    self._append_rtt_log(
                        f"=== Connected to RTT telnet port {payload} ===\n", "status"
                    )
                elif kind == "rtt_done":
                    self._finish_rtt(int(payload))  # type: ignore[arg-type]
        except queue.Empty:
            pass
        self.root.after(60, self._poll_events)

    def _on_close(self) -> None:
        if self.is_running or self.rtt_running:
            if not messagebox.askyesno(
                "Still running", "A build or RTT has not finished. Close anyway?"
            ):
                return
        self.rtt_stop_event.set()
        for process in (self.process, self.rtt_process):
            if process is not None:
                try:
                    process.terminate()
                except OSError:
                    pass
        self._save_config()
        self.root.destroy()


def main() -> None:
    root = Tk()
    try:
        ttk.Style().theme_use("vista" if os.name == "nt" else "clam")
    except TclError:
        pass
    BuildApplication(root)
    root.mainloop()


if __name__ == "__main__":
    main()
