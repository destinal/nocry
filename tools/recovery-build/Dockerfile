# Toolchain for the nocry recovery image: Buildroot (initramfs) + Linux 6.6 (k1-debian tree)
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential gcc-mipsel-linux-gnu binutils-mipsel-linux-gnu bc bison flex \
      libssl-dev libncurses-dev libelf-dev u-boot-tools device-tree-compiler kmod cpio \
      rsync wget unzip file python3 perl git ca-certificates xz-utils patch gzip bzip2 lzop \
    && rm -rf /var/lib/apt/lists/*
