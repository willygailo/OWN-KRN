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
    "-cpu", "qemu64,+nx",
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

def read_until(marker: bytes, timeout: float = 10.0) -> str:
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
    # Pace serial input gently to respect the 16-byte 16550 UART RX FIFO
    data = (line + "\n").encode("ascii")
    for ch in data:
        proc.stdin.write(bytes([ch]))
        proc.stdin.flush()
        time.sleep(0.004)
    return read_until(wait_prompt, timeout=10.0)

def expect(haystack: str, needle: str, label: str) -> None:
    if needle not in haystack:
        proc.kill()
        raise AssertionError(
            f"FAILED [{label}]: expected {needle!r} in:\n{haystack}"
        )
    print(f"  [PASS] {label}")

try:
    print("Booting OwnKrn in QEMU headless...")
    boot_out = read_until(b"ownkrn:/# ", timeout=12.0)
    expect(boot_out, "Long Mode active in higher half", "Higher-half 64-bit boot")
    expect(boot_out, "W^X enforced", "4-level VMM W^X enforcement")
    expect(boot_out, "SYSCALL/SYSRET MSRs configured", "SYSCALL/SYSRET MSR setup")
    expect(boot_out, "Welcome to OwnKrn", "/bin/init displayed /etc/motd")

    out = send_cmd("uname -a")
    expect(out, "OwnKrn ownkrn 0.1.0", "uname -a syscall")
    expect(out, "x86_64", "uname machine architecture")

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

    out = send_cmd("echo kernel-from-scratch > /tmp/demo/note.txt", wait_prompt=b"ownkrn:/tmp/demo# ")
    out = send_cmd("cat /tmp/demo/note.txt", wait_prompt=b"ownkrn:/tmp/demo# ")
    expect(out, "kernel-from-scratch", "tmpfs file write + redirection + cat")

    out = send_cmd("ls", wait_prompt=b"ownkrn:/tmp/demo# ")
    expect(out, "note.txt", "ls shows newly created file")

    out = send_cmd("rm /tmp/demo/note.txt", wait_prompt=b"ownkrn:/tmp/demo# ")
    out = send_cmd("cd /", wait_prompt=b"ownkrn:/# ")
    out = send_cmd("rm /tmp/demo")

    out = send_cmd("free")
    expect(out, "MemTotal:", "/proc/meminfo MemTotal")
    expect(out, "MemFree:", "/proc/meminfo MemFree")

    out = send_cmd("ps")
    expect(out, "kidle", "/proc/tasks shows Ring 0 kidle")
    expect(out, "init", "/proc/tasks shows Ring 3 init")
    expect(out, "sh", "/proc/tasks shows Ring 3 sh")

    out = send_cmd("uptime")
    expect(out, "ticks", "/proc/uptime reports PIT ticks")

    out = send_cmd("hello World")
    expect(out, "Hello, World! (from Ring 3 ELF, pid=", "external Ring 3 ELF fork+execve")

    out = send_cmd("selftest")
    expect(out, "Kernel-pointer syscall checks: write=-14 read=-14", "-EFAULT kernel pointer isolation")
    expect(out, "arg1=execve-arg-ok", "nested fork+execve argument passing")
    expect(out, "exit_code=42", "waitpid captures child exit code")
    expect(out, "Ring 3 selftest PASSED.", "full Ring 3 syscall/brk/VFS/fork/execve selftest")

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
    print("\nALL OWNKRN END-TO-END TESTS PASSED!")
finally:
    if proc.poll() is None:
        proc.kill()
PY
