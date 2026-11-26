//! Device orchestration shared by the front ends: find out what state the board is in, boot
//! the stage that is needed (fastboot stage2, recovery Linux, or the U-Boot disk), and open
//! the eMMC. Everything is reported through `events`.

use crate::bootrom::{BootOptions, Bootrom, SocProfile, SOCS};
use crate::{fastboot, image, restore, say, usb};
use anyhow::{bail, Context, Result};
use std::path::PathBuf;
use std::time::Duration;

#[derive(Clone, Copy, PartialEq, clap::ValueEnum)]
pub enum Via {
    Auto,
    Linux,
    Uboot,
}

#[derive(Clone, Copy, clap::ValueEnum)]
pub enum UmsMode {
    Ro,
    Rw,
}

pub fn soc_by_name(n: &str) -> Result<&'static SocProfile> {
    SOCS.iter().find(|s| s.name == n).with_context(|| {
        format!("unknown SoC {n:?}; known: {}", SOCS.iter().map(|s| s.name).collect::<Vec<_>>().join(", "))
    })
}

pub fn read(p: &PathBuf) -> Result<Vec<u8>> {
    std::fs::read(p).with_context(|| format!("reading {}", p.display()))
}

/// Prints the checks; returns the number of errors.
pub fn check_images(soc: &SocProfile, spl: Option<&[u8]>, stage2: Option<&[u8]>) -> usize {
    let mut errors = 0;
    if let Some(b) = spl {
        let s = image::inspect_stage1(b, soc.s1_max);
        say!("stage1: {} bytes, SPL code {} bytes", b.len(), s.code_len);
        if let Some(d) = &s.ddr {
            say!(
                "  DDR {} {} @ {} MHz ({:?} header, type {}), UART{} @ {}",
                d.name,
                d.type_name,
                d.freq / 1_000_000,
                d.gen,
                d.type_raw,
                s.uart_idx,
                s.baud
            );
        }
        for w in &s.warnings {
            say!("  warning: {w}");
        }
        for e in &s.errors {
            say!("  error: {e}");
        }
        errors += s.errors.len();
    }
    if let Some(b) = stage2 {
        let (kind, warns, errs) = image::inspect_stage2(b);
        say!("stage2: {} bytes, {kind}", b.len());
        for w in &warns {
            say!("  warning: {w}");
        }
        for e in &errs {
            say!("  error: {e}");
        }
        errors += errs.len();
    }
    errors
}

/// Load stage1+stage2 into the board in USB boot mode; returns the stage2 USB device.
pub fn boot_board(soc: &SocProfile, s1: &[u8], s2: &[u8], device: Option<&str>, spl_timeout: f32, wait: f32, force: bool) -> Result<Option<usb::Found>> {
    let n = check_images(soc, Some(s1), Some(s2));
    if n > 0 && !force {
        bail!("{n} image error(s); not sending anything (use --force to override)");
    }
    let f = usb::select(soc, device)?;
    say!("device {} (port {})", f.id(), f.port_path());
    let mut b = Bootrom::new(usb::open(&f)?);
    b.log = Box::new(move |m| say!("{m}"));
    let info = b.cpu_info().context("bootrom did not answer GET_CPU_INFO")?;
    say!("bootrom cpu info: {info:?}");
    let opts = BootOptions { spl_timeout: Duration::from_secs_f32(spl_timeout), ..Default::default() };
    b.boot(soc, s1, s2, &opts)?;
    drop(b); // release the interface before the device re-enumerates
    if wait <= 0.0 {
        say!("stage2 started");
        return Ok(None);
    }
    say!("waiting up to {wait:.0}s for stage2 to enumerate...");
    match usb::wait_for_stage2(&f, SOCS, Duration::from_secs_f32(wait))? {
        Some(n) => {
            say!("stage2 up: {} {:04x}:{:04x} {}", n.id(), n.vid, n.pid, n.what);
            Ok(Some(n))
        }
        None => bail!(
            "stage2 did not enumerate within {wait:.0}s. Check the UART log; the board stays in \
             RAM-only state, power-cycle to recover"
        ),
    }
}

/// Where the built images live.
pub fn images_dir(opt: Option<&PathBuf>) -> Result<PathBuf> {
    let mut cands: Vec<PathBuf> = vec![];
    if let Some(p) = opt {
        cands.push(p.clone());
    }
    if let Ok(p) = std::env::var("NOCRY_IMAGES") {
        cands.push(p.into());
    }
    if let Ok(exe) = std::env::current_exe().and_then(|e| e.canonicalize()) {
        if let Some(d) = exe.parent() {
            cands.push(d.join("images"));
            cands.push(d.join("../Resources/images")); // macOS app bundle
            // host/target/{release,debug}/nocry -> project/tools/burner-build/out
            if let Some(proj) = d.parent().and_then(|p| p.parent()).and_then(|p| p.parent()) {
                cands.push(proj.join("tools/burner-build/out"));
            }
        }
    }
    if let Ok(cwd) = std::env::current_dir() {
        cands.push(cwd.join("tools/burner-build/out"));
    }
    let explicit = opt.is_some();
    for c in &cands {
        if c.join("spl-x2000e/spl-x2000e.bin").is_file() {
            return Ok(c.clone());
        }
        if explicit {
            bail!("{} does not contain spl-x2000e/spl-x2000e.bin", c.display());
        }
    }
    bail!(
        "cannot find the built images (looked in: {}). Build them with tools/burner-build/build.sh \
         or pass --images DIR",
        cands.iter().map(|c| c.display().to_string()).collect::<Vec<_>>().join(", ")
    )
}

#[derive(PartialEq, Clone, Copy)]
pub enum Want {
    /// read-only stage2 requested explicitly
    Ro,
    /// writes may follow (dry runs): boot read-write, accept a running read-only one
    PreferRw,
    /// writes will happen now
    Rw,
}

/// Find a running nocry-ums disk, or boot one if a board is in USB boot mode.
pub fn ensure_ums(soc: &SocProfile, want: Want, images: Option<&PathBuf>, device: Option<&str>, via: Via) -> Result<PathBuf> {
    let fastboot_up = !usb::running_fastboot(SOCS)?.is_empty();
    if fastboot_up && via == Via::Uboot {
        bail!(
            "the board is running nocry fastboot, not the USB disk stage2. BOOT+RESET into USB boot mode \
             and run this again (or drop --via uboot to use the recovery Linux)"
        );
    }
    let running = usb::running_ums(SOCS)?;
    if running.is_empty() && !usb::running_recovery(SOCS)?.is_empty() {
        bail!(
            "the board is running the nocry recovery Linux without a USB disk. Use adb, or BOOT+RESET and \
             `nocry recovery --ums rw` (or --ums ro)"
        );
    }
    if running.len() > 1 {
        bail!(
            "several nocry-ums disks attached ({}); pass --target",
            running.iter().map(|d| d.dev.display().to_string()).collect::<Vec<_>>().join(", ")
        );
    }
    if let Some(d) = running.into_iter().next() {
        let kind = if d.read_only { "read-only" } else { "read-write" };
        let src = if d.recovery { "nocry recovery disk" } else { "nocry-ums stage2" };
        say!("using running {src} ({kind}): {} ({:.2} GiB)", d.dev.display(), d.bytes as f64 / (1u64 << 30) as f64);
        if d.read_only && want == Want::Rw && d.recovery {
            bail!(
                "the recovery's disk is read-only. BOOT+RESET the board and run `nocry recovery --ums rw`, \
                 then this again"
            );
        }
        if d.read_only && want == Want::Rw {
            bail!(
                "the running stage2 is read-only and cannot switch modes. Power-cycle the board into USB boot \
                 mode (hold BOOT, tap RESET, release BOOT) and run this again; nocry will boot the read-write stage2"
            );
        }
        if d.read_only && want == Want::PreferRw {
            say!("  note: read-only stage2; writing (--yes) will need a power-cycle into USB boot mode first");
        }
        if !d.read_only && want == Want::Ro {
            say!("  note: a read-write stage2 is already running; using it");
        }
        usb::wait_accessible(&d, Duration::from_secs(10))?;
        return Ok(d.dev);
    }
    // Nothing running yet: prefer the recovery Linux (HS200 8-bit eMMC, ~40 MB/s), fall back to
    // the U-Boot disk stage2 (~11 MB/s). The choice is made before booting: once a stage2 runs,
    // the bootrom is gone and switching needs BOOT+RESET.
    let rimg = recovery_image(None).ok();
    let linux = match via {
        Via::Uboot => false,
        Via::Linux => true,
        Via::Auto => rimg.is_some() || fastboot_up,
    };
    if linux {
        let mode = if want == Want::Ro { UmsMode::Ro } else { UmsMode::Rw };
        if rimg.is_none() {
            bail!("no recovery image (build tools/recovery-build/build.sh), or use --via uboot");
        }
        say!(
            "using the recovery Linux ({}; --via uboot for the U-Boot disk)",
            if want == Want::Ro { "read-only disk" } else { "read-write disk" }
        );
        let fail_hint = "the recovery Linux path failed. BOOT+RESET the board and run this again with --via uboot";
        let r = start_recovery(soc, images, device, None, Some(mode), None, 60.0).context(fail_hint)?;
        let d = usb::block_device_for(&r, Duration::from_secs(20)).context(fail_hint)?;
        usb::wait_accessible(&d, Duration::from_secs(10))?;
        if want == Want::Ro && !d.read_only {
            bail!("{} should be read-only but is not; not using it", d.dev.display());
        }
        say!(
            "eMMC exported as {} ({:.2} GiB, {})",
            d.dev.display(),
            d.bytes as f64 / (1u64 << 30) as f64,
            if d.read_only { "read-only" } else { "read-write" }
        );
        return Ok(d.dev);
    }
    if via == Via::Auto {
        say!("note: no recovery image built; using the slower U-Boot disk (tools/recovery-build/build.sh)");
    }
    let dir = images_dir(images)?;
    let s2name = if want == Want::Ro { "nocry_ums_ro_x2000_v12" } else { "nocry_ums_x2000_v12" };
    let s1p = dir.join("spl-x2000e/spl-x2000e.bin");
    let s2p = dir.join(s2name).join("u-boot.bin");
    if usb::select(soc, device).is_err() {
        bail!(
            "no board found: neither a nocry-ums disk nor a board in USB boot mode. Hold BOOT, tap RESET, \
             release BOOT (data-capable cable), then run this again"
        );
    }
    say!("board in USB boot mode: booting {} + {}", s1p.display(), s2p.display());
    let s1 = read(&s1p)?;
    let s2 = read(&s2p)?;
    let n = boot_board(soc, &s1, &s2, device, 5.0, 20.0, false)?.context("stage2 did not come up")?;
    let d = usb::block_device_for(&n, Duration::from_secs(15))
        .context("stage2 is up but no block device appeared (check `lsblk` / dmesg)")?;
    usb::wait_accessible(&d, Duration::from_secs(10))?;
    say!("eMMC exported as {} ({:.2} GiB, {})", d.dev.display(), d.bytes as f64 / (1u64 << 30) as f64, if d.read_only { "read-only" } else { "read-write" });
    Ok(d.dev)
}

/// A running nocry fastboot stage2, booting one if the board is in USB boot mode.
pub fn ensure_fastboot(soc: &SocProfile, images: Option<&PathBuf>, device: Option<&str>) -> Result<usb::Found> {
    if let Some(f) = usb::running_fastboot(SOCS)?.into_iter().next() {
        say!("nocry fastboot already running: {} (port {})", f.id(), f.port_path());
        return Ok(f);
    }
    if !usb::running_recovery(SOCS)?.is_empty() {
        bail!("the board is running the nocry recovery Linux; BOOT+RESET it into USB boot mode first");
    }
    if !usb::running_ums(SOCS)?.is_empty() {
        bail!("a nocry-ums stage2 is running; BOOT+RESET the board into USB boot mode first");
    }
    let dir = images_dir(images)?;
    let s1p = dir.join("spl-x2000e/spl-x2000e.bin");
    let s2p = dir.join("nocry_fb_x2000_v12/u-boot.bin");
    if usb::select(soc, device).is_err() {
        bail!("no board found in USB boot mode. Hold BOOT, tap RESET, release BOOT (data-capable cable), then run this again");
    }
    say!("board in USB boot mode: booting {} + {}", s1p.display(), s2p.display());
    let s1 = read(&s1p)?;
    let s2 = read(&s2p)?;
    let n = boot_board(soc, &s1, &s2, device, 5.0, 20.0, false)?.context("stage2 did not come up")?;
    if n.pid != usb::NOCRY_FB_PID {
        bail!("unexpected stage2 {:04x}:{:04x}", n.vid, n.pid);
    }
    Ok(n)
}

/// Where the recovery image lives.
pub fn recovery_image(opt: Option<&PathBuf>) -> Result<PathBuf> {
    if let Some(p) = opt {
        return Ok(p.clone());
    }
    let mut cands: Vec<PathBuf> = vec![];
    if let Ok(p) = std::env::var("NOCRY_RECOVERY") {
        cands.push(p.into());
    }
    if let Ok(exe) = std::env::current_exe().and_then(|e| e.canonicalize()) {
        if let Some(d) = exe.parent() {
            cands.push(d.join("images/nocry-recovery.uImage"));
            cands.push(d.join("../Resources/images/nocry-recovery.uImage")); // macOS app bundle
            if let Some(proj) = d.parent().and_then(|p| p.parent()).and_then(|p| p.parent()) {
                cands.push(proj.join("tools/recovery-build/out/nocry-recovery.uImage"));
            }
        }
    }
    cands.iter().find(|c| c.is_file()).cloned().with_context(|| {
        format!(
            "no recovery image found (looked in: {}); build it with tools/recovery-build/build.sh or pass --image",
            cands.iter().map(|c| c.display().to_string()).collect::<Vec<_>>().join(", ")
        )
    })
}

/// Boots the recovery Linux (via a running or freshly booted fastboot stage2) unless it is
/// already running; returns its USB device.
pub fn start_recovery(
    soc: &SocProfile,
    images: Option<&PathBuf>,
    device: Option<&str>,
    image: Option<&PathBuf>,
    ums: Option<UmsMode>,
    cmdline: Option<String>,
    wait: f32,
) -> Result<usb::Found> {
    if let Some(r) = usb::running_recovery(SOCS)?.into_iter().next() {
        say!("nocry recovery already running: {} (port {})", r.id(), r.port_path());
        if ums.is_some() && usb::block_device_for(&r, Duration::from_secs(1)).is_none() {
            say!("  note: it has no USB disk; BOOT+RESET and run `nocry recovery --ums ...` again");
        }
    } else {
        let path = recovery_image(image)?;
        let img = read(&path)?;
        if img.len() < 64 || u32::from_be_bytes(img[..4].try_into().unwrap()) != fastboot::UIMAGE_MAGIC {
            bail!("{} is not a uImage", path.display());
        }
        let mut args: Vec<String> = cmdline.into_iter().collect();
        match ums {
            Some(UmsMode::Ro) => args.push("nocry.ums=ro".into()),
            Some(UmsMode::Rw) => args.push("nocry.ums=rw".into()),
            None => {}
        }
        let payload = if args.is_empty() { img } else { fastboot::bootimg_v0(&img, &args.join(" "))? };
        let f = ensure_fastboot(soc, images, device)?;
        say!(
            "sending {} ({:.1} MiB){}",
            path.display(),
            payload.len() as f64 / (1u64 << 20) as f64,
            if args.is_empty() { String::new() } else { format!(", cmdline \"{}\"", args.join(" ")) }
        );
        let mut fb = fastboot::Fastboot::open(&f)?;
        let v = fb.getvar("version-bootloader").unwrap_or_default();
        if !v.starts_with("nocry-fastboot") {
            bail!("{} is not the nocry fastboot stage2 (version-bootloader {v:?})", f.id());
        }
        let max = fb.getvar("max-download-size").ok().and_then(|m| u64::from_str_radix(m.trim_start_matches("0x"), 16).ok());
        if let Some(max) = max {
            if payload.len() as u64 > max {
                bail!("recovery image is {} bytes; the stage2 accepts at most {max}", payload.len());
            }
        }
        let t0 = std::time::Instant::now();
        fb.download(&payload)?;
        say!("downloaded in {:.1}s; booting (RAM only, the eMMC is not written)", t0.elapsed().as_secs_f32());
        fb.boot()?;
        drop(fb);
        let t0 = std::time::Instant::now();
        loop {
            if let Some(r) = usb::running_recovery(SOCS)?.into_iter().next() {
                say!("recovery up after {:.0}s: {} (port {})", t0.elapsed().as_secs_f32(), r.id(), r.port_path());
                break;
            }
            if t0.elapsed().as_secs_f32() > wait {
                bail!(
                    "the recovery did not enumerate within {wait:.0}s. BOOT+RESET (do not power-cycle), then \
                     `nocry fastboot && fastboot oem dmesg` shows its kernel log"
                );
            }
            std::thread::sleep(Duration::from_millis(250));
        }
    }
    usb::running_recovery(SOCS)?.into_iter().next().context("recovery vanished")
}

/// Opens the eMMC for a command: an explicit --target; else the recovery's fastboot service
/// (no disk device, on-board hashing); else a nocry USB disk (U-Boot fallback).
#[allow(clippy::too_many_arguments)]
pub fn open_target(
    soc: &SocProfile,
    explicit: Option<&PathBuf>,
    want: Want,
    write: bool,
    allow_any: bool,
    images: Option<&PathBuf>,
    device: Option<&str>,
    via: Via,
) -> Result<restore::Target> {
    if let Some(p) = explicit {
        return restore::Target::open(p, write, allow_any);
    }
    if via != Via::Uboot {
        let mut rec = usb::running_recovery(SOCS)?.into_iter().next();
        if rec.is_none()
            && usb::running_ums(SOCS)?.is_empty()
            && (recovery_image(None).is_ok() || !usb::running_fastboot(SOCS)?.is_empty())
        {
            say!("using the recovery Linux (--via uboot for the U-Boot disk)");
            rec = Some(
                start_recovery(soc, images, device, None, None, None, 60.0)
                    .context("the recovery Linux path failed. BOOT+RESET the board and run this again with --via uboot")?,
            );
        }
        if let Some(r) = rec {
            let t = restore::Target::open_fastboot(&r, write)?;
            say!("using the recovery's fastboot service: {} ({:.2} GiB)", t.path.display(), t.size as f64 / (1u64 << 30) as f64);
            return Ok(t);
        }
    }
    let p = ensure_ums(soc, want, images, device, via)?;
    restore::Target::open(&p, write, allow_any)
}

/// What is attached right now (cheap: a USB scan, nothing is sent to the board).
#[derive(Debug, Clone, PartialEq)]
pub enum BoardKind {
    None,
    /// SoC bootrom, waiting for stage1/stage2
    UsbBoot,
    /// nocry fastboot stage2 (U-Boot)
    Fastboot,
    /// nocry recovery Linux (fastboot service, adb, console)
    Recovery,
    /// nocry U-Boot USB-disk stage2
    UbootDisk,
}

#[derive(Debug, Clone)]
pub struct BoardState {
    pub kind: BoardKind,
    pub port: String,
    pub count: usize,
}

pub fn board_state() -> Result<BoardState> {
    let all = usb::scan(SOCS)?;
    let rank = |f: &usb::Found| {
        if f.vid == usb::NOCRY_UMS_VID && f.pid == usb::NOCRY_RECOVERY_PID {
            (0, BoardKind::Recovery)
        } else if f.vid == usb::NOCRY_UMS_VID && f.pid == usb::NOCRY_FB_PID {
            (1, BoardKind::Fastboot)
        } else if f.vid == usb::NOCRY_UMS_VID && f.pid == usb::NOCRY_UMS_PID {
            (2, BoardKind::UbootDisk)
        } else if SOCS.iter().any(|s| s.vid == f.vid && s.pid == f.pid) {
            (3, BoardKind::UsbBoot)
        } else {
            (9, BoardKind::None)
        }
    };
    let best = all.iter().map(|f| (rank(f), f)).filter(|((r, _), _)| *r < 9).min_by_key(|((r, _), _)| *r);
    Ok(match best {
        Some(((_, kind), f)) => BoardState { kind, port: f.port_path(), count: all.len() },
        None => BoardState { kind: BoardKind::None, port: String::new(), count: 0 },
    })
}

/// What a front end shows about a board. Reading it boots the recovery if needed (RAM only).
#[derive(Debug, Clone, Default)]
pub struct DeviceInfo {
    pub access: String,
    pub emmc_bytes: u64,
    pub sn: String,
    pub mac: String,
    pub model: String,
    pub model_description: String,
    pub board: String,
    pub identity_valid: bool,
    /// "a", "b", or "" when there is no partition table
    pub active_slot: String,
    /// (name, offset, size)
    pub partitions: Vec<(String, u64, u64)>,
    /// what is installed in each firmware slot (None on a blank board)
    pub slots: Option<Slots>,
}

/// `target` overrides the device (an image file, for tests and screenshots).
pub fn device_info(soc: &SocProfile, via: Via, target: Option<&PathBuf>) -> Result<DeviceInfo> {
    let t = open_target(soc, target, Want::PreferRw, false, false, None, None, via)?;
    let mut d = DeviceInfo { access: t.path.display().to_string(), emmc_bytes: t.size, ..Default::default() };
    let Ok(g) = crate::gpt::Gpt::parse(&t.read(0, crate::gpt::GPT_AREA)?) else {
        say!("no valid partition table: this looks like a blank or erased board");
        return Ok(d);
    };
    d.partitions = g.parts.iter().map(|p| (p.name.clone(), p.offset(), p.size())).collect();
    d.slots = read_slots(&t).ok().map(|x| x.1);
    if let Some(p) = g.find("ota") {
        let b = t.read(p.offset(), 512)?;
        d.active_slot = if b.starts_with(b"ota:kernel2") { "b" } else { "a" }.into();
    }
    if let Some(p) = g.find("sn_mac") {
        if let Some(r) = crate::snmac::SnMac::parse(&t.read(p.offset(), crate::snmac::RECORD_AREA)?) {
            d.sn = r.sn().into();
            d.mac = r.mac().into();
            d.model = r.model().into();
            d.model_description = crate::snmac::describe_model(r.model()).into();
            d.board = r.board().into();
            d.identity_valid = r.identity_valid();
        }
    }
    Ok(d)
}

pub struct BackupResult {
    pub sha256: String,
    pub bytes: u64,
    pub seconds: f64,
}

/// Full-disk backup to `out` (+ `out.sha256`), verified. Boots the recovery if needed.
pub fn backup(soc: &SocProfile, via: Via, out: &std::path::Path, verify: bool, force: bool) -> Result<BackupResult> {
    let t0 = std::time::Instant::now();
    let t = open_target(soc, None, Want::Ro, false, true, None, None, via)?;
    let sha256 = crate::diskimage::backup(&t, out, verify, force)?;
    Ok(BackupResult { sha256, bytes: t.size, seconds: t0.elapsed().as_secs_f64() })
}

/// Result of planning (and optionally running) a firmware installation.
#[derive(Debug, Clone, Default)]
pub struct InstallReport {
    pub firmware_board: String,
    pub firmware_version: String,
    pub has_boot_area: bool,
    /// "BOARD VERSION" currently on the device, or "" if unreadable
    pub device_firmware: String,
    pub model_before: String,
    pub model_after: String,
    /// names for dialogs ("Retail Nebula Pad", "Ender-3 V3 KE"); "" when unknown/blank
    pub model_before_name: String,
    pub model_after_name: String,
    pub firmware_name: String,
    pub device_firmware_name: String,
    pub identity_generated: bool,
    pub notes: Vec<String>,
    /// one line per component: unchanged, or how much will be written
    pub details: String,
    pub bytes_to_write: u64,
    pub executed: bool,
    /// where the pre-write copy of the first 3 MiB went, if one was taken
    pub safety_copy: String,
}

/// Plan an installation of `os` (.ingenic, or a directory with xImage/rootfs.squashfs/zero.bin)
/// and, with `execute`, carry it out.
/// keep_data: minimal repair (identity, settings, files and the other slot stay);
/// otherwise a clean install: data partitions are erased, identity is kept.
/// `target` overrides the device (an image file, for tests).
pub fn install(
    soc: &SocProfile,
    via: Via,
    os: &std::path::Path,
    keep_data: bool,
    execute: bool,
    safety_dir: Option<&std::path::Path>,
    target: Option<&PathBuf>,
) -> Result<InstallReport> {
    let pkg = restore::Package::load(os)?;
    let mut r = InstallReport {
        firmware_board: pkg.ota_board.clone().unwrap_or_default(),
        firmware_version: pkg.ota_version.clone().unwrap_or_default(),
        has_boot_area: pkg.boot.is_some(),
        ..Default::default()
    };
    say!("firmware: {} {} from {}", r.firmware_board, r.firmware_version, pkg.source);
    let want = if execute { Want::Rw } else { Want::PreferRw };
    let t = open_target(soc, target, want, execute, false, None, None, via)?;
    if let Ok(g) = crate::gpt::Gpt::parse(&t.read(0, crate::gpt::GPT_AREA)?) {
        if let Some(p) = g.find("sn_mac") {
            if let Some(cur) = crate::snmac::SnMac::parse(&t.read(p.offset(), crate::snmac::RECORD_AREA)?) {
                r.model_before = cur.model().into();
            }
        }
    }
    let opts = restore::Options {
        model: None,
        board: None,
        sn: None,
        mac: None,
        sn_prefix: None,
        new_identity: false,
        keep_userdata: false,
        wipe: restore::Wipe::Quick,
        factory_guids: false,
        force_model: false,
        repair: keep_data,
        force_boot: false,
        keep_overlay: false,
        wipe_overlay: false,
        rewrite: false,
    };
    let plan = restore::plan(&t, &pkg, None, &opts)?;
    r.device_firmware = plan.device_firmware.as_ref().map(|(b, v)| format!("{b} {v}")).unwrap_or_default();
    r.model_after = plan.snmac_new.model().into();
    r.model_after_name = crate::snmac::product_name(&r.model_after);
    if !r.model_before.is_empty() {
        r.model_before_name = crate::snmac::product_name(&r.model_before);
    }
    r.firmware_name = crate::snmac::product_name_for_ota_board(&r.firmware_board);
    if let Some((b, _)) = &plan.device_firmware {
        r.device_firmware_name = crate::snmac::product_name_for_ota_board(b);
    }
    r.identity_generated = plan.identity_generated;
    r.notes = plan.notes.clone();
    r.details = plan.describe();
    r.bytes_to_write = plan.bytes();
    if !execute || r.bytes_to_write == 0 {
        return Ok(r);
    }
    if let Some(dir) = safety_dir {
        // partition table, ota and sn_mac: enough to undo an identity or layout mistake
        let head = t.read(0, 3 << 20)?;
        std::fs::create_dir_all(dir)?;
        let ts = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_secs();
        let f = dir.join(format!("before-install-{ts}.first3MiB.bin"));
        std::fs::write(&f, head).with_context(|| format!("saving the safety copy to {}", f.display()))?;
        say!("safety copy of the first 3 MiB saved to {}", f.display());
        r.safety_copy = f.display().to_string();
    }
    if let (true, Some(dir)) = (plan.identity_generated, safety_dir) {
        use std::io::Write;
        std::fs::create_dir_all(dir)?;
        let ts = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_secs();
        let mut f = std::fs::OpenOptions::new().create(true).append(true).open(dir.join("generated-identities.log"))?;
        let n = &plan.snmac_new;
        writeln!(f, "{ts}\t{}\t{}\t{}\t{}", n.sn(), n.mac(), n.model(), n.board())?;
        say!("the newly generated serial number and MAC address were recorded in {}", dir.display());
    }
    plan.execute(&t)?;
    r.executed = true;
    say!("installation complete and verified");
    Ok(r)
}

/// One of the two system slots (A: rtos/kernel/rootfs, B: rtos2/kernel2/rootfs2).
#[derive(Debug, Clone, Default)]
pub struct SlotInfo {
    /// a kernel image is present
    pub has_kernel: bool,
    /// product name of the firmware ("Ender-3 V3 KE"), "" if no readable system
    pub firmware_name: String,
    pub firmware_version: String,
}

impl SlotInfo {
    pub fn bootable(&self) -> bool {
        self.has_kernel && !self.firmware_version.is_empty()
    }
}

#[derive(Debug, Clone, Default)]
pub struct Slots {
    /// "a" or "b"
    pub active: String,
    pub a: SlotInfo,
    pub b: SlotInfo,
}

fn read_slots(t: &restore::Target) -> Result<(crate::gpt::Gpt, Slots)> {
    let g = crate::gpt::Gpt::parse(&t.read(0, crate::gpt::GPT_AREA)?)
        .context("this board has no valid partition table (blank board?); install firmware first")?;
    let ota = g.find("ota").context("no ota partition")?;
    let active = if t.read(ota.offset(), 512)?.starts_with(b"ota:kernel2") { "b" } else { "a" };
    let slot = |kernel: &str, rootfs: &str| -> Result<SlotInfo> {
        let mut s = SlotInfo::default();
        if let Some(k) = g.find(kernel) {
            s.has_kernel = t.read(k.offset(), 512)?[..4] == fastboot::UIMAGE_MAGIC.to_be_bytes();
        }
        if let Some((b, v)) = restore::firmware_in(t, &g, rootfs) {
            s.firmware_name = crate::snmac::product_name_for_ota_board(&b);
            s.firmware_version = v;
        }
        Ok(s)
    };
    let slots = Slots { active: active.into(), a: slot("kernel", "rootfs")?, b: slot("kernel2", "rootfs2")? };
    Ok((g, slots))
}

/// What is installed in each slot and which one starts. `target` overrides the device (tests).
pub fn slots(soc: &SocProfile, via: Via, target: Option<&PathBuf>) -> Result<Slots> {
    let t = open_target(soc, target, Want::PreferRw, false, false, None, None, via)?;
    Ok(read_slots(&t)?.1)
}

/// Choose which slot starts at the next power-on. Refuses a slot without a system.
pub fn set_active_slot(
    soc: &SocProfile,
    via: Via,
    slot: &str,
    safety_dir: Option<&std::path::Path>,
    target: Option<&PathBuf>,
) -> Result<Slots> {
    if slot != "a" && slot != "b" {
        bail!("slot must be a or b");
    }
    let t = open_target(soc, target, Want::Rw, true, false, None, None, via)?;
    let (g, mut s) = read_slots(&t)?;
    let info = if slot == "a" { &s.a } else { &s.b };
    if !info.bootable() {
        bail!("slot {} has no complete firmware installed; the board would not start from it", slot.to_uppercase());
    }
    if s.active == slot {
        say!("slot {} is already the active one", slot.to_uppercase());
        return Ok(s);
    }
    if let Some(dir) = safety_dir {
        std::fs::create_dir_all(dir)?;
        let ts = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_secs();
        let f = dir.join(format!("before-switch-{ts}.first3MiB.bin"));
        std::fs::write(&f, t.read(0, 3 << 20)?)?;
        say!("safety copy of the first 3 MiB saved to {}", f.display());
    }
    // Creality's own record: "ota:kernel2" starts kernel2/rootfs2/rtos2, "ota:kernel" the first set
    let mut rec = vec![0u8; 512];
    let text: &[u8] = if slot == "b" { b"ota:kernel2\n" } else { b"ota:kernel\n" };
    rec[..text.len()].copy_from_slice(text);
    t.write_verified(g.find("ota").context("no ota partition")?.offset(), &rec, "ota")?;
    s.active = slot.into();
    say!("slot {} will start at the next power-on", slot.to_uppercase());
    Ok(s)
}

#[derive(Debug, Clone, Default)]
pub struct ResetReport {
    /// partitions that are (to be) reset: (name, size)
    pub partitions: Vec<(String, u64)>,
    /// how much actually has to be written; 0 = already blank
    pub bytes_to_write: u64,
    pub executed: bool,
}

/// Factory reset: blank the data partitions the firmware recreates on first boot
/// (`rootfs_data`: settings and system changes; `userdata`: user files). Firmware, both
/// slots, the slot choice and the identity are not touched.
/// The filesystems are made unrecoverable for the board's own fsck (every superblock copy
/// is zeroed), which is what makes the firmware format them again; file contents are not
/// overwritten sector by sector.
pub fn factory_reset(
    soc: &SocProfile,
    via: Via,
    keep_userdata: bool,
    execute: bool,
    safety_dir: Option<&std::path::Path>,
    target: Option<&PathBuf>,
) -> Result<ResetReport> {
    let want = if execute { Want::Rw } else { Want::PreferRw };
    let t = open_target(soc, target, want, execute, false, None, None, via)?;
    let g = crate::gpt::Gpt::parse(&t.read(0, crate::gpt::GPT_AREA)?)
        .context("this board has no valid partition table (blank board?); install firmware instead")?;
    let mut names = vec!["rootfs_data"];
    if !keep_userdata {
        names.push("userdata");
    }
    let mut r = ResetReport::default();
    let mut ranges: Vec<(u64, usize)> = vec![];
    for n in names {
        let p = g.find(n).with_context(|| format!("no {n} partition"))?;
        r.partitions.push((n.to_string(), p.size()));
        for (off, len) in restore::ext_quick_wipe_ranges(p.size()) {
            ranges.push((p.offset() + off, len as usize));
        }
    }
    let zeros = vec![0u8; 1 << 20];
    let mut dirty = vec![];
    for (i, &(off, len)) in ranges.iter().enumerate() {
        crate::events::check_cancelled()?;
        if !t.matches(off, &zeros[..len])? {
            dirty.push((off, len));
            r.bytes_to_write += len as u64;
        }
        if i % 32 == 0 {
            crate::events::progress("checking", i as u64, ranges.len() as u64, "");
        }
    }
    crate::events::progress_end();
    if !execute || dirty.is_empty() {
        return Ok(r);
    }
    if let Some(dir) = safety_dir {
        std::fs::create_dir_all(dir)?;
        let ts = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_secs();
        let f = dir.join(format!("before-reset-{ts}.first3MiB.bin"));
        std::fs::write(&f, t.read(0, 3 << 20)?)?;
        say!("safety copy of the first 3 MiB saved to {}", f.display());
    }
    for (i, &(off, len)) in dirty.iter().enumerate() {
        t.write_raw(off, &zeros[..len], "factory reset")?;
        if i % 16 == 0 {
            crate::events::progress("erasing", i as u64, dirty.len() as u64, "");
        }
    }
    t.sync()?;
    for (i, &(off, len)) in dirty.iter().enumerate() {
        if !t.matches(off, &zeros[..len])? {
            bail!("verify FAILED at {off:#x} during factory reset");
        }
        if i % 32 == 0 {
            crate::events::progress("verifying", i as u64, dirty.len() as u64, "");
        }
    }
    crate::events::progress_end();
    r.executed = true;
    say!("factory reset complete and verified");
    Ok(r)
}

#[derive(Debug, Clone, Default)]
pub struct RestoreReport {
    pub check: crate::diskimage::ImageCheck,
    pub bytes_differed: u64,
    pub bytes_written: u64,
    pub executed: bool,
}

/// Restore a full backup image. execute=false: only check that the file fits this board and
/// whose identity it carries. execute=true: verify the file's checksum (if it has one), then
/// make the board identical to it (only differing parts are written; all verified).
pub fn restore_backup(
    soc: &SocProfile,
    via: Via,
    image: &std::path::Path,
    execute: bool,
    allow_identity_change: bool,
    safety_dir: Option<&std::path::Path>,
    target: Option<&PathBuf>,
) -> Result<RestoreReport> {
    let want = if execute { Want::Rw } else { Want::PreferRw };
    let t = open_target(soc, target, want, execute, false, None, None, via)?;
    let mut r = RestoreReport { check: crate::diskimage::check_image(&t, image)?, ..Default::default() };
    if !execute {
        return Ok(r);
    }
    if r.check.different_board && !allow_identity_change {
        bail!("this backup comes from a different board; restoring it would copy that board's serial number and MAC address");
    }
    crate::diskimage::verify_checksum_file(image)?;
    if let Some(dir) = safety_dir {
        std::fs::create_dir_all(dir)?;
        let ts = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_secs();
        let f = dir.join(format!("before-restore-{ts}.first3MiB.bin"));
        std::fs::write(&f, t.read(0, 3 << 20)?)?;
        say!("safety copy of the first 3 MiB saved to {}", f.display());
    }
    let o = crate::diskimage::WriteImageOptions { yes: true, allow_identity_change };
    let (differ, written) = crate::diskimage::write_image(&t, image, &o)?;
    r.bytes_differed = differ;
    r.bytes_written = written;
    r.executed = true;
    Ok(r)
}

/// Ways to get a shell on the running recovery system, as far as this computer can tell.
#[derive(Debug, Clone, Default)]
pub struct ShellStatus {
    /// the `adb` program was found on this computer
    pub adb_installed: bool,
    /// adb lists the board as ready; `adb -s <adb_serial> shell` will work
    pub adb_ready: bool,
    pub adb_serial: String,
    /// what adb says about the board when it is listed but not ready ("unauthorized", "offline"...)
    pub adb_state: String,
    /// the recovery's USB serial console: /dev/ttyACM0, /dev/cu.usbmodem..., COM5; "" if not found
    pub console_port: String,
}

fn quiet_command(program: &str) -> std::process::Command {
    #[allow(unused_mut)]
    let mut c = std::process::Command::new(program);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        c.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no console flash from a GUI
    }
    c
}

#[cfg(target_os = "linux")]
fn find_console_port(_serial: &str) -> String {
    // the ACM interface of a 1209:0003 device
    let Ok(rd) = std::fs::read_dir("/sys/class/tty") else { return String::new() };
    let mut found: Vec<String> = vec![];
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        if !name.starts_with("ttyACM") {
            continue;
        }
        // /sys/class/tty/ttyACM0/device -> the USB interface; its parent is the USB device
        let Ok(dev) = std::fs::canonicalize(e.path().join("device")) else { continue };
        let Some(usb) = dev.parent() else { continue };
        let rdf = |n: &str| std::fs::read_to_string(usb.join(n)).unwrap_or_default().trim().to_lowercase();
        if rdf("idVendor") == format!("{:04x}", usb::NOCRY_UMS_VID) && rdf("idProduct") == format!("{:04x}", usb::NOCRY_RECOVERY_PID) {
            found.push(format!("/dev/{name}"));
        }
    }
    found.sort();
    found.into_iter().next().unwrap_or_default()
}

#[cfg(target_os = "macos")]
fn find_console_port(serial: &str) -> String {
    // macOS names a CDC-ACM port after the device's serial number: /dev/cu.usbmodem<serial><n>
    let Ok(rd) = std::fs::read_dir("/dev") else { return String::new() };
    let mut all: Vec<String> = rd
        .flatten()
        .map(|e| e.file_name().to_string_lossy().to_string())
        .filter(|n| n.starts_with("cu.usbmodem"))
        .map(|n| format!("/dev/{n}"))
        .collect();
    all.sort();
    if !serial.is_empty() {
        if let Some(p) = all.iter().find(|p| p.contains(serial)) {
            return p.clone();
        }
    }
    if all.len() == 1 {
        all.remove(0)
    } else {
        String::new()
    }
}

#[cfg(windows)]
fn find_console_port(_serial: &str) -> String {
    // COM port assigned to interface 0 (CDC-ACM) of 1209:0003, if it is currently present
    let key = format!("HKLM\\SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_{:04X}&PID_{:04X}&MI_00", usb::NOCRY_UMS_VID, usb::NOCRY_RECOVERY_PID);
    let query = |args: &[&str]| quiet_command("reg").args(args).output().map(|o| String::from_utf8_lossy(&o.stdout).to_string()).unwrap_or_default();
    let assigned: Vec<String> = query(&["query", &key, "/s", "/v", "PortName"])
        .lines()
        .filter_map(|l| l.split_whitespace().last().filter(|w| w.starts_with("COM")).map(String::from))
        .collect();
    let active = query(&["query", "HKLM\\HARDWARE\\DEVICEMAP\\SERIALCOMM"]);
    assigned.into_iter().find(|p| active.split_whitespace().any(|w| w == p)).unwrap_or_default()
}

#[cfg(not(any(target_os = "linux", target_os = "macos", windows)))]
fn find_console_port(_serial: &str) -> String {
    String::new()
}

/// Looks for the recovery's serial console and asks `adb` (if installed) whether it sees the
/// board. `serial` is the board's serial number (its USB and adb serial). Starts the adb
/// server if it is not running, like any adb command does.
pub fn shell_status(serial: &str) -> ShellStatus {
    let mut s = ShellStatus { console_port: find_console_port(serial), ..Default::default() };
    if let Ok(o) = quiet_command("adb").arg("devices").output() {
        s.adb_installed = true;
        let out = String::from_utf8_lossy(&o.stdout);
        let mut rows: Vec<(String, String)> = out
            .lines()
            .skip(1)
            .filter_map(|l| {
                let mut w = l.split_whitespace();
                Some((w.next()?.to_string(), w.next()?.to_string()))
            })
            .collect();
        // prefer the row with this board's serial; otherwise a lone device
        let row = match rows.iter().position(|(sn, _)| !serial.is_empty() && sn == serial) {
            Some(i) => Some(rows.remove(i)),
            None if rows.len() == 1 && serial.is_empty() => Some(rows.remove(0)),
            None => None,
        };
        if let Some((sn, state)) = row {
            s.adb_ready = state == "device";
            s.adb_serial = sn;
            s.adb_state = state;
        }
    }
    s
}
