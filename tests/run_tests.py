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
            deadline = time.monotonic() + timeout
            while True:
                if marker in transcript[start_pos:]:
                    return transcript[start_pos:].decode("utf-8", errors="replace")
                rem = deadline - time.monotonic()
                if rem <= 0 or (eof_reached and proc.poll() is not None):
                    dump = transcript.decode("utf-8", errors="replace")
                    proc.kill()
                    raise AssertionError(
                        f"Timed out waiting for {marker!r}.\n"
                        f"--- Serial transcript ---\n{dump}\n-------------------------"
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
        print(f"Booting OwnKrn 0.3.0 in QEMU headless (+nx,+smep,ATA0) on {sys.platform}...")
        boot_out = read_until(b"ownkrn:/# ", timeout=15.0, start_pos=0)
        expect(boot_out, "Long Mode active in higher half", "Higher-half 64-bit boot")
        expect(boot_out, "NXE=on, WP=on, SMEP=on", "CPU hardening (NXE + WP + SMEP)")
        expect(boot_out, "CMOS RTC", "CMOS RTC wall-clock init")
        expect(boot_out, "W^X enforced", "4-level VMM W^X enforcement")
        expect(boot_out, "ATA0 Master", "ATA0 PIO 28-bit LBA hard disk initialized")
        expect(boot_out, "/disk=diskfs", "Persistent DiskFS mounted at /disk")
        expect(boot_out, "SYSCALL/SYSRET MSRs configured", "SYSCALL/SYSRET MSR setup")
        expect(boot_out, "OWN KRN", "/bin/init launched /bin/fetch dashboard on boot")
        expect(boot_out, "Welcome to OwnKrn 0.3.0", "/bin/init displayed /etc/motd")

        out = send_cmd("uname -a")
        expect(out, "OwnKrn ownkrn 0.3.0", "uname -a syscall")
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
        print("\nALL 54 OWNKRN v0.3 CROSS-PLATFORM END-TO-END TESTS PASSED!")
    finally:
        if proc.poll() is None:
            proc.kill()


if __name__ == "__main__":
    main()
