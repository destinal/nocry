//! The first-stage boot loader (SPL) on the eMMC: what is installed, installing nocryboot,
//! going back to the factory loader. See docs/nocryboot.md.
//!
//! The loader is 24 KiB at byte 0x4400. Before nocry replaces a factory loader it keeps a copy
//! on the board itself, in the unused end of the boot area, so that any PC can put it back.

use crate::boards::FamilyId;
use crate::gpt::Gpt;
use anyhow::{bail, Result};
use sha2::Digest as _;

pub const SPL_OFFSET: u64 = 0x4400;
pub const SPL_SIZE: usize = 24 * 1024;
/// Kept factory loader: one sector of text, then the loader itself.
pub const SAVED_MARK_OFFSET: u64 = 0xefe00;
pub const SAVED_OFFSET: u64 = 0xf0000;
const SAVED_TAG: &str = "nocry:factory-loader";
const MIB: u64 = 1 << 20;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Kind {
    /// Creality's own loader
    Factory,
    /// nocryboot; version is None for builds from before it carried one
    Nocryboot { version: Option<String> },
    /// something nocry does not recognise (or nothing)
    Unknown,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Info {
    pub kind: Kind,
    /// board family the loader is built for, from its console setting
    pub family: Option<FamilyId>,
    /// "U-Boot SPL 2013.07 (Oct 20 2023 - 15:43:37)"
    pub banner: String,
    /// it can run a slot on trial (count of starts)
    pub counts: bool,
    /// it restarts into USB boot mode when nothing can be started
    pub door: bool,
}

impl Info {
    pub fn describe(&self) -> String {
        let fam = match self.family {
            Some(FamilyId::Nebula) => "Nebula Pad family",
            Some(FamilyId::K1) => "K1 family",
            None => "unknown board family",
        };
        match &self.kind {
            Kind::Factory => format!("Creality factory loader ({fam}; {})", self.banner),
            Kind::Nocryboot { version: Some(v) } => format!("nocryboot {v} ({fam})"),
            Kind::Nocryboot { version: None } => {
                let what = match (self.counts, self.door) {
                    (true, _) => "early build with the count of starts",
                    (false, true) => "early build with the recovery door",
                    _ => "first build, without recovery door",
                };
                format!("nocryboot, {what} ({fam})")
            }
            Kind::Unknown => "not a loader nocry knows".into(),
        }
    }
}

fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    hay.windows(needle.len()).position(|w| w == needle)
}

fn text_at(b: &[u8], at: usize) -> String {
    let end = b[at..].iter().position(|&c| !(0x20..0x7f).contains(&c)).map_or(b.len(), |n| at + n);
    String::from_utf8_lossy(&b[at..end]).into_owned()
}

/// Look at a loader image (24 KiB as it sits on the eMMC).
pub fn inspect(spl: &[u8]) -> Info {
    let mut i = Info { kind: Kind::Unknown, family: None, banner: String::new(), counts: false, door: false };
    if spl.len() < 0x900 || &spl[..4] != b"LPSM" || &spl[0x100..0x104] != b"INGE" {
        return i;
    }
    i.banner = find(spl, b"U-Boot SPL ").map(|at| text_at(spl, at)).unwrap_or_default();
    i.family = match (find(spl, b"console=ttyS4,").is_some(), find(spl, b"console=ttyS2,").is_some()) {
        (true, false) => Some(FamilyId::Nebula),
        (false, true) => Some(FamilyId::K1),
        _ => None,
    };
    i.counts = find(spl, b"nocryboot:try=").is_some();
    i.door = find(spl, b"restarting in USB boot mode").is_some();
    // everything Creality's loader and ours need: slot record, identity for the picture, panel id
    let boots = find(spl, b"ota:kernel2").is_some() && find(spl, b"sn_mac").is_some() && find(spl, b"lcm_id=").is_some();
    if let Some(at) = find(spl, b"nocryboot-loader ") {
        let t = text_at(spl, at);
        let mut w = t.split(' ').skip(1);
        let version = w.next().map(str::to_string);
        match w.next() {
            Some("nebula") => i.family = Some(FamilyId::Nebula),
            Some("k1") => i.family = Some(FamilyId::K1),
            _ => {}
        }
        i.kind = Kind::Nocryboot { version };
    } else if !boots {
        // a loader for something else (e.g. one that starts U-Boot)
    } else if i.door || find(spl, b"nocryboot").is_some() || find(spl, b" 2026 - ").is_some() && !i.banner.contains("2023") {
        // builds before the identification text: recognised by their messages, the very first one
        // only by not being one of Creality's (built 2023)
        i.kind = Kind::Nocryboot { version: None };
    } else if find(spl, b"[lcm-start").is_some() {
        i.kind = Kind::Factory;
    }
    i
}

/// Where the loader expects the partitions it reads (it has its own table and does not read
/// the one on the disk). Offsets in MiB.
pub fn expected_layout(family: FamilyId) -> &'static [(&'static str, u64)] {
    match family {
        FamilyId::Nebula => &[
            ("ota", 1), ("sn_mac", 2), ("rtos", 3), ("rtos2", 7), ("kernel", 11), ("kernel2", 19), ("rootfs", 27), ("rootfs2", 527),
        ],
        FamilyId::K1 => &[
            ("ota", 1), ("sn_mac", 2), ("rtos", 3), ("rtos2", 7), ("kernel", 11), ("kernel2", 19), ("rootfs", 27), ("rootfs2", 327),
        ],
    }
}

/// Refuses a board whose partitions are not where the loader for `family` will look for them.
pub fn check_layout(g: &Gpt, family: FamilyId) -> Result<()> {
    for (name, mib) in expected_layout(family) {
        match g.find(name) {
            Some(p) if p.offset() == mib * MIB => {}
            Some(p) => bail!(
                "partition {name} starts at {:#x} on this board, but the loader looks for it at {:#x}; \
                 this board's layout does not fit the loader",
                p.offset(),
                mib * MIB
            ),
            None => bail!("this board has no partition {name}; its layout does not fit the loader"),
        }
    }
    Ok(())
}

pub fn sha256(b: &[u8]) -> String {
    format!("{:x}", sha2::Sha256::digest(b))
}

/// The sector that marks a kept factory loader.
pub fn saved_mark(loader: &[u8]) -> Vec<u8> {
    let mut v = vec![0u8; 512];
    let t = format!("{SAVED_TAG} sha256={} size={}\n", sha256(loader), loader.len());
    v[..t.len()].copy_from_slice(t.as_bytes());
    v
}

/// Checks a kept factory loader read from the board: mark sector + data. Returns the loader.
pub fn saved_loader(mark: &[u8], data: &[u8]) -> Option<Vec<u8>> {
    let end = mark.iter().position(|&c| c == b'\n')?;
    let t = std::str::from_utf8(&mark[..end]).ok()?.strip_prefix(SAVED_TAG)?;
    let mut sha = None;
    let mut size = None;
    for w in t.split_whitespace() {
        if let Some(x) = w.strip_prefix("sha256=") {
            sha = Some(x);
        } else if let Some(x) = w.strip_prefix("size=") {
            size = x.parse::<usize>().ok();
        }
    }
    let (sha, size) = (sha?, size?);
    if size != SPL_SIZE || data.len() < size || sha256(&data[..size]) != sha {
        return None;
    }
    let l = data[..size].to_vec();
    (inspect(&l).kind == Kind::Factory).then_some(l)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fake(console: &str, extra: &[&str]) -> Vec<u8> {
        let mut v = vec![0u8; SPL_SIZE];
        v[..4].copy_from_slice(b"LPSM");
        v[0x100..0x104].copy_from_slice(b"INGE");
        let mut at = 0x900;
        let mut put = |s: &str| {
            v[at..at + s.len()].copy_from_slice(s.as_bytes());
            at += s.len() + 1;
        };
        put(&format!("console={console},115200n8  [mem-start---mem-end] [lcm-start---lcm-end]"));
        for s in ["ota:kernel2", "sn_mac", "lcm_id="].iter().chain(extra) {
            put(s);
        }
        v
    }

    #[test]
    fn tells_loaders_apart() {
        let f = inspect(&fake("ttyS4", &["U-Boot SPL 2013.07 (Oct 20 2023 - 15:43:37)"]));
        assert_eq!((f.kind.clone(), f.family, f.counts, f.door), (Kind::Factory, Some(FamilyId::Nebula), false, false));
        assert!(f.describe().contains("factory"));

        let n = inspect(&fake("ttyS2", &["nocryboot-loader 0.1 k1", "nocryboot:try=", "restarting in USB boot mode"]));
        assert_eq!(n.kind, Kind::Nocryboot { version: Some("0.1".into()) });
        assert_eq!((n.family, n.counts, n.door), (Some(FamilyId::K1), true, true));

        let first = inspect(&fake("ttyS4", &["U-Boot SPL 2013.07 (Nov 27 2026 - 00:41:57)"]));
        assert_eq!(first.kind, Kind::Nocryboot { version: None });

        assert_eq!(inspect(&[0u8; SPL_SIZE]).kind, Kind::Unknown);
        assert_eq!(inspect(b"LPSM").kind, Kind::Unknown);
        let mut other = fake("ttyS4", &[]);
        let at = find(&other, b"ota:kernel2").unwrap();
        other[at..at + 3].copy_from_slice(b"xxx"); // a loader that does not do slots
        assert_eq!(inspect(&other).kind, Kind::Unknown);
    }

    #[test]
    fn kept_loader_round_trip() {
        let f = fake("ttyS2", &["U-Boot SPL 2013.07 (Nov 14 2023 - 14:30:39)"]);
        let m = saved_mark(&f);
        assert_eq!(saved_loader(&m, &f), Some(f.clone()));
        let mut bad = f.clone();
        bad[0x2000] ^= 1;
        assert_eq!(saved_loader(&m, &bad), None, "a changed copy is not accepted");
        assert_eq!(saved_loader(&[0u8; 512], &f), None);
        // only a factory loader counts as "the factory loader"
        let n = fake("ttyS2", &["nocryboot-loader 0.1 k1"]);
        assert_eq!(saved_loader(&saved_mark(&n), &n), None);
    }
}
