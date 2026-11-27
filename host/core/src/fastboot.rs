//! Minimal fastboot client for the nocry fastboot stage2 (no android-tools needed):
//! getvar, download, boot. Speaks to the interface with class ff/42/03.

use crate::usb::Found;
use anyhow::{anyhow, bail, Context, Result};
use rusb::{DeviceHandle, Direction, GlobalContext, TransferType};
use std::time::Duration;

const TIMEOUT: Duration = Duration::from_secs(10);
const DATA_TIMEOUT: Duration = Duration::from_secs(60);
const CHUNK: usize = 1 << 20;

#[derive(Clone, Copy, PartialEq, clap::ValueEnum)]
pub enum Hash {
    Sha256,
    Xxh64,
}

impl Hash {
    pub fn name(self) -> &'static str {
        match self {
            Hash::Sha256 => "sha256",
            Hash::Xxh64 => "xxh64",
        }
    }
    pub fn len(self) -> usize {
        match self {
            Hash::Sha256 => 32,
            Hash::Xxh64 => 8,
        }
    }
    /// The same digest computed locally.
    pub fn of(self, data: &[u8]) -> Vec<u8> {
        match self {
            Hash::Sha256 => {
                use sha2::Digest;
                sha2::Sha256::digest(data).to_vec()
            }
            Hash::Xxh64 => xxhash_rust::xxh64::xxh64(data, 0).to_le_bytes().to_vec(),
        }
    }
}

/// How the commands travel: USB bulk transfers, or the encrypted network link to nocry-netd.
enum Io {
    Usb { h: DeviceHandle<GlobalContext>, ep_in: u8, ep_out: u8 },
    Net(Box<crate::net::Link>),
}

pub struct Fastboot {
    io: Io,
    /// called for every INFO line
    pub info: Box<dyn FnMut(&str)>,
}

impl Fastboot {
    pub fn open(f: &Found) -> Result<Self> {
        let dev = rusb::devices()?
            .iter()
            .find(|d| d.bus_number() == f.bus && d.address() == f.addr)
            .ok_or_else(|| anyhow!("fastboot device {} vanished", f.id()))?;
        let cfg = dev.active_config_descriptor().context("reading config descriptor")?;
        for intf in cfg.interfaces() {
            for alt in intf.descriptors() {
                if (alt.class_code(), alt.sub_class_code(), alt.protocol_code()) != (0xff, 0x42, 0x03) {
                    continue;
                }
                let (mut ep_in, mut ep_out) = (None, None);
                for ep in alt.endpoint_descriptors() {
                    if ep.transfer_type() == TransferType::Bulk {
                        match ep.direction() {
                            Direction::In => ep_in = Some(ep.address()),
                            Direction::Out => ep_out = Some(ep.address()),
                        }
                    }
                }
                let (Some(ep_in), Some(ep_out)) = (ep_in, ep_out) else { continue };
                let h = dev.open().context("opening fastboot device (udev rule installed?)")?;
                let _ = h.set_auto_detach_kernel_driver(true);
                h.claim_interface(alt.interface_number()).map_err(|e| match e {
                    rusb::Error::Busy => anyhow!("another program is using the board. Close other nocry windows and stop running nocry, adb or fastboot commands, then try again"),
                    e => anyhow!("claiming the board's fastboot interface: {e}"),
                })?;
                return Ok(Fastboot { io: Io::Usb { h, ep_in, ep_out }, info: Box::new(|m| crate::say!("(bootloader) {m}")) });
            }
        }
        bail!("{} has no fastboot interface (ff/42/03)", f.id())
    }

    /// A board running nocry-rescue, over the network: `addr` is "host" or "host:port",
    /// `code` the pairing code on its screen.
    pub fn open_net(addr: &str, code: &str) -> Result<Self> {
        let link = crate::net::Link::connect(addr, code)?;
        Ok(Fastboot { io: Io::Net(Box::new(link)), info: Box::new(|m| crate::say!("(board) {m}")) })
    }

    pub fn is_net(&self) -> bool {
        matches!(self.io, Io::Net(_))
    }

    /// One command packet.
    fn send(&mut self, data: &[u8]) -> Result<()> {
        match &mut self.io {
            Io::Usb { h, ep_out, .. } => {
                let n = h.write_bulk(*ep_out, data, TIMEOUT).context("fastboot write")?;
                if n != data.len() {
                    bail!("short fastboot write ({n} of {})", data.len());
                }
                Ok(())
            }
            Io::Net(l) => l.send_packet(data),
        }
    }

    /// A piece of a data phase.
    fn send_data(&mut self, data: &[u8]) -> Result<()> {
        match &mut self.io {
            Io::Net(l) => l.send(data),
            Io::Usb { .. } => self.send(data),
        }
    }

    fn recv_packet(&mut self, timeout: Duration) -> Result<Vec<u8>> {
        match &mut self.io {
            Io::Usb { h, ep_in, .. } => {
                let mut buf = [0u8; 256];
                let n = h.read_bulk(*ep_in, &mut buf, timeout).context("fastboot read")?;
                Ok(buf[..n].to_vec())
            }
            Io::Net(l) => {
                l.set_timeout(timeout.max(Duration::from_secs(20)));
                l.recv_packet()
            }
        }
    }

    /// Reads responses until OKAY/FAIL/DATA; returns (kind, payload).
    fn response(&mut self, timeout: Duration) -> Result<(String, String)> {
        loop {
            let buf = self.recv_packet(timeout)?;
            let s = String::from_utf8_lossy(&buf).to_string();
            if s.len() < 4 {
                bail!("short fastboot response {s:?}");
            }
            let (kind, rest) = s.split_at(4);
            match kind {
                "INFO" => (self.info)(rest),
                "OKAY" | "DATA" => return Ok((kind.into(), rest.into())),
                "FAIL" => bail!("remote: {rest}"),
                _ => bail!("unexpected fastboot response {s:?}"),
            }
        }
    }

    pub fn command(&mut self, cmd: &str) -> Result<String> {
        self.command_timeout(cmd, TIMEOUT)
    }

    /// For commands that take long before they answer (erasing).
    pub fn command_timeout(&mut self, cmd: &str, timeout: Duration) -> Result<String> {
        self.send(cmd.as_bytes())?;
        let (kind, rest) = self.response(timeout)?;
        if kind != "OKAY" {
            bail!("{cmd}: expected OKAY, got {kind}{rest}");
        }
        Ok(rest)
    }

    pub fn getvar(&mut self, v: &str) -> Result<String> {
        self.command(&format!("getvar:{v}"))
    }

    pub fn download(&mut self, data: &[u8]) -> Result<()> {
        if data.len() > u32::MAX as usize {
            bail!("image too large for fastboot");
        }
        self.send(format!("download:{:08x}", data.len()).as_bytes())?;
        let (kind, rest) = self.response(TIMEOUT)?;
        if kind != "DATA" || u32::from_str_radix(&rest, 16).ok() != Some(data.len() as u32) {
            bail!("download: device answered {kind}{rest}");
        }
        for c in data.chunks(CHUNK) {
            self.send_data(c)?;
        }
        let (kind, rest) = self.response(TIMEOUT)?;
        if kind != "OKAY" {
            bail!("download: device answered {kind}{rest}");
        }
        Ok(())
    }

    /// Reads a device->host data phase of `total` bytes, then the final OKAY.
    fn read_data(&mut self, total: usize, mut sink: impl FnMut(&[u8]) -> Result<()>) -> Result<()> {
        let mut buf = vec![0u8; CHUNK];
        let mut got = 0;
        while got < total {
            let want = CHUNK.min(total - got);
            let n = match &mut self.io {
                Io::Usb { h, ep_in, .. } => h.read_bulk(*ep_in, &mut buf[..want], DATA_TIMEOUT).context("fastboot data read")?,
                Io::Net(l) => {
                    // the stream carries exactly the announced bytes
                    l.set_timeout(DATA_TIMEOUT);
                    l.recv_exact(&mut buf[..want])?;
                    want
                }
            };
            if n == 0 {
                bail!("fastboot data read returned nothing at {got}/{total}");
            }
            if !self.is_net() && n < want && buf[..n].starts_with(b"FAIL") {
                bail!("remote: {}", String::from_utf8_lossy(&buf[4..n]));
            }
            sink(&buf[..n])?;
            got += n;
        }
        let (kind, rest) = self.response(DATA_TIMEOUT)?;
        if kind != "OKAY" {
            bail!("expected OKAY after data, got {kind}{rest}");
        }
        Ok(())
    }

    fn expect_data(&mut self, cmd: &str) -> Result<usize> {
        self.send(cmd.as_bytes())?;
        let (kind, rest) = self.response(TIMEOUT)?;
        if kind != "DATA" {
            bail!("{cmd}: expected DATA, got {kind}{rest}");
        }
        usize::from_str_radix(&rest, 16).with_context(|| format!("bad DATA length {rest:?}"))
    }

    /// fetch:<name>:<off>:<size> (bytes; size below 1 GiB per call), streamed into `sink`.
    pub fn fetch(&mut self, name: &str, off: u64, size: u64, sink: impl FnMut(&[u8]) -> Result<()>) -> Result<()> {
        let n = self.expect_data(&format!("fetch:{name}:{off:x}:{size:x}"))?;
        if n as u64 != size {
            bail!("fetch: device offers {n} bytes, asked for {size}");
        }
        self.read_data(n, sink)
    }

    /// nocry-write (nocry-fbd only): streams `data` to <name> at byte offset `off`. Not verified
    /// by the device; follow with `hash`.
    pub fn write(&mut self, name: &str, off: u64, data: &[u8]) -> Result<()> {
        let n = self.expect_data(&format!("nocry-write:{name}:{off:x}:{:x}", data.len()))?;
        if n != data.len() {
            bail!("nocry-write: device accepts {n} bytes, have {}", data.len());
        }
        for c in data.chunks(CHUNK) {
            self.send_data(c)?;
        }
        let (kind, rest) = self.response(DATA_TIMEOUT)?;
        if kind != "OKAY" {
            bail!("nocry-write: device answered {kind}{rest}");
        }
        Ok(())
    }

    /// nocry-copy (nocry-fbd 0.4+): copies `size` bytes inside <name> on the board itself, from
    /// `src` to `dst`. Parts that are already identical are not written; written parts are read
    /// back and compared by the board. Returns the bytes written.
    pub fn copy(&mut self, name: &str, src: u64, dst: u64, size: u64) -> Result<u64> {
        let r = self.command_timeout(&format!("nocry-copy:{name}:{src:x}:{dst:x}:{size:x}"), Duration::from_secs(300))?;
        u64::from_str_radix(r.trim(), 16).with_context(|| format!("nocry-copy: bad answer {r:?}"))
    }

    /// nocry-hash (nocry-fbd only): one digest per `chunk` bytes, computed on the board.
    pub fn hash(&mut self, algo: Hash, name: &str, off: u64, size: u64, chunk: u64) -> Result<Vec<Vec<u8>>> {
        let n = self.expect_data(&format!("nocry-hash:{}:{name}:{off:x}:{size:x}:{chunk:x}", algo.name()))?;
        let mut all = Vec::with_capacity(n);
        self.read_data(n, |b| {
            all.extend_from_slice(b);
            Ok(())
        })?;
        Ok(all.chunks(algo.len()).map(|c| c.to_vec()).collect())
    }

    /// nocry-secure-erase (nocry-fbd 0.2+): returns the method the card allowed.
    pub fn secure_erase(&mut self, name: &str, off: u64, size: u64) -> Result<String> {
        self.command_timeout(&format!("nocry-secure-erase:{name}:{off:x}:{size:x}"), Duration::from_secs(600))
    }

    /// nocry-dmesg (nocry-fbd 0.2+): the recovery system's kernel log.
    pub fn dmesg(&mut self) -> Result<String> {
        let n = self.expect_data("nocry-dmesg")?;
        let mut all = Vec::with_capacity(n);
        self.read_data(n, |b| {
            all.extend_from_slice(b);
            Ok(())
        })?;
        Ok(String::from_utf8_lossy(&all).to_string())
    }

    /// Runs a command whose answer comes as INFO lines (e.g. the U-Boot stage's "oem dmesg").
    pub fn info_lines(&mut self, cmd: &str) -> Result<Vec<String>> {
        let lines = std::rc::Rc::new(std::cell::RefCell::new(Vec::new()));
        let sink = lines.clone();
        let old = std::mem::replace(&mut self.info, Box::new(move |m| sink.borrow_mut().push(m.to_string())));
        let r = self.command_timeout(cmd, Duration::from_secs(30));
        self.info = old;
        r?;
        let v = lines.borrow().clone();
        Ok(v)
    }

    /// Boots the downloaded image. The device drops off USB right after OKAY.
    pub fn boot(&mut self) -> Result<()> {
        self.command("boot").map(|_| ())
    }
}

pub const UIMAGE_MAGIC: u32 = 0x2705_1956;

/// Android boot image v0 wrapping `kernel` (a uImage for the nocry stage2) with `cmdline`.
pub fn bootimg_v0(kernel: &[u8], cmdline: &str) -> Result<Vec<u8>> {
    const PAGE: usize = 2048;
    if cmdline.len() > 511 {
        bail!("kernel command line too long ({} > 511 bytes)", cmdline.len());
    }
    let mut h = vec![0u8; PAGE];
    h[..8].copy_from_slice(b"ANDROID!");
    let put = |h: &mut Vec<u8>, off: usize, v: u32| h[off..off + 4].copy_from_slice(&v.to_le_bytes());
    put(&mut h, 8, kernel.len() as u32); // kernel_size
    put(&mut h, 12, 0x8001_0000); // kernel_addr (informational; the uImage carries its own)
    put(&mut h, 36, PAGE as u32); // page_size
    h[48..48 + 14].copy_from_slice(b"nocry-recovery");
    h[64..64 + cmdline.len()].copy_from_slice(cmdline.as_bytes());
    let mut out = h;
    out.extend_from_slice(kernel);
    out.resize(out.len().div_ceil(PAGE) * PAGE, 0);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn bootimg_layout() {
        let k = vec![0xabu8; 5000];
        let b = bootimg_v0(&k, "nocry.ums=ro").unwrap();
        assert_eq!(&b[..8], b"ANDROID!");
        assert_eq!(u32::from_le_bytes(b[8..12].try_into().unwrap()), 5000);
        assert_eq!(u32::from_le_bytes(b[36..40].try_into().unwrap()), 2048);
        assert_eq!(&b[64..76], b"nocry.ums=ro");
        assert_eq!(b[76], 0);
        assert_eq!(&b[2048..2048 + 5000], &k[..]);
        assert_eq!(b.len() % 2048, 0);
        assert!(bootimg_v0(&k, &"x".repeat(512)).is_err());
    }
}
