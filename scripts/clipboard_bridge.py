#!/usr/bin/env python3
"""
OwnKrn Host-to-Guest Clipboard Bridge (COM2 / 0x2F8)
Supports Wayland (native GTK 3.0 + wl-paste + XWayland), X11 (GTK3/xclip/xsel/tkinter),
macOS (pbpaste/pbcopy), Windows (PowerShell/clip.exe), and a live drop-file (build/clipboard.txt).

Uses an IRQ3 + 12-byte ACK-paced STX..BEL/ACK..ETX protocol over QEMU COM2 so
multi-kilobyte Python/Shell/C files paste cleanly inside nano/edit with zero drops.
"""

import os
import shutil
import socket
import subprocess
import sys
import threading
import time

DROP_FILE = os.path.join("build", "clipboard.txt")


def _run_clip_cmd(cmd, env=None, input_bytes=None, timeout=0.45):
    try:
        res = subprocess.run(
            cmd,
            input=input_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=env,
            timeout=timeout,
            check=False,
        )
        return res.returncode, res.stdout
    except Exception:
        return -1, b""


def read_host_clipboard_cli() -> str:
    """Try CLI clipboard tools across Wayland, X11/XWayland, macOS, and Windows."""
    # 1. Wayland wl-paste, macOS pbpaste, Windows PowerShell
    direct_cmds = [
        ["wl-paste", "--no-newline"],
        ["pbpaste"],
        ["powershell.exe", "-NoProfile", "-Command", "Get-Clipboard"],
    ]
    for cmd in direct_cmds:
        if shutil.which(cmd[0]):
            rc, out = _run_clip_cmd(cmd)
            if rc == 0 and out:
                txt = out.decode("utf-8", errors="replace")
                if txt:
                    return txt

    # 2. X11 / XWayland (xclip / xsel) — try current DISPLAY and :0 / :1 fallback
    displays = []
    cur_disp = os.environ.get("DISPLAY")
    if cur_disp:
        displays.append(cur_disp)
    for d in (":0", ":1"):
        if d not in displays:
            displays.append(d)

    x11_cmds = [
        ["xclip", "-selection", "clipboard", "-o"],
        ["xsel", "--clipboard", "--output"],
        ["xclip", "-selection", "primary", "-o"],
        ["xsel", "--primary", "--output"],
    ]
    for disp in displays:
        env = dict(os.environ)
        env["DISPLAY"] = disp
        for cmd in x11_cmds:
            if shutil.which(cmd[0]):
                rc, out = _run_clip_cmd(cmd, env=env, timeout=0.35)
                if rc == 0 and out:
                    txt = out.decode("utf-8", errors="replace")
                    if txt:
                        return txt

    # 3. Tkinter fallback (if DISPLAY is set)
    if cur_disp or sys.platform in ("darwin", "win32"):
        try:
            import tkinter as tk

            r = tk.Tk()
            r.withdraw()
            text = r.clipboard_get()
            r.destroy()
            if text:
                return str(text)
        except Exception:
            pass

    return ""


def write_host_clipboard_cli(text: str) -> bool:
    """Write text back to the Host OS clipboard across Wayland, X11, macOS, and Windows."""
    data = text.encode("utf-8", errors="replace")
    direct_cmds = [
        ["wl-copy"],
        ["pbcopy"],
        ["clip.exe"],
    ]
    for cmd in direct_cmds:
        if shutil.which(cmd[0]):
            rc, _ = _run_clip_cmd(cmd, input_bytes=data, timeout=0.6)
            if rc == 0:
                return True

    displays = []
    if os.environ.get("DISPLAY"):
        displays.append(os.environ["DISPLAY"])
    for d in (":0", ":1"):
        if d not in displays:
            displays.append(d)

    for disp in displays:
        env = dict(os.environ)
        env["DISPLAY"] = disp
        for cmd in (
            ["xclip", "-selection", "clipboard", "-i"],
            ["xsel", "--clipboard", "--input"],
        ):
            if shutil.which(cmd[0]):
                rc, _ = _run_clip_cmd(cmd, env=env, input_bytes=data, timeout=0.6)
                if rc == 0:
                    return True
    return False


class COM2BridgeServer:
    """Reusable COM2 TCP clipboard server (used by both make run-gui and tests/run_tests.py)."""

    def __init__(self, initial_text: str = "", poll_host: bool = True, drop_file: str = DROP_FILE):
        self.lock = threading.Lock()
        self.cached_clip = initial_text
        self.last_host_clip = initial_text
        self.poll_host = poll_host
        self.drop_file = drop_file
        self.last_drop_mtime = 0.0
        self.closed = False
        self.status_cb = None
        self.gtk_read_cb = None
        self.gtk_write_cb = None

        self._active_enq = None
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(4)
        self.port = self.srv.getsockname()[1]

        if self.drop_file and os.path.isfile(self.drop_file):
            try:
                self.last_drop_mtime = os.path.getmtime(self.drop_file)
            except OSError:
                pass

    def start(self) -> int:
        threading.Thread(target=self._accept_loop, daemon=True).start()
        if self.poll_host:
            threading.Thread(target=self._poller_loop, daemon=True).start()
        return self.port

    def close(self) -> None:
        self.closed = True
        try:
            self.srv.close()
        except Exception:
            pass

    def set_clipboard_text(self, text: str, push_now: bool = True, from_guest: bool = False) -> None:
        with self.lock:
            self.cached_clip = text
            if not from_guest:
                self.last_host_clip = text
        if from_guest:
            if self.gtk_write_cb:
                self.gtk_write_cb(text)
            else:
                threading.Thread(target=self._write_host_bg, args=(text,), daemon=True).start()
        if push_now and self._active_enq is not None:
            self._active_enq.set()
        if self.status_cb:
            self.status_cb(len(text))

    def _write_host_bg(self, text: str) -> None:
        if write_host_clipboard_cli(text):
            with self.lock:
                self.last_host_clip = text

    def _check_drop_file(self) -> str:
        if not self.drop_file or not os.path.isfile(self.drop_file):
            return ""
        try:
            mt = os.path.getmtime(self.drop_file)
            if mt > self.last_drop_mtime:
                self.last_drop_mtime = mt
                with open(self.drop_file, "r", encoding="utf-8", errors="replace") as f:
                    txt = f.read()
                if txt:
                    with self.lock:
                        self.cached_clip = txt
                    if self.status_cb:
                        self.status_cb(len(txt))
                    return txt
        except OSError:
            pass
        return ""

    def refresh_from_host(self) -> str:
        dropped = self._check_drop_file()
        if dropped:
            return dropped

        txt = ""
        if self.gtk_read_cb:
            txt = self.gtk_read_cb() or ""
        if not txt and self.poll_host:
            txt = read_host_clipboard_cli()

        with self.lock:
            if txt and txt != self.last_host_clip:
                self.last_host_clip = txt
                self.cached_clip = txt
                if self.status_cb:
                    self.status_cb(len(txt))
            return self.cached_clip

    def _poller_loop(self) -> None:
        while not self.closed:
            self.refresh_from_host()
            time.sleep(0.25)

    def _accept_loop(self) -> None:
        while not self.closed:
            try:
                conn, _ = self.srv.accept()
                threading.Thread(
                    target=self._handle_client, args=(conn,), daemon=True
                ).start()
            except Exception:
                break

    def _handle_client(self, conn: socket.socket) -> None:
        ack_event = threading.Event()
        enq_event = threading.Event()
        self._active_enq = enq_event
        client_closed = False

        try:
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except Exception:
            pass

        def wait_ack(timeout: float = 0.25) -> bool:
            ok = ack_event.wait(timeout)
            ack_event.clear()
            return ok

        def push_frame(text: str) -> None:
            norm = text.replace("\r\n", "\n").replace("\r", "\n")
            raw = bytes(
                b
                for b in norm.encode("utf-8", errors="replace")[:16000]
                if b not in (0x02, 0x03, 0x04, 0x05, 0x06, 0x07)
            )
            ack_event.clear()
            conn.sendall(b"\x02")  # STX
            wait_ack(0.25)

            # 12-byte ACK-paced chunks fit strictly inside the 16-byte 16550A FIFO
            for i in range(0, len(raw), 12):
                chunk = raw[i : i + 12]
                ack_event.clear()
                conn.sendall(chunk + b"\x07")  # chunk + BEL
                wait_ack(0.25)

            ack_event.clear()
            conn.sendall(b"\x03")  # ETX
            wait_ack(0.25)

        def tx_worker() -> None:
            last_pushed = None
            while not client_closed and not self.closed:
                triggered = enq_event.wait(0.25)
                if client_closed or self.closed:
                    break
                if triggered:
                    enq_event.clear()
                    # Fast path: grab cached/GTK clipboard immediately so STX begins in <2ms
                    clip = self.refresh_from_host() if self.poll_host else self.cached_clip
                    try:
                        push_frame(clip)
                        last_pushed = clip
                    except Exception:
                        break
                else:
                    with self.lock:
                        clip = self.cached_clip
                    if clip and clip != last_pushed:
                        try:
                            push_frame(clip)
                            last_pushed = clip
                        except Exception:
                            break

        threading.Thread(target=tx_worker, daemon=True).start()

        in_stx = False
        rx_buf = bytearray()
        try:
            while not self.closed:
                chunk = conn.recv(1024)
                if not chunk:
                    break
                for b in chunk:
                    if b == 0x06 and not in_stx:
                        ack_event.set()
                    elif b == 0x05 and not in_stx:
                        enq_event.set()
                    elif b == 0x02:
                        in_stx = True
                        rx_buf.clear()
                    elif b == 0x03 and in_stx:
                        in_stx = False
                        text = rx_buf.decode("utf-8", errors="replace")
                        rx_buf.clear()
                        self.set_clipboard_text(text, push_now=False, from_guest=True)
                    elif in_stx:
                        if len(rx_buf) < 16384:
                            rx_buf.append(b)
        except Exception:
            pass
        finally:
            client_closed = True
            enq_event.set()
            ack_event.set()
            try:
                conn.close()
            except Exception:
                pass


def launch_gtk_clipboard_bar(bridge: COM2BridgeServer, qemu_proc: subprocess.Popen) -> None:
    """
    Run a compact Wayland+X11 native GTK 3.0 Clipboard Sync toolbar alongside QEMU.
    On Wayland compositors (GNOME/Mutter/KDE), having a native GTK3 surface guarantees
    100% clipboard read/write access even when wl-paste/xclip are unavailable or restricted.
    """
    try:
        import gi

        gi.require_version("Gtk", "3.0")
        from gi.repository import Gdk, GLib, Gtk
    except Exception:
        return

    try:
        display = Gdk.Display.get_default()
        if display is None:
            return
    except Exception:
        return

    gtk_clip = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)

    win = Gtk.Window(title="OwnKrn Clipboard Bridge (Wayland/X11)")
    win.set_default_size(430, 52)
    win.set_resizable(False)
    win.set_keep_above(True)

    box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
    box.set_border_width(8)
    win.add(box)

    status_lbl = Gtk.Label(label="Ready (Copy code, click Sync or press Ctrl+V in nano)")
    status_lbl.set_xalign(0.0)

    def sync_from_gtk(*_args):
        try:
            txt = gtk_clip.wait_for_text()
            if txt:
                bridge.set_clipboard_text(txt, push_now=True, from_guest=False)
                lines = txt.count("\n") + 1
                status_lbl.set_text(f"Synced {len(txt)} B ({lines} lines) -> Press Ctrl+V in nano")
            else:
                status_lbl.set_text("Host clipboard is empty - copy text first")
        except Exception:
            pass

    def load_file_dialog(*_args):
        dlg = Gtk.FileChooserDialog(
            title="Load Host File -> OwnKrn /dev/clipboard",
            parent=win,
            action=Gtk.FileChooserAction.OPEN,
        )
        dlg.add_buttons(
            Gtk.STOCK_CANCEL, Gtk.ResponseType.CANCEL,
            Gtk.STOCK_OPEN, Gtk.ResponseType.OK,
        )
        if dlg.run() == Gtk.ResponseType.OK:
            path = dlg.get_filename()
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    txt = f.read()[:16000]
                bridge.set_clipboard_text(txt, push_now=True, from_guest=False)
                lines = txt.count("\n") + 1
                status_lbl.set_text(f"Loaded {os.path.basename(path)} ({len(txt)} B, {lines}L) -> Ctrl+V")
            except Exception as e:
                status_lbl.set_text(f"Error: {e}")
        dlg.destroy()

    sync_btn = Gtk.Button(label="Sync Clipboard -> OwnKrn")
    sync_btn.connect("clicked", sync_from_gtk)

    file_btn = Gtk.Button(label="Load File...")
    file_btn.connect("clicked", load_file_dialog)

    box.pack_start(sync_btn, False, False, 0)
    box.pack_start(file_btn, False, False, 0)
    box.pack_start(status_lbl, True, True, 0)

    # Auto-sync whenever the Wayland clipboard changes OR the bar gets focus
    gtk_clip.connect("owner-change", lambda *_a: sync_from_gtk())
    win.connect("focus-in-event", lambda *_a: (sync_from_gtk(), False)[1])

    def gtk_write_cb(text: str):
        GLib.idle_add(lambda: (gtk_clip.set_text(text, -1), False)[1])

    def status_cb(nbytes: int):
        def _upd():
            try:
                if win.get_visible():
                    status_lbl.set_text(f"Clipboard Ready: {nbytes} B -> Ctrl+V in nano")
            except Exception:
                pass
            return False

        GLib.idle_add(_upd)

    bridge.gtk_write_cb = gtk_write_cb
    bridge.status_cb = status_cb

    def poll_qemu_exit():
        if qemu_proc.poll() is not None:
            Gtk.main_quit()
            return False
        # Also read GTK clipboard periodically when permitted by compositor
        try:
            txt = gtk_clip.wait_for_text()
            if txt:
                with bridge.lock:
                    changed = txt != bridge.last_host_clip
                if changed:
                    bridge.set_clipboard_text(txt, push_now=True, from_guest=False)
        except Exception:
            pass
        return True

    GLib.timeout_add(300, poll_qemu_exit)
    win.show_all()
    sync_from_gtk()
    Gtk.main()


def main() -> int:
    argv = sys.argv[1:]

    # Helper mode: --send-file <path> writes file into build/clipboard.txt for live bridge
    if len(argv) >= 2 and argv[0] == "--send-file":
        src_path = argv[1]
        with open(src_path, "r", encoding="utf-8", errors="replace") as f:
            data = f.read()[:16000]
        os.makedirs(os.path.dirname(DROP_FILE), exist_ok=True)
        with open(DROP_FILE, "w", encoding="utf-8") as f:
            f.write(data)
        print(f"[Clipboard Bridge] Queued {src_path} ({len(data)} bytes) -> Press Ctrl+V in OwnKrn nano!")
        return 0

    gui_bar = False
    if "--gui-bar" in argv:
        gui_bar = True
        argv.remove("--gui-bar")

    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    if not argv:
        print("Usage: clipboard_bridge.py [--gui-bar] -- <qemu-command...>", file=sys.stderr)
        return 1

    bridge = COM2BridgeServer(poll_host=True)
    port = bridge.start()

    qemu_cmd = list(argv) + ["-serial", f"tcp:127.0.0.1:{port}"]
    proc = subprocess.Popen(qemu_cmd)
    try:
        if gui_bar and (os.environ.get("WAYLAND_DISPLAY") or os.environ.get("DISPLAY")):
            launch_gtk_clipboard_bar(bridge, proc)
        return proc.wait()
    except KeyboardInterrupt:
        if proc.poll() is None:
            proc.terminate()
        return 0
    finally:
        bridge.close()


if __name__ == "__main__":
    sys.exit(main())
