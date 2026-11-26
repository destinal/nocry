//! Ingenic XBurst bootrom USB protocol (stage 0) — the only protocol this module speaks.
//!
//! Requests (vendor, device recipient), from jz4760_tools / ballaswag usbboot and
//! Ingenic's include/cloner/cloner.h:
//!   0x00 GET_CPU_INFO   IN, 8 bytes ASCII
//!   0x01 SET_DATA_ADDR  wValue:wIndex = address (hi:lo)
//!   0x02 SET_DATA_LEN   wValue:wIndex = length
//!   0x03 FLUSH_CACHE
//!   0x04 PROG_STAGE1    wValue:wIndex = exec address; returns to bootrom when done
//!   0x05 PROG_STAGE2    wValue:wIndex = exec address; does not return
//! Data goes out on bulk EP 0x01 after SET_DATA_ADDR/SET_DATA_LEN.
//!
//! Requests >= 0x10 belong to the cloner/burner firmware and include eFuse and
//! secure-boot burning (0x20, 0x21, 0x24). [`Transport`] implementations are
//! only reachable through [`Bootrom`], which refuses anything outside 0x00..=0x05.

use anyhow::{anyhow, bail, Context, Result};
use std::thread::sleep;
use std::time::{Duration, Instant};

pub const VR_GET_CPU_INFO: u8 = 0x00;
pub const VR_SET_DATA_ADDR: u8 = 0x01;
pub const VR_SET_DATA_LEN: u8 = 0x02;
pub const VR_FLUSH_CACHE: u8 = 0x03;
pub const VR_PROG_STAGE1: u8 = 0x04;
pub const VR_PROG_STAGE2: u8 = 0x05;
const ALLOWED: std::ops::RangeInclusive<u8> = VR_GET_CPU_INFO..=VR_PROG_STAGE2;

pub const EP_BULK_OUT: u8 = 0x01;

/// Raw USB access. Implemented by the libusb backend and by the test mock.
pub trait Transport {
    fn vendor_out(&mut self, req: u8, value: u16, index: u16, timeout: Duration) -> Result<()>;
    fn vendor_in(&mut self, req: u8, value: u16, index: u16, buf: &mut [u8], timeout: Duration) -> Result<usize>;
    fn bulk_out(&mut self, ep: u8, data: &[u8], timeout: Duration) -> Result<usize>;
}

/// Load/exec addresses for one SoC.
#[derive(Debug, Clone, Copy)]
pub struct SocProfile {
    pub name: &'static str,
    pub vid: u16,
    pub pid: u16,
    pub s1_load: u32,
    pub s1_exec: u32,
    pub s1_max: usize,
    pub s2_load: u32,
    pub s2_exec: u32,
}

/// X2000/X2000E. Stage1 lands in TCSM; the parameter header occupies 0x800 bytes
/// before the SPL entry. Limit = header + CONFIG_SPL_MAX_SIZE (0x5800).
pub const X2000: SocProfile = SocProfile {
    name: "x2000",
    vid: 0xa108,
    pid: 0xeaef,
    s1_load: 0xb240_1000,
    s1_exec: 0xb240_1800,
    s1_max: 0x800 + 0x5800,
    s2_load: 0x8010_0000,
    s2_exec: 0x8010_0000,
};

pub const SOCS: &[SocProfile] = &[X2000];

#[derive(Debug, Clone)]
pub struct BootOptions {
    /// Longest time the SPL may take to init DDR and hand control back.
    pub spl_timeout: Duration,
    /// Minimum settle time before polling the bootrom again.
    pub spl_settle: Duration,
}

impl Default for BootOptions {
    fn default() -> Self {
        Self { spl_timeout: Duration::from_secs(5), spl_settle: Duration::from_millis(300) }
    }
}

pub struct Bootrom<T: Transport> {
    t: T,
    pub log: Box<dyn FnMut(&str)>,
}

const CTRL_TIMEOUT: Duration = Duration::from_millis(1000);

fn split(v: u32) -> (u16, u16) {
    ((v >> 16) as u16, (v & 0xffff) as u16)
}

impl<T: Transport> Bootrom<T> {
    pub fn new(t: T) -> Self {
        Self { t, log: Box::new(|_| {}) }
    }

    #[cfg(test)]
    pub fn into_inner(self) -> T {
        self.t
    }

    fn out(&mut self, req: u8, param: u32, timeout: Duration) -> Result<()> {
        if !ALLOWED.contains(&req) {
            bail!("refusing vendor request {req:#04x}: only bootrom requests 0x00-0x05 are allowed");
        }
        let (v, i) = split(param);
        self.t.vendor_out(req, v, i, timeout)
    }

    pub fn cpu_info_timeout(&mut self, timeout: Duration) -> Result<String> {
        let mut buf = [0u8; 8];
        let n = self.t.vendor_in(VR_GET_CPU_INFO, 0, 0, &mut buf, timeout)?;
        let s = String::from_utf8_lossy(&buf[..n]);
        Ok(s.trim_end_matches('\0').trim().to_string())
    }

    pub fn cpu_info(&mut self) -> Result<String> {
        self.cpu_info_timeout(CTRL_TIMEOUT)
    }

    pub fn set_data_addr(&mut self, a: u32) -> Result<()> {
        self.out(VR_SET_DATA_ADDR, a, CTRL_TIMEOUT).context("SET_DATA_ADDR")
    }
    pub fn set_data_len(&mut self, n: u32) -> Result<()> {
        self.out(VR_SET_DATA_LEN, n, CTRL_TIMEOUT).context("SET_DATA_LEN")
    }
    pub fn flush_cache(&mut self) -> Result<()> {
        self.out(VR_FLUSH_CACHE, 0, CTRL_TIMEOUT).context("FLUSH_CACHE")
    }
    pub fn prog_stage1(&mut self, a: u32) -> Result<()> {
        self.out(VR_PROG_STAGE1, a, CTRL_TIMEOUT).context("PROG_STAGE1")
    }
    pub fn prog_stage2(&mut self, a: u32) -> Result<()> {
        self.out(VR_PROG_STAGE2, a, CTRL_TIMEOUT).context("PROG_STAGE2")
    }

    /// SET_DATA_ADDR, SET_DATA_LEN, bulk OUT of the whole image.
    pub fn download(&mut self, addr: u32, data: &[u8]) -> Result<()> {
        if data.is_empty() {
            bail!("empty image");
        }
        let len = u32::try_from(data.len()).map_err(|_| anyhow!("image too large"))?;
        self.set_data_addr(addr)?;
        self.set_data_len(len)?;
        // ~1 s per MiB worst case on full-speed, plus slack
        let timeout = Duration::from_secs(5) + Duration::from_millis(data.len() as u64 / 1024);
        let mut sent = 0;
        while sent < data.len() {
            let n = self
                .t
                .bulk_out(EP_BULK_OUT, &data[sent..], timeout)
                .with_context(|| format!("bulk OUT at offset {sent:#x}"))?;
            if n == 0 {
                bail!("device accepted 0 bytes at offset {sent:#x}");
            }
            sent += n;
        }
        Ok(())
    }

    /// Run stage1 and wait for the SPL to return control to the bootrom.
    pub fn run_stage1(&mut self, soc: &SocProfile, spl: &[u8], opts: &BootOptions) -> Result<Duration> {
        if spl.len() > soc.s1_max {
            bail!("stage1 is {} bytes, limit for {} is {}", spl.len(), soc.name, soc.s1_max);
        }
        (self.log)(&format!("stage1: {} bytes -> {:#010x}, exec {:#010x}", spl.len(), soc.s1_load, soc.s1_exec));
        self.download(soc.s1_load, spl)?;
        let t0 = Instant::now();
        self.prog_stage1(soc.s1_exec)?;
        sleep(opts.spl_settle);
        // The bootrom stops servicing USB while the SPL runs; it answers again
        // once the SPL returns (DDR is up). Poll instead of a fixed sleep.
        loop {
            match self.cpu_info_timeout(Duration::from_millis(250)) {
                Ok(info) => {
                    let dt = t0.elapsed();
                    (self.log)(&format!("stage1: SPL returned after {:.2}s (cpu info {info:?})", dt.as_secs_f32()));
                    return Ok(dt);
                }
                Err(_) if t0.elapsed() < opts.spl_timeout => sleep(Duration::from_millis(100)),
                Err(e) => {
                    return Err(e).context(format!(
                        "SPL did not hand control back within {:.1}s. Likely causes: DDR init hung \
                         (wrong DDR parameters for this board), wrong stage1 image, or a secure-boot \
                         chip that only runs signed SPLs (e.g. K1C 2025 / X2600E). Check the UART log.",
                        opts.spl_timeout.as_secs_f32()
                    ))
                }
            }
        }
    }

    /// Load stage2 into DDR and jump to it. The device leaves the bootrom.
    pub fn run_stage2(&mut self, soc: &SocProfile, img: &[u8]) -> Result<()> {
        (self.log)(&format!("stage2: {} bytes -> {:#010x}, exec {:#010x}", img.len(), soc.s2_load, soc.s2_exec));
        self.download(soc.s2_load, img)?;
        self.flush_cache()?;
        // The jump can race the status stage; a failure here is not conclusive.
        if let Err(e) = self.prog_stage2(soc.s2_exec) {
            (self.log)(&format!("stage2: PROG_STAGE2 status not received ({e:#}); device probably jumped already"));
        }
        Ok(())
    }

    pub fn boot(&mut self, soc: &SocProfile, spl: &[u8], stage2: &[u8], opts: &BootOptions) -> Result<()> {
        self.run_stage1(soc, spl, opts)?;
        self.run_stage2(soc, stage2)
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use std::collections::VecDeque;

    #[derive(Debug, Clone, PartialEq)]
    pub enum Ev {
        Out(u8, u32),
        In(u8),
        Bulk(u8, usize),
    }

    /// Records traffic. `cpu_info_fail` = how many GET_CPU_INFO calls time out
    /// (simulates the SPL running).
    #[derive(Default)]
    pub struct Mock {
        pub ev: Vec<Ev>,
        pub cpu_info_fail: VecDeque<bool>,
        pub bulk_chunk: Option<usize>,
        pub fail_stage2_status: bool,
    }

    impl Transport for Mock {
        fn vendor_out(&mut self, req: u8, v: u16, i: u16, _: Duration) -> Result<()> {
            self.ev.push(Ev::Out(req, (v as u32) << 16 | i as u32));
            if req == VR_PROG_STAGE2 && self.fail_stage2_status {
                bail!("pipe error");
            }
            Ok(())
        }
        fn vendor_in(&mut self, req: u8, _: u16, _: u16, buf: &mut [u8], _: Duration) -> Result<usize> {
            self.ev.push(Ev::In(req));
            if self.cpu_info_fail.pop_front().unwrap_or(false) {
                bail!("timeout");
            }
            let s = b"X2000V12";
            buf[..8].copy_from_slice(s);
            Ok(8)
        }
        fn bulk_out(&mut self, ep: u8, d: &[u8], _: Duration) -> Result<usize> {
            let n = self.bulk_chunk.map_or(d.len(), |c| c.min(d.len()));
            self.ev.push(Ev::Bulk(ep, n));
            Ok(n)
        }
    }

    fn fast() -> BootOptions {
        BootOptions { spl_timeout: Duration::from_secs(2), spl_settle: Duration::from_millis(1) }
    }

    #[test]
    fn boot_sequence_matches_known_good_order() {
        // Same order as ballaswag/ingenic-usbboot start_x2000_uboot(), which works on K1 boards,
        // with the fixed sleep replaced by GET_CPU_INFO polling.
        let mock = Mock { cpu_info_fail: VecDeque::from(vec![true, true]), ..Default::default() };
        let mut b = Bootrom::new(mock);
        b.boot(&X2000, &vec![0u8; 16712], &vec![0u8; 177276], &fast()).unwrap();
        let ev = b.into_inner().ev;
        assert_eq!(
            ev,
            vec![
                Ev::Out(VR_SET_DATA_ADDR, 0xb2401000),
                Ev::Out(VR_SET_DATA_LEN, 16712),
                Ev::Bulk(EP_BULK_OUT, 16712),
                Ev::Out(VR_PROG_STAGE1, 0xb2401800),
                Ev::In(VR_GET_CPU_INFO),
                Ev::In(VR_GET_CPU_INFO),
                Ev::In(VR_GET_CPU_INFO),
                Ev::Out(VR_SET_DATA_ADDR, 0x80100000),
                Ev::Out(VR_SET_DATA_LEN, 177276),
                Ev::Bulk(EP_BULK_OUT, 177276),
                Ev::Out(VR_FLUSH_CACHE, 0),
                Ev::Out(VR_PROG_STAGE2, 0x80100000),
            ]
        );
    }

    #[test]
    fn only_bootrom_requests_can_be_sent() {
        let mut b = Bootrom::new(Mock::default());
        for req in [0x06u8, 0x10, 0x12, 0x14, 0x20, 0x21, 0x24, 0xff] {
            assert!(b.out(req, 0, CTRL_TIMEOUT).is_err(), "request {req:#x} must be refused");
        }
        assert!(b.into_inner().ev.is_empty());
    }

    #[test]
    fn partial_bulk_writes_are_continued() {
        let mut b = Bootrom::new(Mock { bulk_chunk: Some(4096), ..Default::default() });
        b.download(0x80100000, &vec![1u8; 10000]).unwrap();
        let bulks: Vec<_> = b.into_inner().ev.into_iter().filter(|e| matches!(e, Ev::Bulk(..))).collect();
        assert_eq!(bulks, vec![Ev::Bulk(1, 4096), Ev::Bulk(1, 4096), Ev::Bulk(1, 1808)]);
    }

    #[test]
    fn spl_that_never_returns_is_reported() {
        let mock = Mock { cpu_info_fail: VecDeque::from(vec![true; 10_000]), ..Default::default() };
        let mut b = Bootrom::new(mock);
        let opts = BootOptions { spl_timeout: Duration::from_millis(300), spl_settle: Duration::from_millis(1) };
        let err = b.run_stage1(&X2000, &[0u8; 64], &opts).unwrap_err();
        assert!(format!("{err:#}").contains("secure-boot"));
    }

    #[test]
    fn oversized_stage1_rejected_before_any_traffic() {
        let mut b = Bootrom::new(Mock::default());
        assert!(b.run_stage1(&X2000, &vec![0u8; X2000.s1_max + 1], &fast()).is_err());
        assert!(b.into_inner().ev.is_empty());
    }

    #[test]
    fn stage2_status_loss_is_tolerated() {
        let mut b = Bootrom::new(Mock { fail_stage2_status: true, ..Default::default() });
        assert!(b.run_stage2(&X2000, &[0u8; 16]).is_ok());
    }
}
