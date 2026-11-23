#!/usr/bin/env python3
"""
Portable cross-platform image builder for OwnKrn (Windows, macOS, Linux).
Replaces GNU-specific 'tar --format=ustar' and '/dev/zero' dd commands.
"""
import os
import stat
import sys
import tarfile


def build_initrd(out_tar: str, root_dir: str) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(out_tar)), exist_ok=True)
    with tarfile.open(out_tar, "w", format=tarfile.USTAR_FORMAT) as tf:
        for dirpath, dirnames, filenames in os.walk(root_dir):
            dirnames[:] = sorted(
                d for d in dirnames if not d.startswith("._") and d != ".DS_Store"
            )
            filenames = sorted(
                f for f in filenames if not f.startswith("._") and f != ".DS_Store"
            )

            rel_dir = os.path.relpath(dirpath, root_dir).replace("\\", "/")
            if rel_dir != ".":
                ti = tarfile.TarInfo(name="./" + rel_dir + "/")
                ti.type = tarfile.DIRTYPE
                ti.mode = 0o755
                ti.uid = 0
                ti.gid = 0
                ti.uname = "root"
                ti.gname = "root"
                tf.addfile(ti)

            for fname in filenames:
                full_path = os.path.join(dirpath, fname)
                rel_path = os.path.relpath(full_path, root_dir).replace("\\", "/")
                arcname = "./" + rel_path
                st = os.stat(full_path)

                ti = tarfile.TarInfo(name=arcname)
                ti.size = st.st_size
                ti.mtime = int(st.st_mtime)
                ti.type = tarfile.REGTYPE
                is_bin = rel_path.startswith("bin/")
                ti.mode = 0o755 if is_bin else 0o644
                ti.uid = 0
                ti.gid = 0
                ti.uname = "root"
                ti.gname = "root"
                with open(full_path, "rb") as fobj:
                    tf.addfile(ti, fobj)


def ensure_disk_img(out_img: str, size_mb: int) -> None:
    if os.path.exists(out_img):
        return
    os.makedirs(os.path.dirname(os.path.abspath(out_img)), exist_ok=True)
    size_bytes = size_mb * 1024 * 1024
    with open(out_img, "wb") as f:
        f.seek(size_bytes - 1)
        f.write(b"\x00")
    print(f"Created persistent ATA disk image {out_img} ({size_mb} MiB)")


def main() -> None:
    if len(sys.argv) < 2:
        print("Usage: mkimg.py [initrd <out.tar> <root_dir> | disk <out.img> <mb>]")
        sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "initrd":
        build_initrd(sys.argv[2], sys.argv[3])
    elif cmd == "disk":
        ensure_disk_img(sys.argv[2], int(sys.argv[3]))
    else:
        print(f"Unknown command: {cmd}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
