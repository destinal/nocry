//! Restore / provision an X2000E Nebula-family eMMC, from blank or from any state.
//!
//! Mirrors Creality's factory Cloner policy (docs/ingenic-package.md):
//!   erase everything except sn_mac, then write
//!   boot area (MBR+GPT+SPL+U-Boot) @0, rtos, kernel, rootfs.
//! Works on the nocry-ums (read-write) block device or on an image file.

use crate::events;
use crate::gpt::{Gpt, GPT_AREA};
use crate::snmac::{self, SnMac, NEBULA_BOARD};
use anyhow::{anyhow, bail, Context, Result};
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom};

// positional file I/O and block-device detection, per platform
#[cfg(unix)]
fn read_exact_at(f: &File, buf: &mut [u8], off: u64) -> std::io::Result<()> {
    std::os::unix::fs::FileExt::read_exact_at(f, buf, off)
}
#[cfg(unix)]
fn write_all_at(f: &File, buf: &[u8], off: u64) -> std::io::Result<()> {
    std::os::unix::fs::FileExt::write_all_at(f, buf, off)
}
#[cfg(unix)]
fn is_block_device(md: &std::fs::Metadata) -> bool {
    std::os::unix::fs::FileTypeExt::is_block_device(&md.file_type())
}
#[cfg(windows)]
fn read_exact_at(f: &File, mut buf: &mut [u8], mut off: u64) -> std::io::Result<()> {
    use std::os::windows::fs::FileExt;
    while !buf.is_empty() {
        let n = f.seek_read(buf, off)?;
        if n == 0 {
            return Err(std::io::ErrorKind::UnexpectedEof.into());
        }
        buf = &mut buf[n..];
        off += n as u64;
    }
    Ok(())
}
#[cfg(windows)]
fn write_all_at(f: &File, mut buf: &[u8], mut off: u64) -> std::io::Result<()> {
    use std::os::windows::fs::FileExt;
    while !buf.is_empty() {
        let n = f.seek_write(buf, off)?;
        buf = &buf[n..];
        off += n as u64;
    }
    Ok(())
}
#[cfg(windows)]
fn is_block_device(_md: &std::fs::Metadata) -> bool {
    false // raw disks are not used on Windows; the fastboot service is
}
use std::path::{Path, PathBuf};

const MIB: u64 = 1 << 20;
const XBOOT_END: u64 = 0x100000; // boot area region before the first partition (ota)
const SPL_OFFSET: usize = GPT_AREA; // 0x4400

// ---------------------------------------------------------------------------
// Firmware package

pub struct Package {
    pub source: String,
    pub boot: Option<Vec<u8>>,
    pub rtos: Vec<u8>,
    pub kernel: Vec<u8>,
    pub rootfs: Vec<u8>,
    pub ota_board: Option<String>,
    pub ota_version: Option<String>,
}

fn is_zip(p: &Path) -> bool {
    let mut m = [0u8; 4];
    File::open(p).and_then(|mut f| f.read_exact(&mut m)).is_ok() && &m == b"PK\x03\x04"
}

fn zip_entry(p: &Path, name: &str) -> Result<Option<Vec<u8>>> {
    let mut z = zip::ZipArchive::new(File::open(p)?).with_context(|| format!("{} is not a valid zip", p.display()))?;
    let res = match z.by_name(name) {
        Ok(mut e) => {
            let mut v = Vec::with_capacity(e.size() as usize);
            e.read_to_end(&mut v)?;
            Some(v)
        }
        Err(zip::result::ZipError::FileNotFound) => None,
        Err(e) => return Err(e.into()),
    };
    Ok(res)
}

fn read_opt(p: &Path) -> Result<Option<Vec<u8>>> {
    if p.exists() {
        Ok(Some(std::fs::read(p).with_context(|| format!("reading {}", p.display()))?))
    } else {
        Ok(None)
    }
}

/// `unsquashfs -cat rootfs etc/ota_info` -> (board, version)
/// Reads etc/ota_info out of the rootfs squashfs (no external tools).
fn ota_info(rootfs: &[u8]) -> (Option<String>, Option<String>) {
    let read = || -> Option<String> {
        let out = crate::squashfs::read_file(rootfs, "etc/ota_info", 64 * 1024)?;
        Some(String::from_utf8_lossy(&out).to_string())
    };
    let s = read().unwrap_or_default();
    let get = |k: &str| s.lines().find_map(|l| l.strip_prefix(k)).map(|v| v.trim().to_string());
    (get("ota_board_name="), get("ota_version="))
}

impl Package {
    /// `.ingenic` (Cloner zip) or a directory with xImage, rootfs.squashfs, zero.bin
    /// [, u-boot-with-spl-mbr-gpt.bin] (e.g. extract_creality_ota.py output).
    pub fn load(p: &Path) -> Result<Package> {
        if !p.exists() {
            let cwd = std::env::current_dir().map(|d| d.display().to_string()).unwrap_or_default();
            bail!("{}: no such file or directory (relative paths are resolved from {cwd})", p.display());
        }
        let (boot, rtos, kernel, rootfs) = if p.is_dir() {
            (
                read_opt(&p.join("u-boot-with-spl-mbr-gpt.bin"))?,
                read_opt(&p.join("zero.bin"))?,
                read_opt(&p.join("xImage"))?,
                read_opt(&p.join("rootfs.squashfs"))?,
            )
        } else if is_zip(p) {
            (
                zip_entry(p, "images/u-boot-with-spl-mbr-gpt.bin")?,
                zip_entry(p, "images/zero.bin")?,
                zip_entry(p, "images/xImage")?,
                zip_entry(p, "images/rootfs.squashfs")?,
            )
        } else if crate::ota::is_ota(p) {
            let o = crate::ota::load(p)?;
            (None, o.rtos, o.kernel, o.rootfs)
        } else {
            bail!(
                "{}: not a firmware nocry can read. Expected a Creality .ingenic package, an OTA update \
                 file (.img) for an Ingenic-based printer, or a folder with an unpacked update",
                p.display()
            );
        };
        let need = |v: Option<Vec<u8>>, n: &str| v.ok_or_else(|| anyhow!("{}: missing {n}", p.display()));
        let (rtos, kernel, rootfs) = (need(rtos, "zero.bin (rtos)")?, need(kernel, "xImage")?, need(rootfs, "rootfs.squashfs")?);
        if rtos.get(8..12) != Some(b"RTOS") {
            bail!("zero.bin has no RTOS magic");
        }
        if kernel.get(..4) != Some(&[0x27, 0x05, 0x19, 0x56]) {
            bail!("xImage is not a uImage");
        }
        if rootfs.get(..4) != Some(b"hsqs") {
            bail!("rootfs.squashfs has no squashfs magic");
        }
        if let Some(b) = &boot {
            check_boot(b)?;
        }
        let (ota_board, ota_version) = ota_info(&rootfs);
        Ok(Package { source: p.display().to_string(), boot, rtos, kernel, rootfs, ota_board, ota_version })
    }
}

pub fn check_boot(b: &[u8]) -> Result<Gpt> {
    if b.len() <= SPL_OFFSET + 4096 || b[0x1fe..0x200] != [0x55, 0xaa] {
        bail!("boot area: missing protective MBR or too small");
    }
    let g = Gpt::parse(b).context("boot area GPT")?;
    if b[SPL_OFFSET..SPL_OFFSET + 64].iter().all(|&x| x == 0) {
        bail!("boot area: no SPL at {SPL_OFFSET:#x}");
    }
    for n in ["ota", "sn_mac", "rtos", "kernel", "rootfs", "rootfs_data", "userdata"] {
        if g.find(n).is_none() {
            bail!("boot area GPT lacks partition {n:?}; not a Nebula-family layout");
        }
    }
    Ok(g)
}

/// Boot area from a `.ingenic` or a raw u-boot-with-spl-mbr-gpt.bin.
pub fn load_boot(p: &Path) -> Result<Vec<u8>> {
    if !p.exists() {
        bail!("{}: no such file or directory", p.display());
    }
    let b = if is_zip(p) {
        zip_entry(p, "images/u-boot-with-spl-mbr-gpt.bin")?.ok_or_else(|| anyhow!("{} has no boot area", p.display()))?
    } else {
        std::fs::read(p)?
    };
    check_boot(&b)?;
    Ok(b)
}

// ---------------------------------------------------------------------------
// Target (block device or image file)

enum Backend {
    File(File),
    /// nocry-fbd in the recovery Linux; offsets address the whole eMMC ("mmc0")
    Fb(std::cell::RefCell<crate::fastboot::Fastboot>),
}

pub struct Target {
    b: Backend,
    writable: bool,
    pub path: PathBuf,
    pub size: u64,
    pub is_dev: bool,
}

#[cfg(target_os = "linux")]
fn check_blockdev(path: &Path, allow_any: bool) -> Result<()> {
    let real = std::fs::canonicalize(path)?;
    let name = real.file_name().and_then(|n| n.to_str()).ok_or_else(|| anyhow!("bad device path"))?.to_string();
    let sys = PathBuf::from("/sys/class/block").join(&name);
    if sys.join("partition").exists() {
        bail!("{} is a partition; give the whole disk (e.g. /dev/sda)", real.display());
    }
    let model = std::fs::read_to_string(sys.join("device/model")).unwrap_or_default();
    let ro = std::fs::read_to_string(sys.join("ro")).unwrap_or_default();
    if !model.contains("nocry eMMC") && !allow_any {
        bail!(
            "{} is not a nocry eMMC (model {:?}). Refusing to write to an unknown disk; \
             use --allow-any-device only if you are sure",
            real.display(),
            model.trim()
        );
    }
    if ro.trim() == "1" {
        bail!("{} is read-only: use `nocry recovery --ums rw` or the read-write U-Boot stage2", real.display());
    }
    let mounts = std::fs::read_to_string("/proc/mounts").unwrap_or_default();
    let dev = format!("/dev/{name}");
    let busy: Vec<&str> = mounts.lines().filter(|l| l.starts_with(&dev)).collect();
    if !busy.is_empty() {
        bail!("partitions of {dev} are mounted (desktop automount?); unmount first:\n  {}", busy.join("\n  "));
    }
    Ok(())
}

#[cfg(not(target_os = "linux"))]
fn check_blockdev(_path: &Path, allow_any: bool) -> Result<()> {
    if !allow_any {
        bail!("block-device safety checks are Linux-only; use --allow-any-device");
    }
    Ok(())
}

const HCHUNK: u64 = MIB;
const FB_STEP: u64 = 256 * MIB;

impl Target {
    pub fn open(path: &Path, write: bool, allow_any: bool) -> Result<Target> {
        let md = std::fs::metadata(path).with_context(|| format!("{}", path.display()))?;
        let is_dev = is_block_device(&md);
        if is_dev && write {
            check_blockdev(path, allow_any)?;
        }
        let mut f = OpenOptions::new().read(true).write(write).open(path).with_context(|| {
            format!("opening {} (on Linux, are you in the 'disk' group / did you re-login?)", path.display())
        })?;
        let size = f.seek(SeekFrom::End(0))?;
        Ok(Target { b: Backend::File(f), writable: write, path: path.to_path_buf(), size, is_dev })
    }

    /// The recovery's fastboot service (no disk device on the host).
    pub fn open_fastboot(found: &crate::usb::Found, write: bool) -> Result<Target> {
        let mut fb = crate::fastboot::Fastboot::open(found)?;
        fb.info = Box::new(|_| {});
        let ext = fb.getvar("nocry-ext").unwrap_or_default();
        if !ext.contains("write") || !ext.contains("hash:xxh64") {
            bail!("the board's fastboot service lacks the nocry extensions (old recovery image?)");
        }
        let size = u64::from_str_radix(fb.getvar("emmc-size")?.trim_start_matches("0x"), 16).context("bad emmc-size")?;
        let sn = fb.getvar("serialno").unwrap_or_default();
        if write {
            // nocry does its own checks; the service's guard is for hand-typed fastboot commands
            fb.command("flashing unlock_critical")?;
        }
        Ok(Target {
            b: Backend::Fb(std::cell::RefCell::new(fb)),
            writable: write,
            path: PathBuf::from(format!("fastboot:{sn}")),
            size,
            is_dev: true,
        })
    }

    pub fn is_fastboot(&self) -> bool {
        matches!(self.b, Backend::Fb(_))
    }

    pub fn read(&self, off: u64, len: usize) -> Result<Vec<u8>> {
        match &self.b {
            Backend::File(f) => {
                let mut v = vec![0u8; len];
                read_exact_at(f, &mut v, off).with_context(|| format!("read {len} @ {off:#x}"))?;
                Ok(v)
            }
            Backend::Fb(fb) => {
                let a = off & !511;
                let end = (off + len as u64).div_ceil(512) * 512;
                let mut v = Vec::with_capacity((end - a) as usize);
                let mut at = a;
                while at < end {
                    let n = FB_STEP.min(end - at);
                    fb.borrow_mut().fetch("mmc0", at, n, |b| {
                        v.extend_from_slice(b);
                        Ok(())
                    })?;
                    at += n;
                }
                let s = (off - a) as usize;
                Ok(v[s..s + len].to_vec())
            }
        }
    }

    pub fn drop_cache(&self, off: u64, len: u64) {
        #[cfg(target_os = "linux")]
        if let Backend::File(f) = &self.b {
            use std::os::unix::io::AsRawFd;
            unsafe {
                libc::posix_fadvise(f.as_raw_fd(), off as i64, len as i64, libc::POSIX_FADV_DONTNEED);
            }
        }
        let _ = (off, len);
    }

    /// Per-chunk digests computed on the board (fastboot only): comparing without moving the data.
    pub fn chunk_hashes(&self, off: u64, len: u64, chunk: u64) -> Result<Option<Vec<Vec<u8>>>> {
        match &self.b {
            Backend::Fb(fb) if off % 512 == 0 => {
                Ok(Some(fb.borrow_mut().hash(crate::fastboot::Hash::Xxh64, "mmc0", off, len, chunk)?))
            }
            _ => Ok(None),
        }
    }

    /// Hash of `data` as chunk_hashes would report it.
    pub fn local_hash(data: &[u8]) -> Vec<u8> {
        crate::fastboot::Hash::Xxh64.of(data)
    }

    /// Does the device hold exactly `expected` at `off`? (page cache bypassed)
    pub fn matches(&self, off: u64, expected: &[u8]) -> Result<bool> {
        if let Some(h) = self.chunk_hashes(off, expected.len() as u64, HCHUNK)? {
            let mut it = h.iter();
            return Ok(expected.chunks(HCHUNK as usize).all(|c| {
                // the board hashes whole sectors of the last chunk only up to `len`
                it.next().map(|d| d == &Self::local_hash(c)).unwrap_or(false)
            }));
        }
        self.drop_cache(off, expected.len() as u64);
        Ok(self.read(off, expected.len())? == expected)
    }

    /// Write without verifying (call sync, then matches).
    pub fn write_raw(&self, off: u64, data: &[u8], what: &str) -> Result<()> {
        if !self.writable {
            bail!("{} was opened read-only", self.path.display());
        }
        match &self.b {
            Backend::File(f) => write_all_at(f, data, off).with_context(|| format!("writing {what}")),
            Backend::Fb(fb) => {
                // the service writes whole sectors: merge unaligned edges with what is there
                let start = off & !511;
                let end = off + data.len() as u64;
                let padded: Vec<u8>;
                let mut d = data;
                if start != off || end % 512 != 0 {
                    let mut v = Vec::with_capacity(data.len() + 1024);
                    if start != off {
                        v.extend_from_slice(&self.read(start, (off - start) as usize)?);
                    }
                    v.extend_from_slice(data);
                    if end % 512 != 0 {
                        let tail = self.read(end & !511, 512)?;
                        v.extend_from_slice(&tail[(end % 512) as usize..]);
                    }
                    padded = v;
                    d = &padded[..];
                }
                let off = start;
                for (i, c) in d.chunks(FB_STEP as usize).enumerate() {
                    fb.borrow_mut().write("mmc0", off + i as u64 * FB_STEP, c).with_context(|| format!("writing {what}"))?;
                }
                Ok(())
            }
        }
    }

    /// Erase a range so the old data cannot be read back: eMMC secure discard where the board
    /// service and card support it, otherwise overwriting with zeros. Always ends all-zero.
    /// Returns the method used ("secure-discard", "discard" or "overwrite").
    pub fn secure_erase(&self, off: u64, len: u64, what: &str, tick: &mut dyn FnMut(u64)) -> Result<String> {
        if !self.writable {
            bail!("{} was opened read-only", self.path.display());
        }
        if let Backend::Fb(fb) = &self.b {
            let has = fb.borrow_mut().getvar("nocry-ext").unwrap_or_default().contains("secure-erase");
            if has && off % 512 == 0 && len % 512 == 0 {
                // in pieces, so progress is visible and no single command runs very long
                let (mut done, mut method) = (0u64, String::new());
                while done < len {
                    let n = (128 * MIB).min(len - done);
                    method = fb.borrow_mut().secure_erase("mmc0", off + done, n).with_context(|| format!("erasing {what}"))?;
                    done += n;
                    tick(n);
                }
                return Ok(method);
            }
        }
        let zeros = vec![0u8; (32 * MIB) as usize];
        let mut done = 0;
        while done < len {
            let n = (32 * MIB).min(len - done) as usize;
            if !self.matches(off + done, &zeros[..n])? {
                self.write_raw(off + done, &zeros[..n], what)?;
            }
            done += n as u64;
            tick(n as u64);
        }
        self.sync()?;
        Ok("overwrite".into())
    }

    pub fn sync(&self) -> Result<()> {
        if let Backend::File(f) = &self.b {
            f.sync_all()?;
        }
        Ok(()) // nocry-fbd syncs at the end of every write command
    }

    /// Write, flush, then compare what the device holds (page cache dropped / on-board hash).
    pub fn write_verified(&self, off: u64, data: &[u8], what: &str) -> Result<()> {
        let chunk = 4 * MIB as usize;
        let total = data.len();
        let mut done = 0;
        while done < total {
            let n = if self.is_fastboot() { total } else { chunk.min(total - done) };
            self.write_raw(off + done as u64, &data[done..done + n], what)?;
            done += n;
            if total > 8 * MIB as usize && !self.is_fastboot() {
                events::progress(what, done as u64, total as u64, "");
            }
        }
        if total > 8 * MIB as usize && !self.is_fastboot() {
            events::progress_end();
        }
        self.sync()?;
        let mut done = 0;
        while done < total {
            let n = (64 * MIB as usize).min(total - done);
            if !self.matches(off + done as u64, &data[done..done + n])? {
                bail!("verify FAILED for {what} within {:#x}..{:#x}", off + done as u64, off + (done + n) as u64);
            }
            done += n;
        }
        Ok(())
    }
}

// ---------------------------------------------------------------------------
// Plan

#[derive(Debug, Clone, Copy, PartialEq, clap::ValueEnum)]
pub enum Wipe {
    /// zero the first MiB and the superblock slot of every block group for every
    /// ext block size (fast; e2fsck cannot resurrect anything, and the board
    /// formats rootfs_data/userdata on first boot like after a factory flash)
    Quick,
    /// zero the whole partitions (slow, exactly like the factory erase)
    Full,
}

pub struct Options {
    pub model: Option<String>,
    pub board: Option<String>,
    pub sn: Option<String>,
    pub mac: Option<String>,
    pub sn_prefix: Option<String>,
    pub new_identity: bool,
    pub keep_userdata: bool,
    pub wipe: Wipe,
    pub factory_guids: bool,
    pub force_model: bool,
    /// minimal repair: keep a valid boot area, B slots, userdata, and the overlay
    /// when the device already runs the same firmware version
    pub repair: bool,
    pub force_boot: bool,
    pub keep_overlay: bool,
    pub wipe_overlay: bool,
    /// write every planned chunk even if it already matches (tests the write path)
    pub rewrite: bool,
}

const CH: u64 = MIB; // comparison / write granularity

/// Expected content of an extent.
enum Src {
    Bytes(Vec<u8>),
    Zeros(u64),
}

impl Src {
    fn len(&self) -> u64 {
        match self {
            Src::Bytes(v) => v.len() as u64,
            Src::Zeros(n) => *n,
        }
    }
    fn chunk<'a>(&'a self, off: u64, n: usize, zeros: &'a [u8]) -> &'a [u8] {
        match self {
            Src::Bytes(v) => &v[off as usize..off as usize + n],
            Src::Zeros(_) => &zeros[..n],
        }
    }
}

struct Step {
    what: String,
    extents: Vec<(u64, Src)>,
    /// (extent index, offset within extent, length) of chunks that differ on the device
    dirty: Vec<(usize, u64, usize)>,
}

impl Step {
    fn total(&self) -> u64 {
        self.extents.iter().map(|e| e.1.len()).sum()
    }
    fn dirty_bytes(&self) -> u64 {
        self.dirty.iter().map(|d| d.2 as u64).sum()
    }
}

pub struct Plan {
    steps: Vec<Step>,
    pub snmac_new: SnMac,
    pub identity_generated: bool,
    pub notes: Vec<String>,
    pub device_firmware: Option<(String, String)>,
}

/// Every place an ext2/3/4 superblock (primary or backup) can live, for every
/// block size 1..64 KiB: the first MiB, plus 4 KiB at the start of each block
/// group (group g starts at g * 8*bs*bs; with 1 KiB blocks the superblock sits
/// 1 KiB into it). e2fsck walks backup groups 1,3,5,7,9,25,... when the primary
/// is gone, so zeroing only group 1 is NOT enough (tested: it resurrected data).
/// Covers sparse_super, sparse_super2 and non-sparse layouts alike.
pub fn ext_quick_wipe_ranges(part_size: u64) -> Vec<(u64, u64)> {
    let mut starts = std::collections::BTreeSet::new();
    for bs in [1024u64, 2048, 4096, 8192, 16384, 32768, 65536] {
        let group = 8 * bs * bs;
        let sb_in_group = if bs == 1024 { 1024 } else { 0 };
        let mut g = 1;
        while g * group + sb_in_group < part_size {
            starts.insert((g * group + sb_in_group) & !0xfff);
            g += 1;
        }
    }
    let mut v = vec![(0, MIB.min(part_size))];
    for s in starts {
        if s >= MIB {
            v.push((s, 4096.min(part_size - s)));
        }
    }
    v
}

/// Firmware board/version of the squashfs currently in `rootfs` on the device.
fn device_firmware(t: &Target, layout: &Gpt) -> Option<(String, String)> {
    firmware_in(t, layout, "rootfs")
}

/// (ota_board_name, version) of the squashfs in partition `part` ("rootfs" or "rootfs2").
pub fn firmware_in(t: &Target, layout: &Gpt, part: &str) -> Option<(String, String)> {
    let p = layout.find(part)?;
    t.drop_cache(p.offset(), 4096);
    let sb = t.read(p.offset(), 4096).ok()?;
    if &sb[..4] != b"hsqs" {
        return None;
    }
    let used = u64::from_le_bytes(sb[40..48].try_into().ok()?);
    if used == 0 || used > p.size() {
        return None;
    }
    t.drop_cache(p.offset(), used);
    let img = t.read(p.offset(), used as usize).ok()?;
    match ota_info(&img) {
        (Some(b), Some(v)) => Some((b, v)),
        _ => None,
    }
}

fn boot_looks_valid(t: &Target) -> bool {
    let Ok(b) = t.read(0, XBOOT_END as usize) else { return false };
    Gpt::parse(&b).is_ok()
        && b[SPL_OFFSET..SPL_OFFSET + 64].iter().any(|&x| x != 0)
        && b[SPL_OFFSET..].windows(6).any(|w| w == b"U-Boot")
}

pub fn plan(t: &Target, pkg: &Package, boot_override: Option<Vec<u8>>, o: &Options) -> Result<Plan> {
    let mut steps: Vec<Step> = vec![];
    let mut notes = vec![];
    let mut add = |what: String, extents: Vec<(u64, Src)>| steps.push(Step { what, extents, dirty: vec![] });

    // --- boot area / layout
    let existing = t.read(0, GPT_AREA).ok().and_then(|b| Gpt::parse(&b).ok());
    let boot_valid = boot_looks_valid(t);
    let boot = boot_override.or_else(|| pkg.boot.clone());
    let keep_boot = o.repair && boot_valid && !o.force_boot;
    let layout = match (&boot, &existing) {
        (Some(b), ex) if !keep_boot => {
            let bg = check_boot(b)?;
            match ex {
                Some(eg) if eg.same_layout(&bg) && !o.factory_guids => {
                    add("boot area (SPL+U-Boot; GPT kept)".into(), vec![(SPL_OFFSET as u64, Src::Bytes(b[SPL_OFFSET..].to_vec()))]);
                }
                Some(eg) if !eg.same_layout(&bg) => {
                    bail!("target GPT layout differs from the package's; refusing to repartition a non-blank device");
                }
                _ => {
                    let mut nb = b.clone();
                    if !o.factory_guids {
                        bg.randomize_guids(&mut nb)?;
                        Gpt::parse(&nb).context("re-parsing patched GPT")?;
                    }
                    let what = if o.factory_guids { "boot area (MBR+GPT+SPL+U-Boot)" } else { "boot area (MBR+GPT with fresh GUIDs+SPL+U-Boot)" };
                    add(what.into(), vec![(0, Src::Bytes(nb))]);
                }
            }
            if (b.len() as u64) < XBOOT_END {
                add("rest of boot region".into(), vec![(b.len() as u64, Src::Zeros(XBOOT_END - b.len() as u64))]);
            }
            bg
        }
        (_, Some(eg)) if boot_valid => {
            notes.push(if keep_boot {
                "boot area: existing SPL/U-Boot looks valid, kept (--force-boot to rewrite)".into()
            } else {
                "boot area: keeping the target's existing SPL/U-Boot (package has none)".into()
            });
            if let Some(b) = &boot {
                if !eg.same_layout(&check_boot(b)?) {
                    bail!("target GPT layout differs from the package's");
                }
            }
            eg.clone()
        }
        _ => bail!(
            "this board has no working bootloader or partition table (a blank or erased board), and an update \
             file (.img) or unpacked update does not contain them. Install a Creality .ingenic package for any \
             Nebula-family printer first: it includes both (command line: --boot <.ingenic>)"
        ),
    };
    if t.size < layout.required_size() {
        bail!("target is {} bytes, layout needs {}", t.size, layout.required_size());
    }
    let part = |n: &str| layout.find(n).cloned().ok_or_else(|| anyhow!("layout lacks {n}"));

    let dev_fw = device_firmware(t, &layout);

    // --- firmware (always offered; unchanged chunks are skipped)
    for (name, data) in [("rtos", &pkg.rtos), ("kernel", &pkg.kernel), ("rootfs", &pkg.rootfs)] {
        let p = part(name)?;
        if data.len() as u64 > p.size() {
            bail!("{name} image ({} B) larger than partition ({} B)", data.len(), p.size());
        }
        let mut ext = vec![(p.offset(), Src::Bytes(data.clone()))];
        if name != "rootfs" {
            ext.push((p.offset() + data.len() as u64, Src::Zeros(p.size() - data.len() as u64)));
        }
        add(format!("{name} ({:.1} MiB image)", data.len() as f64 / MIB as f64), ext);
    }

    // --- boot slot selection
    let ota = part("ota")?;
    if o.repair {
        let head = t.read(ota.offset(), 512)?;
        let slot_a = head.iter().all(|&x| x == 0) || head.starts_with(b"ota:kernel\n");
        if !slot_a {
            let txt = String::from_utf8_lossy(&head[..head.iter().position(|&c| c == 0).unwrap_or(32).min(32)]).into_owned();
            notes.push(format!("ota partition says {txt:?}; resetting so U-Boot boots slot A (what is being installed)"));
            add("ota (reset to slot A)".into(), vec![(ota.offset(), Src::Zeros(ota.size()))]);
        }
    } else {
        add("ota".into(), vec![(ota.offset(), Src::Zeros(ota.size()))]);
    }

    // --- factory-equivalent erase of the B slots (full mode only)
    if !o.repair {
        for n in ["rtos2", "kernel2"] {
            let p = part(n)?;
            add(n.into(), vec![(p.offset(), Src::Zeros(p.size()))]);
        }
        let p = part("rootfs2")?;
        add("rootfs2 (squashfs superblock)".into(), vec![(p.offset(), Src::Zeros(MIB))]);
    }

    // --- data filesystems
    let same_fw = match (&dev_fw, &pkg.ota_board, &pkg.ota_version) {
        (Some((db, dv)), Some(pb), Some(pv)) => db == pb && dv == pv,
        _ => false,
    };
    let wipe_overlay = if o.wipe_overlay {
        true
    } else if o.keep_overlay {
        notes.push("overlay (rootfs_data) kept (--keep-overlay)".into());
        false
    } else if o.repair && same_fw {
        notes.push("overlay (rootfs_data) kept: device already runs this firmware version".into());
        false
    } else {
        if o.repair {
            notes.push(format!(
                "overlay (rootfs_data) will be wiped: device firmware {} differs from the package's or is unreadable (--keep-overlay to keep)",
                dev_fw.as_ref().map(|(b, v)| format!("{b} {v}")).unwrap_or_else(|| "unknown".into())
            ));
        }
        true
    };
    let wipe_userdata = !(o.keep_userdata || o.repair);
    if !wipe_userdata {
        notes.push(if o.repair { "userdata kept (repair mode)".into() } else { "userdata kept (--keep-userdata)".into() });
    }
    for (n, wipe) in [("rootfs_data", wipe_overlay), ("userdata", wipe_userdata)] {
        if !wipe {
            continue;
        }
        let p = part(n)?;
        match o.wipe {
            Wipe::Full => add(format!("{n} (full wipe)"), vec![(p.offset(), Src::Zeros(p.size()))]),
            Wipe::Quick => {
                let r = ext_quick_wipe_ranges(p.size());
                let k = r.len() - 1;
                add(
                    format!("{n} (quick wipe: first MiB + {k} superblock slots)"),
                    r.into_iter().map(|(off, len)| (p.offset() + off, Src::Zeros(len))).collect(),
                );
            }
        }
    }

    // --- identity
    let snp = part("sn_mac")?;
    let current = SnMac::parse(&t.read(snp.offset(), snmac::RECORD_AREA)?);
    let pkg_model = pkg.ota_board.as_deref().and_then(snmac::model_for_ota_board);
    let cur_model = current.as_ref().map(|c| c.model().to_string()).filter(|m| !m.is_empty());
    // Firmware for another mainboard would leave this board unable to start. Only the Nebula
    // family is known so far; anything else is refused unless explicitly forced.
    if pkg_model.is_none() && !o.force_model {
        match pkg.ota_board.as_deref() {
            Some(b) => bail!(
                "this firmware is for a different mainboard ({}), not for a Nebula-family board. Installing it \
                 would leave this board unable to start",
                snmac::foreign_board_name(b)
            ),
            None => bail!(
                "cannot tell which printer this firmware is for (no readable etc/ota_info in its root filesystem); \
                 refusing to install it"
            ),
        }
    }
    if o.repair && o.model.is_none() {
        if let (Some(cm), Some(pm)) = (&cur_model, pkg_model) {
            if cm != pm && !o.force_model {
                bail!(
                    "device is a {cm:?} ({}) but the firmware is for {pm:?} ({}): that is a conversion, not a repair. \
                     Run without --repair (and with --model if intended)",
                    snmac::describe_model(cm),
                    snmac::describe_model(pm)
                );
            }
        }
    }
    let model = match (&o.model, pkg_model, &cur_model) {
        (Some(m), _, _) => m.clone(),
        (None, Some(m), _) => m.to_string(),
        (None, None, Some(c)) => c.clone(),
        _ => bail!("cannot tell the model (firmware's ota_board_name unknown); pass --model"),
    };
    if let (Some(pm), false) = (pkg_model, o.force_model) {
        if pm != model {
            bail!(
                "firmware is for model {pm:?} ({}) but model {model:?} was requested; the printer would get \
                 no Klipper config. Use --force-model to override",
                snmac::describe_model(pm)
            );
        }
    }
    let mut rec = current.clone().unwrap_or_default();
    let mut generated = false;
    let keep_identity = !o.new_identity && current.as_ref().is_some_and(|c| c.identity_valid());
    match (&o.sn, &o.mac) {
        (Some(sn), Some(mac)) => {
            rec.fields[0] = sn.to_ascii_uppercase();
            rec.fields[1] = mac.to_ascii_uppercase();
        }
        (Some(_), None) | (None, Some(_)) => bail!("give both --sn and --mac, or neither"),
        (None, None) if keep_identity => {}
        (None, None) => {
            let mac = snmac::generate_mac()?;
            rec.fields[0] = snmac::generate_sn(&mac, o.sn_prefix.as_deref())?;
            rec.fields[1] = mac;
            generated = true;
        }
    }
    rec.fields[2] = model;
    rec.fields[3] = o.board.clone().unwrap_or_else(|| {
        current.as_ref().map(|c| c.board().to_string()).filter(|b| !b.is_empty()).unwrap_or_else(|| NEBULA_BOARD.into())
    });
    if rec.fields[4] == "1" {
        notes.push("PCBA factory-test flag was set; clearing it".into());
        rec.fields[4].clear();
    }
    rec.validate_for_write()?;
    if current.as_ref() != Some(&rec) {
        let mut area = rec.to_area()?;
        area.resize(4096, 0);
        add("sn_mac record".into(), vec![(snp.offset(), Src::Bytes(area))]);
    }
    notes.push(match &current {
        Some(c) => format!("sn_mac before: {c}"),
        None => "sn_mac before: blank".into(),
    });

    // --- compare everything against the device (bypassing the page cache)
    let zeros = vec![0u8; CH as usize];
    let total: u64 = steps.iter().map(|s| s.total()).sum();
    let mut seen = 0u64;
    for st in steps.iter_mut() {
        for (ei, (off, src)) in st.extents.iter().enumerate() {
            let len = src.len();
            let mut c = 0u64;
            while c < len {
                let n = CH.min(len - c) as usize;
                if !t.matches(off + c, src.chunk(c, n, &zeros))? {
                    st.dirty.push((ei, c, n));
                }
                c += n as u64;
                seen += n as u64;
                if total > 32 * MIB && seen % (16 * MIB) < CH {
                    events::progress("comparing with device", seen, total, "");
                }
            }
        }
    }
    if total > 32 * MIB {
        events::progress("comparing with device", total, total, "");
        events::progress_end();
    }
    if o.rewrite {
        let changed: u64 = steps.iter().map(|s| s.dirty_bytes()).sum();
        notes.push(format!(
            "--rewrite: writing every planned chunk ({} MiB actually differed)",
            (changed + MIB - 1) >> 20
        ));
        for st in steps.iter_mut() {
            st.dirty.clear();
            for (ei, (_, src)) in st.extents.iter().enumerate() {
                let len = src.len();
                let mut c = 0;
                while c < len {
                    let n = CH.min(len - c) as usize;
                    st.dirty.push((ei, c, n));
                    c += n as u64;
                }
            }
        }
    }
    Ok(Plan { steps, snmac_new: rec, identity_generated: generated, notes, device_firmware: dev_fw })
}

impl Plan {
    pub fn describe(&self) -> String {
        let mut s = String::new();
        for st in &self.steps {
            let (tot, d) = (st.total(), st.dirty_bytes());
            let first = st.extents.first().map(|e| e.0).unwrap_or(0);
            if d == 0 {
                s += &format!("  ok    {:>10} B @ {:#011x}  {} (unchanged)\n", tot, first, st.what);
            } else if d == tot {
                s += &format!("  WRITE {:>10} B @ {:#011x}  {}\n", d, first, st.what);
            } else {
                s += &format!("  WRITE {:>10} B @ {:#011x}  {} ({} of {} MiB differ)\n", d, first, st.what, (d + MIB - 1) >> 20, (tot + MIB - 1) >> 20);
            }
        }
        s
    }

    /// Bytes that will actually be written.
    pub fn bytes(&self) -> u64 {
        self.steps.iter().map(|s| s.dirty_bytes()).sum()
    }

    pub fn execute(&self, t: &Target) -> Result<()> {
        let zeros = vec![0u8; CH as usize];
        for st in self.steps.iter().filter(|s| !s.dirty.is_empty()) {
            crate::say!("writing {} ({} MiB)", st.what, (st.dirty_bytes() + MIB - 1) >> 20);
            // consecutive dirty chunks go out as one write (up to 64 MiB)
            let mut i = 0;
            while i < st.dirty.len() {
                let (ei, c0, n0) = st.dirty[i];
                let (off, src) = &st.extents[ei];
                let mut buf = src.chunk(c0, n0, &zeros).to_vec();
                let mut j = i + 1;
                while j < st.dirty.len() && buf.len() < 64 * MIB as usize {
                    let (ej, cj, nj) = st.dirty[j];
                    if ej != ei || cj != c0 + buf.len() as u64 {
                        break;
                    }
                    buf.extend_from_slice(src.chunk(cj, nj, &zeros));
                    j += 1;
                }
                t.write_raw(off + c0, &buf, &st.what)?;
                i = j;
                if st.dirty.len() > 16 {
                    events::progress(&st.what, (i as u64) << 20, (st.dirty.len() as u64) << 20, "");
                }
            }
            if st.dirty.len() > 16 {
                events::progress_end();
            }
            t.sync()?;
            for &(ei, c, n) in &st.dirty {
                let (off, src) = &st.extents[ei];
                if !t.matches(off + c, src.chunk(c, n, &zeros))? {
                    bail!("verify FAILED for {} at {:#x}", st.what, off + c);
                }
            }
        }
        t.sync()?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn quick_wipe_covers_superblock_backups() {
        let size = 6 << 30;
        let r = ext_quick_wipe_ranges(size);
        assert_eq!(r[0], (0, MIB));
        let covered = |b: u64| r.iter().any(|&(o, l)| b >= o && b + 1024 <= o + l);
        // superblock of every group, every block size, incl. sparse_super groups 3,5,7,9,25,...
        for bs in [1024u64, 2048, 4096, 8192, 16384, 32768, 65536] {
            let group = 8 * bs * bs;
            let mut g = 1;
            while g * group < size {
                let sb = g * group + if bs == 1024 { 1024 } else { 0 };
                assert!(covered(sb), "bs {bs} group {g} @ {sb:#x}");
                g += 1;
            }
        }
        assert!(r.iter().all(|&(o, l)| o + l <= size));
        assert!(r.iter().map(|x| x.1).sum::<u64>() < 8 * MIB, "stays quick");
    }
}
