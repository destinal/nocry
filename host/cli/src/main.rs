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
    /// Work with a board running nocry-rescue over the network instead of USB: its address
    /// (name or IP, optionally :port). Needs --code
    #[arg(long, global = true, requires = "code")]
    net: Option<String>,
    /// The pairing code shown on the board's screen (with --net)
    #[arg(long, global = true)]
    code: Option<String>,
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

/// What the board can do for its own repair (needs the recovery or rescue system of the same version)
#[derive(Subcommand)]
enum BoardCmd {
    /// eMMC health: wear, spare blocks and userdata usage
    Health,
    /// Run fsck (check and repair) on a file system: "data" (userdata) or "settings" (rootfs_data, the system overlay)
    Check { which: String },
    /// List a folder of the data partition (the printer's /usr/data)
    Ls {
        #[arg(default_value = "")]
        dir: String,
    },
    /// Fetch a file of the data partition
    Get {
        path: String,
        #[arg(short, long)]
        output: PathBuf,
    },
    /// Store a local file in the data partition (a file of the same name is replaced)
    Put { local: PathBuf, path: String },
    /// Delete a file or an empty folder of the data partition
    Rm { path: String },
    /// Save the logs (the printer's and the recovery system's) as one tar.gz
    Logs {
        #[arg(short, long)]
        output: PathBuf,
    },
    /// Save the printer's configuration files as one tar.gz
    Config {
        #[arg(short, long)]
        output: PathBuf,
    },
    /// Reboot: leaves the recovery system and boots the active slot
    Restart,
}

#[derive(Subcommand)]
enum Cmd {
    /// The rescue image stored on the board (Nebula Pad, with the nocryboot loader 0.2 or
    /// later): hold the screen at power-on to boot it, no computer needed. Without
    /// options: show whether it is installed. --install installs it, --remove removes it
    Rescue {
        #[arg(long, conflicts_with = "remove")]
        install: bool,
        #[arg(long)]
        remove: bool,
        /// the rescue image to install (default: nocry-rescue.uImage among the board images)
        #[arg(long)]
        image: Option<PathBuf>,
    },
    /// Repair actions run by the board itself: eMMC health, fsck, files, logs, reboot
    #[command(subcommand)]
    Board(BoardCmd),
    /// The Wi-Fi of a board running nocry-rescue: show its connection and the networks in range.
    /// --join connects it to a network (it returns to access point mode if that fails);
    /// --own switches it to access point mode (SSID "nocry")
    Wifi {
        #[arg(long)]
        join: Option<String>,
        /// password of the network to connect to (omit for an open network)
        #[arg(long, default_value = "")]
        password: String,
        #[arg(long, conflicts_with = "join")]
        own: bool,
    },
    /// Look for boards running nocry-rescue on the local network
    Discover,
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
    /// Try a first-stage loader without installing it: from USB boot mode, run the given eMMC
    /// loader (the factory one or nocryboot's) from RAM; it then boots what is on the eMMC
    RunSpl {
        /// Loader image as it sits on the eMMC (header + code, at most 24 KiB)
        spl: PathBuf,
        /// Send it even if it does not look like an eMMC loader or the chip is not an X2000
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
    /// Erase rootfs_data and userdata (all settings and files) so they cannot be recovered: eMMC
    /// secure erase where the card supports it, else overwrite; verified. Firmware stays.
    SecureErase {
        #[arg(long)]
        yes: bool,
    },
    /// Unpack a firmware file (.ingenic package, update .img, or folder) into DIR:
    /// rtos.bin (zero.bin), kernel.bin (xImage), rootfs.squashfs
    Unpack { firmware: PathBuf, dir: PathBuf },
    /// Show the two firmware slots, or with --set a|b set the active slot
    Slots {
        #[arg(long)]
        set: Option<String>,
        /// With --set: put the slot on trial for this many boots (1-9). Unless the booted
        /// system reports success in time, the nocryboot loader reverts to the other slot
        #[arg(long = "try")]
        tries: Option<u8>,
    },
    /// Show which boot logo (the splash of the first seconds) each firmware slot has.
    /// --nocry installs nocry's open boot logo, --firmware restores the firmware's own
    /// (it is kept on the board). Dry run unless --yes
    #[command(alias = "picture")]
    Logo {
        #[arg(long, conflicts_with = "firmware")]
        nocry: bool,
        #[arg(long)]
        firmware: bool,
        /// Work on a disk image file instead of a board
        #[arg(long)]
        target: Option<PathBuf>,
        #[arg(long)]
        yes: bool,
    },
    /// Show the board's first-stage boot loader. --install installs nocryboot (the factory
    /// loader is kept on the board); --factory restores the factory loader. Dry run unless --yes
    Loader {
        #[arg(long, conflicts_with = "factory")]
        install: bool,
        #[arg(long)]
        factory: bool,
        /// The factory loader as a file (a 24 KiB loader, or a backup image of this board made
        /// before nocryboot went on). With --factory: use it instead of the copy kept on the
        /// board. With --install: keep it on the board, if the board has no copy yet
        #[arg(long)]
        file: Option<PathBuf>,
        /// Work on a disk image file instead of a board
        #[arg(long)]
        target: Option<PathBuf>,
        #[arg(long)]
        yes: bool,
    },
    /// Copy the firmware of one slot (a or b) into the other, verified. The active slot
    /// selection, rootfs_data, userdata and the identity record are not changed.
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
    /// Show the firmware catalog: what nocry can download and install (--refresh fetches the
    /// current catalog from Creality)
    Catalog {
        #[arg(long)]
        refresh: bool,
        /// one line per firmware, tab-separated: key, name, version, released, bytes, file
        #[arg(long)]
        tsv: bool,
    },
    /// Install firmware the way the window does, with the same checks: FIRMWARE is a .ingenic
    /// package, an update .img, or an unpacked folder. User data is preserved unless --clean;
    /// --other-slot installs to the inactive slot and changes nothing else.
    /// Shows what would be done; writes only with --yes
    Install {
        firmware: PathBuf,
        #[arg(long, conflicts_with = "other_slot")]
        clean: bool,
        #[arg(long)]
        other_slot: bool,
        /// printer type / mainboard for the identity record, when nocry asks for one
        #[arg(long)]
        model: Option<String>,
        #[arg(long)]
        board: Option<String>,
        /// keep the firmware's own boot logo instead of nocry's
        #[arg(long)]
        firmware_logo: bool,
        /// print the outcome as key=value lines (for other programs)
        #[arg(long)]
        report: bool,
        #[arg(long)]
        yes: bool,
        #[arg(long)]
        target: Option<PathBuf>,
    },
    /// Download a firmware from the firmware catalog (verified, cached). MODEL: NEBULA, F005, F003, F004;
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
        /// Allow writing a protected region (mmc0, gpt, bootloader, sn_mac)
        #[arg(long)]
        critical: bool,
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
    let mut fb = match network_board() {
        Some((addr, code)) => fastboot::Fastboot::open_net(&addr, &code)?,
        None => {
            let f = usb::running_recovery(SOCS)?
                .into_iter()
                .chain(usb::running_fastboot(SOCS)?)
                .next()
                .context("no board with a fastboot interface (run `nocry recovery` or `nocry fastboot`)")?;
            fastboot::Fastboot::open(&f)?
        }
    };
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
        FbCmd::Write { name, file, offset, critical } => {
            let off = num(&offset)?;
            if critical {
                fb.command("flashing unlock_critical")?;
            }
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


/// Ctrl-C or a polite kill: finish the write under way, then stop (as Cancel does in the window).
/// A second one ends the program at once: not everything it waits for can be stopped politely.
extern "C" fn on_stop_signal(_: libc::c_int) {
    static ASKED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
    if ASKED.swap(true, std::sync::atomic::Ordering::SeqCst) {
        unsafe { libc::_exit(130) }
    }
    nocry_core::events::cancel();
}

fn main() -> Result<()> {
    #[cfg(unix)]
    unsafe {
        libc::signal(libc::SIGINT, on_stop_signal as extern "C" fn(libc::c_int) as libc::sighandler_t);
        libc::signal(libc::SIGTERM, on_stop_signal as extern "C" fn(libc::c_int) as libc::sighandler_t);
    }
    let cli = Cli::parse();
    let soc = soc_by_name(&cli.soc)?;
    let images = cli.images.clone();
    if let (Some(addr), Some(code)) = (&cli.net, &cli.code) {
        set_network_board(Some((addr.clone(), code.clone())));
    }
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
                bail!("this erases rootfs_data and userdata (all settings and files) for good; add --yes to do it");
            }
            let w = secure_wipe_data(soc, Via::Auto, None, None)?;
            for (n, m) in &w.partitions {
                println!("{n}: {m}");
            }
            println!("erased and verified {} bytes", w.bytes);
        }
        Cmd::Unpack { firmware, dir } => {
            let p = restore::Package::load(&firmware)?;
            std::fs::create_dir_all(&dir)?;
            for (n, d) in [("rtos.bin", &p.rtos), ("kernel.bin", &p.kernel), ("rootfs.squashfs", &p.rootfs)] {
                std::fs::write(dir.join(n), &d[..])?;
                println!("{n}: {} bytes", d.len());
            }
            println!("board {}, version {}", p.ota_board.as_deref().unwrap_or("?"), p.ota_version.as_deref().unwrap_or("?"));
            for s in &p.support {
                println!("supports {:?} ({}){}", s.model, nocry_core::boards::product_name(&s.model),
                    if s.mainboards.is_empty() { String::new() } else { format!(" on {}", s.mainboards.join(", ")) });
            }
        }
        Cmd::Wifi { join, password, own } => {
            if join.is_none() && !own {
                let w = nocry_core::session::wifi_status()?;
                match w.mode.as_str() {
                    "ap" => println!("access point mode: SSID {}, address {}", w.ssid, w.address),
                    "client" => println!("connected to {}, address {}", w.ssid, w.address),
                    m => println!("Wi-Fi: {m}"),
                }
                if let Some(n) = w.source.strip_prefix("failed:") {
                    println!("connection to {n} failed (authentication failed, or out of range)");
                }
                if !w.saved_from.is_empty() {
                    println!("the installed firmware has a saved network: {}", w.saved_names);
                }
                println!("in range (scanned when rescue booted):");
                for (name, sig, lock) in &w.networks {
                    println!("  {sig:4} dBm  {}  {name}", if *lock { "password" } else { "open    " });
                }
            } else {
                let over_net = nocry_core::session::network_board().is_some();
                nocry_core::session::wifi_change(join.as_deref(), &password)?;
                match &join {
                    Some(n) => println!("the board is connecting to {n}; on failure it returns to access point mode (SSID \"nocry\") within a minute"),
                    None => println!("the board is switching to access point mode (SSID \"nocry\")"),
                }
                if over_net {
                    println!("looking for the board (put this computer on the same network)...");
                    let addr = nocry_core::session::refind_network_board(Duration::from_secs(180))?;
                    println!("found it at {addr}; use --net {addr} with the same code");
                }
            }
        }
        Cmd::Rescue { install, remove, image } => {
            use nocry_core::session as s;
            if install {
                let img = s::rescue_image(image.as_ref())?;
                let copies = s::rescue_install(&img)?;
                println!("rescue image installed and verified ({copies} of 2 copies)");
                if copies < 2 {
                    println!("one slot has no room for a copy: its system image reaches into the last 32 MiB of the partition");
                }
                println!("to boot it: have a finger on the screen while switching the board on");
                println!("(that needs the nocryboot loader 0.3 or later: see `nocry loader`)");
            } else if remove {
                s::rescue_remove()?;
                println!("rescue image removed");
            } else {
                match s::rescue_status()? {
                    (st, n) if st == "yes" => println!("rescue image: installed, verified ({n} of 2 copies)"),
                    (st, _) if st == "damaged" => println!("rescue image: checksum mismatch (overwritten); reinstall with --install"),
                    _ => println!("rescue image: not installed (--install installs it)"),
                }
            }
        }
        Cmd::Board(b) => {
            use nocry_core::session as s;
            match b {
                BoardCmd::Health => {
                    let h = s::board_health()?;
                    println!("eMMC {}: wear {} % (type A){}, spare blocks: {}", h.chip, h.wear_a, if h.wear_b != h.wear_a { format!(", {} % (type B)", h.wear_b) } else { String::new() }, h.spare);
                    if h.data_state == "readable" {
                        println!("userdata: {} MB used of {} MB, {} MB free", h.data_used_mb, h.data_total_mb, h.data_free_mb);
                    } else {
                        println!("userdata: {}", h.data_state);
                    }
                }
                BoardCmd::Check { which } => println!("{}", s::board_check(&which)?),
                BoardCmd::Ls { dir } => {
                    for e in s::board_list(&dir)? {
                        if e.is_dir { println!("{:>12}  {}/", "", e.name) } else { println!("{:>12}  {}", e.size, e.name) }
                    }
                }
                BoardCmd::Get { path, output } => {
                    let d = s::board_get(&path)?;
                    std::fs::write(&output, &d)?;
                    println!("{} bytes saved to {}", d.len(), output.display());
                }
                BoardCmd::Put { local, path } => {
                    let d = std::fs::read(&local).with_context(|| format!("reading {}", local.display()))?;
                    s::board_put(&path, &d)?;
                    println!("{} bytes stored as {path}", d.len());
                }
                BoardCmd::Rm { path } => {
                    s::board_delete(&path)?;
                    println!("deleted {path}");
                }
                BoardCmd::Logs { output } => {
                    let d = s::board_bundle("logs")?;
                    std::fs::write(&output, &d)?;
                    println!("logs saved to {} ({} bytes)", output.display(), d.len());
                }
                BoardCmd::Config { output } => {
                    let d = s::board_bundle("config")?;
                    std::fs::write(&output, &d)?;
                    println!("configuration saved to {} ({} bytes)", output.display(), d.len());
                }
                BoardCmd::Restart => {
                    s::board_restart()?;
                    println!("rebooting: the board boots the active slot");
                }
            }
        }
        Cmd::Discover => {
            let found = nocry_core::discover::discover(Duration::from_secs(3));
            if found.is_empty() {
                println!("no board running nocry-rescue answered on this network");
                println!("(on the board's own Wi-Fi \"nocry\" its address is {})", nocry_core::discover::AP_ADDRESS);
            }
            for b in found {
                let how = if b.found_by == "mdns" { "announced itself" } else { "at the address of its own Wi-Fi" };
                println!("{}  {}:{}  ({how})", b.name, b.address, b.port);
                println!("    use it with:  nocry --net {} --code <code on its screen> <command>", b.address);
            }
        }
        Cmd::Slots { set, tries } => {
            let s = match (set, tries) {
                (Some(x), Some(n)) => start_trial(soc, Via::Auto, &x.to_ascii_lowercase(), n, None)?,
                (Some(x), None) => set_active_slot(soc, Via::Auto, &x.to_ascii_lowercase(), None, None)?,
                (None, Some(_)) => bail!("--try needs --set a|b"),
                (None, None) => slots(soc, Via::Auto, None)?,
            };
            for (n, i) in [("A", &s.a), ("B", &s.b)] {
                println!("{n}: {} {}  rtos {}  kernel {}  startable {}", i.firmware_name, i.firmware_version, i.has_rtos, i.has_kernel, i.bootable());
            }
            println!("active: {}", s.active.to_uppercase());
            println!("loader: {}", if s.loader_counts { "nocryboot (counts starts)" } else { "does not count starts" });
            if let Some(n) = &s.note {
                println!("note: {}", n.describe());
            }
        }
        Cmd::Logo { nocry, firmware, target, yes } => {
            let want = if nocry { Some(PictureChoice::Nocry) } else if firmware { Some(PictureChoice::Firmware) } else { None };
            let st = set_picture(soc, Via::Auto, images.as_ref(), want, yes, target.as_ref())?;
            let show = |i: &PictureInfo| {
                let k = match i.kind {
                    nocry_core::picture::Kind::Nocry => "nocry",
                    nocry_core::picture::Kind::Firmware => "the firmware's own",
                    nocry_core::picture::Kind::None => "no image",
                };
                format!("{k}{}", if i.firmware_kept { " (the firmware's own is kept)" } else { "" })
            };
            println!("slot A: {}\nslot B: {}", show(&st.a), show(&st.b));
            if !st.available {
                println!("this copy of nocry has no boot logo image for this board family");
            } else if !st.proven {
                println!("note: nocry's boot logo has not been tested on this board family's own screens yet");
            }
            if want.is_some() && st.changed && !yes {
                println!("dry run: nothing was written. Add --yes to do it.");
            }
        }
        Cmd::Loader { install, factory, file, target, yes } => {
            let st = if install {
                let dir = catalog::default_data_dir().join("safety");
                install_loader(soc, Via::Auto, images.as_ref(), Some(&dir), file.as_ref(), yes, target.as_ref())?
            } else if factory {
                restore_factory_loader(soc, Via::Auto, images.as_ref(), file.as_ref(), yes, target.as_ref())?
            } else {
                loader_status(soc, Via::Auto, images.as_ref(), target.as_ref())?
            };
            println!("loader: {}", st.on_board.describe());
            println!("factory loader kept on the board: {}", if st.factory_kept { "yes" } else { "no" });
            if !install && !factory {
                match (&st.blocked, &st.available) {
                    (Some(why), _) => println!("nocryboot cannot be installed: {why}"),
                    (None, Some(a)) if st.on_board != *a => println!("can install: {}  (nocry loader --install --yes)", a.describe()),
                    _ => {}
                }
            } else if st.changed && !yes {
                println!("dry run: nothing was written. Add --yes to do it.");
            } else if st.changed {
                println!("done. Power the board off and on; if it stays dark, use the BOOT button and `nocry loader --factory --yes`.");
            }
        }
        Cmd::CopySlot { from, yes } => {
            let from = from.to_ascii_lowercase();
            if !yes {
                bail!("this overwrites the firmware in the other slot; add --yes to do it");
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
        Cmd::Install { firmware, clean, other_slot, model, board, firmware_logo, report, yes, target } => {
            let logo = if firmware_logo { nocry_core::session::PictureChoice::Firmware } else { nocry_core::session::PictureChoice::Default };
            let r = if other_slot {
                nocry_core::session::install_other_slot(soc, via, &firmware, yes, None, target.as_ref(), logo)?
            } else {
                nocry_core::session::install(soc, via, &firmware, !clean, yes, None, target.as_ref(), model.as_deref(), board.as_deref(), logo)?
            };
            if report {
                println!("firmware={} {}", if r.firmware_name.is_empty() { &r.firmware_board } else { &r.firmware_name }, r.firmware_version);
                println!("on_board={}", if r.device_firmware_name.is_empty() { r.device_firmware.clone() } else { format!("{} {}", r.device_firmware_name, r.device_firmware.split(' ').nth(1).unwrap_or("")) });
                println!("slot={}", r.target_slot);
                println!("to_write={}", r.bytes_to_write);
                println!("executed={}", if r.executed { "yes" } else { "no" });
                println!("blank_board={}", if r.blank_board { "yes" } else { "no" });
                if !r.model_before.is_empty() && r.model_before != r.model_after {
                    println!("converting={} -> {}", r.model_before_name, r.model_after_name);
                }
                if !r.need_choice.is_empty() {
                    println!("need_choice={}", r.need_choice);
                    for (v, n) in &r.choices {
                        println!("choice={v}\t{n}");
                    }
                }
                for n in &r.notes {
                    println!("note={}", n.replace('\n', " "));
                }
            } else {
                for n in &r.notes {
                    println!("note: {n}");
                }
                print!("{}", r.details);
                if !r.need_choice.is_empty() {
                    let list: Vec<String> = r.choices.iter().map(|(v, n)| format!("{v} ({n})")).collect();
                    bail!("say which {} this board is with --{}: {}", r.need_choice, if r.need_choice == "model" { "model" } else { "board" }, list.join(", "));
                }
                if r.bytes_to_write == 0 {
                    println!("nothing to do: the board already holds this firmware, intact");
                } else if r.executed {
                    println!("installed and verified ({} MiB written)", r.bytes_to_write >> 20);
                } else {
                    println!("{} MiB to write; run again with --yes to install", r.bytes_to_write >> 20);
                }
            }
        }
        Cmd::Catalog { refresh, tsv } => {
            let dir = catalog::default_data_dir();
            let c = if refresh { catalog::refresh(&dir)? } else { catalog::load(Some(&dir)) };
            if tsv {
                for m in &c.models {
                    for f in &m.firmwares {
                        println!("{}\t{}\t{}\t{}\t{}\t{}", m.key, m.name, f.version, f.released, f.size, f.file);
                    }
                }
                return Ok(());
            }
            println!("firmware catalog of {} ({})", c.built, if c.updated { "updated from Creality" } else { "built in" });
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
        Cmd::RunSpl { spl, force } => {
            let s1 = read(&spl)?;
            run_spl_only(soc, &s1, device.as_deref(), force)?;
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
