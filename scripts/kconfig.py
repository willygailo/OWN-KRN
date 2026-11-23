#!/usr/bin/env python3
"""
torvalds/linux-style Kconfig & menuconfig engine for OwnKrn.
Supports:
  - python3 scripts/kconfig.py defconfig
  - python3 scripts/kconfig.py syncconfig
  - python3 scripts/kconfig.py menuconfig
Generates .config and include/generated/autoconf.h.
"""
import os
import shutil
import sys

DEFCONFIG_PATH = "configs/x86_64_defconfig"
DOTCONFIG_PATH = ".config"
AUTOCONF_PATH = "include/generated/autoconf.h"

DESCRIPTIONS = {
    "CONFIG_LOCALVERSION": "Local version appended to kernel release",
    "CONFIG_DEFAULT_HOSTNAME": "Default system hostname (uname.nodename)",
    "CONFIG_IKCONFIG_PROC": "Expose active .config via /proc/config",
    "CONFIG_PRINTK_RINGBUF_KB": "Kernel dmesg log ring buffer size (KiB)",
    "CONFIG_64BIT": "64-bit x86_64 Long Mode kernel",
    "CONFIG_SMP": "Symmetric Multi-Processing support",
    "CONFIG_PREEMPT": "Preemptive Round-Robin task scheduler",
    "CONFIG_HZ": "PIT system timer frequency (Hz)",
    "CONFIG_X86_SMEP": "Supervisor Mode Execution Prevention (CR4.SMEP)",
    "CONFIG_STRICT_KERNEL_RWX": "Strict 4-level VMM W^X page permissions",
    "CONFIG_PCI": "PCI Bus hardware enumeration (/proc/pci & lspci)",
    "CONFIG_FB_VBE": "Bochs VBE 1024x576x32 Widescreen Framebuffer",
    "CONFIG_ATA_PIO": "ATA0 28-bit LBA PIO persistent disk driver",
    "CONFIG_PC_SPEAKER": "PIT Channel 2 PC Speaker audio driver",
    "CONFIG_RTC_CMOS": "CMOS RTC wall-clock driver (/proc/rtc)",
    "CONFIG_SERIAL_8250": "COM1 16550A UART serial driver",
    "CONFIG_TMPFS": "Writable in-memory tmpfs root filesystem",
    "CONFIG_DISKFS": "Persistent ATA0 DiskFS mounted at /disk",
    "CONFIG_PROC_FS": "/proc kernel & process telemetry filesystem",
    "CONFIG_DEVTMPFS": "/dev character device filesystem",
}


def parse_config(path: str) -> list[tuple[str, str]]:
    items: list[tuple[str, str]] = []
    if not os.path.exists(path):
        return items
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("# CONFIG_") and line.endswith(" is not set"):
                key = line.split()[1]
                items.append((key, "n"))
            elif not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                items.append((k.strip(), v.strip()))
    return items


def save_config(items: list[tuple[str, str]], path: str = DOTCONFIG_PATH) -> None:
    with open(path, "w", encoding="utf-8") as f:
        f.write("#\n# Automatically generated file; DO NOT EDIT.\n")
        f.write("# OwnKrn/x86_64 0.4.0 Kernel Configuration\n#\n")
        for k, v in items:
            if v == "n":
                f.write(f"# {k} is not set\n")
            else:
                f.write(f"{k}={v}\n")


def generate_autoconf(items: list[tuple[str, str]]) -> None:
    os.makedirs(os.path.dirname(AUTOCONF_PATH), exist_ok=True)
    cfg_lines = []
    for k, v in items:
        if v == "n":
            cfg_lines.append(f"# {k} is not set")
        else:
            cfg_lines.append(f"{k}={v}")
    escaped_blob = "\\n".join(l.replace('"', '\\"') for l in cfg_lines) + "\\n"

    with open(AUTOCONF_PATH, "w", encoding="utf-8") as f:
        f.write("/*\n * Automatically generated C config: don't edit\n")
        f.write(" * OwnKrn/x86_64 0.4.0 Kernel Configuration\n */\n")
        f.write("#ifndef OWNKRN_GENERATED_AUTOCONF_H\n")
        f.write("#define OWNKRN_GENERATED_AUTOCONF_H\n\n")
        for k, v in items:
            if v == "y":
                f.write(f"#define {k} 1\n")
            elif v == "n":
                f.write(f"/* #undef {k} */\n")
            else:
                f.write(f"#define {k} {v}\n")
        f.write(f'\n#define OWNKRN_IKCONFIG_TEXT "{escaped_blob}"\n\n')
        f.write("#endif /* OWNKRN_GENERATED_AUTOCONF_H */\n")


def cmd_defconfig() -> None:
    shutil.copyfile(DEFCONFIG_PATH, DOTCONFIG_PATH)
    items = parse_config(DOTCONFIG_PATH)
    generate_autoconf(items)
    print(f"*** Default configuration written to {DOTCONFIG_PATH} & {AUTOCONF_PATH} ***")


def cmd_syncconfig() -> None:
    if not os.path.exists(DOTCONFIG_PATH):
        shutil.copyfile(DEFCONFIG_PATH, DOTCONFIG_PATH)
    items = parse_config(DOTCONFIG_PATH)
    generate_autoconf(items)


def run_curses_menuconfig(items: list[tuple[str, str]]) -> bool:
    import curses

    def _ui(stdscr) -> bool:
        curses.curs_set(0)
        stdscr.keypad(True)
        curses.start_color()
        curses.use_default_colors()
        curses.init_pair(1, curses.COLOR_WHITE, curses.COLOR_BLUE)
        curses.init_pair(2, curses.COLOR_BLACK, curses.COLOR_CYAN)
        curses.init_pair(3, curses.COLOR_YELLOW, -1)

        sel = 0
        saved = False
        while True:
            stdscr.erase()
            h, w = stdscr.getmaxyx()
            title = " OwnKrn/x86_64 0.4.0 Kernel Configuration (Kconfig / menuconfig) "
            stdscr.addstr(0, 0, title.ljust(w)[: w - 1], curses.color_pair(1) | curses.A_BOLD)
            stdscr.addstr(
                1, 2,
                "Arrow keys navigate | [Space/Enter] Toggle/Edit | [S] Save & Exit | [Q] Quit",
                curses.color_pair(3),
            )
            stdscr.addstr(2, 0, "-" * (w - 1))

            max_rows = max(4, h - 7)
            top = max(0, min(sel - max_rows // 2, max(0, len(items) - max_rows)))
            for idx in range(top, min(len(items), top + max_rows)):
                r = 3 + (idx - top)
                k, v = items[idx]
                desc = DESCRIPTIONS.get(k, k)
                if v in ("y", "n"):
                    tag = "[*]" if v == "y" else "[ ]"
                else:
                    tag = f"({v})"
                line = f"  {tag:<12} {k:<28} - {desc}"
                attr = curses.color_pair(2) | curses.A_BOLD if idx == sel else curses.A_NORMAL
                stdscr.addstr(r, 1, line.ljust(w - 2)[: w - 2], attr)

            stdscr.addstr(h - 3, 0, "-" * (w - 1))
            stdscr.addstr(
                h - 2, 2,
                "  [ Save (.config + autoconf.h) ]       [ Load defconfig ]       [ Exit ]",
                curses.color_pair(1) | curses.A_BOLD,
            )
            stdscr.refresh()

            ch = stdscr.getch()
            if ch in (ord("q"), ord("Q"), 27):
                break
            if ch in (ord("s"), ord("S")):
                save_config(items)
                generate_autoconf(items)
                saved = True
                break
            if ch in (curses.KEY_UP, ord("k")) and sel > 0:
                sel -= 1
            elif ch in (curses.KEY_DOWN, ord("j")) and sel + 1 < len(items):
                sel += 1
            elif ch in (ord(" "), 10, 13):
                k, v = items[sel]
                if v == "y":
                    items[sel] = (k, "n")
                elif v == "n":
                    items[sel] = (k, "y")
        return saved

    return curses.wrapper(_ui)


def cmd_menuconfig() -> None:
    if not os.path.exists(DOTCONFIG_PATH):
        shutil.copyfile(DEFCONFIG_PATH, DOTCONFIG_PATH)
    items = parse_config(DOTCONFIG_PATH)
    try:
        saved = run_curses_menuconfig(items)
        if saved:
            print(f"*** Configuration saved to {DOTCONFIG_PATH} & {AUTOCONF_PATH} ***")
        else:
            print("*** menuconfig exited without saving changes ***")
    except Exception:
        generate_autoconf(items)


def main() -> None:
    mode = sys.argv[1] if len(sys.argv) > 1 else "syncconfig"
    if mode == "defconfig":
        cmd_defconfig()
    elif mode == "menuconfig":
        cmd_menuconfig()
    else:
        cmd_syncconfig()


if __name__ == "__main__":
    main()
