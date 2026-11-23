#!/usr/bin/env python3
"""
OwnKrn Host-to-Guest Clipboard Bridge (COM2 / 0x2F8)
Bridges the Host OS clipboard (Linux X11/Wayland, macOS, Windows) with OwnKrn's
/dev/clipboard device over QEMU COM2 so Ctrl+V / Ctrl+Y / Ctrl+K inside
nano/edit or /bin/sh in the QEMU GUI window works seamlessly.
"""

import os
import shutil
import socket
import subprocess
import sys
import threading


def get_host_clipboard() -> str:
    """Read text from the Host OS clipboard across Linux, macOS, and Windows."""
    # 1. Try Wayland / X11 / macOS / Windows CLI clipboard utilities
    cmds = [
        ["wl-paste", "--no-newline"],
        ["xclip", "-selection", "clipboard", "-o"],
        ["xsel", "--clipboard", "--output"],
        ["pbpaste"],
        ["powershell.exe", "-NoProfile", "-Command", "Get-Clipboard"],
    ]
    for cmd in cmds:
        if shutil.which(cmd[0]):
            try:
                res = subprocess.run(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    timeout=1.2,
                    check=False,
                )
                if res.returncode == 0 and res.stdout:
                    return res.stdout.decode("utf-8", errors="replace")
            except Exception:
                pass

    # 2. Fallback to Python Tkinter clipboard
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


def set_host_clipboard(text: str) -> None:
    """Write text to the Host OS clipboard across Linux, macOS, and Windows."""
    data = text.encode("utf-8", errors="replace")
    cmds = [
        ["wl-copy"],
        ["xclip", "-selection", "clipboard", "-i"],
        ["xsel", "--clipboard", "--input"],
        ["pbcopy"],
        ["clip.exe"],
    ]
    for cmd in cmds:
        if shutil.which(cmd[0]):
            try:
                subprocess.run(
                    cmd,
                    input=data,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=1.2,
                    check=False,
                )
                return
            except Exception:
                pass

    try:
        import tkinter as tk

        r = tk.Tk()
        r.withdraw()
        r.clipboard_clear()
        r.clipboard_append(text)
        r.update()
        r.destroy()
    except Exception:
        pass


def handle_com2_client(conn: socket.socket) -> None:
    in_stx = False
    rx_buf = bytearray()
    try:
        while True:
            chunk = conn.recv(1024)
            if not chunk:
                break
            for b in chunk:
                if b == 0x05 and not in_stx:  # ENQ: Guest requests Host Clipboard
                    clip = get_host_clipboard().replace("\r\n", "\n")
                    payload = b"\x06" + clip.encode("utf-8", errors="replace")[:15000] + b"\x04"
                    conn.sendall(payload)
                elif b == 0x02:  # STX: Guest begins sending clipboard to Host
                    in_stx = True
                    rx_buf.clear()
                elif b == 0x03 and in_stx:  # ETX: Guest finished sending clipboard
                    in_stx = False
                    set_host_clipboard(rx_buf.decode("utf-8", errors="replace"))
                    rx_buf.clear()
                elif in_stx:
                    if len(rx_buf) < 16384:
                        rx_buf.append(b)
    except Exception:
        pass
    finally:
        try:
            conn.close()
        except Exception:
            pass


def server_loop(srv: socket.socket) -> None:
    while True:
        try:
            conn, _ = srv.accept()
            handle_com2_client(conn)
        except Exception:
            break


def main() -> int:
    argv = sys.argv[1:]
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    if not argv:
        print("Usage: clipboard_bridge.py -- <qemu-command...>", file=sys.stderr)
        return 1

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(2)
    port = srv.getsockname()[1]

    t = threading.Thread(target=server_loop, args=(srv,), daemon=True)
    t.start()

    qemu_cmd = list(argv) + ["-serial", f"tcp:127.0.0.1:{port}"]
    try:
        return subprocess.call(qemu_cmd)
    except KeyboardInterrupt:
        return 0
    finally:
        try:
            srv.close()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
