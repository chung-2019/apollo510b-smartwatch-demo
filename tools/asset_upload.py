# -*- coding: utf-8 -*-
"""
Write asset files into the board's eMMC over RTT.

Swapping artwork used to need three firmware flashes (see docs/EMMC_ASSET_STORE.md).
This tool puts the files straight into eMMC with no firmware rebuild at all.

    python tools/asset_upload.py              # GUI
    python tools/asset_upload.py --list       # only list what is on eMMC now
    python tools/asset_upload.py --delete A.BIN B.BIN   # delete these from eMMC
    python tools/asset_upload.py a.bin b.bin  # command line: send these files directly
    python tools/asset_upload.py HOME.png     # images are converted automatically before sending

## How it works

An AssetUploadTask in the firmware polls the RTT down buffer (see src/asset_upload.h).
This tool connects to the RTT telnet port of JLinkGDBServer and throws over
"magic + file name + size + contents + checksum"; once the firmware has it all, it writes to eMMC and reports in the log.

RTT's up and down buffers are separate blocks of memory, so one connection can send a file
and read the firmware's responses at the same time without interference.

## Prerequisites

1. The firmware must include the upload feature (WATCHFACE_ASSET_UPLOAD=1 in the Makefile, on by default)
2. JLinkGDBServer must be running with its RTT telnet port open

   The easiest way is to leave the RTT log window of build_gui.py open -- that is the server it starts.
   Or start one yourself:

       JLinkGDBServerCL -device AP510NFA-CBR -if SWD -speed 4000 -rtttelnetport 19021

## File names

The name sent up has to be one the firmware knows (see src/asset_store.h):

    HOME.BIN  WATCHBG.BIN  HANDHR.BIN  HANDMIN.BIN  HANDSEC.BIN
    MAPTILE0.BIN  MAPTILE1.BIN  MAPTILE2.BIN

The size must match too; a mismatch is accepted by the firmware but leaves the image
missing on screen (it prints short read at boot).
The files produced by tools/asset_export.py and tools/png_to_bin.py are always right.

## Sending an image directly

A file that is not a .BIN goes through png_to_bin.convert() first (Pillow required) with
the output in tools/assets_out/, and is then sent as usual -- so swapping artwork is just
"open the GUI, add the image, upload", with no separate conversion step.

The target is inferred from the file name: HOME.png -> HOME.BIN. With any other name the
GUI opens a dialog to choose one; the command line simply errors out (it has nowhere to
ask). The size need not match exactly, as png_to_bin scales to the target with LANCZOS;
whether the PNG is RGBA or paletted makes no difference either, since conversion always
resamples into the RGB565 or RGBA8888 the firmware wants.
"""
import os
import socket
import struct
import sys
import threading
import time

#
# The PNG -> .BIN conversion is optional.
#
# png_to_bin needs Pillow, and someone who "only wants to upload ready-made .BIN files"
# should not be unable to open the GUI over one missing library, so it is imported lazily
# when an image is actually added. png_to_bin calls sys.exit() outright when Pillow is
# missing, and that has to be caught too, or the GUI would close silently.
#
_png_mod = None


def load_png_to_bin():
    """Return the png_to_bin module, or None when it cannot be loaded (no Pillow)."""
    global _png_mod

    if _png_mod is None:
        here = os.path.dirname(os.path.abspath(__file__))
        if here not in sys.path:
            sys.path.insert(0, here)
        try:
            import png_to_bin
            _png_mod = png_to_bin
        except (ImportError, SystemExit):
            _png_mod = False

    return _png_mod or None


HOST = "127.0.0.1"
PORT = 19021           # JLinkGDBServer's RTT telnet port, the same as build_gui.py

MAGIC = b"ASU1"       # upload one file
LIST_MAGIC = b"ASL1"  # ask the target to list the eMMC contents
DEL_MAGIC = b"ASD1"   # ask the target to delete one file
NAME_MAX = 12

#
#
# The size of each send.
#
CHUNK = 1024

#
# The cap on unacknowledged data (the flow control window).
#
# **This decides whether large files get through at all.** The firmware's RTT down buffer
# is 4 KB (BUFFER_SIZE_DOWN in the Makefile), and once it is full J-Link **simply discards
# the excess** rather than blocking us. So sending at our own pace loses everything past
# 4 KB, the target sits "a few bytes short", and three seconds later it prints
# `abort <name>: timeout` -- every file over 4 KB fails without exception.
#
# The firmware prints one `[upload] ack <bytes>` line per 1 KB received (see
# UPLOAD_ACK_BYTES in src/asset_upload.c). Waiting for an ack before sending the next
# piece caps the data in flight at WINDOW - 1 + CHUNK = 3071 bytes, always under 4 KB.
#
WINDOW = 2048

#
# How long to wait for "the first ack of this file" before concluding the firmware is an
# old build (which sends no acks) and falling back to timed sending.
#
ACK_WAIT = 3.0

#
# How long to wait when acks were arriving and then stop.
#
# This has to be far longer than ACK_WAIT: the firmware reports its progress again after
# stalling for more than 250 ms (UPLOAD_REACK_MS), so a stall almost always recovers by
# itself given time. Giving up too early falls back to timed sending with no flow control,
# and that is what really fails.
#
ACK_STALL_WAIT = 20.0

#
# The gap between chunks when falling back to timed sending. Slow enough for the firmware
# to keep up -- this path has no feedback at all and can only be conservative.
#
CHUNK_DELAY = 0.02

#
# The assets the firmware currently loads and the size each should be.
#
# Two uses: flagging files of the wrong size when showing the eMMC contents (the only
# warning of "short read, image not drawn at all"), and reporting which are missing.
#
# It has to follow ASSET_*_W / _H in src/asset_store.h. A file not listed here is marked
# "(not an asset)" and does **not** appear in the missing list -- which is exactly the trap
# hit once: after switching to the Ambiq face, WF4*.BIN had not been uploaded while this
# list still held only the original 8 files, so the tool reported "all present" and the
# face was blank with no indication why. Add new assets here at the same time.
#
EXPECTED = {
    "HOME.BIN": 454 * 454 * 2,
    "WATCHBG.BIN": 454 * 454 * 2,
    "HANDHR.BIN": 15 * 90 * 4,
    "HANDMIN.BIN": 15 * 150 * 4,
    "HANDSEC.BIN": 13 * 179 * 4,
    "MAPTILE0.BIN": 468 * 468 * 2,
    "MAPTILE1.BIN": 468 * 468 * 2,
    "MAPTILE2.BIN": 468 * 468 * 2,
    # The third watch face (numbers)
    "WF3BG.BIN": 454 * 454 * 2,
    "WF3HR.BIN": 23 * 107 * 4,
    "WF3MIN.BIN": 33 * 178 * 4,
    "WF3SEC.BIN": 18 * 213 * 4,
    # The Ambiq face (which took aurora's slots). CAP is the centre cap, which the earlier faces do not have.
    "WF4BG.BIN": 454 * 454 * 2,
    "WF4HR.BIN": 30 * 130 * 4,
    "WF4MIN.BIN": 30 * 193 * 4,
    "WF4SEC.BIN": 18 * 126 * 4,
    "WF4CAP.BIN": 46 * 46 * 4,
}

#
# Assets still on eMMC that the current firmware does not load.
#
# aurora's slots went to the Ambiq face (the GPU pool cannot hold four, see VMEM_SIZE in
# gcc/Makefile), but the files were not deleted -- switching back only needs one entry of
# g_sFaces[] in render_task.c changed.
#
# They live here rather than in EXPECTED: the size is still verified, but they are not
# listed as "missing assets", or every read would report a set of files nobody needs.
#
KEPT = {
    "WF2BG.BIN": 390 * 390 * 2,
    "WF2HR.BIN": 22 * 104 * 4,
    "WF2MIN.BIN": 28 * 153 * 4,
    "WF2SEC.BIN": 18 * 219 * 4,
}


class Uploader:
    """The RTT connection and transfers. Shared by the GUI and the command line."""

    def __init__(self, host=HOST, port=PORT):
        self.host = host
        self.port = port
        self.sock = None
        self.rx_lines = []
        self._rx_buf = b""
        self._stop = False
        self._thread = None

        #
        # Whether the firmware sends acks (flow control, see WINDOW). The answer from the
        # first file is remembered -- otherwise every file would wait out ACK_WAIT again
        # before falling back to timed sending, wasting twenty-odd seconds over eight files.
        #
        self._ack_ok = None

    # ---------------------------------------------------------------- connection

    def connect(self, timeout=3.0):
        self.sock = socket.create_connection((self.host, self.port), timeout)
        self.sock.settimeout(0.2)
        self._stop = False
        self._thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._thread.start()

    def close(self):
        self._stop = True
        if self._thread:
            self._thread.join(timeout=1.0)
        if self.sock:
            self.sock.close()
            self.sock = None

    def _rx_loop(self):
        """Read the firmware's log in the background, splitting it into lines."""
        while not self._stop:
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            self._rx_buf += data
            while b"\n" in self._rx_buf:
                line, self._rx_buf = self._rx_buf.split(b"\n", 1)
                self.rx_lines.append(line.decode("utf-8", "replace").rstrip())

    def wait_for(self, needles, timeout=10.0):
        """Wait for any of the keywords to appear in the log and return the whole line; None on timeout."""
        deadline = time.time() + timeout
        seen = 0
        while time.time() < deadline:
            while seen < len(self.rx_lines):
                line = self.rx_lines[seen]
                seen += 1
                for n in needles:
                    if n in line:
                        return line
            time.sleep(0.05)
        return None

    # ---------------------------------------------------------------- queries

    def list_files(self, timeout=10.0, retries=1):
        """Ask the target to list the eMMC contents.

        Returns (file list, info string), where the list is [(name, bytes), ...].
        The target replies over the log, bracketed by "[list] begin" and "[list] end".

        It resends once when no reply arrives: in the instant after connecting, the GDB
        server is still hooking up RTT's control block and the first command can be dropped
        entirely. Resending once is better than making the user press the button again.
        """
        for attempt in range(retries + 1):
            files, info = self._list_once(timeout)
            if files is not None:
                return files, info
            if attempt < retries:
                time.sleep(0.4)
        return None, info

    def delete_file(self, name, timeout=10.0):
        """Ask the target to delete one file. Returns (ok, message).

        The protocol is magic + namelen(1) + name (see ASSET_DEL_MAGIC in
        src/asset_upload.h), and the target replies with "[delete] ok|fail <name>".

        When the file was never there the target replies fail -- this tool passes that on
        verbatim and lets the caller decide whether it counts as a failure (usually not during a bulk cleanup).
        """
        name = name.upper()
        raw = name.encode("ascii", "replace")

        if len(raw) > NAME_MAX:
            return False, "file name longer than %d characters: %s" % (NAME_MAX, name)

        mark = len(self.rx_lines)
        self.sock.sendall(DEL_MAGIC + bytes([len(raw)]) + raw)

        #
        # Only the line carrying the same name counts: the log is shared with other tasks,
        # and during a bulk delete the previous file's reply may still be in the buffer.
        #
        deadline = time.time() + timeout
        seen = mark
        while time.time() < deadline:
            while seen < len(self.rx_lines):
                line = self.rx_lines[seen].strip()
                seen += 1
                if "[delete] ok " + name in line:
                    return True, "deleted"
                if "[delete] fail " + name in line:
                    return False, "the target could not delete it (file missing?)"
            time.sleep(0.05)

        return False, "no reply from the target"

    def _list_once(self, timeout):
        mark = len(self.rx_lines)
        self.sock.sendall(LIST_MAGIC)

        #
        # Wait for begin first, then read until end. The log is shared with other tasks and
        # [render] or [sensor] lines in between are normal, so only lines starting with "[list] " are taken.
        #
        deadline = time.time() + timeout
        seen = mark
        started = False
        files = []
        info = ""

        while time.time() < deadline:
            while seen < len(self.rx_lines):
                line = self.rx_lines[seen].strip()
                seen += 1

                if "[list] begin" in line:
                    started = True
                    files = []
                    continue
                if not started:
                    continue
                if "[list] error" in line:
                    return None, "the target reported a read failure"
                if "[list] end" in line:
                    info = line.split("[list] end", 1)[1].strip()
                    return files, info

                #
                # An ordinary entry: "[list] HOME.BIN 412232"
                #
                idx = line.find("[list] ")
                if idx >= 0:
                    parts = line[idx + 7:].split()
                    if len(parts) >= 2 and parts[1].isdigit():
                        files.append((parts[0], int(parts[1])))

            time.sleep(0.05)

        if started:
            return files, "(timed out; the list may be incomplete)"
        return None, "no reply from the target (check WATCHFACE_ASSET_UPLOAD=1 and that it is flashed)"

    # ---------------------------------------------------------------- sending

    def _scan_acks(self, start):
        """Scan forward from rx_lines[start] for the target's progress reports.

        Returns (latest acknowledged byte count, new scan position, failure message). A
        third value other than None means the target has given up and there is no point sending more.
        """
        ack = None
        i = start

        while i < len(self.rx_lines):
            line = self.rx_lines[i]
            i += 1

            if "[upload] abort" in line or "[upload] err" in line:
                return ack, i, line

            idx = line.find("[upload] ack ")
            if idx >= 0:
                try:
                    ack = int(line[idx + len("[upload] ack "):].split()[0])
                except (ValueError, IndexError):
                    pass

        return ack, i, None

    def send_file(self, path, progress=None):
        """Send one file. Returns (ok, message)."""
        name = os.path.basename(path).upper()

        if len(name) > NAME_MAX:
            return False, "file name longer than %d characters: %s" % (NAME_MAX, name)

        with open(path, "rb") as f:
            data = f.read()

        if not data:
            return False, "the file is empty"

        want = EXPECTED.get(name)
        if want is not None and len(data) != want:
            return False, ("size mismatch: %d, expected %d (the firmware accepts it, but the image will be missing)"
                           % (len(data), want))

        checksum = sum(data) & 0xFFFFFFFF

        header = (MAGIC
                  + struct.pack("<B", len(name))
                  + name.encode("ascii")
                  + struct.pack("<I", len(data)))

        #
        # Clear the earlier log, so the acks and the result read next definitely belong to this transfer.
        #
        del self.rx_lines[:]
        self.sock.sendall(header)

        sent = 0
        acked = 0
        scan = 0
        use_ack = (self._ack_ok is not False)
        stalled = None

        while sent < len(data):
            #
            # A full window waits for the target's acknowledgement (see WINDOW).
            #
            while use_ack and (sent - acked) >= WINDOW:
                got, scan, bad = self._scan_acks(scan)

                if bad:
                    #
                    # The target has given up; no point sending the remaining bytes into the void.
                    #
                    return False, bad

                if got is not None and got > acked:
                    acked = got
                    stalled = None
                    self._ack_ok = True
                    continue

                if stalled is None:
                    stalled = time.time()
                else:
                    #
                    # Wait longer when this file is already producing acks -- the firmware
                    # reports its progress again after stalling 250 ms and almost always
                    # recovers. Only with no ack at all from start to finish is it old
                    # firmware, and only then is timed sending the fallback.
                    #
                    limit = ACK_STALL_WAIT if acked else ACK_WAIT

                    if (time.time() - stalled) > limit:
                        if acked:
                            return False, ("the target stopped responding at %d/%d bytes"
                                           % (acked, len(data)))
                        use_ack = False
                        self._ack_ok = False
                        break

                time.sleep(0.005)

            block = data[sent:sent + CHUNK]
            self.sock.sendall(block)
            sent += len(block)

            if progress:
                progress(sent, len(data))

            if not use_ack:
                time.sleep(CHUNK_DELAY)

        self.sock.sendall(struct.pack("<I", checksum))

        #
        # Wait for the firmware's report. It prints "[upload] ok <name>" or "[upload] err ...".
        #
        line = self.wait_for(["[upload] ok", "[upload] err", "[upload] abort"],
                             timeout=15.0)

        if line is None:
            return False, "no reply from the firmware (check WATCHFACE_ASSET_UPLOAD=1 and that it is flashed)"
        if "[upload] ok" in line:
            return True, line
        return False, line


# ====================================================================== GUI

def run_gui():
    import tkinter as tk
    from tkinter import filedialog, ttk

    root = tk.Tk()
    root.title("eMMC asset upload")
    root.geometry("940x580")

    up = Uploader()
    files = []

    # ---------------------------------------------------------------- layout
    top = ttk.Frame(root, padding=8)
    top.pack(fill="x")

    status = tk.StringVar(value="Not connected")
    ttk.Label(top, textvariable=status).pack(side="left")

    btn_conn = ttk.Button(top, text="Connect")
    btn_conn.pack(side="right")

    #
    # Two columns: "files to send" on the left, "what is on the board now" on the right.
    # Side by side is what makes the difference visible -- which file has not been sent, which one is the wrong size.
    #
    mid = ttk.Frame(root, padding=(8, 0))
    mid.pack(fill="both", expand=True)

    left = ttk.Frame(mid)
    left.pack(side="left", fill="both", expand=True)
    ttk.Label(left, text="To upload").pack(anchor="w")
    listbox = tk.Listbox(left, selectmode="extended")
    listbox.pack(fill="both", expand=True)

    right = ttk.Frame(mid)
    right.pack(side="left", fill="both", expand=True, padx=(8, 0))
    ttk.Label(right, text="On the eMMC").pack(anchor="w")
    remote = tk.Listbox(right)
    remote.pack(fill="both", expand=True)

    bar = ttk.Progressbar(root, mode="determinate")
    bar.pack(fill="x", padx=8, pady=4)

    log = tk.Text(root, height=10, wrap="none")
    log.pack(fill="both", expand=True, padx=8, pady=(0, 8))

    btns = ttk.Frame(root, padding=8)
    btns.pack(fill="x")

    def say(msg):
        log.insert("end", msg + "\n")
        log.see("end")
        root.update_idletasks()

    # ---------------------------------------------------------------- actions
    def add_one(path, label=None):
        """Add one .BIN to the upload list.

        Re-adding the same path is an "update" rather than "ignore": conversion output
        always lands at assets_out/<target>.BIN, so swapping a second image onto the same
        target gives the same path, and ignoring it would leave the previous image's
        information in the list.
        """
        name = (label or os.path.basename(path)).upper()
        size = os.path.getsize(path)
        want = EXPECTED.get(name)
        tag = "" if (want is None or size == want) else "  <- size mismatch!"
        row = "%-14s %8d bytes%s" % (name, size, tag)

        if path in files:
            i = files.index(path)
            listbox.delete(i)
            listbox.insert(i, row)
            return

        files.append(path)
        listbox.insert("end", row)

    def pick_target(png, img_name):
        """When an image's name matches no target, let the user point at one.

        Without this step an image called my_photo.png would have to be renamed first; and
        "which asset do you want to replace" is something the user already knows, so asking is quickest.
        """
        dlg = tk.Toplevel(root)
        dlg.title("Which asset should this image replace?")
        dlg.transient(root)
        dlg.grab_set()

        ttk.Label(dlg, text=img_name, padding=8).pack(anchor="w")
        ttk.Label(dlg, text="Choose the target asset (a size mismatch is scaled automatically):",
                  padding=(8, 0)).pack(anchor="w")

        names = list(png.TARGETS)

        #
        # exportselection=0 is necessary: the main window already has two Listboxes, and by
        # default they fight over the X selection -- the moment focus leaves here, this
        # Listbox's selection is cleared, curselection() is empty when "OK" is pressed, and
        # the whole image is treated as "no target chosen" and skipped.
        #
        box = tk.Listbox(dlg, height=len(names), width=40, exportselection=0)
        for t in names:
            w, h, fmt = png.TARGETS[t]
            box.insert("end", "%-14s %dx%d %s" % (t, w, h, fmt))
        box.pack(fill="both", expand=True, padx=8)

        #
        # It is recorded on the Python side as well. A Listbox's selection can be affected by
        # other widgets while this variable cannot -- "OK" reads this one. It defaults to
        # item 0, so pressing OK straight away always gives a result.
        #
        picked = [0]
        box.selection_set(0)
        box.activate(0)
        box.bind("<<ListboxSelect>>",
                 lambda e: picked.__setitem__(0, box.curselection()[0])
                 if box.curselection() else None)

        chosen = tk.StringVar(value="")

        def ok():
            chosen.set(names[picked[0]])
            dlg.destroy()

        row = ttk.Frame(dlg, padding=8)
        row.pack(fill="x")
        ttk.Button(row, text="OK", command=ok).pack(side="right")
        ttk.Button(row, text="Skip this one", command=dlg.destroy).pack(side="right",
                                                                  padx=4)

        box.bind("<Double-Button-1>", lambda e: ok())
        dlg.bind("<Return>", lambda e: ok())
        dlg.bind("<Escape>", lambda e: dlg.destroy())

        box.focus_set()
        root.wait_window(dlg)
        return chosen.get() or None

    def add_image(path):
        """Convert one image into a .BIN and add it to the list. Returns whether it succeeded."""
        png = load_png_to_bin()
        name = os.path.basename(path)

        if png is None:
            say("Adding an image directly needs Pillow: pip install pillow")
            say("(or run python tools/png_to_bin.py yourself first and add the .BIN)")
            return False

        #
        # The target is inferred from the file name first (HOME.png -> HOME.BIN) and only
        # asked for when that fails. The rule matches png_to_bin.py --dir, so both routes give the same result.
        #
        stem = os.path.splitext(name)[0].upper()
        target = stem if stem.endswith(".BIN") else stem + ".BIN"

        if target not in png.TARGETS:
            target = pick_target(png, name)
            if target is None:
                say("skipped %s (no target chosen)" % name)
                return False

        ok, msg = png.convert(path, target, quiet=True)
        if not ok:
            say("conversion failed %s: %s" % (name, msg))
            return False

        say("converted %s -> %s" % (name, target))
        add_one(msg, target)        # on success, convert() returns the output path in msg
        return True

    def add_files():
        paths = filedialog.askopenfilenames(
            title="Select an image or asset file",
            filetypes=[("Images and assets", "*.png *.PNG *.bmp *.BMP *.jpg *.JPG "
                                      "*.jpeg *.webp *.BIN *.bin"),
                       ("Images", "*.png *.PNG *.bmp *.BMP *.jpg *.JPG *.jpeg "
                                "*.webp"),
                       ("Assets", "*.BIN *.bin"),
                       ("All", "*.*")])

        for p in paths:
            #
            # A .BIN is taken as-is; everything else goes to Pillow as an image -- it knows
            # far more than PNG, so there is no extension table to maintain here, and convert() errors out on anything it cannot read.
            #
            if os.path.splitext(p)[1].lower() == ".bin":
                add_one(p)
            else:
                add_image(p)

    def add_exported():
        """Add the whole output of asset_export.py at once."""
        here = os.path.dirname(os.path.abspath(__file__))
        out = os.path.join(here, "assets_out")
        if not os.path.isdir(out):
            say("%s not found; run python tools/asset_export.py first" % out)
            return
        for name in sorted(os.listdir(out)):
            p = os.path.join(out, name)
            if os.path.isfile(p):
                add_one(p)

    def remove_selected():
        """Remove the selected entries from the upload list (the eMMC is untouched)."""
        picks = listbox.curselection()
        if not picks:
            say("Select the entries to remove in the left column first.")
            return
        #
        # Deleted back to front, so the earlier indices do not shift.
        #
        for i in sorted(picks, reverse=True):
            listbox.delete(i)
            del files[i]

    def clear_files():
        files.clear()
        listbox.delete(0, "end")

    def toggle_conn():
        if up.sock is None:
            try:
                up.connect()
            except OSError as e:
                status.set("Connection failed")
                say("cannot connect to %s:%d -- %s" % (HOST, PORT, e))
                say("Check that JLinkGDBServer is running (the RTT log window of build_gui.py will do)")
                return
            status.set("Connected %s:%d" % (HOST, PORT))
            btn_conn.config(text="Disconnect")
            say("Connected.")
        else:
            up.close()
            status.set("Disconnected")
            btn_conn.config(text="Connect")

    def do_upload():
        if up.sock is None:
            say("Connect first.")
            return
        if not files:
            say("Select a file first.")
            return

        #
        # With a selection in the left column, only those are sent. Changing one image need
        # not resend all eight assets -- each file is an independent transfer with no dependencies.
        #
        picks = listbox.curselection()
        todo = [files[i] for i in picks] if picks else list(files)

        if picks:
            say("Sending only the %d selected (clear the selection to send everything)." % len(todo))

        ok_count = 0
        for path in todo:
            name = os.path.basename(path).upper()
            say("sending %s ..." % name)
            bar["value"] = 0

            def progress(done, total):
                bar["value"] = done * 100.0 / total
                root.update_idletasks()

            ok, msg = up.send_file(path, progress)
            say(("  OK   " if ok else "  FAIL ") + msg)
            if ok:
                ok_count += 1

        bar["value"] = 0
        say("Done: %d / %d succeeded." % (ok_count, len(todo)))
        if ok_count:
            say("Reboot (or switch to the map page) to see the new assets.")
            #
            # Re-read immediately after writing, so the sizes in the right column are the
            # board's real state -- the user need not press refresh to confirm the write landed.
            #
            do_refresh()

    def do_refresh():
        """Read what is currently on eMMC and show it in the right column."""
        if up.sock is None:
            say("Connect first.")
            return

        remote.delete(0, "end")
        remote.insert("end", "reading...")
        root.update_idletasks()

        files, info = up.list_files()

        remote.delete(0, "end")

        if files is None:
            remote.insert("end", "read failed")
            say("failed to read the eMMC contents: " + info)
            return

        if not files:
            remote.insert("end", "(empty)")
        else:
            for name, size in sorted(files):
                key = name.upper()
                want = EXPECTED.get(key)
                kept = want is None

                if kept:
                    want = KEPT.get(key)

                if want is None:
                    tag = "  (not an asset)"
                elif size != want:
                    tag = "  <- size mismatch!"
                elif kept:
                    tag = "  (kept, not loaded by the firmware)"
                else:
                    tag = ""

                remote.insert("end", "%-14s %8d bytes%s" % (name, size, tag))

        #
        # A check that the assets are complete. Saying which one is missing beats making the user count.
        #
        # The count uses len(EXPECTED) rather than a literal: it used to say "all 8 assets
        # present", and after a face was added it still said 8 -- reporting everything fine while a file was missing.
        #
        have = {n.upper() for n, _ in files}
        missing = [n for n in EXPECTED if n not in have]

        say("eMMC holds %d files, %s" % (len(files), info))
        if missing:
            say("  missing assets (%d/%d): %s"
                % (len(missing), len(EXPECTED), ", ".join(sorted(missing))))
        else:
            say("  all %d assets present." % len(EXPECTED))

    ttk.Button(btns, text="Add image / asset...", command=add_files).pack(side="left")
    ttk.Button(btns, text="Add assets_out", command=add_exported).pack(side="left", padx=4)
    ttk.Button(btns, text="Remove selected", command=remove_selected).pack(side="left")
    ttk.Button(btns, text="Clear", command=clear_files).pack(side="left", padx=4)
    ttk.Button(btns, text="Upload", command=do_upload).pack(side="right")
    ttk.Button(btns, text="Read eMMC contents", command=do_refresh).pack(side="right", padx=4)

    #
    # Read once right after connecting, so the user sees what is on the board immediately.
    #
    _orig_toggle = toggle_conn

    def toggle_and_refresh():
        _orig_toggle()
        if up.sock is not None:
            #
            # Just after connecting, the GDB server is still hooking up RTT's control block
            # and a command sent too early is dropped entirely. Read a moment later;
            # list_files() retries once on its own as well.
            #
            root.after(400, do_refresh)

    btn_conn.config(command=toggle_and_refresh)

    say("Steps: 1) check JLinkGDBServer is running  2) press \"Connect\" (it reads the eMMC contents once)")
    say("       3) add images or .BIN files  4) press \"Upload\"")
    say("Changing one image means adding only that one -- each file is sent independently, with no need to resend the set.")
    say("(With a selection in the left column, \"Upload\" sends only those.)")
    say("Images (PNG/JPG/BMP...) are converted into asset files automatically, and a size mismatch is scaled.")
    say("Naming a file HOME / WATCHBG / HANDHR / HANDMIN / HANDSEC / MAPTILE0-2")
    say("matches it automatically; any other name opens a dialog to choose what it replaces.")

    root.mainloop()
    up.close()


# =================================================================== command line

def resolve_paths(paths):
    """Convert any images among the command line paths into .BIN first.

    The command line cannot open a dialog the way the GUI does, so the target is inferred
    from the file name only; when that fails it errors out and lists the available names,
    which is easier to chase than silently skipping.
    """
    out = []

    for p in paths:
        if os.path.splitext(p)[1].lower() == ".bin":
            out.append(p)
            continue

        png = load_png_to_bin()
        if png is None:
            print("Sending an image directly needs Pillow: pip install pillow")
            return None

        stem = os.path.splitext(os.path.basename(p))[0].upper()
        target = stem if stem.endswith(".BIN") else stem + ".BIN"

        if target not in png.TARGETS:
            print("%s: the file name matches no target asset (available: %s)"
                  % (os.path.basename(p), ", ".join(png.TARGETS)))
            print("or run python tools/png_to_bin.py <image> <target name> first")
            return None

        ok, msg = png.convert(p, target, quiet=True)
        if not ok:
            print("%s: %s" % (os.path.basename(p), msg))
            return None

        print("converted %-14s -> %s" % (os.path.basename(p), target))
        out.append(msg)

    return out


def run_cli(paths, list_only=False, delete=False):
    if not (list_only or delete):
        paths = resolve_paths(paths)
        if paths is None:
            return 1

    up = Uploader()
    try:
        up.connect()
    except OSError as e:
        print("cannot connect to %s:%d -- %s" % (HOST, PORT, e))
        print("Check that JLinkGDBServer is running.")
        return 1

    print("connected %s:%d" % (HOST, PORT))

    if delete:
        #
        # paths here are names rather than paths -- deleting needs no local copy of the file.
        #
        failed = 0
        for name in paths:
            name = os.path.basename(name).upper()
            ok, msg = up.delete_file(name)
            print("  %-14s %s" % (name, msg))
            if not ok:
                failed += 1
        up.close()
        return 1 if failed else 0

    if list_only:
        files, info = up.list_files()
        if files is None:
            print("read failed: " + info)
            up.close()
            return 1
        print("eMMC contents (%s):" % info)
        for name, size in sorted(files):
            want = EXPECTED.get(name.upper())
            tag = "" if (want is None or size == want) else "  <- size mismatch!"
            print("  %-14s %8d bytes%s" % (name, size, tag))
        if not files:
            print("  (empty)")
        up.close()
        return 0

    failed = 0
    for path in paths:
        name = os.path.basename(path).upper()
        sys.stdout.write("sending %-14s " % name)
        sys.stdout.flush()

        def progress(done, total):
            sys.stdout.write("\rsending %-14s %5.1f%%" % (name, done * 100.0 / total))
            sys.stdout.flush()

        ok, msg = up.send_file(path, progress)
        print("\rsending %-14s %s" % (name, "OK" if ok else "FAIL: " + msg))
        if not ok:
            failed += 1

    up.close()
    return 1 if failed else 0


def main():
    opts = [a for a in sys.argv[1:] if a.startswith("-")]
    args = [a for a in sys.argv[1:] if not a.startswith("-")]

    if "--list" in opts or "-l" in opts:
        return run_cli([], list_only=True)
    if "--delete" in opts:
        if not args:
            print("--delete must be followed by file names, e.g.: --delete OLD.BIN OLD2.BIN")
            return 1
        return run_cli(args, delete=True)
    if args:
        return run_cli(args)

    run_gui()
    return 0


if __name__ == "__main__":
    sys.exit(main())
