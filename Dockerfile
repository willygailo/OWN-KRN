# OwnKrn Reproducible Cross-Platform Build & Test Container
# Works on Windows, macOS (Intel & Apple Silicon), and Linux
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    gcc \
    binutils \
    gcc-x86-64-linux-gnu \
    binutils-x86-64-linux-gnu \
    nasm \
    make \
    python3 \
    qemu-system-x86 \
    grub-pc-bin \
    grub-common \
    xorriso \
    mtools \
    gdb \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY . /workspace

CMD ["sh", "-c", "make clean && make && make test"]
