//! nocry: USB-boot loader for Ingenic X2000(E) boards (Creality K1 family).
//!
//! Loads a stage1 image (parameter header + SPL, see tools/splhdr) and a stage2
//! (e.g. nocry-ums) through the SoC bootrom. Speaks bootrom requests 0x00-0x05 only.


use anyhow::{bail, Context, Result};
use nocry_core::session::*;
use nocry_core::{bootrom, catalog, diskimage, fastboot, gpt, restore, snmac, usb};
use bootrom::{Bootrom, SocProfile, SOCS};
use clap::{Parser, Subcommand};
use std::path::PathBuf;
use std::time::Duration;

#[derive(Parser)]
#[command(name = "nocry", version, about = "USB-boot loader for Ingenic X2000(E) boards")]
struct Cli {
    /// SoC profile
    #[arg(long, global = true, default_value = "x2000")]
    soc: String,
    #[arg(short, long, global = true)]
    verbose: bool,
    /// Directory with built images (spl-x2000e/, nocry_ums_x2000_v12/, nocry_ums_ro_x2000_v12/);
    /// default: $NOCRY_IMAGES, next to the binary, or the project's tools/burner-build/out
    #[arg(long, global = true)]
    images: Option<PathBuf>,
    /// BUS:ADDR when several boards are attached
    #[arg(long, global = true)]
    device: Option<String>,
    /// How to reach the eMMC when nocry has to boot something: the recovery Linux (fast,
    /// HS200 8-bit) or the U-Boot USB-disk stage2. auto = Linux if its image is built, else U-Boot
    #[arg(long, global = true, value_enum, default_value_t = Via::Auto)]
    via: Via,
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// List boards in USB boot mode and running nocry stage2s
    List,
    /// Ask the bootrom for its CPU info string
    Info,
    /// Check images without touching USB
    Inspect {
        #[arg(long)]
        spl: Option<PathBuf>,
        #[arg(long)]
        stage2: Option<PathBuf>,
    },
    /// Load stage1 (SPL) and stage2 and run them
    Boot {
        /// Stage1 image: parameter header + SPL (e.g. spl-x2000e.bin)
        #[arg(long)]
        spl: PathBuf,
        /// Stage2 image (e.g. nocry_ums_ro_x2000/u-boot.bin)
        #[arg(long)]
        stage2: PathBuf,
        /// Seconds to wait for the SPL to hand control back
        #[arg(long, default_value_t = 5.0)]
        spl_timeout: f32,
        /// Seconds to wait for the stage2 USB device afterwards (0 = don't wait)
        #[arg(long, default_value_t = 20.0)]
        wait: f32,
        /// Proceed despite image check errors
        #[arg(long)]
        force: bool,
    },
    /// Export the board's eMMC as a USB disk (boots stage1+stage2 if the board is in USB boot mode)
    Ums {
        /// Read-only stage2 (for backups); default read-write
        #[arg(long)]
        ro: bool,
    },
    /// Start nocry fastboot (boots stage1+stage2 if the board is in USB boot mode); then use the
    /// stock `fastboot` tool: flash/erase/fetch by GPT name, mmc0, gpt, bootloader, kernel_a/_b...
    Fastboot,
    /// Erase settings and files (rootfs_data, userdata) so they cannot be recovered: eMMC
    /// secure erase where the card supports it, else overwrite; verified. Firmware stays.
    SecureErase {
        #[arg(long)]
        yes: bool,
    },
    /// Show the two firmware slots, or with --set a|b choose the one that starts
    Slots {
        #[arg(long)]
        set: Option<String>,
    },
    /// Copy the firmware of one slot (a or b) into the other, verified. The active slot,
    /// settings and identity are not changed.
    CopySlot {
        from: String,
        #[arg(long)]
        yes: bool,
    },
    /// Print the board's own log (recovery kernel log, or what the last start left in memory)
    BootLog,
    /// Boot the nocry recovery Linux from RAM (via nocry fastboot; nothing is written) and wait
    /// for adb. --ums also exports the eMMC as a USB disk (HS200 8-bit; restore/snmac use it).
    Recovery {
        /// Recovery image (uImage); default: next to the binary or tools/recovery-build/out
        #[arg(long)]
        image: Option<PathBuf>,
        /// Also export the eMMC as a USB disk
        #[arg(long, value_enum)]
        ums: Option<UmsMode>,
        /// Extra kernel command line arguments
        #[arg(long)]
        cmdline: Option<String>,
        /// Seconds to wait for the recovery to come up
        #[arg(long, default_value_t = 60.0)]
        wait: f32,
    },
    /// Full-disk backup to an image file (+ .sha256), verified by a second read of the device
    Backup {
        /// Output image, e.g. backups/nebula-2026-11-25.img
        #[arg(short, long)]
        output: PathBuf,
        /// Block device or image to read (default: auto, preferring the recovery Linux)
        #[arg(long)]
        target: Option<PathBuf>,
        /// Skip the verification pass
        #[arg(long)]
        no_verify: bool,
        /// Overwrite an existing output file
        #[arg(long)]
        force: bool,
    },
    /// Write a full-disk image (from `nocry backup`) back to the board. Only differing chunks
    /// are written and every write is verified. Dry run unless --yes
    WriteImage {
        image: PathBuf,
        #[arg(long)]
        target: Option<PathBuf>,
        /// Allow writing an image whose sn_mac belongs to a different board (clones SN/MAC)
        #[arg(long)]
        allow_identity_change: bool,
        #[arg(long)]
        allow_any_device: bool,
        #[arg(long)]
        yes: bool,
    },
    /// List the Creality firmware nocry can download and install (--refresh asks Creality for
    /// the current list)
    Catalog {
        #[arg(long)]
        refresh: bool,
    },
    /// Download a firmware from the list (verified, cached). MODEL: NEBULA, F005, F003, F004;
    /// VERSION: e.g. 1.1.0.30 or "latest". Prints the local path for --os
    Fetch { model: String, version: String },
    /// Low-level access through fastboot (the recovery's nocry-fbd, or the U-Boot fastboot stage2)
    #[command(subcommand)]
    Fb(FbCmd),
    /// Provision/restore an eMMC (blank or not) like Creality's factory flash.
    /// Without --target: uses a running nocry-ums disk, or boots one if the board is in USB boot mode.
    Restore {
        /// Block device or image file (default: auto, see above)
        #[arg(long)]
        target: Option<PathBuf>,
        /// Firmware: a .ingenic package or a directory from extract_creality_ota.py
        #[arg(long)]
        os: PathBuf,
        /// Boot area (.ingenic or u-boot-with-spl-mbr-gpt.bin); default: from --os if it has one
        #[arg(long)]
        boot: Option<PathBuf>,
        /// sn_mac model field (default: derived from the firmware: NEBULA->"Nebula Pad", F00x->F00x)
        #[arg(long)]
        model: Option<String>,
        #[arg(long)]
        board: Option<String>,
        /// Use this serial (14 hex) instead of keeping/generating one (needs --mac)
        #[arg(long)]
        sn: Option<String>,
        /// Use this MAC (12 hex) instead of keeping/generating one (needs --sn)
        #[arg(long)]
        mac: Option<String>,
        /// 6-hex prefix for a generated serial (default random)
        #[arg(long)]
        sn_prefix: Option<String>,
        /// Generate a new SN/MAC even if the device has a valid one
        #[arg(long)]
        new_identity: bool,
        #[arg(long)]
        keep_userdata: bool,
        #[arg(long, value_enum, default_value_t = restore::Wipe::Quick)]
        wipe: restore::Wipe,
        /// Write the package's GPT verbatim (identical GUIDs on every unit, as the factory does)
        #[arg(long)]
        factory_guids: bool,
        /// Allow a model that does not match the firmware
        #[arg(long)]
        force_model: bool,
        /// Minimal repair: keep a valid boot area, the B slots and userdata; keep the
        /// overlay if the device already runs this firmware version; write only what differs
        #[arg(long)]
        repair: bool,
        /// With --repair: rewrite the boot area even if it looks valid
        #[arg(long)]
        force_boot: bool,
        /// Keep rootfs_data (the rootfs overlay) even if the firmware version changes
        #[arg(long, conflicts_with = "wipe_overlay")]
        keep_overlay: bool,
        /// Wipe rootfs_data even in --repair mode with the same firmware
        #[arg(long)]
        wipe_overlay: bool,
        /// Write every planned chunk even if it already matches (exercises the write path)
        #[arg(long)]
        rewrite: bool,
        /// Skip the "is this a nocry eMMC" block-device check
        #[arg(long)]
        allow_any_device: bool,
        /// Where generated identities are recorded
        #[arg(long, default_value = "nocry-identities.log")]
        identity_log: PathBuf,
        /// Actually write (default is a dry run)
        #[arg(long)]
        yes: bool,
    },
    /// Show (or change the model of) the sn_mac record (target auto-detected/booted if omitted)
    Snmac {
        #[arg(long)]
        target: Option<PathBuf>,
        #[arg(long)]
        set_model: Option<String>,
        #[arg(long)]
        allow_any_device: bool,
        #[arg(long)]
        yes: bool,
    },
}

#[derive(Subcommand)]
enum FbCmd {
    Getvar { name: String },
    /// Read NAME (partition, mmc0, gpt, bootloader...) to a file
    Fetch {
        name: String,
        #[arg(short, long)]
        output: PathBuf,
        /// byte offset (decimal or 0x..)
        #[arg(long, default_value = "0")]
        offset: String,
        /// bytes (default: to the end)
        #[arg(long)]
        size: Option<String>,
    },
    /// Write a file to NAME at an offset (streamed), then verify by on-board hashes
    Write {
        name: String,
        file: PathBuf,
        #[arg(long, default_value = "0")]
        offset: String,
    },
    /// Hash NAME on the board and print a digest of all chunk digests
    Hash {
        name: String,
        #[arg(long, value_enum, default_value_t = fastboot::Hash::Xxh64)]
        algo: fastboot::Hash,
        /// compare with this local file
        #[arg(long)]
        compare: Option<PathBuf>,
    },
}

fn num(s: &str) -> Result<u64> {
    let r = if let Some(h) = s.strip_prefix("0x") { u64::from_str_radix(h, 16) } else { s.parse() };
    r.with_context(|| format!("bad number {s:?}"))
}

fn cmd_fb(c: FbCmd) -> Result<()> {
    let f = usb::running_recovery(SOCS)?
        .into_iter()
        .chain(usb::running_fastboot(SOCS)?)
        .next()
        .context("no board with a fastboot interface (run `nocry recovery` or `nocry fastboot`)")?;
    let mut fb = fastboot::Fastboot::open(&f)?;
    let part_size = |fb: &mut fastboot::Fastboot, name: &str| -> Result<u64> {
        let v = fb.getvar(&format!("partition-size:{name}"))?;
        u64::from_str_radix(v.trim_start_matches("0x"), 16).context("bad partition-size")
    };
    const STEP: u64 = 256 << 20;
    const HCHUNK: u64 = 1 << 20;
    let rate = |bytes: u64, t: std::time::Instant| format!("{:.1}s, {:.1} MB/s", t.elapsed().as_secs_f64(), bytes as f64 / t.elapsed().as_secs_f64() / 1e6);
    match c {
        FbCmd::Getvar { name } => println!("{}", fb.getvar(&name)?),
        FbCmd::Fetch { name, output, offset, size } => {
            use std::io::Write;
            let off = num(&offset)?;
            let size = match size {
                Some(s) => num(&s)?,
                None => part_size(&mut fb, &name)? - off,
            };
            let mut out = std::fs::File::create(&output)?;
            let t = std::time::Instant::now();
            let mut done = 0;
            while done < size {
                let n = STEP.min(size - done);
                fb.fetch(&name, off + done, n, |b| out.write_all(b).map_err(Into::into))?;
                done += n;
            }
            println!("fetched {size} bytes of {name} in {}", rate(size, t));
        }
        FbCmd::Write { name, file, offset } => {
            let off = num(&offset)?;
            let mut data = read(&file)?;
            data.resize(data.len().div_ceil(512) * 512, 0);
            let t = std::time::Instant::now();
            for (i, c) in data.chunks(STEP as usize).enumerate() {
                fb.write(&name, off + i as u64 * STEP, c)?;
            }
            println!("wrote {} bytes to {name} in {}", data.len(), rate(data.len() as u64, t));
            let t = std::time::Instant::now();
            let dev = fb.hash(fastboot::Hash::Xxh64, &name, off, data.len() as u64, HCHUNK)?;
            let bad = data.chunks(HCHUNK as usize).zip(&dev).filter(|(c, d)| &fastboot::Hash::Xxh64.of(c) != *d).count();
            if bad > 0 || dev.len() != data.len().div_ceil(HCHUNK as usize) {
                bail!("VERIFY FAILED: {bad} of {} chunks differ on the board", dev.len());
            }
            println!("verified by on-board hashes in {}", rate(data.len() as u64, t));
        }
        FbCmd::Hash { name, algo, compare } => {
            let size = part_size(&mut fb, &name)?;
            let t = std::time::Instant::now();
            let mut dev = vec![];
            let mut done = 0;
            while done < size {
                let n = (1u64 << 30).min(size - done);
                dev.extend(fb.hash(algo, &name, done, n, HCHUNK)?);
                done += n;
            }
            println!("{} chunk hashes ({}) of {name} in {}", dev.len(), algo.name(), rate(size, t));
            if let Some(p) = compare {
                use std::io::Read;
                let mut f = std::fs::File::open(&p)?;
                let mut buf = vec![0u8; HCHUNK as usize];
                let mut differ = 0;
                for d in &dev {
                    let mut n = 0;
                    while n < buf.len() {
                        let k = f.read(&mut buf[n..])?;
                        if k == 0 {
                            break;
                        }
                        n += k;
                    }
                    buf[n..].fill(0);
                    if &algo.of(&buf) != d {
                        differ += 1;
                    }
                }
                println!("{differ} of {} MiB differ from {}", dev.len(), p.display());
            }
        }
    }
    Ok(())
}


fn cmd_recovery(
    soc: &SocProfile,
    images: Option<&PathBuf>,
    device: Option<&str>,
    image: Option<PathBuf>,
    ums: Option<UmsMode>,
    cmdline: Option<String>,
    wait: f32,
) -> Result<()> {
    let r = start_recovery(soc, images, device, image.as_ref(), ums, cmdline, wait)?;
    let serial = usb::serial_of(&r).unwrap_or_default();
    if ums.is_some() {
        let d = usb::block_device_for(&r, Duration::from_secs(20))
            .context("recovery is up but its USB disk did not appear (check dmesg)")?;
        usb::wait_accessible(&d, Duration::from_secs(10))?;
        if matches!(ums, Some(UmsMode::Ro)) && !d.read_only {
            bail!(
                "{} is NOT write-protected although --ums ro was asked for. Do not use it; \
                 BOOT+RESET the board (an older recovery image had this bug)",
                d.dev.display()
            );
        }
        println!(
            "eMMC exported as {} ({:.2} GiB, {})",
            d.dev.display(),
            d.bytes as f64 / (1u64 << 30) as f64,
            if d.read_only { "read-only" } else { "read-write" }
        );
    }
    // adb (android-tools) is optional
    let mut adb_ok = false;
    for _ in 0..40 {
        match std::process::Command::new("adb").arg("devices").output() {
            Ok(o) => {
                let out = String::from_utf8_lossy(&o.stdout);
                if out.lines().any(|l| l.starts_with(&serial) && l.ends_with("device")) {
                    adb_ok = true;
                    break;
                }
            }
            Err(_) => break, // no adb installed
        }
        std::thread::sleep(Duration::from_millis(500));
    }
    if adb_ok {
        println!("adb ready:  adb -s {serial} shell");
    } else {
        println!("adb not seen (is android-tools installed?); the USB console still works");
    }
    let console = if std::path::Path::new("/dev/nocry-console").exists() { "/dev/nocry-console" } else { "/dev/ttyACM*" };
    println!("console:    picocom {console}");
    Ok(())
}


fn main() -> Result<()> {
    let cli = Cli::parse();
    let soc = soc_by_name(&cli.soc)?;
    let images = cli.images.clone();
    let device = cli.device.clone();
    let via = cli.via;
    match cli.cmd {
        c @ Cmd::Restore { .. } => cmd_restore(c, soc, images.as_ref(), device.as_deref(), via)?,
        Cmd::Backup { output, target, no_verify, force } => {
            let t = open_target(soc, target.as_ref(), Want::Ro, false, true, images.as_ref(), device.as_deref(), via)?;
            println!("backing up {} ({:.2} GiB) to {}", t.path.display(), t.size as f64 / (1u64 << 30) as f64, output.display());
            diskimage::backup(&t, &output, !no_verify, force)?;
        }
        Cmd::WriteImage { image, target, allow_identity_change, allow_any_device, yes } => {
            let want = if yes { Want::Rw } else { Want::PreferRw };
            let t = open_target(soc, target.as_ref(), want, yes, allow_any_device, images.as_ref(), device.as_deref(), via)?;
            println!("target: {} ({} bytes){}", t.path.display(), t.size, if yes { "" } else { "  [dry run]" });
            diskimage::write_image(&t, &image, &diskimage::WriteImageOptions { yes, allow_identity_change })?;
        }
        Cmd::Snmac { target, set_model, allow_any_device, yes } => {
            let write = set_model.is_some() && yes;
            let want = if write { Want::Rw } else { Want::PreferRw };
            let t = open_target(soc, target.as_ref(), want, write, allow_any_device, images.as_ref(), device.as_deref(), via)?;
            cmd_snmac(t, set_model, yes)?
        }
        Cmd::SecureErase { yes } => {
            if !yes {
                bail!("this erases all settings and files on the board for good; add --yes to do it");
            }
            let w = secure_wipe_data(soc, Via::Auto, None, None)?;
            for (n, m) in &w.partitions {
                println!("{n}: {m}");
            }
            println!("erased and verified {} bytes", w.bytes);
        }
        Cmd::Slots { set } => {
            let s = match set {
                Some(x) => set_active_slot(soc, Via::Auto, &x.to_ascii_lowercase(), None, None)?,
                None => slots(soc, Via::Auto, None)?,
            };
            for (n, i) in [("A", &s.a), ("B", &s.b)] {
                println!("{n}: {} {}  rtos {}  kernel {}  startable {}", i.firmware_name, i.firmware_version, i.has_rtos, i.has_kernel, i.bootable());
            }
            println!("active: {}", s.active.to_uppercase());
        }
        Cmd::CopySlot { from, yes } => {
            let from = from.to_ascii_lowercase();
            if !yes {
                bail!("this replaces the firmware in the other slot; add --yes to do it");
            }
            let s = copy_slot(soc, Via::Auto, &from, None, None)?;
            println!("A: {} {}\nB: {} {}\nactive: {}", s.a.firmware_name, s.a.firmware_version, s.b.firmware_name, s.b.firmware_version, s.active.to_uppercase());
        }
        Cmd::BootLog => {
            let (kind, text) = boot_log()?;
            eprintln!("[{kind}]");
            println!("{text}");
        }
        Cmd::Fastboot => {
            ensure_fastboot(soc, images.as_ref(), device.as_deref())?;
            println!("ready. Try: fastboot getvar all | fastboot oem gpt | fastboot fetch sn_mac sn_mac.bin");
            println!("critical regions (mmc0, gpt, bootloader, sn_mac) need: fastboot flashing unlock_critical");
        }
        Cmd::Recovery { image, ums, cmdline, wait } => {
            cmd_recovery(soc, images.as_ref(), device.as_deref(), image, ums, cmdline, wait)?
        }
        Cmd::Fb(c) => cmd_fb(c)?,
        Cmd::Catalog { refresh } => {
            let dir = catalog::default_data_dir();
            let c = if refresh { catalog::refresh(&dir)? } else { catalog::load(Some(&dir)) };
            println!("firmware list of {} ({})", c.built, if c.updated { "updated from Creality" } else { "built in" });
            for m in &c.models {
                println!("{} ({})", m.name, m.key);
                for f in &m.firmwares {
                    let have = catalog::cached_path(&dir.join("firmware"), &f.url, &f.file).exists();
                    println!("  {:<12} {}  {:>4} MiB  {}{}", f.version, f.released, f.size >> 20, f.file, if have { "  [downloaded]" } else { "" });
                }
            }
        }
        Cmd::Fetch { model, version } => {
            let dir = catalog::default_data_dir();
            let c = catalog::load(Some(&dir));
            let (m, f) = catalog::find(&c, &model, &version)?;
            println!("{} firmware {} ({} MiB)", m.name, f.version, f.size >> 20);
            let p = catalog::download(&f.url, &f.file, f.size, &dir.join("firmware"))?;
            println!("{}", p.display());
        }
        Cmd::Ums { ro } => {
            let dev = ensure_ums(soc, if ro { Want::Ro } else { Want::Rw }, images.as_ref(), device.as_deref(), via)?;
            println!("ready: {}", dev.display());
        }
        Cmd::List => {
            let found = usb::scan(SOCS)?;
            if found.is_empty() {
                println!("no boards found");
            }
            for f in &found {
                println!("{}  {:04x}:{:04x}  port {:<10} {}", f.id(), f.vid, f.pid, f.port_path(), f.what);
            }
            for d in usb::running_ums(SOCS)? {
                println!(
                    "  eMMC: {} ({:.2} GiB, {})",
                    d.dev.display(),
                    d.bytes as f64 / (1u64 << 30) as f64,
                    if d.read_only { "read-only" } else { "read-write" }
                );
            }
            match images_dir(images.as_ref()) {
                Ok(d) => println!("images: {}", d.display()),
                Err(e) => println!("images: {e:#}"),
            }
        }
        Cmd::Info => {
            let f = usb::select(soc, device.as_deref())?;
            let mut b = Bootrom::new(usb::open(&f)?);
            println!("{}: cpu info {:?}", f.id(), b.cpu_info()?);
        }
        Cmd::Inspect { spl, stage2 } => {
            if spl.is_none() && stage2.is_none() {
                bail!("give --spl and/or --stage2");
            }
            let s1 = spl.as_ref().map(read).transpose()?;
            let s2 = stage2.as_ref().map(read).transpose()?;
            let n = check_images(soc, s1.as_deref(), s2.as_deref());
            if n > 0 {
                bail!("{n} error(s)");
            }
        }
        Cmd::Boot { spl, stage2, spl_timeout, wait, force } => {
            let s1 = read(&spl)?;
            let s2 = read(&stage2)?;
            if let Some(n) = boot_board(soc, &s1, &s2, device.as_deref(), spl_timeout, wait, force)? {
                if n.vid == usb::NOCRY_UMS_VID {
                    match usb::block_device_for(&n, Duration::from_secs(10)) {
                        Some(d) => println!(
                            "eMMC exported as {} ({:.2} GiB, {})",
                            d.dev.display(),
                            d.bytes as f64 / (1u64 << 30) as f64,
                            if d.read_only { "read-only" } else { "read-write" }
                        ),
                        None => println!("block device not found yet (check `lsblk` / dmesg)"),
                    }
                }
            }
        }
    }
    Ok(())
}

fn cmd_restore(cmd: Cmd, soc: &SocProfile, images: Option<&PathBuf>, device: Option<&str>, via: Via) -> Result<()> {
    let Cmd::Restore {
        target, os, boot, model, board, sn, mac, sn_prefix, new_identity, keep_userdata, wipe,
        factory_guids, force_model, repair, force_boot, keep_overlay, wipe_overlay, rewrite,
        allow_any_device, identity_log, yes,
    } = cmd else { unreachable!() };
    let pkg = restore::Package::load(&os)?;
    println!(
        "firmware: {} (ota_board_name={}, version={}){}",
        pkg.source,
        pkg.ota_board.as_deref().unwrap_or("?"),
        pkg.ota_version.as_deref().unwrap_or("?"),
        if pkg.boot.is_some() { ", includes boot area" } else { "" }
    );
    if pkg.ota_board.is_none() {
        println!("  note: could not read etc/ota_info (is unsquashfs installed?)");
    }
    let boot_bytes = boot.as_deref().map(restore::load_boot).transpose()?;
    let want = if yes { Want::Rw } else { Want::PreferRw };
    let t = open_target(soc, target.as_ref(), want, yes, allow_any_device, images, device, via)?;
    println!("target: {} ({} bytes{})", t.path.display(), t.size, if t.is_dev { ", block device" } else { "" });
    let opts = restore::Options {
        model, board, sn, mac, sn_prefix, new_identity, keep_userdata, wipe, factory_guids, force_model,
        repair, force_boot, keep_overlay, wipe_overlay, rewrite,
    };
    println!("mode: {}", if repair { "repair (minimal)" } else { "full (factory-equivalent)" });
    let plan = restore::plan(&t, &pkg, boot_bytes, &opts)?;
    println!(
        "device firmware now: {}",
        plan.device_firmware.as_ref().map(|(b, v)| format!("{b} {v}")).unwrap_or_else(|| "unreadable / none".into())
    );
    for n in &plan.notes {
        println!("note: {n}");
    }
    println!(
        "sn_mac after: {}  [{}]{}",
        plan.snmac_new,
        snmac::describe_model(plan.snmac_new.model()),
        if plan.identity_generated { "  <- NEWLY GENERATED identity" } else { "" }
    );
    print!("{}", plan.describe());
    println!("total {:.1} MiB to write (unchanged chunks are skipped)", plan.bytes() as f64 / (1u64 << 20) as f64);
    if plan.bytes() == 0 {
        println!("nothing to do: the device already matches");
        return Ok(());
    }
    if !yes {
        println!("dry run; re-run with --yes to write");
        return Ok(());
    }
    if plan.identity_generated {
        use std::io::Write;
        let mut f = std::fs::OpenOptions::new().create(true).append(true).open(&identity_log)?;
        let ts = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_secs();
        writeln!(f, "{ts}\t{}\t{}\t{}\t{}\t{}", plan.snmac_new.sn(), plan.snmac_new.mac(), plan.snmac_new.model(), plan.snmac_new.board(), t.path.display())?;
        println!("generated identity recorded in {}", identity_log.display());
    }
    plan.execute(&t)?;
    println!("restore complete and verified. Power-cycle the board to boot it.");
    Ok(())
}

fn cmd_snmac(t: restore::Target, set_model: Option<String>, yes: bool) -> Result<()> {
    let g = gpt::Gpt::parse(&t.read(0, gpt::GPT_AREA)?).context("no valid GPT on target")?;
    let p = g.find("sn_mac").context("no sn_mac partition")?.clone();
    let area = t.read(p.offset(), snmac::RECORD_AREA)?;
    let Some(mut r) = snmac::SnMac::parse(&area) else {
        println!("sn_mac: blank");
        return Ok(());
    };
    println!("sn_mac: {r}  [{}]  ({} separators)", snmac::describe_model(r.model()), snmac::SnMac::semicolons(&area));
    println!("  SN valid: {}  MAC valid: {}", r.sn_valid(), r.mac_valid());
    if let Some(m) = set_model {
        r.fields[2] = m;
        let mut a = r.to_area()?;
        a.resize(4096, 0);
        println!("new:    {r}  [{}]", snmac::describe_model(r.model()));
        if !yes {
            println!("dry run; re-run with --yes to write");
            return Ok(());
        }
        t.write_verified(p.offset(), &a, "sn_mac")?;
        println!("written and verified");
    }
    Ok(())
}
