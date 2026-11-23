#!/usr/bin/env bash
set -euo pipefail

KERNEL="${KERNEL:-build/kernel.elf}"
INITRD="${INITRD:-build/initrd.tar}"

if [[ ! -f "$KERNEL" || ! -f "$INITRD" ]]; then
    echo "ERROR: $KERNEL or $INITRD missing; run 'make' first." >&2
    exit 1
fi

python3 - "$KERNEL" "$INITRD" <<'PY'
import os
import select
import subprocess
import sys
import time

kernel, initrd = sys.argv[1], sys.argv[2]

cmd = [
    "qemu-system-x86_64",
    "-machine", "pc",
    "-cpu", "qemu64,+nx,+smep",
    "-m", "256M",
    "-no-reboot",
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

def read_until(marker: bytes, timeout: float = 12.0) -> str:
    start_pos = len(transcript)
    deadline = time.monotonic() + timeout
    fd = proc.stdout.fileno()
    while time.monotonic() < deadline:
        rem = max(0.01, deadline - time.monotonic())
        r, _, _ = select.select([fd], [], [], min(0.1, rem))
        if r:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            transcript.extend(chunk)
            if marker in transcript[start_pos:]:
                return transcript[start_pos:].decode("utf-8", errors="replace")
        elif proc.poll() is not None:
            break
    dump = transcript.decode("utf-8", errors="replace")
    proc.kill()
    raise AssertionError(
        f"Timed out waiting for {marker!r}.\n--- Serial transcript ---\n{dump}\n-------------------------"
    )

def send_cmd(line: str, wait_prompt: bytes = b"ownkrn:") -> str:
    data = (line + "\n").encode("ascii")
    for ch in data:
        proc.stdin.write(bytes([ch]))
        proc.stdin.flush()
        time.sleep(0.004)
    return read_until(wait_prompt, timeout=12.0)

def expect(haystack: str, needle: str, label: str) -> None:
    if needle not in haystack:
        proc.kill()
        raise AssertionError(
            f"FAILED [{label}]: expected {needle!r} in:\n{haystack}"
        )
    print(f"  [PASS] {label}")

try:
    print("Booting OwnKrn 0.2.0 in QEMU headless (+nx,+smep)...")
    boot_out = read_until(b"ownkrn:/# ", timeout=12.0)
    expect(boot_out, "Long Mode active in higher half", "Higher-half 64-bit boot")
    expect(boot_out, "NXE=on, WP=on, SMEP=on", "CPU hardening (NXE + WP + SMEP)")
    expect(boot_out, "CMOS RTC", "CMOS RTC wall-clock init")
    expect(boot_out, "W^X enforced", "4-level VMM W^X enforcement")
    expect(boot_out, "SYSCALL/SYSRET MSRs configured", "SYSCALL/SYSRET MSR setup")
    expect(boot_out, "OWN KRN", "/bin/init launched /bin/fetch dashboard on boot")
    expect(boot_out, "Welcome to OwnKrn 0.2.0", "/bin/init displayed /etc/motd")

    out = send_cmd("uname -a")
    expect(out, "OwnKrn ownkrn 0.2.0", "uname -a syscall")
    expect(out, "x86_64", "uname machine architecture")

    out = send_cmd("neofetch")
    expect(out, "Security:", "neofetch / fetch ASCII logo & telemetry")

    out = send_cmd("top -n 1")
    expect(out, "OwnKrn Live System Monitor", "top -n 1 batch mode monitor")

    out = send_cmd("tree /dev")
    expect(out, "tty", "tree command displays /dev hierarchy")

    out = send_cmd("stat /etc/motd")
    expect(out, "regular file", "stat builtin reports inode metadata")

    out = send_cmd("history")
    expect(out, "uname -a", "history builtin records previous shell commands")

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

    # Test external ELF64 stdout redirection via dup2() + '<' stdin redirection!
    out = send_cmd("hello RedirTest > /tmp/demo/ext.txt", wait_prompt=b"ownkrn:/tmp/demo# ")
    out = send_cmd("cat < /tmp/demo/ext.txt", wait_prompt=b"ownkrn:/tmp/demo# ")
    expect(out, "Hello, RedirTest! (from Ring 3 ELF", "external ELF64 dup2 '>' & '<' redirection")

    out = send_cmd("echo line2 >> /tmp/demo/ext.txt", wait_prompt=b"ownkrn:/tmp/demo# ")
    out = send_cmd("cat /tmp/demo/ext.txt | wc", wait_prompt=b"ownkrn:/tmp/demo# ")
    expect(out, "     2 ", "pipe 'cat | wc' counts 2 lines")

    out = send_cmd("rm /tmp/demo/ext.txt", wait_prompt=b"ownkrn:/tmp/demo# ")
    out = send_cmd("cd /", wait_prompt=b"ownkrn:/# ")
    out = send_cmd("rm /tmp/demo")

    # Test multi-stage pipelines + Ring 3 coreutils (grep, wc, hexdump)
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

    # Negative security test: Ring 3 NULL dereference must kill child, NOT kernel or shell!
    out = send_cmd("faulttest")
    expect(out, "killed by CPU exception vec=14", "Ring 3 #PF contained cleanly by kernel")
    out = send_cmd("echo shell-still-alive")
    expect(out, "shell-still-alive", "/bin/sh survived child Ring 3 segfault")

    # Comprehensive Ring 3 selftest (coalescing malloc, -EFAULT, open-unlink refcount, pipe, fork/execve, kill SIGTERM, clock_gettime)
    out = send_cmd("selftest")
    expect(out, "Kernel-pointer syscall checks: write=-14 read=-14", "-EFAULT kernel pointer isolation")
    expect(out, "arg1=execve-arg-ok", "nested fork+execve argument passing")
    expect(out, "exit_code=42", "waitpid captures child exit code")
    expect(out, "kill(SIGTERM) -> pid=", "POSIX kill(SIGTERM) termination")
    expect(out, "exit_code=143", "killed child status is 128+SIGTERM (143)")
    expect(out, "Ring 3 selftest PASSED.", "full Ring 3 v0.2 selftest (refcount, pipe, dup2, malloc, kill, rtc)")

    # Clean ACPI shutdown
    for ch in b"halt\n":
        proc.stdin.write(bytes([ch]))
        proc.stdin.flush()
        time.sleep(0.004)

    rc = proc.wait(timeout=8.0)
    full_log = transcript.decode("utf-8", errors="replace")
    if "KERNEL PANIC" in full_log or "KERNEL EXCEPTION" in full_log:
        raise AssertionError("Kernel panic detected in serial log!")
    if rc != 0:
        raise AssertionError(f"QEMU exited with non-zero status {rc}")

    print("  [PASS] Clean ACPI poweroff (exit code 0)")
    print("\nALL OWNKRN v0.2 END-TO-END TESTS PASSED!")
finally:
    if proc.poll() is None:
        proc.kill()
PY
