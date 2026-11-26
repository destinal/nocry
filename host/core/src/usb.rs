//! libusb backend (rusb), device discovery, post-boot detection.

use crate::bootrom::{SocProfile, Transport};
use anyhow::{anyhow, bail, Context, Result};
use rusb::{Device, DeviceHandle, Direction, GlobalContext, Recipient, RequestType};
use std::thread::sleep;
use std::time::{Duration, Instant};

/// nocry-ums gadget ID (pid.codes test PID; placeholder until a PID is allocated).
pub const NOCRY_UMS_VID: u16 = 0x1209;
pub const NOCRY_UMS_PID: u16 = 0x0001;
pub const NOCRY_FB_PID: u16 = 0x0002;
/// nocry recovery Linux (adb + ACM console, optional mass storage)
pub const NOCRY_RECOVERY_PID: u16 = 0x0003;

pub struct UsbTransport {
    h: DeviceHandle<GlobalContext>,
}

fn map_err(e: rusb::Error, what: &str) -> anyhow::Error {
    match e {
        rusb::Error::Access => anyhow!(
            "{what}: permission denied. On Linux install host/contrib/60-nocry.rules into \
             /etc/udev/rules.d/ (then `udevadm control --reload` and replug), or run as root"
        ),
        rusb::Error::Busy => anyhow!("another program is using the board. Close other nocry windows and stop running nocry, adb or fastboot commands, then try again"),
        rusb::Error::NoDevice => anyhow!("{what}: device disconnected"),
        rusb::Error::Timeout => anyhow!("{what}: timeout"),
        e => anyhow!("{what}: {e}"),
    }
}

impl Transport for UsbTransport {
    fn vendor_out(&mut self, req: u8, value: u16, index: u16, timeout: Duration) -> Result<()> {
        let rt = rusb::request_type(Direction::Out, RequestType::Vendor, Recipient::Device);
        self.h
            .write_control(rt, req, value, index, &[], timeout)
            .map_err(|e| map_err(e, &format!("vendor request {req:#04x}")))?;
        Ok(())
    }
    fn vendor_in(&mut self, req: u8, value: u16, index: u16, buf: &mut [u8], timeout: Duration) -> Result<usize> {
        let rt = rusb::request_type(Direction::In, RequestType::Vendor, Recipient::Device);
        self.h
            .read_control(rt, req, value, index, buf, timeout)
            .map_err(|e| map_err(e, &format!("vendor request {req:#04x}")))
    }
    fn bulk_out(&mut self, ep: u8, data: &[u8], timeout: Duration) -> Result<usize> {
        self.h.write_bulk(ep, data, timeout).map_err(|e| map_err(e, "bulk OUT"))
    }
}

#[derive(Debug, Clone)]
pub struct Found {
    pub bus: u8,
    pub addr: u8,
    pub ports: Vec<u8>,
    pub vid: u16,
    pub pid: u16,
    pub what: String,
}

impl Found {
    pub fn id(&self) -> String {
        format!("{:03}:{:03}", self.bus, self.addr)
    }
    /// sysfs-style port path, e.g. "1-2.4"
    pub fn port_path(&self) -> String {
        let p: Vec<String> = self.ports.iter().map(|p| p.to_string()).collect();
        format!("{}-{}", self.bus, p.join("."))
    }
}

fn describe(vid: u16, pid: u16, socs: &[SocProfile]) -> Option<String> {
    if let Some(s) = socs.iter().find(|s| s.vid == vid && s.pid == pid) {
        return Some(format!("Ingenic {} USB boot (bootrom or burner)", s.name));
    }
    if vid == 0xa108 {
        return Some(format!("Ingenic device {vid:04x}:{pid:04x} (unsupported SoC)"));
    }
    if vid == NOCRY_UMS_VID && pid == NOCRY_UMS_PID {
        return Some("nocry-ums USB mass storage (stage2 running)".into());
    }
    if vid == NOCRY_UMS_VID && pid == NOCRY_FB_PID {
        return Some("nocry fastboot (stage2 running)".into());
    }
    if vid == NOCRY_UMS_VID && pid == NOCRY_RECOVERY_PID {
        return Some("nocry recovery Linux (adb + console)".into());
    }
    None
}

pub fn scan(socs: &[SocProfile]) -> Result<Vec<Found>> {
    let mut out = vec![];
    for d in rusb::devices().context("enumerating USB devices")?.iter() {
        let Ok(desc) = d.device_descriptor() else { continue };
        let (vid, pid) = (desc.vendor_id(), desc.product_id());
        if let Some(what) = describe(vid, pid, socs) {
            out.push(Found {
                bus: d.bus_number(),
                addr: d.address(),
                ports: d.port_numbers().unwrap_or_default(),
                vid,
                pid,
                what,
            });
        }
    }
    Ok(out)
}

fn find_device(bus: u8, addr: u8) -> Result<Device<GlobalContext>> {
    rusb::devices()?
        .iter()
        .find(|d| d.bus_number() == bus && d.address() == addr)
        .ok_or_else(|| anyhow!("device {bus:03}:{addr:03} not found"))
}

/// Pick the single bootrom-mode device for `soc`, or the one named by `sel` ("BUS:ADDR").
pub fn select(soc: &SocProfile, sel: Option<&str>) -> Result<Found> {
    let all: Vec<Found> = scan(std::slice::from_ref(soc))?
        .into_iter()
        .filter(|f| f.vid == soc.vid && f.pid == soc.pid)
        .collect();
    if let Some(sel) = sel {
        let (b, a) = sel.split_once(':').ok_or_else(|| anyhow!("--device expects BUS:ADDR, e.g. 003:017"))?;
        let (b, a): (u8, u8) = (b.parse()?, a.parse()?);
        return all
            .into_iter()
            .find(|f| f.bus == b && f.addr == a)
            .ok_or_else(|| anyhow!("no {} device at {sel}", soc.name));
    }
    match all.len() {
        0 => bail!(
            "no {} device in USB boot mode ({:04x}:{:04x}) found. Hold BOOT, tap RESET, release BOOT, \
             and use a data-capable cable",
            soc.name,
            soc.vid,
            soc.pid
        ),
        1 => Ok(all.into_iter().next().unwrap()),
        _ => bail!(
            "several devices found, pick one with --device: {}",
            all.iter().map(|f| f.id()).collect::<Vec<_>>().join(", ")
        ),
    }
}

pub fn open(f: &Found) -> Result<UsbTransport> {
    let dev = find_device(f.bus, f.addr)?;
    let h = dev.open().map_err(|e| map_err(e, "open"))?;
    let _ = h.set_auto_detach_kernel_driver(true); // unsupported on some platforms; harmless
    h.claim_interface(0).map_err(|e| map_err(e, "claim interface 0"))?;
    Ok(UsbTransport { h })
}

/// After PROG_STAGE2: wait for the bootrom device to go away and the stage2
/// device to appear. Returns what showed up (None on timeout).
pub fn wait_for_stage2(before: &Found, socs: &[SocProfile], timeout: Duration) -> Result<Option<Found>> {
    let t0 = Instant::now();
    while t0.elapsed() < timeout {
        for f in scan(socs)? {
            let same_port = f.bus == before.bus && f.ports == before.ports;
            if f.vid == NOCRY_UMS_VID && (f.pid == NOCRY_UMS_PID || f.pid == NOCRY_FB_PID) && (same_port || f.ports.is_empty()) {
                return Ok(Some(f));
            }
            // Ingenic burner re-uses the bootrom ID but gets a new address.
            if same_port && f.vid == before.vid && f.pid == before.pid && f.addr != before.addr {
                return Ok(Some(f));
            }
        }
        sleep(Duration::from_millis(250));
    }
    Ok(None)
}

/// A running nocry-ums stage2 and its block device.
#[derive(Debug, Clone)]
pub struct UmsDisk {
    /// exported by the recovery Linux (`nocry recovery --ums`), not the U-Boot stage2
    pub recovery: bool,
    pub dev: std::path::PathBuf,
    pub bytes: u64,
    pub read_only: bool,
}

/// Linux: map a nocry-ums USB device to its /dev/sdX via sysfs (waits up to `timeout`).
#[cfg(target_os = "linux")]
pub fn block_device_for(f: &Found, timeout: Duration) -> Option<UmsDisk> {
    let needle = format!("/{}/", f.port_path());
    let t0 = Instant::now();
    loop {
        if let Ok(rd) = std::fs::read_dir("/sys/block") {
            for e in rd.flatten() {
                let Ok(real) = std::fs::canonicalize(e.path()) else { continue };
                if !real.to_string_lossy().contains(&needle) {
                    continue;
                }
                let rdf = |n: &str| std::fs::read_to_string(e.path().join(n)).unwrap_or_default();
                let sectors: u64 = rdf("size").trim().parse().unwrap_or(0);
                if sectors == 0 {
                    continue; // medium not ready yet
                }
                return Some(UmsDisk {
                    recovery: f.pid == NOCRY_RECOVERY_PID,
                    dev: std::path::PathBuf::from("/dev").join(e.file_name()),
                    bytes: sectors * 512,
                    read_only: rdf("ro").trim() == "1",
                });
            }
        }
        if t0.elapsed() >= timeout {
            return None;
        }
        sleep(Duration::from_millis(250));
    }
}

#[cfg(not(target_os = "linux"))]
pub fn block_device_for(_f: &Found, _timeout: Duration) -> Option<UmsDisk> {
    None
}

#[cfg(unix)]
/// udev sets the node's group/ACL a moment after the kernel creates it; opening
/// it in that window fails with EACCES. Wait until we may open it (read, and
/// write unless the disk is read-only).
pub fn wait_accessible(d: &UmsDisk, timeout: Duration) -> Result<()> {
    use std::ffi::CString;
    use std::os::unix::ffi::OsStrExt;
    let c = CString::new(d.dev.as_os_str().as_bytes())?;
    let mode = if d.read_only { libc::R_OK } else { libc::R_OK | libc::W_OK };
    let t0 = Instant::now();
    loop {
        if unsafe { libc::access(c.as_ptr(), mode) } == 0 {
            return Ok(());
        }
        if t0.elapsed() >= timeout {
            bail!(
                "no permission to open {} after {:.0}s. Install host/contrib/60-nocry.rules \
                 (gives the logged-in user access to the nocry disk), or join the 'disk' group and re-login",
                d.dev.display(),
                timeout.as_secs_f32()
            );
        }
        sleep(Duration::from_millis(100));
    }
}

#[cfg(not(unix))]
pub fn wait_accessible(_d: &UmsDisk, _timeout: Duration) -> Result<()> {
    Ok(())
}

/// Any nocry fastboot stage2 already running on this host.
pub fn running_fastboot(socs: &[SocProfile]) -> Result<Vec<Found>> {
    Ok(scan(socs)?.into_iter().filter(|f| f.vid == NOCRY_UMS_VID && f.pid == NOCRY_FB_PID).collect())
}

/// Any nocry eMMC disk on this host: the U-Boot nocry-ums stage2, or the recovery Linux
/// booted with `--ums ro|rw` (same SCSI identity, "nocry eMMC").
pub fn running_ums(socs: &[SocProfile]) -> Result<Vec<UmsDisk>> {
    Ok(scan(socs)?
        .into_iter()
        .filter(|f| f.vid == NOCRY_UMS_VID && (f.pid == NOCRY_UMS_PID || f.pid == NOCRY_RECOVERY_PID))
        .filter_map(|f| block_device_for(&f, Duration::from_secs(if f.pid == NOCRY_UMS_PID { 3 } else { 1 })))
        .collect())
}

/// Running recovery Linux instances.
pub fn running_recovery(socs: &[SocProfile]) -> Result<Vec<Found>> {
    Ok(scan(socs)?.into_iter().filter(|f| f.vid == NOCRY_UMS_VID && f.pid == NOCRY_RECOVERY_PID).collect())
}

/// iSerialNumber of a device (the board's SN for nocry gadgets; also its adb serial).
pub fn serial_of(f: &Found) -> Option<String> {
    let dev = find_device(f.bus, f.addr).ok()?;
    let desc = dev.device_descriptor().ok()?;
    let h = dev.open().ok()?;
    h.read_serial_number_string_ascii(&desc).ok()
}
