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
/// Start a first-stage loader that boots the board from its eMMC on its own (the factory SPL,
/// or nocryboot's), straight from USB boot mode. Nothing is written: the loader runs from RAM
/// and then starts whatever is installed. Used to try a loader before installing it.
pub fn run_spl_only(soc: &SocProfile, spl: &[u8], device: Option<&str>, force: bool) -> Result<()> {
    if spl.len() > soc.s1_max {
        bail!("the loader is {} bytes; the boot ROM takes at most {}", spl.len(), soc.s1_max);
    }
    // An eMMC loader starts with "MSPL" and carries its memory settings ("INGE") at 0x100.
    if (spl.len() < 0x900 || &spl[..4] != b"LPSM" || &spl[0x100..0x104] != b"INGE") && !force {
        bail!("this does not look like a first-stage loader for the eMMC (no MSPL/INGE header); not sending it");
    }
    let f = usb::select(soc, device)?;
    say!("device {} (port {})", f.id(), f.port_path());
    let mut b = Bootrom::new(usb::open(&f)?);
    b.log = Box::new(move |m| say!("{m}"));
    let info = b.cpu_info().context("bootrom did not answer GET_CPU_INFO")?;
    say!("bootrom cpu info: {info:?}");
    if !info.to_ascii_uppercase().starts_with("X2000") && !force {
        bail!("this board reports the processor {info:?}, not the X2000 family; nothing was sent");
    }
    say!("loader: {} bytes -> {:#010x}, exec {:#010x}", spl.len(), soc.s1_load, soc.s1_exec);
    b.download(soc.s1_load, spl)?;
    if let Err(e) = b.prog_stage1(soc.s1_exec) {
        say!("start request not acknowledged ({e:#}); the loader has probably started already");
    }
    say!("loader started; the board now boots from its eMMC and leaves USB boot mode");
    Ok(())
}

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
    // nocry's SPL and stage2 are built for the X2000 family only. Other Ingenic chips (the
    // 2025 K1C's X2600E among them) would not run them, and boards with secure boot only
    // run code signed by the manufacturer.
    if !info.to_ascii_uppercase().starts_with("X2000") && !force {
        bail!(
            "this board reports the processor {info:?}, not the X2000 family nocry supports. Boards with a \
             different Ingenic chip, such as the 2025 K1C (X2600E, with secure boot), cannot be loaded: \
             they only run software signed by Creality. Nothing was sent to the board"
        );
    }
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

static NETWORK_BOARD: std::sync::Mutex<Option<(String, String)>> = std::sync::Mutex::new(None);

/// Work with a board running nocry-rescue over the network instead of USB: its address
/// ("host" or "host:port") and the pairing code on its screen. `None` goes back to USB.
pub fn set_network_board(board: Option<(String, String)>) {
    *NETWORK_BOARD.lock().unwrap() = board;
}

pub fn network_board() -> Option<(String, String)> {
    NETWORK_BOARD.lock().unwrap().clone()
}

/// Pairs with the board at `addr` using `code` and, when that works, makes it the board
/// every later command uses. Returns what the board says it is (model, service version).
/// Nothing on the board is read or changed beyond those two answers.
pub fn connect_network_board(addr: &str, code: &str) -> Result<(String, String)> {
    let code: String = code.chars().filter(|c| !c.is_whitespace() && *c != '-').collect();
    if addr.trim().is_empty() {
        bail!("no address given");
    }
    if code.is_empty() {
        bail!("no pairing code given (it is on the board's screen)");
    }
    let mut fb = crate::fastboot::Fastboot::open_net(addr.trim(), &code)?;
    let product = fb.getvar("product")?;
    let version = fb.getvar("version-bootloader")?;
    set_network_board(Some((addr.trim().to_string(), code)));
    Ok((product, version))
}

/// Whether the network board still accepts connections (no pairing: one TCP connect).
pub fn network_board_reachable() -> bool {
    use std::net::ToSocketAddrs;
    let Some((addr, _)) = network_board() else { return false };
    let target = if addr.contains(':') && !addr.starts_with('[') || addr.contains("]:") { addr } else { format!("{addr}:{}", crate::net::PORT) };
    match target.to_socket_addrs().ok().and_then(|mut a| a.next()) {
        Some(sa) => std::net::TcpStream::connect_timeout(&sa, Duration::from_secs(3)).is_ok(),
        None => false,
    }
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
    if let Some((addr, code)) = network_board() {
        say!("using the board at {addr} over the network");
        return restore::Target::open_net(&addr, &code, write);
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
    /// a board running nocry-rescue, paired over the network (`port` holds its address)
    Network,
    /// an Ingenic device in USB boot mode that nocry has no support for (other chip family)
    Unsupported,
}

#[derive(Debug, Clone)]
pub struct BoardState {
    pub kind: BoardKind,
    pub port: String,
    pub count: usize,
}

pub fn board_state() -> Result<BoardState> {
    if let Some((addr, _)) = network_board() {
        return Ok(BoardState { kind: BoardKind::Network, port: addr, count: 1 });
    }
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
        } else if f.vid == 0xa108 {
            (4, BoardKind::Unsupported)
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
    /// "a"/"b" when this installs into the inactive slot only; "" for a normal install
    pub target_slot: String,
    /// the slot gets (got) nocry's boot picture instead of the firmware's
    pub nocry_picture: bool,
    /// the board had no partition table: this is a rebuild from scratch
    pub blank_board: bool,
    /// where the pre-write copy of the first 3 MiB went, if one was taken
    pub safety_copy: String,
    /// mainboard field before and after
    pub mainboard_before: String,
    pub mainboard_after: String,
    /// "Nebula Pad family" / "K1 family"
    pub family: String,
    /// Not planned yet: the user has to say which printer type ("model") or mainboard revision
    /// ("mainboard") the board is; `choices` are (value, name to show). Call again with it.
    pub need_choice: String,
    pub choices: Vec<(String, String)>,
    /// what the firmware itself says it supports, for the dialogs: (model, name, mainboards)
    pub firmware_supports: Vec<(String, String, Vec<String>)>,
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
    model: Option<&str>,
    mainboard: Option<&str>,
    picture: PictureChoice,
) -> Result<InstallReport> {
    let pkg = restore::Package::load(os)?;
    let line = pkg.ota_board.as_deref().and_then(crate::boards::line);
    let mut r = InstallReport {
        firmware_board: pkg.ota_board.clone().unwrap_or_default(),
        firmware_version: pkg.ota_version.clone().unwrap_or_default(),
        has_boot_area: pkg.boot.is_some(),
        family: line.map(|l| crate::boards::family(l.family).name.to_string()).unwrap_or_default(),
        firmware_supports: pkg.support.iter().map(|s| (s.model.clone(), crate::boards::product_name(&s.model), s.mainboards.clone())).collect(),
        ..Default::default()
    };
    say!("firmware: {} {} from {}", r.firmware_board, r.firmware_version, pkg.source);
    if let Some(n) = &pkg.remark {
        say!("note: {n}");
        r.notes.push(n.clone());
    }
    let want = if execute { Want::Rw } else { Want::PreferRw };
    let t = open_target(soc, target, want, execute, false, None, None, via)?;
    let mut blank = true;
    if let Ok(g) = crate::gpt::Gpt::parse(&t.read(0, crate::gpt::GPT_AREA)?) {
        blank = false;
        if let Some(p) = g.find("sn_mac") {
            if let Some(cur) = crate::snmac::SnMac::parse(&t.read(p.offset(), crate::snmac::RECORD_AREA)?) {
                r.model_before = cur.model().into();
                r.mainboard_before = cur.board().into();
            }
        }
    }
    // A blank board has no partition table or bootloader, and update files carry neither:
    // take them from Creality's factory package (downloaded once, checksum-verified).
    let mut boot = None;
    if blank {
        say!("the board is blank: it will be rebuilt from scratch (partition table, bootloader, firmware, identity)");
        if pkg.boot.is_none() {
            let fam = line.map(|l| l.family).with_context(|| {
                format!("nocry does not know this firmware ({}) and has no bootloader for it", crate::boards::unknown_firmware(&r.firmware_board))
            })?;
            let dir = safety_dir.and_then(|d| d.parent()).map(|d| d.to_path_buf()).unwrap_or_else(crate::catalog::default_data_dir);
            boot = Some(crate::catalog::boot_area(&dir, fam)?);
        }
    }
    r.blank_board = blank;
    let keep_data = keep_data && !blank; // nothing to keep
    let opts = restore::Options {
        model: model.filter(|m| !m.is_empty()).map(String::from),
        board: mainboard.filter(|m| !m.is_empty()).map(String::from),
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
    let plan = match restore::plan(&t, &pkg, boot, &opts) {
        Ok(p) => p,
        Err(e) => match e.downcast_ref::<restore::NeedChoice>() {
            // not an error for a front end: it asks the user and calls again
            Some(c) => {
                r.need_choice = c.what.into();
                r.choices = c.choices.clone();
                r.notes.push(c.to_string());
                return Ok(r);
            }
            None => return Err(e),
        },
    };
    r.mainboard_after = plan.snmac_new.board().into();
    r.device_firmware = plan.device_firmware.as_ref().map(|(b, v)| format!("{b} {v}")).unwrap_or_default();
    r.model_after = plan.snmac_new.model().into();
    r.model_after_name = crate::snmac::product_name(&r.model_after);
    if !r.model_before.is_empty() {
        r.model_before_name = crate::snmac::product_name(&r.model_before);
    }
    r.firmware_name = crate::snmac::product_name_for_ota_board(&r.firmware_board);
    if pkg.remark.is_some() {
        r.firmware_name += " (for the CFS-C upgrade)";
    }
    if let Some((b, _)) = &plan.device_firmware {
        r.device_firmware_name = crate::snmac::product_name_for_ota_board(b);
    }
    r.identity_generated = plan.identity_generated;
    r.notes.extend(plan.notes.iter().cloned());
    r.details = plan.describe();
    r.bytes_to_write = plan.bytes();
    // the boot picture of the slot being installed (A), decided by the family of the result
    let fam = crate::boards::family_of_mainboard(&plan.snmac_new.board()).map(|f| f.id);
    let nocry_picture = match (picture, fam) {
        (PictureChoice::Firmware, _) | (_, None) => false,
        (PictureChoice::Nocry, Some(f)) => nocry_picture_image(None, f).is_some(),
        (PictureChoice::Default, Some(f)) => picture_proven(f) && nocry_picture_image(None, f).is_some(),
    };
    r.nocry_picture = nocry_picture;
    if nocry_picture {
        r.notes.push("The nocry boot picture is shown at power-on. Creality's is kept on the board and can be brought back (Advanced, Boot picture).".into());
    }
    // With the firmware already in place, setting the picture is all there is to do; it has to
    // show up as work, or callers that stop at "nothing to write" would never get to it.
    let picture_only = r.bytes_to_write == 0
        && match (nocry_picture, fam, crate::gpt::Gpt::parse(&t.read(0, crate::gpt::GPT_AREA)?)) {
            (true, Some(f), Ok(g)) => apply_picture(&t, &g, None, f, crate::picture::Kind::Nocry, &["a"], false)?,
            _ => false,
        };
    if picture_only {
        r.bytes_to_write = nocry_picture_image(None, fam.unwrap()).map_or(0, |i| i.len() as u64);
        r.details += "  rtos: the nocry boot picture\n";
    }
    if !execute {
        return Ok(r);
    }
    if picture_only {
        let g = crate::gpt::Gpt::parse(&t.read(0, crate::gpt::GPT_AREA)?)?;
        r.executed = apply_picture(&t, &g, None, fam.unwrap(), crate::picture::Kind::Nocry, &["a"], true)?;
        return Ok(r);
    }
    if r.bytes_to_write == 0 {
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
    if let (true, Some(f)) = (nocry_picture, fam) {
        let g = crate::gpt::Gpt::parse(&t.read(0, crate::gpt::GPT_AREA)?)?;
        apply_picture(&t, &g, None, f, crate::picture::Kind::Nocry, &["a"], true)?;
    }
    r.executed = true;
    say!("installation complete and verified");
    Ok(r)
}

/// One of the two system slots (A: rtos/kernel/rootfs, B: rtos2/kernel2/rootfs2).
#[derive(Debug, Clone, Default)]
pub struct SlotInfo {
    /// a kernel image is present
    pub has_kernel: bool,
    /// the rtos partition holds an image the board's loader accepts (it hangs otherwise)
    pub has_rtos: bool,
    /// product name of the firmware ("Ender-3 V3 KE"), "" if no readable system
    pub firmware_name: String,
    pub firmware_version: String,
}

impl SlotInfo {
    pub fn bootable(&self) -> bool {
        self.has_kernel && self.has_rtos && !self.firmware_version.is_empty()
    }
}

#[derive(Debug, Clone, Default)]
pub struct Slots {
    /// "a" or "b"
    pub active: String,
    pub a: SlotInfo,
    pub b: SlotInfo,
    /// nocryboot's note in the second sector of `ota`, if any
    pub note: Option<BootNote>,
    /// the first-stage loader on the board is nocryboot with the start counter
    pub loader_counts: bool,
}

/// What nocryboot wrote (or was told) in the second sector of the `ota` partition.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BootNote {
    /// slot ("a"/"b") is on trial with this many starts left
    Trial { slot: String, left: u8 },
    /// the slot never reported a working start; the loader went back to the other one
    Failed(String),
    /// the slot had parts missing; the loader went to the other one
    Incomplete(String),
    /// the slot never reported a working start, but there was nothing else to start
    Unconfirmed(String),
}

impl BootNote {
    pub fn parse(sector: &[u8]) -> Option<BootNote> {
        let end = sector.iter().position(|&c| c == b'\n' || c == 0).unwrap_or(sector.len());
        let t = std::str::from_utf8(&sector[..end]).ok()?.strip_prefix("nocryboot:")?;
        let slot = |x: &str| match x {
            "A" => Some("a".to_string()),
            "B" => Some("b".to_string()),
            _ => None,
        };
        if let Some(r) = t.strip_prefix("try=") {
            let (s, l) = r.split_once(":left=")?;
            return Some(BootNote::Trial { slot: slot(s)?, left: l.parse().ok().filter(|n| *n <= 9)? });
        }
        if let Some(r) = t.strip_prefix("failed=") {
            return Some(BootNote::Failed(slot(r)?));
        }
        if let Some(r) = t.strip_prefix("incomplete=") {
            return Some(BootNote::Incomplete(slot(r)?));
        }
        t.strip_prefix("unconfirmed=").and_then(slot).map(BootNote::Unconfirmed)
    }

    pub fn describe(&self) -> String {
        match self {
            BootNote::Trial { slot, left } => format!("slot {} is on trial: {left} start(s) left to report success", slot.to_uppercase()),
            BootNote::Failed(s) => format!("slot {} never reported a working start; the loader went back to the other slot", s.to_uppercase()),
            BootNote::Incomplete(s) => format!("slot {} had parts missing; the loader started the other slot", s.to_uppercase()),
            BootNote::Unconfirmed(s) => format!("slot {} never reported a working start, but there was no other slot to start", s.to_uppercase()),
        }
    }
}

/// The script nocry puts into the firmware's settings area so that a started system reports success.
pub const CONFIRM_SCRIPT: &str = include_str!("../data/nocryboot-confirm.sh");
pub const CONFIRM_SCRIPT_PATH: &str = "upper/etc/init.d/S98nocryboot";
pub use crate::loader::{SPL_OFFSET, SPL_SIZE};


fn read_slots(t: &restore::Target) -> Result<(crate::gpt::Gpt, Slots)> {
    let g = crate::gpt::Gpt::parse(&t.read(0, crate::gpt::GPT_AREA)?)
        .context("this board has no valid partition table (blank board?); install firmware first")?;
    let ota = g.find("ota").context("no ota partition")?;
    let active = if t.read(ota.offset(), 512)?.starts_with(b"ota:kernel2") { "b" } else { "a" };
    let slot = |rtos: &str, kernel: &str, rootfs: &str| -> Result<SlotInfo> {
        let mut s = SlotInfo::default();
        if let Some(r) = g.find(rtos) {
            s.has_rtos = crate::rtos::RtosHeader::parse(&t.read(r.offset(), 512)?).is_ok_and(|h| h.loaded_len() <= r.size());
        }
        if let Some(k) = g.find(kernel) {
            s.has_kernel = t.read(k.offset(), 512)?[..4] == fastboot::UIMAGE_MAGIC.to_be_bytes();
        }
        if let Some((b, v)) = restore::firmware_in(t, &g, rootfs) {
            s.firmware_name = crate::snmac::product_name_for_ota_board(&b);
            s.firmware_version = v;
        }
        Ok(s)
    };
    let note = BootNote::parse(&t.read(ota.offset() + 512, 512)?);
    let loader_counts = crate::loader::inspect(&t.read(SPL_OFFSET, SPL_SIZE)?).counts;
    let slots =
        Slots { active: active.into(), a: slot("rtos", "kernel", "rootfs")?, b: slot("rtos2", "kernel2", "rootfs2")?, note, loader_counts };
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
    let ota = g.find("ota").context("no ota partition")?.offset();
    if s.note.is_some() {
        t.write_verified(ota + 512, &[0u8; 512], "ota")?; // a plain choice ends a trial and clears old notes
        s.note = None;
    }
    t.write_verified(ota, &rec, "ota")?;
    s.active = slot.into();
    say!("slot {} will start at the next power-on", slot.to_uppercase());
    Ok(s)
}

/// Put a slot on trial: it starts at the next power-on, and unless the started system reports
/// success within `tries` starts, nocryboot goes back to the other slot by itself.
/// Needs nocryboot as the board's loader, and the firmware's settings area (the success
/// script is stored there; a factory reset removes it).
pub fn start_trial(soc: &SocProfile, via: Via, slot: &str, tries: u8, target: Option<&PathBuf>) -> Result<Slots> {
    if slot != "a" && slot != "b" {
        bail!("slot must be a or b");
    }
    if !(1..=9).contains(&tries) {
        bail!("the number of starts must be 1 to 9");
    }
    let t = open_target(soc, target, Want::Rw, true, false, None, None, via)?;
    let (g, mut s) = read_slots(&t)?;
    let (info, other) = if slot == "a" { (&s.a, &s.b) } else { (&s.b, &s.a) };
    if !info.bootable() {
        bail!("slot {} has no complete firmware installed; the board would not start from it", slot.to_uppercase());
    }
    if !s.loader_counts {
        bail!(
            "the loader on this board does not count starts (it is the factory loader or an older nocryboot); \
             a trial needs nocryboot with the start counter. Nothing was changed"
        );
    }
    if !other.bootable() {
        bail!(
            "slot {} has no complete firmware, so there is nothing to go back to if slot {} fails. Nothing was changed",
            if slot == "a" { "B" } else { "A" },
            slot.to_uppercase()
        );
    }
    // 1. the success script, so a working system can say so. Without it the trial would always fail.
    t.put_file("rootfs_data", CONFIRM_SCRIPT_PATH, 0o755, CONFIRM_SCRIPT.as_bytes()).context(
        "could not store the success script in the settings area (after a factory reset or clean install \
         the firmware creates that area on its first start; start the printer once, then try again). Nothing was changed",
    )?;
    let ota = g.find("ota").context("no ota partition")?.offset();
    // 2. the count, 3. the slot choice (Creality's own record)
    let mut note = vec![0u8; 512];
    let text = format!("nocryboot:try={}:left={tries}\n", slot.to_uppercase());
    note[..text.len()].copy_from_slice(text.as_bytes());
    t.write_verified(ota + 512, &note, "ota")?;
    let mut rec = vec![0u8; 512];
    let text: &[u8] = if slot == "b" { b"ota:kernel2\n" } else { b"ota:kernel\n" };
    rec[..text.len()].copy_from_slice(text);
    t.write_verified(ota, &rec, "ota")?;
    s.active = slot.into();
    s.note = Some(BootNote::Trial { slot: slot.into(), left: tries });
    say!(
        "slot {} is on trial: it has {tries} start(s) to come up and report success, else the board returns to slot {}",
        slot.to_uppercase(),
        if slot == "a" { "B" } else { "A" }
    );
    Ok(s)
}

/// Which boot picture a slot should show.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum PictureChoice {
    /// nocry's on board families where it is proven on the real screens, else the firmware's
    #[default]
    Default,
    Nocry,
    Firmware,
}

/// Board families on whose own screens nocry's boot picture image has been seen working.
/// (K1 family: runs on the mainboard, but its four 480x800 panels are still untested.)
pub fn picture_proven(family: crate::boards::FamilyId) -> bool {
    family == crate::boards::FamilyId::Nebula
}

/// nocry's boot picture image for a board family: `<images>/nocry-rtos/nocry-rtos-<family>.bin`
/// (in a source tree: tools/rtos-build/out).
pub fn nocry_picture_image(images: Option<&PathBuf>, family: crate::boards::FamilyId) -> Option<Vec<u8>> {
    let (name, dev) = match family {
        crate::boards::FamilyId::Nebula => ("nocry-rtos-nebula.bin", "nocry-rtos.bin"),
        crate::boards::FamilyId::K1 => ("nocry-rtos-k1.bin", "nocry-rtos-k1.bin"),
    };
    let dir = images_dir(images).ok()?;
    [dir.join("nocry-rtos").join(name), dir.join("../../rtos-build/out").join(dev)]
        .iter()
        .find_map(|p| std::fs::read(p).ok())
        .filter(|b| crate::picture::kind(b) == crate::picture::Kind::Nocry && crate::picture::image_len(b) == Some(b.len()))
}

/// The boot picture of one slot.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PictureInfo {
    pub kind: crate::picture::Kind,
    /// the firmware's own image is kept in the partition and can be brought back
    pub firmware_kept: bool,
}

#[derive(Debug, Clone)]
pub struct PictureStatus {
    pub a: PictureInfo,
    pub b: PictureInfo,
    pub family: Option<crate::boards::FamilyId>,
    /// nocry's picture is proven on this family's screens
    pub proven: bool,
    /// this nocry has its picture image for the family
    pub available: bool,
    pub changed: bool,
}

fn picture_of(t: &restore::Target, g: &crate::gpt::Gpt, part: &str) -> Result<(PictureInfo, Option<Vec<u8>>, Option<Vec<u8>>)> {
    use crate::picture as P;
    let none = (PictureInfo { kind: P::Kind::None, firmware_kept: false }, None, None);
    let Some(p) = g.find(part) else { return Ok(none) };
    if p.size() < 4 << 20 {
        return Ok(none);
    }
    let cur = match P::image_len(&t.read(p.offset(), 512)?) {
        Some(n) if (n as u64) <= P::KEPT_MARK_OFFSET => Some(t.read(p.offset(), n)?),
        _ => None,
    };
    let kind = cur.as_deref().map_or(P::Kind::None, P::kind);
    let mark = t.read(p.offset() + P::KEPT_MARK_OFFSET, 512)?;
    let kept = match P::kept_size(&mark) {
        Some(n) => P::kept_image(&mark, &t.read(p.offset() + P::KEPT_OFFSET, n)?),
        None => None,
    };
    Ok((PictureInfo { kind, firmware_kept: kept.is_some() }, cur, kept))
}

fn board_family(t: &restore::Target, g: &crate::gpt::Gpt) -> Result<Option<crate::boards::FamilyId>> {
    let from_identity = g
        .find("sn_mac")
        .and_then(|p| t.read(p.offset(), 4096).ok())
        .and_then(|a| crate::snmac::SnMac::parse(&a))
        .and_then(|sm| crate::boards::family_of_mainboard(&sm.board()).map(|f| f.id));
    Ok(from_identity.or(crate::loader::inspect(&t.read(crate::loader::SPL_OFFSET, crate::loader::SPL_SIZE)?).family))
}

/// Puts the chosen boot picture into the given slots ("a", "b"); slots without a valid image
/// are skipped. Returns whether anything was (or would be) written.
fn apply_picture(
    t: &restore::Target,
    g: &crate::gpt::Gpt,
    images: Option<&PathBuf>,
    family: crate::boards::FamilyId,
    want: crate::picture::Kind,
    slots: &[&str],
    execute: bool,
) -> Result<bool> {
    use crate::picture as P;
    let mut changed = false;
    for slot in slots {
        let part = if *slot == "b" { "rtos2" } else { "rtos" };
        let (info, cur, kept) = picture_of(t, g, part)?;
        let (Some(p), Some(cur)) = (g.find(part), cur) else { continue };
        let s = slot.to_uppercase();
        match (want, info.kind) {
            (P::Kind::Nocry, P::Kind::Firmware) | (P::Kind::Nocry, P::Kind::Nocry) => {
                let img = nocry_picture_image(images, family).context("this copy of nocry has no boot picture image for this board family")?;
                if cur == img {
                    continue;
                }
                changed = true;
                say!("slot {s}: nocry boot picture{}", if info.kind == P::Kind::Firmware { " (the firmware's own is kept)" } else { " (updated)" });
                if execute {
                    if info.kind == P::Kind::Firmware && kept.as_deref() != Some(&cur[..]) {
                        t.write_verified(p.offset() + P::KEPT_OFFSET, &cur, part)?;
                        t.write_verified(p.offset() + P::KEPT_MARK_OFFSET, &P::kept_mark(&cur), part)?;
                    }
                    // the loader reads exactly the image; clear what a longer old one leaves behind it
                    let mut data = img.clone();
                    data.resize(cur.len().max(img.len()), 0);
                    t.write_verified(p.offset(), &data, part)?;
                }
            }
            (P::Kind::Firmware, P::Kind::Nocry) => {
                let Some(fw) = kept else {
                    bail!("slot {s} holds no copy of its firmware's boot picture; install the firmware again to get it back. Nothing was changed in this slot");
                };
                changed = true;
                say!("slot {s}: the firmware's own boot picture");
                if execute {
                    let mut data = fw.clone();
                    data.resize(cur.len().max(fw.len()), 0);
                    t.write_verified(p.offset(), &data, part)?;
                }
            }
            _ => {}
        }
    }
    Ok(changed)
}

/// Which boot picture each slot shows.
pub fn picture_status(soc: &SocProfile, via: Via, images: Option<&PathBuf>, target: Option<&PathBuf>) -> Result<PictureStatus> {
    set_picture(soc, via, images, None, false, target)
}

/// Choose the boot picture for every slot that holds firmware. `want` None only reports.
pub fn set_picture(
    soc: &SocProfile,
    via: Via,
    images: Option<&PathBuf>,
    want: Option<PictureChoice>,
    execute: bool,
    target: Option<&PathBuf>,
) -> Result<PictureStatus> {
    use crate::picture as P;
    let write = execute && want.is_some();
    let t = open_target(soc, target, if write { Want::Rw } else { Want::PreferRw }, write, false, images, None, via)?;
    let g = crate::gpt::Gpt::parse(&t.read(0, crate::gpt::GPT_AREA)?)
        .context("this board has no valid partition table (blank board?); install firmware first")?;
    let family = board_family(&t, &g)?;
    let mut changed = false;
    if let Some(w) = want {
        let fam = family.context("nocry cannot tell which board family this is")?;
        let kind = match w {
            PictureChoice::Nocry => P::Kind::Nocry,
            PictureChoice::Firmware => P::Kind::Firmware,
            PictureChoice::Default if picture_proven(fam) => P::Kind::Nocry,
            PictureChoice::Default => P::Kind::Firmware,
        };
        changed = apply_picture(&t, &g, images, fam, kind, &["a", "b"], execute)?;
    }
    Ok(PictureStatus {
        a: picture_of(&t, &g, "rtos")?.0,
        b: picture_of(&t, &g, "rtos2")?.0,
        family,
        proven: family.is_some_and(picture_proven),
        available: family.is_some_and(|f| nocry_picture_image(images, f).is_some()),
        changed,
    })
}

/// What `loader_status` and the install/restore calls report.
#[derive(Debug, Clone)]
pub struct LoaderStatus {
    /// the loader on the board now
    pub on_board: crate::loader::Info,
    /// board family, from the identity record or else from the loader
    pub family: Option<crate::boards::FamilyId>,
    /// a copy of the factory loader is kept on the board
    pub factory_kept: bool,
    /// the nocryboot build this nocry would install, if it has one for the family
    pub available: Option<crate::loader::Info>,
    /// why nocryboot cannot be installed on this board, if so
    pub blocked: Option<String>,
    /// something was (or with `execute` would be) written
    pub changed: bool,
}

/// nocryboot's image for a board family: `<images>/nocryboot/nocryboot-<family>.bin`
/// (in a source tree: tools/nocryboot-build/out).
pub fn nocryboot_image(images: Option<&PathBuf>, family: crate::boards::FamilyId) -> Option<Vec<u8>> {
    let name = match family {
        crate::boards::FamilyId::Nebula => "nocryboot-nebula.bin",
        crate::boards::FamilyId::K1 => "nocryboot-k1.bin",
    };
    let dir = images_dir(images).ok()?;
    [dir.join("nocryboot").join(name), dir.join("../../nocryboot-build/out").join(name)]
        .iter()
        .find_map(|p| std::fs::read(p).ok())
        .filter(|b| b.len() == crate::loader::SPL_SIZE)
}

fn loader_state(t: &restore::Target, images: Option<&PathBuf>) -> Result<(crate::gpt::Gpt, LoaderStatus, Option<Vec<u8>>, Option<Vec<u8>>)> {
    use crate::loader as L;
    let g = crate::gpt::Gpt::parse(&t.read(0, crate::gpt::GPT_AREA)?)
        .context("this board has no valid partition table (blank board?); install firmware first")?;
    let on_board = L::inspect(&t.read(L::SPL_OFFSET, L::SPL_SIZE)?);
    let from_identity = g
        .find("sn_mac")
        .and_then(|p| t.read(p.offset(), 4096).ok())
        .and_then(|a| crate::snmac::SnMac::parse(&a))
        .and_then(|sm| crate::boards::family_of_mainboard(&sm.board()).map(|f| f.id));
    let kept = L::saved_loader(&t.read(L::SAVED_MARK_OFFSET, 512)?, &t.read(L::SAVED_OFFSET, L::SPL_SIZE)?);
    let family = from_identity.or(on_board.family);
    let image = family.and_then(|f| nocryboot_image(images, f));
    let available = image.as_deref().map(L::inspect);
    let blocked = match (family, &on_board.kind) {
        (None, _) => Some("nocry cannot tell which board family this is".to_string()),
        (_, L::Kind::Unknown) => Some("the loader on this board is not one nocry knows; it is left alone".to_string()),
        (Some(f), _) if from_identity.is_some() && on_board.family.is_some() && on_board.family != Some(f) => {
            Some("the board's identity record and its loader name different board families".to_string())
        }
        (Some(f), _) => L::check_layout(&g, f).err().map(|e| e.to_string()).or_else(|| match &available {
            None => Some("this copy of nocry has no nocryboot image for this board family".to_string()),
            Some(a) if a.family != Some(f) || !matches!(a.kind, L::Kind::Nocryboot { .. }) => {
                Some("the nocryboot image found is not for this board family".to_string())
            }
            Some(L::Info { kind: L::Kind::Nocryboot { version: Some(v) }, .. }) if v.contains("test") => {
                Some("the nocryboot image found is a test build".to_string())
            }
            _ => None,
        }),
    };
    let st = LoaderStatus { on_board, family, factory_kept: kept.is_some(), available, blocked, changed: false };
    Ok((g, st, image, kept))
}

/// Which first-stage loader the board has, and whether nocryboot can be installed.
pub fn loader_status(soc: &SocProfile, via: Via, images: Option<&PathBuf>, target: Option<&PathBuf>) -> Result<LoaderStatus> {
    let t = open_target(soc, target, Want::PreferRw, false, false, images, None, via)?;
    Ok(loader_state(&t, images)?.1)
}

/// A loader from a file: the 24 KiB loader itself, or a backup image of an eMMC that contains it.
fn read_loader_file(p: &PathBuf) -> Result<Vec<u8>> {
    use std::io::{Read as _, Seek as _};
    let mut f = std::fs::File::open(p).with_context(|| format!("{}", p.display()))?;
    let mut b = vec![0u8; crate::loader::SPL_SIZE];
    if f.metadata()?.len() > crate::loader::SPL_OFFSET + crate::loader::SPL_SIZE as u64 {
        f.seek(std::io::SeekFrom::Start(crate::loader::SPL_OFFSET))?;
    }
    f.read_exact(&mut b).with_context(|| format!("{} is too short to hold a loader", p.display()))?;
    Ok(b)
}

fn save_loader_copy(dir: Option<&std::path::Path>, t: &restore::Target, what: &str, loader: &[u8]) -> Result<()> {
    if let Some(dir) = dir {
        std::fs::create_dir_all(dir)?;
        let ts = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_secs();
        // "fastboot:<serial>" for a board, a file name for a disk image
        let name = t.path.to_string_lossy();
        let id: String = name.rsplit(['/', '\\', ':']).next().unwrap_or("").chars().filter(|c| c.is_ascii_alphanumeric()).collect();
        let f = dir.join(format!("{what}-loader-{id}-{ts}.bin"));
        std::fs::write(&f, loader)?;
        say!("copy of the {what} loader saved to {}", f.display());
    }
    Ok(())
}

/// Install nocryboot as the board's first-stage loader. A factory loader is first kept on the
/// board (unused end of the boot area) and, with `safety_dir`, on this computer. Without
/// `execute` nothing is written; the report says what would happen.
pub fn install_loader(
    soc: &SocProfile,
    via: Via,
    images: Option<&PathBuf>,
    safety_dir: Option<&std::path::Path>,
    keep_file: Option<&PathBuf>,
    execute: bool,
    target: Option<&PathBuf>,
) -> Result<LoaderStatus> {
    use crate::loader as L;
    let t = open_target(soc, target, if execute { Want::Rw } else { Want::PreferRw }, execute, false, images, None, via)?;
    let (_g, mut st, image, mut kept) = loader_state(&t, images)?;
    if let Some(why) = &st.blocked {
        bail!("nocryboot cannot be installed: {why}. Nothing was changed");
    }
    // A board whose factory loader was replaced before nocry kept copies: take it from a file.
    if let (Some(p), None, false) = (keep_file, &kept, st.on_board.kind == L::Kind::Factory) {
        let f = read_loader_file(p)?;
        let fi = L::inspect(&f);
        if fi.kind != L::Kind::Factory || fi.family != st.family {
            bail!("{} does not hold the factory loader of this board family ({}). Nothing was changed", p.display(), fi.describe());
        }
        if t.read(L::SAVED_MARK_OFFSET, 512 + L::SPL_SIZE)?.iter().any(|&b| b != 0) {
            bail!("the place in the boot area where nocry keeps the factory loader is not empty on this board. Nothing was changed");
        }
        say!("factory loader to keep on the board: {}", fi.describe());
        st.changed = true;
        if execute {
            t.write_verified(L::SAVED_OFFSET, &f, "kept factory loader")?;
            t.write_verified(L::SAVED_MARK_OFFSET, &L::saved_mark(&f), "kept factory loader")?;
            st.factory_kept = true;
            kept = Some(f);
        }
    }
    let image = image.context("no nocryboot image")?;
    let current = t.read(L::SPL_OFFSET, L::SPL_SIZE)?;
    if current == image {
        say!("this board already has {}", st.on_board.describe());
        return Ok(st);
    }
    let keep = st.on_board.kind == L::Kind::Factory;
    if keep && kept.is_none() {
        // the place for the copy must be unused
        let area = t.read(L::SAVED_MARK_OFFSET, 512 + L::SPL_SIZE)?;
        if area.iter().any(|&b| b != 0) {
            bail!("the place in the boot area where nocry keeps the factory loader is not empty on this board. Nothing was changed");
        }
    }
    say!("loader now:  {}", st.on_board.describe());
    say!("to install:  {}", st.available.as_ref().map(|a| a.describe()).unwrap_or_default());
    st.changed = true;
    if !execute {
        return Ok(st);
    }
    if keep {
        save_loader_copy(safety_dir, &t, "factory", &current)?;
        if kept.as_deref() != Some(&current[..]) {
            t.write_verified(L::SAVED_OFFSET, &current, "kept factory loader")?;
            t.write_verified(L::SAVED_MARK_OFFSET, &L::saved_mark(&current), "kept factory loader")?;
        }
        st.factory_kept = true;
        say!("factory loader kept on the board");
    }
    t.write_verified(L::SPL_OFFSET, &image, "boot loader")?;
    if t.read(L::SPL_OFFSET, L::SPL_SIZE)? != image {
        bail!("the loader does not read back as written; do not power the board off, and try again");
    }
    st.on_board = L::inspect(&image);
    say!("installed: {}", st.on_board.describe());
    Ok(st)
}

/// Put the factory loader back. It comes from the copy kept on the board, or from `file`
/// (a 24 KiB loader, or a backup image of this board's eMMC that contains it).
pub fn restore_factory_loader(
    soc: &SocProfile,
    via: Via,
    images: Option<&PathBuf>,
    file: Option<&PathBuf>,
    execute: bool,
    target: Option<&PathBuf>,
) -> Result<LoaderStatus> {
    use crate::loader as L;
    let t = open_target(soc, target, if execute { Want::Rw } else { Want::PreferRw }, execute, false, images, None, via)?;
    let (_g, mut st, _image, kept) = loader_state(&t, images)?;
    if st.on_board.kind == L::Kind::Factory {
        say!("this board already has the {}", st.on_board.describe());
        return Ok(st);
    }
    let factory = match file {
        Some(p) => read_loader_file(p)?,
        None => kept.context(
            "no copy of the factory loader is kept on this board. Give one with --file: a backup image of this \
             board made before nocryboot was installed contains it",
        )?,
    };
    let fi = L::inspect(&factory);
    if fi.kind != L::Kind::Factory {
        bail!("that is not a Creality factory loader ({}). Nothing was changed", fi.describe());
    }
    if st.family.is_some() && fi.family != st.family {
        bail!("that factory loader is for a different board family ({}). Nothing was changed", fi.describe());
    }
    say!("loader now:  {}", st.on_board.describe());
    say!("to install:  {}", fi.describe());
    st.changed = true;
    if !execute {
        return Ok(st);
    }
    if st.on_board.kind != L::Kind::Unknown {
        // the trial count means nothing to the factory loader; leave no half-finished trial behind
        if let Some(ota) = _g.find("ota") {
            if BootNote::parse(&t.read(ota.offset() + 512, 512)?).is_some() {
                t.write_verified(ota.offset() + 512, &[0u8; 512], "ota")?;
            }
        }
    }
    t.write_verified(L::SPL_OFFSET, &factory, "boot loader")?;
    if t.read(L::SPL_OFFSET, L::SPL_SIZE)? != factory {
        bail!("the loader does not read back as written; do not power the board off, and try again");
    }
    st.on_board = fi;
    say!("installed: {}", st.on_board.describe());
    Ok(st)
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

/// Result of a partition or whole-storage operation.
#[derive(Debug, Clone, Default)]
pub struct PartReport {
    pub name: String,
    pub bytes: u64,
    /// bytes that had to be written (restore/erase); 0 = already as wanted
    pub bytes_written: u64,
    pub sha256: String,
    pub executed: bool,
}

const PIECE: u64 = 32 << 20;

fn find_part(t: &restore::Target, name: &str) -> Result<(u64, u64)> {
    let g = crate::gpt::Gpt::parse(&t.read(0, crate::gpt::GPT_AREA)?)
        .context("this board has no valid partition table (blank board?)")?;
    let p = g.find(name).with_context(|| format!("no partition named {name:?}"))?;
    Ok((p.offset(), p.size()))
}

fn safety_copy(t: &restore::Target, dir: Option<&std::path::Path>, why: &str) -> Result<()> {
    if let Some(dir) = dir {
        std::fs::create_dir_all(dir)?;
        let ts = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_secs();
        let f = dir.join(format!("before-{why}-{ts}.first3MiB.bin"));
        std::fs::write(&f, t.read(0, 3 << 20)?)?;
        say!("safety copy of the first 3 MiB saved to {}", f.display());
    }
    Ok(())
}

/// Save one partition to `out` (+ `out.sha256`). The board is only read.
pub fn partition_backup(soc: &SocProfile, via: Via, name: &str, out: &std::path::Path, target: Option<&PathBuf>) -> Result<PartReport> {
    use sha2::Digest as _;
    use std::io::Write as _;
    let t = open_target(soc, target, Want::Ro, false, true, None, None, via)?;
    let (off, size) = find_part(&t, name)?;
    let part = out.with_extension("part");
    let r = (|| -> Result<String> {
        let mut f = std::fs::File::create(&part).with_context(|| format!("creating {}", part.display()))?;
        let mut h = sha2::Sha256::new();
        let mut done = 0;
        while done < size {
            crate::events::check_cancelled()?;
            let n = PIECE.min(size - done) as usize;
            let b = t.read(off + done, n)?;
            h.update(&b);
            f.write_all(&b)?;
            done += n as u64;
            crate::events::progress("reading", done, size, "");
        }
        f.sync_all()?;
        Ok(format!("{:x}", h.finalize()))
    })();
    crate::events::progress_end();
    let sha256 = match r {
        Ok(s) => s,
        Err(e) => {
            let _ = std::fs::remove_file(&part);
            return Err(e);
        }
    };
    std::fs::rename(&part, out)?;
    let fname = out.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default();
    std::fs::write(format!("{}.sha256", out.display()), format!("{sha256}  {fname}\n"))?;
    say!("saved partition {name} ({size} bytes) to {}", out.display());
    Ok(PartReport { name: name.into(), bytes: size, sha256, executed: true, ..Default::default() })
}

/// Make `len` bytes at `off` equal to what `src(piece_offset, n)` returns; only pieces that
/// differ are written, and everything written is verified. Returns bytes written.
fn write_range(t: &restore::Target, off: u64, len: u64, what: &str, src: impl FnMut(u64, usize) -> Result<Vec<u8>>) -> Result<u64> {
    let mut m = crate::events::Meter::new(what, len);
    let r = write_range_m(t, off, len, what, src, &mut m);
    m.end();
    r
}

/// `write_range` as one part of a larger operation: progress goes to the operation's meter.
fn write_range_m(
    t: &restore::Target, off: u64, len: u64, what: &str, mut src: impl FnMut(u64, usize) -> Result<Vec<u8>>, m: &mut crate::events::Meter,
) -> Result<u64> {
    const STEP: u64 = 8 << 20;
    let mut written = 0;
    let mut done = 0;
    while done < len {
        crate::events::check_cancelled()?;
        let n = STEP.min(len - done) as usize;
        let data = src(done, n)?;
        if !t.matches(off + done, &data)? {
            t.write_raw(off + done, &data, what)?;
            t.sync()?;
            if !t.matches(off + done, &data)? {
                bail!("verify FAILED for {what} at {:#x}", off + done);
            }
            written += n as u64;
        }
        done += n as u64;
        m.add(n as u64);
    }
    Ok(written)
}

/// Write a file into a partition (from its start; the file may be shorter than the partition).
pub fn partition_restore(
    soc: &SocProfile, via: Via, name: &str, file: &std::path::Path, safety_dir: Option<&std::path::Path>, target: Option<&PathBuf>,
) -> Result<PartReport> {
    use std::io::{Read as _, Seek as _};
    let mut f = std::fs::File::open(file).with_context(|| format!("opening {}", file.display()))?;
    let flen = f.seek(std::io::SeekFrom::End(0))?;
    let t = open_target(soc, target, Want::Rw, true, false, None, None, via)?;
    let (off, size) = find_part(&t, name)?;
    if flen == 0 {
        bail!("{} is empty", file.display());
    }
    if flen > size {
        bail!("{} is {flen} bytes, larger than partition {name} ({size} bytes)", file.display());
    }
    safety_copy(&t, safety_dir, "partition-restore")?;
    let written = write_range(&t, off, flen, &format!("Writing {name}"), |o, n| {
        let mut v = vec![0u8; n];
        f.seek(std::io::SeekFrom::Start(o))?;
        f.read_exact(&mut v)?;
        Ok(v)
    })?;
    say!("partition {name}: {written} of {flen} bytes needed writing; verified");
    Ok(PartReport { name: name.into(), bytes: flen, bytes_written: written, executed: true, ..Default::default() })
}

/// Fill a partition with zeros.
pub fn partition_erase(soc: &SocProfile, via: Via, name: &str, safety_dir: Option<&std::path::Path>, target: Option<&PathBuf>) -> Result<PartReport> {
    let t = open_target(soc, target, Want::Rw, true, false, None, None, via)?;
    let (off, size) = find_part(&t, name)?;
    safety_copy(&t, safety_dir, "partition-erase")?;
    let written = write_range(&t, off, size, &format!("Erasing {name}"), |_, n| Ok(vec![0u8; n]))?;
    say!("partition {name} erased ({written} bytes needed writing); verified");
    Ok(PartReport { name: name.into(), bytes: size, bytes_written: written, executed: true, ..Default::default() })
}

/// Fill the whole eMMC with zeros: partition table, bootloader, firmware, identity, data.
/// The board is blank afterwards and only starts in USB boot mode until it is rebuilt.
pub fn erase_disk(soc: &SocProfile, via: Via, safety_dir: Option<&std::path::Path>, target: Option<&PathBuf>) -> Result<PartReport> {
    let t = open_target(soc, target, Want::Rw, true, false, None, None, via)?;
    safety_copy(&t, safety_dir, "full-erase")?;
    let written = write_range(&t, 0, t.size, "Erasing the whole storage", |_, n| Ok(vec![0u8; n]))?;
    say!("the whole storage is erased ({written} bytes needed writing); verified");
    Ok(PartReport { name: "(entire storage)".into(), bytes: t.size, bytes_written: written, executed: true, ..Default::default() })
}

/// Write the identity record (serial number, MAC address, model, mainboard). Empty `model`
/// or `board` keep the current values (or the Nebula defaults on a record-less board).
pub fn set_identity(
    soc: &SocProfile, via: Via, sn: &str, mac: &str, model: &str, board: &str,
    safety_dir: Option<&std::path::Path>, target: Option<&PathBuf>,
) -> Result<crate::snmac::SnMac> {
    let t = open_target(soc, target, Want::Rw, true, false, None, None, via)?;
    let (off, _) = find_part(&t, "sn_mac")?;
    let mut rec = crate::snmac::SnMac::parse(&t.read(off, crate::snmac::RECORD_AREA)?).unwrap_or_default();
    rec.fields[0] = sn.trim().to_uppercase();
    rec.fields[1] = mac.trim().to_uppercase().replace([':', '-'], "");
    if !model.trim().is_empty() {
        rec.fields[2] = model.trim().to_string();
    } else if rec.fields[2].is_empty() {
        rec.fields[2] = "Nebula Pad".into();
    }
    if !board.trim().is_empty() {
        rec.fields[3] = board.trim().to_string();
    } else if rec.fields[3].is_empty() {
        // only the Nebula family has a single mainboard name; anything else has to be given
        if rec.fields[2] != "Nebula Pad" && crate::boards::LINES.iter().all(|l| l.model != Some(rec.fields[2].as_str()) || l.family != crate::boards::FamilyId::Nebula) {
            bail!("give the mainboard type as well (for a K1-family board: CR4CU220812S10, S11 or S12, as printed on the board)");
        }
        rec.fields[3] = crate::snmac::NEBULA_BOARD.into();
    }
    rec.validate_for_write()?;
    let mut area = rec.to_area()?;
    area.resize(4096, 0);
    safety_copy(&t, safety_dir, "identity-change")?;
    t.write_verified(off, &area, "sn_mac")?;
    say!("identity written and verified: model {:?}, serial number and MAC address updated", rec.model());
    Ok(rec)
}

/// Install firmware into the slot that is NOT active, leaving the running firmware, the slot
/// choice, the settings and the identity alone. Use `set_active_slot` afterwards to start it.
/// Only for firmware of the board's current model: both slots share one identity record and
/// one settings partition, so a different printer type in the other slot would not work.
pub fn install_other_slot(
    soc: &SocProfile, via: Via, os: &std::path::Path, execute: bool, safety_dir: Option<&std::path::Path>, target: Option<&PathBuf>,
    picture: PictureChoice,
) -> Result<InstallReport> {
    let pkg = restore::Package::load(os)?;
    let mut r = InstallReport {
        firmware_board: pkg.ota_board.clone().unwrap_or_default(),
        firmware_version: pkg.ota_version.clone().unwrap_or_default(),
        ..Default::default()
    };
    r.firmware_name = crate::snmac::product_name_for_ota_board(&r.firmware_board);
    let want = if execute { Want::Rw } else { Want::PreferRw };
    let t = open_target(soc, target, want, execute, false, None, None, via)?;
    let (g, slots) = read_slots(&t).context("installing to the other slot needs a board that already has firmware; use a normal install")?;
    let snp = g.find("sn_mac").context("no sn_mac partition")?;
    let cur = crate::snmac::SnMac::parse(&t.read(snp.offset(), crate::snmac::RECORD_AREA)?)
        .context("this board has no identity record; use a normal install")?;
    // Both slots share one identity record, so the firmware has to suit the printer type and
    // mainboard this board is set up as.
    let line = pkg.ota_board.as_deref().and_then(crate::boards::line);
    let Some(line) = line else {
        bail!("nocry does not know this firmware ({})", crate::boards::unknown_firmware(&r.firmware_board));
    };
    if let Some(f) = crate::boards::family_of_mainboard(cur.board()) {
        if f.id != line.family {
            bail!("this firmware ({}) is for the {}, but this board belongs to the {}", line.name, crate::boards::family(line.family).name, f.name);
        }
    }
    let fits = match line.model {
        Some(m) => m == cur.model(),
        None => pkg.support.is_empty() || crate::boards::supports(&pkg.support, cur.model(), cur.board()),
    };
    if !fits {
        bail!(
            "the other slot can only hold firmware for the printer type this board is set up as ({}), and this \
             firmware ({} {}) is not for it. To change printer type, use a clean install",
            crate::snmac::product_name(cur.model()),
            line.name,
            r.firmware_version
        );
    }
    r.model_before = cur.model().into();
    r.model_after = cur.model().into();
    r.model_before_name = crate::snmac::product_name(cur.model());
    r.model_after_name = r.model_before_name.clone();
    let slot = if slots.active == "a" { "b" } else { "a" };
    r.target_slot = slot.into();
    let active = if slots.active == "a" { &slots.a } else { &slots.b };
    r.device_firmware_name = active.firmware_name.clone();
    r.device_firmware = format!("{} {}", r.firmware_board, active.firmware_version);
    let sfx = if slot == "b" { "2" } else { "" };
    // (partition, image, zero the rest of the partition)
    let jobs = [(format!("rtos{sfx}"), &pkg.rtos, true), (format!("kernel{sfx}"), &pkg.kernel, true), (format!("rootfs{sfx}"), &pkg.rootfs, false)];
    let mut plan: Vec<(u64, Vec<u8>, String)> = vec![];
    for (name, img, pad) in &jobs {
        let p = g.find(name).with_context(|| format!("no {name} partition"))?;
        if img.len() as u64 > p.size() {
            bail!("{name}: the image ({} bytes) does not fit the partition ({} bytes)", img.len(), p.size());
        }
        let mut data = (*img).clone();
        if *pad {
            data.resize(p.size() as usize, 0);
        }
        plan.push((p.offset(), data, name.clone()));
    }
    for (off, data, name) in &plan {
        let mut differ = 0u64;
        for (i, c) in data.chunks(1 << 20).enumerate() {
            if !t.matches(off + ((i as u64) << 20), c)? {
                differ += c.len() as u64;
            }
        }
        r.details += &format!("  {name}: {} of {} bytes differ\n", differ, data.len());
        r.bytes_to_write += differ;
    }
    let fam = board_family(&t, &g)?;
    let nocry_picture = match (picture, fam) {
        (PictureChoice::Firmware, _) | (_, None) => false,
        (PictureChoice::Nocry, Some(f)) => nocry_picture_image(None, f).is_some(),
        (PictureChoice::Default, Some(f)) => picture_proven(f) && nocry_picture_image(None, f).is_some(),
    };
    r.nocry_picture = nocry_picture;
    if nocry_picture {
        r.notes.push("The nocry boot picture is shown when this slot starts. Creality's is kept on the board and can be brought back (Advanced, Boot picture).".into());
    }
    let picture_only = r.bytes_to_write == 0
        && match (nocry_picture, fam) {
            (true, Some(f)) => apply_picture(&t, &g, None, f, crate::picture::Kind::Nocry, &[slot], false)?,
            _ => false,
        };
    if picture_only {
        r.bytes_to_write = nocry_picture_image(None, fam.unwrap()).map_or(0, |i| i.len() as u64);
        r.details += "  rtos: the nocry boot picture\n";
    }
    if !execute {
        return Ok(r);
    }
    if picture_only {
        r.executed = apply_picture(&t, &g, None, fam.unwrap(), crate::picture::Kind::Nocry, &[slot], true)?;
        return Ok(r);
    }
    if r.bytes_to_write == 0 {
        return Ok(r);
    }
    safety_copy(&t, safety_dir, "slot-install")?;
    let mut m = crate::events::Meter::new(format!("Installing to slot {}", slot.to_uppercase()), plan.iter().map(|p| p.1.len() as u64).sum());
    for (off, data, name) in &plan {
        write_range_m(&t, *off, data.len() as u64, name, |o, n| Ok(data[o as usize..o as usize + n].to_vec()), &mut m)?;
    }
    m.end();
    if let (true, Some(f)) = (nocry_picture, fam) {
        apply_picture(&t, &g, None, f, crate::picture::Kind::Nocry, &[slot], true)?;
    }
    r.executed = true;
    say!("firmware {} installed into slot {} and verified; the active slot was not changed", r.firmware_version, slot.to_uppercase());
    Ok(r)
}

#[derive(Debug, Clone, Default)]
pub struct WipeReport {
    /// (partition, method the board used: secure-discard, discard or overwrite)
    pub partitions: Vec<(String, String)>,
    pub bytes: u64,
}

/// Factory reset that also makes the old settings and files unrecoverable: both data
/// partitions are erased completely (eMMC secure discard where the card supports it,
/// otherwise overwritten), then checked to be all zeros. Firmware and identity stay.
pub fn secure_wipe_data(soc: &SocProfile, via: Via, safety_dir: Option<&std::path::Path>, target: Option<&PathBuf>) -> Result<WipeReport> {
    let t = open_target(soc, target, Want::Rw, true, false, None, None, via)?;
    let g = crate::gpt::Gpt::parse(&t.read(0, crate::gpt::GPT_AREA)?)
        .context("this board has no valid partition table (blank board?)")?;
    safety_copy(&t, safety_dir, "secure-erase")?;
    let mut r = WipeReport::default();
    let mut parts = vec![];
    for name in ["rootfs_data", "userdata"] {
        let p = g.find(name).with_context(|| format!("no {name} partition"))?;
        parts.push((name, p.offset(), p.size()));
    }
    let total: u64 = parts.iter().map(|p| p.2).sum();
    let mut m = crate::events::Meter::new("Secure erase", total);
    for &(name, off, size) in &parts {
        let method = t.secure_erase(off, size, name, &mut |n| m.add(n))?;
        r.partitions.push((name.to_string(), method));
    }
    m.end();
    // independent check: every byte must now be zero
    const STEP: u64 = 8 << 20;
    let zeros = vec![0u8; STEP as usize];
    let mut m = crate::events::Meter::new("Checking that everything is erased", total);
    for (i, &(name, off, size)) in parts.iter().enumerate() {
        let mut done = 0;
        while done < size {
            let n = STEP.min(size - done) as usize;
            if !t.matches(off + done, &zeros[..n])? {
                bail!("{name} is not fully erased at {:#x}", off + done);
            }
            done += n as u64;
            m.add(n as u64);
        }
        say!("{name}: erased ({}) and verified, {size} bytes", r.partitions[i].1);
        r.bytes += size;
    }
    m.end();
    Ok(r)
}

/// The board's own log of its current or last start, for diagnosis:
/// - recovery running: its live kernel log;
/// - loader (fastboot stage) running: the log the last recovery start left in memory, if any.
pub fn boot_log() -> Result<(String, String)> {
    if let Some(f) = usb::running_recovery(SOCS)?.into_iter().next() {
        let mut fb = fastboot::Fastboot::open(&f)?;
        fb.info = Box::new(|_| {});
        if !fb.getvar("nocry-ext").unwrap_or_default().contains("dmesg") {
            bail!("the recovery system running on the board is too old to give its log; restart the board in USB boot mode and try again");
        }
        return Ok(("recovery".into(), fb.dmesg()?));
    }
    if let Some(f) = usb::running_fastboot(SOCS)?.into_iter().next() {
        let mut fb = fastboot::Fastboot::open(&f)?;
        let lines = fb.info_lines("oem dmesg").context("the loader has no log of a previous recovery start")?;
        return Ok(("previous".into(), lines.join("\n")));
    }
    bail!("the log is only available while the board runs nocry's recovery system or loader")
}

/// Copy the firmware of slot `from` ("a" or "b") into the other slot: its rtos, kernel and
/// rootfs partitions, byte for byte, verified. The active-slot record, settings and identity
/// are not touched. Only parts that differ are written.
pub fn copy_slot(soc: &SocProfile, via: Via, from: &str, safety_dir: Option<&std::path::Path>, target: Option<&PathBuf>) -> Result<Slots> {
    if from != "a" && from != "b" {
        bail!("slot must be a or b");
    }
    let t = open_target(soc, target, Want::Rw, true, false, None, None, via)?;
    let (g, s) = read_slots(&t)?;
    if !(if from == "a" { &s.a } else { &s.b }).bootable() {
        bail!("slot {} has no complete firmware to copy", from.to_uppercase());
    }
    let to = if from == "a" { "b" } else { "a" };
    let sfx = |slot: &str| if slot == "b" { "2" } else { "" };
    let mut jobs = vec![];
    for base in ["rtos", "kernel", "rootfs"] {
        let (sn, dn) = (format!("{base}{}", sfx(from)), format!("{base}{}", sfx(to)));
        let sp = g.find(&sn).with_context(|| format!("no {sn} partition"))?;
        let dp = g.find(&dn).with_context(|| format!("no {dn} partition"))?;
        if sp.size() != dp.size() {
            bail!("{sn} and {dn} differ in size ({} and {} bytes); the slot cannot be copied", sp.size(), dp.size());
        }
        jobs.push((sp.offset(), dp.offset(), sp.size(), dn));
    }
    safety_copy(&t, safety_dir, "slot-copy")?;
    let mut written = 0;
    let mut m = crate::events::Meter::new(format!("Copying slot {} to slot {}", from.to_uppercase(), to.to_uppercase()), jobs.iter().map(|j| j.2).sum());
    for (src, dst, size, name) in &jobs {
        written += write_range_m(&t, *dst, *size, name, |o, n| t.read(src + o, n), &mut m)?;
    }
    m.end();
    say!("slot {} copied to slot {} and verified ({} bytes written); the active slot was not changed", from.to_uppercase(), to.to_uppercase(), written);
    Ok(read_slots(&t)?.1)
}

#[cfg(test)]
mod boot_note_tests {
    use super::BootNote;

    fn sec(t: &str) -> Vec<u8> {
        let mut v = vec![0u8; 512];
        v[..t.len()].copy_from_slice(t.as_bytes());
        v
    }

    #[test]
    fn parses_what_the_loader_writes() {
        assert_eq!(BootNote::parse(&sec("nocryboot:try=B:left=3\n")), Some(BootNote::Trial { slot: "b".into(), left: 3 }));
        assert_eq!(BootNote::parse(&sec("nocryboot:try=A:left=0\n")), Some(BootNote::Trial { slot: "a".into(), left: 0 }));
        assert_eq!(BootNote::parse(&sec("nocryboot:failed=B\n")), Some(BootNote::Failed("b".into())));
        assert_eq!(BootNote::parse(&sec("nocryboot:incomplete=A\n")), Some(BootNote::Incomplete("a".into())));
        assert_eq!(BootNote::parse(&sec("nocryboot:unconfirmed=A\n")), Some(BootNote::Unconfirmed("a".into())));
    }

    #[test]
    fn ignores_everything_else() {
        assert_eq!(BootNote::parse(&[0u8; 512]), None);
        assert_eq!(BootNote::parse(&sec("ota:kernel2\n")), None);
        assert_eq!(BootNote::parse(&sec("nocryboot:try=C:left=3\n")), None);
        assert_eq!(BootNote::parse(&sec("nocryboot:try=B:left=12\n")), None);
        assert_eq!(BootNote::parse(&[0xffu8; 512]), None);
    }

    #[test]
    fn the_script_matches_the_note_format() {
        // the script compares the first 14 bytes of the note with this text
        assert!(super::CONFIRM_SCRIPT.contains("nocryboot:try=*)"));
        assert_eq!("nocryboot:try=B:left=3".len(), 22);
        assert!(super::CONFIRM_SCRIPT.contains("skip=512 count=22"));
    }
}
