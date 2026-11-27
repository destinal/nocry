# Toolchain for the nocry recovery image: Buildroot (initramfs) + Linux 6.6 (k1-debian tree)
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential gcc-mipsel-linux-gnu binutils-mipsel-linux-gnu bc bison flex \
      libssl-dev libncurses-dev libelf-dev u-boot-tools device-tree-compiler kmod cpio \
      rsync wget unzip file python3 perl git ca-certificates xz-utils patch gzip bzip2 lzop \
    && rm -rf /var/lib/apt/lists/*
# Rust, for nocry itself on the board (the rescue system): the board's processor is not a
# ready-made Rust target, so the standard library is built from source with a nightly compiler.
# The date is pinned: the build passes a target feature (nan2008) that newer compilers may refuse.
ENV RUSTUP_HOME=/opt/rust/rustup CARGO_HOME=/opt/rust/cargo PATH=/opt/rust/cargo/bin:$PATH
RUN wget -qO /tmp/rustup-init https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init \
    && chmod +x /tmp/rustup-init \
    && /tmp/rustup-init -y --no-modify-path --profile minimal --default-toolchain nightly-2026-11-25 --component rust-src \
    && rm /tmp/rustup-init && chmod -R a+rX /opt/rust
