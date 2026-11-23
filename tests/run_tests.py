#!/usr/bin/env python3
"""
Cross-platform automated QEMU end-to-end test runner for OwnKrn.
Uses a background reader thread instead of POSIX-only select.select() on pipes,
so it runs identically on Windows, macOS, and Linux.
"""
import os
import subprocess
import sys
import threading
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(SCRIPT_DIR, "..", "scripts"))
from clipboard_bridge import COM2BridgeServer  # noqa: E402

SNAKE_PY_SAMPLE = '''# OwnKrn Python Snake Game (snake.py)
# Controls: W/A/S/D or Arrow Keys | P = Pause | R = Restart | Q = Quit
import random
import time

WIDTH = 500
HEIGHT = 500
CELL = 20

def start_snake_game():
    import tkinter as tk

    root = tk.Tk()
    root.title("OwnKrn Python Snake Game")
    root.resizable(False, False)
    canvas = tk.Canvas(root, width=WIDTH, height=HEIGHT, bg="#0f172a", highlightthickness=0)
    canvas.pack()

    state = {
        "snake": [(12, 12), (11, 12), (10, 12)],
        "dir": (1, 0),
        "food": (18, 12),
        "score": 0,
        "over": False,
    }

    def spawn_food():
        cols, rows = WIDTH // CELL, HEIGHT // CELL
        while True:
            pos = (random.randint(0, cols - 1), random.randint(0, rows - 1))
            if pos not in state["snake"]:
                state["food"] = pos
                break

    def on_key(e):
        k = e.keysym.lower()
        dx, dy = state["dir"]
        if k in ("up", "w") and dy == 0:
            state["dir"] = (0, -1)
        elif k in ("down", "s") and dy == 0:
            state["dir"] = (0, 1)
        elif k in ("left", "a") and dx == 0:
            state["dir"] = (-1, 0)
        elif k in ("right", "d") and dx == 0:
            state["dir"] = (1, 0)
        elif k in ("r", "space") and state["over"]:
            state["snake"] = [(12, 12), (11, 12), (10, 12)]
            state["dir"] = (1, 0)
            state["score"] = 0
            state["over"] = False
            spawn_food()
        elif k in ("q", "escape"):
            root.destroy()

    def loop():
        if not state["over"]:
            hx, hy = state["snake"][0]
            dx, dy = state["dir"]
            nx, ny = hx + dx, hy + dy
            cols, rows = WIDTH // CELL, HEIGHT // CELL
            if nx < 0 or nx >= cols or ny < 0 or ny >= rows or (nx, ny) in state["snake"]:
                state["over"] = True
            else:
                state["snake"].insert(0, (nx, ny))
                if (nx, ny) == state["food"]:
                    state["score"] += 10
                    spawn_food()
                else:
                    state["snake"].pop()

        canvas.delete("all")
        fx, fy = state["food"]
        canvas.create_oval(fx * CELL + 2, fy * CELL + 2, (fx + 1) * CELL - 2, (fy + 1) * CELL - 2, fill="#ef4444", outline="")
        for i, (x, y) in enumerate(state["snake"]):
            color = "#22c55e" if i == 0 else "#4ade80"
            canvas.create_rectangle(x * CELL + 1, y * CELL + 1, (x + 1) * CELL - 1, (y + 1) * CELL - 1, fill=color, outline="")
        canvas.create_text(12, 12, anchor="nw", text=f"Score: {state['score']}", fill="#ffffff", font=("Consolas", 14, "bold"))
        if state["over"]:
            canvas.create_text(WIDTH // 2, HEIGHT // 2, text="GAME OVER! Press SPACE", fill="#f87171", font=("Consolas", 20, "bold"))
        root.after(90, loop)

    root.bind("<KeyPress>", on_key)
    loop()
    root.mainloop()

if __name__ == "__main__":
    start_snake_game()
'''


def ensure_disk(disk_path: str) -> None:
    if not os.path.exists(disk_path):
        os.makedirs(os.path.dirname(os.path.abspath(disk_path)), exist_ok=True)
        with open(disk_path, "wb") as f:
            f.seek(4 * 1024 * 1024 - 1)
            f.write(b"\x00")


def main() -> None:
    kernel = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("KERNEL", "build/kernel.elf")
    initrd = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("INITRD", "build/initrd.tar")
    disk = sys.argv[3] if len(sys.argv) > 3 else os.environ.get("DISK", "build/disk.img")
    qemu = os.environ.get("QEMU", "qemu-system-x86_64")

    if not os.path.isfile(kernel) or not os.path.isfile(initrd):
        print(f"ERROR: {kernel} or {initrd} missing; build first.", file=sys.stderr)
        sys.exit(1)

    ensure_disk(disk)

    # Start real COM2 Host Clipboard Bridge loaded with the ~2.5 KiB snake.py game
    bridge = COM2BridgeServer(initial_text=SNAKE_PY_SAMPLE, poll_host=False, drop_file="")
    bridge_port = bridge.start()

    cmd = [
        qemu,
        "-machine", "pc",
        "-cpu", "qemu64,+nx,+smep",
        "-m", "256M",
        "-no-reboot",
        "-drive", f"file={disk},format=raw,index=0,media=disk",
        "-d", "guest_errors,cpu_reset",
        "-D", "build/qemu.log",
        "-kernel", kernel,
        "-initrd", initrd,
        "-display", "none",
        "-serial", "stdio",
        "-serial", f"tcp:127.0.0.1:{bridge_port}",
    ]

    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )

    transcript = bytearray()
    cond = threading.Condition()
    eof_reached = False

    def reader_worker() -> None:
        nonlocal eof_reached
        fd = proc.stdout.fileno()
        while True:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            with cond:
                transcript.extend(chunk)
                cond.notify_all()
        with cond:
            eof_reached = True
            cond.notify_all()

    reader_thread = threading.Thread(target=reader_worker, daemon=True)
    reader_thread.start()

    def read_until(marker: bytes, timeout: float = 15.0, start_pos: int = -1) -> str:
        with cond:
            if start_pos < 0:
                start_pos = len(transcript)
            start_t = time.monotonic()
            deadline = start_t + timeout
            while True:
                if marker in transcript[start_pos:]:
                    return transcript[start_pos:].decode("utf-8", errors="replace")
                rem = deadline - time.monotonic()
                if rem <= 0 or (eof_reached and proc.poll() is not None):
                    dump = transcript.decode("utf-8", errors="replace")
                    rc_now = proc.poll()
                    elapsed = time.monotonic() - start_t
                    proc.kill()
                    qlog = ""
                    if os.path.isfile("build/qemu.log"):
                        with open("build/qemu.log", "r", encoding="utf-8", errors="replace") as qf:
                            lines = qf.readlines()
                            qlog = "".join(lines[-50:])
                    raise AssertionError(
                        f"Timed out waiting for {marker!r} "
                        f"(rc={rc_now}, eof={eof_reached}, elapsed={elapsed:.2f}s).\n"
                        f"--- Serial transcript ---\n{dump}\n-------------------------\n"
                        f"--- build/qemu.log (tail) ---\n{qlog}\n-----------------------------"
                    )
                cond.wait(timeout=min(0.1, rem))

    def send_cmd(line: str, wait_prompt: bytes = b"ownkrn:") -> str:
        with cond:
            pos = len(transcript)
        data = (line + "\n").encode("ascii")
        for ch in data:
            proc.stdin.write(bytes([ch]))
            proc.stdin.flush()
            time.sleep(0.004)
        return read_until(wait_prompt, timeout=15.0, start_pos=pos)

    def expect(haystack: str, needle: str, label: str) -> None:
        if needle not in haystack:
            proc.kill()
            raise AssertionError(
                f"FAILED [{label}]: expected {needle!r} in:\n{haystack}"
            )
        print(f"  [PASS] {label}")

    try:
        print(f"Booting OwnKrn 0.4.0 in QEMU headless (+nx,+smep,ATA0) on {sys.platform}...")
        boot_out = read_until(b"ownkrn:/# ", timeout=15.0, start_pos=0)
        expect(boot_out, "Long Mode active in higher half", "Higher-half 64-bit boot")
        expect(boot_out, "NXE=on, WP=on, SMEP=on", "CPU hardening (NXE + WP + SMEP)")
        expect(boot_out, "CMOS RTC", "CMOS RTC wall-clock init")
        expect(boot_out, "W^X enforced", "4-level VMM W^X enforcement")
        expect(boot_out, "ATA0 Master", "ATA0 PIO 28-bit LBA hard disk initialized")
        expect(boot_out, "/disk=diskfs", "Persistent DiskFS mounted at /disk")
        expect(boot_out, "SYSCALL/SYSRET MSRs configured", "SYSCALL/SYSRET MSR setup")
        expect(boot_out, "OWN KRN", "/bin/init launched /bin/fetch dashboard on boot")
        expect(boot_out, "Welcome to OwnKrn 0.4.0", "/bin/init displayed /etc/motd")

        out = send_cmd("uname -a")
        expect(out, "OwnKrn ownkrn 0.4.0", "uname -a syscall")
        expect(out, "x86_64", "uname machine architecture")

        out = send_cmd("neofetch")
        expect(out, "Security:", "neofetch / fetch ASCII logo & telemetry")

        out = send_cmd("top -n 1")
        expect(out, "OwnKrn Live System Monitor", "top -n 1 batch mode monitor")

        out = send_cmd("fm -b /")
        expect(out, "OwnKrn Dual-Pane File Manager", "fm -b / dual-pane file manager")

        out = send_cmd("df")
        expect(out, "diskfs(ata0) /disk", "df reports mounted persistent ATA0 /disk")

        out = send_cmd("cat /disk/README.txt")
        expect(out, "OwnKrn Persistent Disk", "read initial README.txt from persistent /disk")

        out = send_cmd("echo persist-data-123 > /disk/saved.txt")
        out = send_cmd("cat /disk/saved.txt")
        expect(out, "persist-data-123", "write and read persistent file on ATA0 /disk")
        out = send_cmd("sync")
        expect(out, "Flushing VFS & ATA0", "sync command flushes persistent storage")

        out = send_cmd("calc 0x20 + 10")
        expect(out, "42", "calc evaluates hex/dec arithmetic (0x20 + 10 = 42)")

        out = send_cmd("cal 11 2026")
        expect(out, "November 2026", "cal renders monthly calendar")

        out = send_cmd("beep 880 10")
        expect(out, "PIT Ch2 PC Speaker", "beep drives PIT Channel 2 PC Speaker")

        out = send_cmd("tree /dev")
        expect(out, "tty", "tree command displays /dev hierarchy")

        out = send_cmd("stat /etc/motd")
        expect(out, "regular file", "stat builtin reports inode metadata")

        out = send_cmd("history")
        expect(out, "uname -a", "history builtin records previous shell commands")

        out = send_cmd("cpuinfo")
        expect(out, "vendor_id", "/proc/cpuinfo hardware CPUID vendor")
        expect(out, "smep", "/proc/cpuinfo reports CPU feature flags")

        out = send_cmd("irqs")
        expect(out, "IRQ0 (PIC1)", "/proc/interrupts reports PIT IRQ0 counter")
        expect(out, "SYSCALL (LSTAR)", "/proc/interrupts reports SYSCALL counter")

        out = send_cmd("dmesg")
        expect(out, "0.0", "dmesg prints timestamped [    0.000000] kernel ring buffer")

        out = send_cmd("lspci")
        expect(out, "1234:1111", "lspci enumerates QEMU Bochs VBE PCI adapter")

        out = send_cmd("lsblk")
        expect(out, "hda", "lsblk reports ATA0 persistent block device")

        out = send_cmd("kconfig")
        expect(out, "CONFIG_64BIT=y", "kconfig (/proc/config) exposes active Kconfig")

        out = send_cmd("cat /etc/os-release")
        expect(out, "ID_LIKE=linux", "/etc/os-release Linux distribution metadata")

        out = send_cmd("id")
        expect(out, "uid=0(root)", "id reports uid=0(root) credentials")

        out = send_cmd("su guest && id && su root")
        expect(out, "uid=1000(guest)", "su guest switches UID/GID & '&&' chaining works")

        out = send_cmd("export MYVAR=linuxkernel ; echo $MYVAR $USER")
        expect(out, "linuxkernel root", "export + ';' chaining + $VAR expansion")

        out = send_cmd("which fetch")
        expect(out, "/bin/fetch", "which locates /bin/fetch executable")

        out = send_cmd("find /etc -name *release")
        expect(out, "/etc/os-release", "find -name wildcard search in /etc")

        # Real COM2 Host-to-Guest 2.5 KiB snake.py Paste Test in nano
        with cond:
            pos = len(transcript)
        for ch in b"nano /disk/snake_com2.py\n":
            proc.stdin.write(bytes([ch]))
            proc.stdin.flush()
            time.sleep(0.004)
        read_until(b"[PYTHON .py]", timeout=10.0, start_pos=pos)

        with cond:
            pos = len(transcript)
        proc.stdin.write(b"\x16")  # Ctrl+V: pull ~2.5 KiB snake.py over COM2 IRQ3 bridge
        proc.stdin.flush()
        paste_out = read_until(b"from clipboard", timeout=15.0, start_pos=pos)
        expect(paste_out, "Pasted ", "COM2 Host Bridge pasted 2.5 KiB snake.py into nano via Ctrl+V")

        with cond:
            pos = len(transcript)
        proc.stdin.write(b"\x13")  # Ctrl+S: Save to /disk/snake_com2.py
        proc.stdin.flush()
        save_out = read_until(b"Saved ", timeout=10.0, start_pos=pos)
        expect(save_out, "/disk/snake_com2.py", "nano saved 2.5 KiB pasted snake.py to persistent /disk")

        with cond:
            pos = len(transcript)
        proc.stdin.write(b"\x11")  # Ctrl+Q: Quit nano
        proc.stdin.flush()
        read_until(b"ownkrn:", timeout=10.0, start_pos=pos)

        out = send_cmd("cat /disk/snake_com2.py")
        expect(out, "# OwnKrn Python Snake Game (snake.py)", "snake_com2.py header intact (zero leading drops)")
        expect(out, "def spawn_food():", "snake_com2.py mid-file functions intact")
        expect(
            out,
            'canvas.create_rectangle(x * CELL + 1, y * CELL + 1, (x + 1) * CELL - 1, (y + 1) * CELL - 1, fill=color, outline="")',
            "snake_com2.py 127-char line preserved with zero column truncation",
        )
        expect(out, "start_snake_game()", "snake_com2.py final line intact (zero trailing drops over 2.5 KiB COM2)")

        out = send_cmd("clip ClipboardBridgeOK && paste")
        expect(out, "ClipboardBridgeOK", "/dev/clipboard device + 'clip' & 'paste' builtins")

        out = send_cmd("code --help")
        expect(out, "OwnKrn Code Studio", "code / nano / edit / micro TUI Code Studio alias")

        out = send_cmd('echo "score = 10 + 5" > /tmp/test.py && echo "for i in range(1, 3):" >> /tmp/test.py && echo "    print(f\'Loop {i} Score={score}\')" >> /tmp/test.py && python3 /tmp/test.py')
        expect(out, "Loop 1 Score=15", "python3 executes for-range loop, arithmetic & f-strings")
        expect(out, "Loop 2 Score=15", "python3 second loop iteration")

        # Interactive nano TUI test 1: Paste (Ctrl+V), Dup (Ctrl+D), Cut (Ctrl+K), Undo (Ctrl+Z), SelectAll (Ctrl+A), Copy (Ctrl+C), Save (Ctrl+S), Quit (Ctrl+Q) on .py
        send_cmd('clip "print(\'NanoPastePyOK\')"')
        with cond:
            pos = len(transcript)
        for ch in b"nano /disk/nano_paste.py\n":
            proc.stdin.write(bytes([ch]))
            proc.stdin.flush()
            time.sleep(0.004)
        nano_open = read_until(b"[PYTHON .py]", timeout=10.0, start_pos=pos)
        expect(nano_open, "OwnKrn Code Studio", "nano launches TUI Code Studio in [PYTHON .py] mode")

        with cond:
            pos = len(transcript)
        # Send Ctrl+V (paste), Ctrl+D (dup), Ctrl+K (cut), Ctrl+Z (undo), Ctrl+A (all), Ctrl+C (copy), Ctrl+S (save), Ctrl+Q (quit)
        for ch in b"\x16\x04\x0b\x1a\x01\x03\x13\x11":
            proc.stdin.write(bytes([ch]))
            proc.stdin.flush()
            time.sleep(0.03)
        read_until(b"ownkrn:", timeout=10.0, start_pos=pos)

        out = send_cmd("python3 /disk/nano_paste.py")
        expect(out, "NanoPastePyOK", "nano Ctrl+V Paste + Ctrl+S Save + python3 execution works")

        # Interactive nano TUI test 2: Ctrl+T (Starter Template) + Ctrl+R (In-Editor Save & Run) on .py
        with cond:
            pos = len(transcript)
        for ch in b"nano /disk/nano_tpl.py\n":
            proc.stdin.write(bytes([ch]))
            proc.stdin.flush()
            time.sleep(0.004)
        read_until(b"[PYTHON .py]", timeout=10.0, start_pos=pos)

        with cond:
            pos = len(transcript)
        # Send Ctrl+T (Template) then Ctrl+R (Save & Run inside nano)
        for ch in b"\x14\x12":
            proc.stdin.write(bytes([ch]))
            proc.stdin.flush()
            time.sleep(0.04)
        run_out = read_until(b"Press ENTER or SPACE to return to Code Studio", timeout=10.0, start_pos=pos)
        expect(run_out, "Welcome to OwnKrn Python, Willy!", "nano Ctrl+T template + Ctrl+R in-editor Python runner")

        with cond:
            pos = len(transcript)
        # Press Enter to return to nano, then Ctrl+Q to quit
        for ch in b"\n\x11":
            proc.stdin.write(bytes([ch]))
            proc.stdin.flush()
            time.sleep(0.04)
        read_until(b"ownkrn:", timeout=10.0, start_pos=pos)

        # Interactive nano TUI test 3: .sh script (Ctrl+T Template + Ctrl+S Save + execute /disk/nano_script.sh)
        with cond:
            pos = len(transcript)
        for ch in b"nano /disk/nano_script.sh\n":
            proc.stdin.write(bytes([ch]))
            proc.stdin.flush()
            time.sleep(0.004)
        sh_open = read_until(b"[SHELL  .sh]", timeout=10.0, start_pos=pos)
        expect(sh_open, "[SHELL  .sh]", "nano auto-detects [SHELL .sh] syntax mode")

        with cond:
            pos = len(transcript)
        for ch in b"\x14\x13\x11":
            proc.stdin.write(bytes([ch]))
            proc.stdin.flush()
            time.sleep(0.03)
        read_until(b"ownkrn:", timeout=10.0, start_pos=pos)

        out = send_cmd("/disk/nano_script.sh")
        expect(out, "=== OwnKrn System Check ===", "nano saved executable .sh script and ran '/disk/nano_script.sh'")

        # Interactive nano TUI test 4: .txt notes (typing + Ctrl+S Save + Ctrl+Q Quit)
        with cond:
            pos = len(transcript)
        for ch in b"nano /disk/nano_notes.txt\n":
            proc.stdin.write(bytes([ch]))
            proc.stdin.flush()
            time.sleep(0.004)
        txt_open = read_until(b"[TEXT  .txt]", timeout=10.0, start_pos=pos)
        expect(txt_open, "[TEXT  .txt]", "nano auto-detects [TEXT .txt] mode")

        with cond:
            pos = len(transcript)
        for ch in b"# OwnKrn Text File\nHello from nano txt!\x13\x11":
            proc.stdin.write(bytes([ch]))
            proc.stdin.flush()
            time.sleep(0.01)
        read_until(b"ownkrn:", timeout=10.0, start_pos=pos)

        out = send_cmd("cat /disk/nano_notes.txt")
        expect(out, "Hello from nano txt!", "nano edited & saved .txt file verified with cat")

        out = send_cmd("sleep 50 &")
        expect(out, "[1]", "background job launch with '&'")
        out = send_cmd("fg")
        expect(out, "sleep 50", "fg brings background job to foreground")

        out = send_cmd("date")
        expect(out, "UTC (epoch=", "CMOS RTC date command & /proc/rtc")

        out = send_cmd("pwd")
        expect(out, "/", "initial cwd is /")

        out = send_cmd("ls /")
        expect(out, "bin/", "VFS root contains /bin")
        expect(out, "dev/", "VFS root contains /dev")
        expect(out, "proc/", "VFS root contains /proc")

        out = send_cmd("mkdir /tmp/demo")
        out = send_cmd("cd /tmp/demo", wait_prompt=b"ownkrn:/tmp/demo# ")
        out = send_cmd("pwd", wait_prompt=b"ownkrn:/tmp/demo# ")
        expect(out, "/tmp/demo", "mkdir + cd + pwd")

        out = send_cmd("hello RedirTest > /tmp/demo/ext.txt", wait_prompt=b"ownkrn:/tmp/demo# ")
        out = send_cmd("cat < /tmp/demo/ext.txt", wait_prompt=b"ownkrn:/tmp/demo# ")
        expect(out, "Hello, RedirTest! (from Ring 3 ELF", "external ELF64 dup2 '>' & '<' redirection")

        out = send_cmd("echo line2 >> /tmp/demo/ext.txt", wait_prompt=b"ownkrn:/tmp/demo# ")
        out = send_cmd("cat /tmp/demo/ext.txt | wc", wait_prompt=b"ownkrn:/tmp/demo# ")
        expect(out, "     2 ", "pipe 'cat | wc' counts 2 lines")

        out = send_cmd("cp /tmp/demo/ext.txt /tmp/demo/copy.txt", wait_prompt=b"ownkrn:/tmp/demo# ")
        out = send_cmd("mv /tmp/demo/copy.txt /tmp/demo/moved.txt", wait_prompt=b"ownkrn:/tmp/demo# ")
        out = send_cmd("tail -n 1 /tmp/demo/moved.txt", wait_prompt=b"ownkrn:/tmp/demo# ")
        expect(out, "line2", "cp + mv (SYS_rename) + tail -n 1")
        out = send_cmd("rm /tmp/demo/moved.txt", wait_prompt=b"ownkrn:/tmp/demo# ")

        out = send_cmd('echo "# comment line" > /tmp/demo/run.sh', wait_prompt=b"ownkrn:/tmp/demo# ")
        out = send_cmd('echo "echo script-ok" >> /tmp/demo/run.sh', wait_prompt=b"ownkrn:/tmp/demo# ")
        out = send_cmd("sh /tmp/demo/run.sh", wait_prompt=b"ownkrn:/tmp/demo# ")
        expect(out, "script-ok", "sh <script.sh> executes script and ignores # comments")
        out = send_cmd("rm /tmp/demo/run.sh", wait_prompt=b"ownkrn:/tmp/demo# ")

        out = send_cmd("rm /tmp/demo/ext.txt", wait_prompt=b"ownkrn:/tmp/demo# ")
        out = send_cmd("cd /", wait_prompt=b"ownkrn:/# ")
        out = send_cmd("rm /tmp/demo")

        out = send_cmd("ps | grep U3")
        expect(out, "init", "pipeline 'ps | grep U3' shows init")
        expect(out, "sh", "pipeline 'ps | grep U3' shows sh")
        if "kidle" in out:
            raise AssertionError(f"'ps | grep U3' leaked non-matching line 'kidle':\n{out}")
        print("  [PASS] pipeline 'ps | grep U3' filters out K0 kidle")

        out = send_cmd("echo OwnKrn | hexdump")
        expect(out, "4f 77 6e 4b 72 6e", "pipeline 'echo | hexdump' hex output")

        out = send_cmd("free")
        expect(out, "MemTotal:", "/proc/meminfo MemTotal")
        expect(out, "MemFree:", "/proc/meminfo MemFree")

        out = send_cmd("uptime")
        expect(out, "ticks", "/proc/uptime reports PIT ticks")

        out = send_cmd("faulttest")
        expect(out, "killed by CPU exception vec=14", "Ring 3 #PF contained cleanly by kernel")
        out = send_cmd("echo shell-still-alive")
        expect(out, "shell-still-alive", "/bin/sh survived child Ring 3 segfault")

        out = send_cmd("selftest")
        expect(out, "Kernel-pointer syscall checks: write=-14 read=-14", "-EFAULT kernel pointer isolation")
        expect(out, "arg1=execve-arg-ok", "nested fork+execve argument passing")
        expect(out, "exit_code=42", "waitpid captures child exit code")
        expect(out, "kill(SIGTERM) -> pid=", "POSIX kill(SIGTERM) termination")
        expect(out, "exit_code=143", "killed child status is 128+SIGTERM (143)")
        expect(out, "Ring 3 selftest PASSED.", "full Ring 3 v0.3 selftest (refcount, pipe, dup2, malloc, kill, rtc, urandom, rename)")

        out = send_cmd("tester -b")
        expect(out, "1. Memory & Heap Stress (MemTest86)", "tester Suite 1: MemTest86 & Heap Stress")
        expect(out, "2. CPU, Syscall & Scheduler Benchmark", "tester Suite 2: RDTSC CPU/Syscall Benchmark")
        expect(out, "[6/6 SUITES PASSED]", "tester -b passes all 6 diagnostic & benchmark suites")

        out = send_cmd("cat /disk/test_report.txt")
        expect(out, "OVERALL VERDICT : 6 / 6 SUITES PASSED", "tester saved persistent report to /disk/test_report.txt")

        for ch in b"halt\n":
            proc.stdin.write(bytes([ch]))
            proc.stdin.flush()
            time.sleep(0.004)

        rc = proc.wait(timeout=10.0)
        full_log = transcript.decode("utf-8", errors="replace")
        if "KERNEL PANIC" in full_log or "KERNEL EXCEPTION" in full_log:
            raise AssertionError("Kernel panic detected in serial log!")
        if rc != 0:
            raise AssertionError(f"QEMU exited with non-zero status {rc}")

        print("  [PASS] Clean ACPI poweroff (exit code 0)")
        print("\nALL 85 OWNKRN v0.4 LINUX-PARITY END-TO-END TESTS PASSED!")
    finally:
        bridge.close()
        if proc.poll() is None:
            proc.kill()


if __name__ == "__main__":
    main()
