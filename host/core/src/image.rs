//! Static checks on stage1/stage2 images before anything is sent.
//!
//! Stage1 = 0x800-byte parameter header (TLVs, see tools/splhdr) + SPL code.

use std::fmt;

pub const HEADER_SIZE: usize = 0x800;
const MAGIC_INFO: u32 = u32::from_be_bytes(*b"BDIF"); // stored LE as "FIDB"
const MAGIC_DDR: u32 = u32::from_be_bytes(*b"DDR\0"); // stored LE as "\0RDD"

fn le32(b: &[u8], off: usize) -> Option<u32> {
    b.get(off..off + 4).map(|s| u32::from_le_bytes(s.try_into().unwrap()))
}

#[derive(Debug, Clone, PartialEq)]
pub enum HeaderGen {
    /// k1-debian burner_x2000 (x2000_v12) SPL: ddr_reg_value 0xd4, LPDDR2 == 3
    V12,
    /// Ingenic cloner / ballaswag spl.bin: DDR TLV 0xb8, LPDDR2 == 2
    Cloner,
    Unknown(u32),
}

#[derive(Debug, Clone)]
pub struct DdrInfo {
    pub name: String,
    pub type_raw: u32,
    pub type_name: &'static str,
    pub freq: u32,
    pub gen: HeaderGen,
}

#[derive(Debug, Clone)]
pub struct Stage1Info {
    pub uart_idx: u32,
    pub baud: u32,
    pub ddr: Option<DdrInfo>,
    pub code_len: usize,
    pub warnings: Vec<String>,
    pub errors: Vec<String>,
}

fn type_name(gen: &HeaderGen, t: u32) -> &'static str {
    let v12 = ["DDR4", "DDR3", "LPDDR", "LPDDR2", "LPDDR3", "DDR2"];
    let cloner = ["DDR3", "LPDDR", "LPDDR2", "LPDDR3", "DDR2"];
    let tbl: &[&str] = match gen {
        HeaderGen::V12 => &v12,
        HeaderGen::Cloner => &cloner,
        HeaderGen::Unknown(_) => &[],
    };
    tbl.get(t as usize).copied().unwrap_or("?")
}

pub fn inspect_stage1(img: &[u8], max: usize) -> Stage1Info {
    let mut s = Stage1Info { uart_idx: 0, baud: 0, ddr: None, code_len: 0, warnings: vec![], errors: vec![] };
    if img.len() > max {
        s.errors.push(format!("{} bytes exceeds the stage1 limit of {} bytes", img.len(), max));
    }
    if img.len() <= HEADER_SIZE || le32(img, 0) != Some(MAGIC_INFO) {
        s.errors.push(
            "no parameter header (expected 'FIDB' TLV at offset 0). A raw u-boot-spl.bin \
             cannot be loaded directly; build it with tools/splhdr/nocry_splhdr.py"
                .into(),
        );
        return s;
    }
    s.code_len = img.len() - HEADER_SIZE;
    let mut off = 0usize;
    while off + 8 <= HEADER_SIZE {
        let (Some(magic), Some(size)) = (le32(img, off), le32(img, off + 4)) else { break };
        if magic == 0 {
            break;
        }
        if magic != MAGIC_INFO && magic != MAGIC_DDR {
            break; // the SPL stops parsing at an unknown magic; trailing bytes are ignored
        }
        let size = size as usize;
        let p = off + 8;
        if p + size > HEADER_SIZE {
            s.errors.push(format!("TLV at {off:#x} overruns the header"));
            break;
        }
        match magic {
            MAGIC_INFO => {
                s.uart_idx = le32(img, p + 0x10).unwrap_or(0);
                s.baud = le32(img, p + 0x14).unwrap_or(0);
            }
            MAGIC_DDR if size >= 0xb8 => {
                let name_raw = &img[p..p + 32];
                let name = String::from_utf8_lossy(name_raw.split(|&c| c == 0).next().unwrap_or(&[])).to_string();
                let type_raw = le32(img, p + 36).unwrap_or(u32::MAX);
                let freq = le32(img, p + 40).unwrap_or(0);
                let gen = match size {
                    0xd4 => HeaderGen::V12,
                    0xb8 => HeaderGen::Cloner,
                    n => HeaderGen::Unknown(n as u32),
                };
                let tn = type_name(&gen, type_raw);
                if let HeaderGen::Unknown(n) = gen {
                    s.warnings.push(format!("unrecognised DDR block size {n:#x}"));
                }
                s.ddr = Some(DdrInfo { name, type_raw, type_name: tn, freq, gen });
            }
            MAGIC_DDR => s.errors.push(format!("DDR TLV too small ({size:#x})")),
            _ => break, // the SPL stops parsing at an unknown magic too
        }
        off = p + size;
    }
    match &s.ddr {
        None => s.errors.push("no DDR parameter block".into()),
        Some(d) => {
            if d.type_name == "?" {
                s.errors.push(format!("DDR type {} is not valid for a {:?} header", d.type_raw, d.gen));
            }
            if d.gen == HeaderGen::Cloner {
                s.warnings.push(
                    "cloner-generation header (Ingenic cloner / ballaswag spl.bin). Only valid with the SPL it \
                     shipped with; the k1-debian v12 SPL would read its DDR type as a different memory type"
                        .into(),
                );
            }
            if d.freq == 0 || d.freq > 1_066_000_000 / 2 {
                s.errors.push(format!("implausible DDR frequency {}", d.freq));
            }
        }
    }
    s
}

#[derive(Debug, Clone, PartialEq)]
pub enum Stage2Kind {
    NocryUms { read_only: bool },
    NocryFastboot,
    IngenicBurner,
    Unknown,
}

impl fmt::Display for Stage2Kind {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Stage2Kind::NocryUms { read_only: true } => write!(f, "nocry-ums (read-only USB mass storage)"),
            Stage2Kind::NocryUms { read_only: false } => write!(f, "nocry-ums (read-write USB mass storage)"),
            Stage2Kind::NocryFastboot => write!(f, "nocry fastboot"),
            Stage2Kind::IngenicBurner => write!(f, "Ingenic cloner burner (vendor protocol)"),
            Stage2Kind::Unknown => write!(f, "unknown"),
        }
    }
}

fn contains(h: &[u8], n: &[u8]) -> bool {
    h.windows(n.len()).any(|w| w == n)
}

pub fn inspect_stage2(img: &[u8]) -> (Stage2Kind, Vec<String>, Vec<String>) {
    let (mut warn, mut err) = (vec![], vec![]);
    if img.is_empty() {
        err.push("empty stage2".into());
    }
    if le32(img, 0) == Some(MAGIC_INFO) {
        err.push("stage2 starts with an SPL parameter header -- stage1 and stage2 swapped?".into());
    }
    if img.len() > 32 << 20 {
        err.push(format!("stage2 is {} bytes; refusing anything over 32 MiB", img.len()));
    }
    let kind = if contains(img, b"Board: nocry-ums-ro") {
        Stage2Kind::NocryUms { read_only: true }
    } else if contains(img, b"Board: nocry-ums") {
        Stage2Kind::NocryUms { read_only: false }
    } else if contains(img, b"Board: nocry-fastboot") {
        Stage2Kind::NocryFastboot
    } else if contains(img, b"burner_x2000") || contains(img, b"vendor burnner interface") {
        warn.push(
            "this is Ingenic's cloner burner. It implements eFuse / secure-boot burning commands; \
             only drive it with tooling that allow-lists its requests"
                .into(),
        );
        Stage2Kind::IngenicBurner
    } else {
        warn.push("stage2 not recognised (no nocry or burner board string)".into());
        Stage2Kind::Unknown
    };
    (kind, warn, err)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hdr(ddr_size: u32, dtype: u32, freq: u32) -> Vec<u8> {
        let mut v = vec![];
        v.extend(MAGIC_INFO.to_le_bytes());
        v.extend(0x94u32.to_le_bytes());
        let mut gi = vec![0u8; 0x94];
        gi[0x10..0x14].copy_from_slice(&2u32.to_le_bytes());
        gi[0x14..0x18].copy_from_slice(&115200u32.to_le_bytes());
        v.extend(gi);
        v.extend(MAGIC_DDR.to_le_bytes());
        v.extend(ddr_size.to_le_bytes());
        let mut d = vec![0u8; ddr_size as usize];
        d[..8].copy_from_slice(b"W97BV6MK");
        d[36..40].copy_from_slice(&dtype.to_le_bytes());
        d[40..44].copy_from_slice(&freq.to_le_bytes());
        v.extend(d);
        v.resize(HEADER_SIZE, 0);
        v.extend([0x02, 0x00, 0x11, 0x04]); // some code
        v
    }

    #[test]
    fn v12_header_ok() {
        let s = inspect_stage1(&hdr(0xd4, 3, 500_000_000), 0x6000);
        assert!(s.errors.is_empty(), "{:?}", s.errors);
        let d = s.ddr.unwrap();
        assert_eq!((d.gen, d.type_name, d.freq), (HeaderGen::V12, "LPDDR2", 500_000_000));
        assert_eq!((s.uart_idx, s.baud, s.code_len), (2, 115200, 4));
    }

    #[test]
    fn cloner_header_warns() {
        let s = inspect_stage1(&hdr(0xb8, 2, 400_000_000), 0x6000);
        assert!(s.errors.is_empty());
        assert_eq!(s.ddr.unwrap().type_name, "LPDDR2");
        assert!(s.warnings.iter().any(|w| w.contains("cloner")));
    }

    #[test]
    fn raw_spl_rejected() {
        let s = inspect_stage1(&[0x02, 0x00, 0x11, 0x04, 0, 0, 0, 0], 0x6000);
        assert!(s.errors.iter().any(|e| e.contains("no parameter header")));
    }

    #[test]
    fn swapped_images_detected() {
        let (_, _, e) = inspect_stage2(&hdr(0xd4, 3, 500_000_000));
        assert!(e.iter().any(|e| e.contains("swapped")));
    }

    #[test]
    fn stage2_kinds() {
        assert_eq!(inspect_stage2(b"..Board: nocry-ums-ro (Ingenic..").0, Stage2Kind::NocryUms { read_only: true });
        assert_eq!(inspect_stage2(b"..Board: nocry-ums (Ingenic..").0, Stage2Kind::NocryUms { read_only: false });
        assert_eq!(inspect_stage2(b"Board: burner_x2000 (").0, Stage2Kind::IngenicBurner);
        assert_eq!(inspect_stage2(b"..Board: nocry-fastboot (Ingenic..").0, Stage2Kind::NocryFastboot);
    }
}
