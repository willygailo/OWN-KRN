#!/usr/bin/env python3
"""
OwnKrn Host-to-Guest Clipboard Bridge (COM2 / 0x2F8)
Bridges the Host OS clipboard (Linux X11/Wayland, macOS, Windows) with OwnKrn's
/dev/clipboard device over QEMU COM2 using an IRQ3 + 12-byte ACK-paced protocol
(fitting cleanly inside the 16-byte 16550A UART hardware FIFO with zero drops).
"""

import shutil
import socket
import subprocess
import sys
import threading
import time

g_clip_lock = threading.Lock()
g_cached_clip = ""


def read_host_clipboard_now() -> str:
    """Read text from the Host OS clipboard (checking CLIPBOARD and PRIMARY)."""
    cmds = [
        ["wl-paste", "--no-newline"],
        ["xclip", "-selection", "clipboard", "-o"],
        ["xsel", "--clipboard", "--output"],
        ["xclip", "-selection", "primary", "-o"],
        ["xsel", "--primary", "--output"],
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
                    timeout=0.5,
                    check=False,
                )
                if res.returncode == 0 and res.stdout:
                    txt = res.stdout.decode("utf-8", errors="replace")
                    if txt:
                        return txt
            except Exception:
                pass

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
    global g_cached_clip
    with g_clip_lock:
        g_cached_clip = text

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
                    timeout=0.8,
                    check=False,
                )
                return
            except Exception:
                pass


def clipboard_poller_loop() -> None:
    """Continuously cache the Host OS clipboard in the background."""
    global g_cached_clip
    while True:
        txt = read_host_clipboard_now()
        if txt:
            with g_clip_lock:
                g_cached_clip = txt
        time.sleep(0.25)


def handle_com2_client(conn: socket.socket) -> None:
    ack_event = threading.Event()
    enq_event = threading.Event()
    closed = False

    try:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except Exception:
        pass

    def wait_ack(timeout: float = 0.08) -> bool:
        ok = ack_event.wait(timeout)
        ack_event.clear()
        return ok

    def push_clipboard_frame(text: str) -> None:
        norm = text.replace("\r\n", "\n").replace("\r", "\n")
        raw = bytes(
            b
            for b in norm.encode("utf-8", errors="replace")[:15000]
            if b not in (0x02, 0x03, 0x04, 0x05, 0x06, 0x07)
        )
        ack_event.clear()
        conn.sendall(b"\x02")  # STX
        wait_ack(0.08)

        # Send in 12-byte chunks paced by BEL (0x07) -> ACK (0x06) so the
        # 16-byte 16550A UART FIFO never overflows
        for i in range(0, len(raw), 12):
            chunk = raw[i : i + 12]
            ack_event.clear()
            conn.sendall(chunk + b"\x07")
            wait_ack(0.08)

        ack_event.clear()
        conn.sendall(b"\x03")  # ETX
        wait_ack(0.08)

    def tx_worker() -> None:
        last_pushed = None
        while not closed:
            triggered = enq_event.wait(0.25)
            if closed:
                break
            if triggered:
                enq_event.clear()
                # Refresh right now on explicit Ctrl+V / ENQ
                fresh = read_host_clipboard_now()
                if fresh:
                    with g_clip_lock:
                        global g_cached_clip
                        g_cached_clip = fresh
                with g_clip_lock:
                    clip = g_cached_clip
                try:
                    push_clipboard_frame(clip)
                    last_pushed = clip
                except Exception:
                    break
            else:
                # Proactively push to guest kernel when Host OS clipboard changes
                with g_clip_lock:
                    clip = g_cached_clip
                if clip and clip != last_pushed:
                    try:
                        push_clipboard_frame(clip)
                        last_pushed = clip
                    except Exception:
                        break

    threading.Thread(target=tx_worker, daemon=True).start()

    in_stx = False
    rx_buf = bytearray()
    try:
        while True:
            chunk = conn.recv(1024)
            if not chunk:
                break
            for b in chunk:
                if b == 0x06 and not in_stx:  # ACK from kernel COM2
                    ack_event.set()
                elif b == 0x05 and not in_stx:  # ENQ: Guest requests Host Clipboard
                    enq_event.set()
                elif b == 0x02:  # STX: Guest sending clipboard to Host
                    in_stx = True
                    rx_buf.clear()
                elif b == 0x03 and in_stx:  # ETX: Guest finished sending clipboard
                    in_stx = False
                    text = rx_buf.decode("utf-8", errors="replace")
                    rx_buf.clear()
                    threading.Thread(
                        target=set_host_clipboard, args=(text,), daemon=True
                    ).start()
                elif in_stx:
                    if len(rx_buf) < 16384:
                        rx_buf.append(b)
    except Exception:
        pass
    finally:
        closed = True
        enq_event.set()
        ack_event.set()
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

    threading.Thread(target=clipboard_poller_loop, daemon=True).start()

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
