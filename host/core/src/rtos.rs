//! The header of the image in the `rtos` / `rtos2` partitions.
//!
//! The board's SPL (Ingenic U-Boot 2013.07, `common/spl/spl_rtos.h`) reads the first sector of
//! the active slot's rtos partition, checks this header, loads `img_end - img_start` bytes to
//! `img_start` and starts it on the second CPU core. A header that fails the check makes the
//! SPL hang, so a slot is only startable when this parses. See docs/rtos-partition.md.

use anyhow::{bail, Result};

/// "RTOS" as the SPL compares it (little-endian word at offset 8).
pub const TAG: u32 = 0x534f_5452;
/// Byte-swapped tag the SPL expects on boards with secure boot enabled (signed image).
pub const TAG_SECURE: u32 = 0x5254_4f53;
pub const HEADER_LEN: usize = 0x24;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RtosHeader {
    /// the first two instructions: a branch over the header and its delay slot
    pub code: [u32; 2],
    pub tag: u32,
    /// bit 0: the SPL waits until the image changes its own first word, then parks the second core
    pub version: u32,
    /// load and entry address
    pub img_start: u32,
    pub img_end: u32,
    pub heap_start: u32,
    pub heap_end: u32,
    /// address of a variable the SPL fills with the size of an optional data partition
    pub mapped_rtosdata_size: u32,
}

impl RtosHeader {
    /// Parses and applies the SPL's own checks (tag, start below end), plus sanity limits.
    pub fn parse(b: &[u8]) -> Result<Self> {
        if b.len() < HEADER_LEN {
            bail!("rtos image is shorter than its header");
        }
        let w = |i: usize| u32::from_le_bytes(b[i * 4..i * 4 + 4].try_into().unwrap());
        let h = RtosHeader {
            code: [w(0), w(1)],
            tag: w(2),
            version: w(3),
            img_start: w(4),
            img_end: w(5),
            heap_start: w(6),
            heap_end: w(7),
            mapped_rtosdata_size: w(8),
        };
        if h.tag == TAG_SECURE {
            bail!("rtos image is the signed kind for boards with secure boot");
        }
        if h.tag != TAG {
            bail!("no rtos image (tag {:#010x})", h.tag);
        }
        if h.img_start >= h.img_end {
            bail!("rtos image header is damaged (start {:#x}, end {:#x})", h.img_start, h.img_end);
        }
        // loaded through kseg0, into RAM
        if !(0x8000_0000..0xa000_0000).contains(&h.img_start) || h.image_len() > 64 << 20 {
            bail!("rtos image header is implausible (start {:#x}, end {:#x})", h.img_start, h.img_end);
        }
        Ok(h)
    }

    /// Bytes the SPL loads from the start of the partition (header included).
    pub fn image_len(&self) -> u64 {
        (self.img_end - self.img_start) as u64
    }

    /// As the SPL reads it: whole 512-byte sectors.
    pub fn loaded_len(&self) -> u64 {
        self.image_len().div_ceil(512) * 512
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Vec<u8> {
        // values of a retail Nebula Pad image
        let w: [u32; 9] = [0x1000_0008, 0, TAG, 1, 0x8600_0000, 0x8606_7948, 0x8607_0400, 0x86fa_1200, 0x8607_0400];
        w.iter().flat_map(|x| x.to_le_bytes()).collect()
    }

    #[test]
    fn parses_retail_header() {
        let h = RtosHeader::parse(&sample()).unwrap();
        assert_eq!(h.img_start, 0x8600_0000);
        assert_eq!(h.image_len(), 0x67948);
        assert_eq!(h.loaded_len(), 0x67a00);
        assert_eq!(h.version & 1, 1);
    }

    #[test]
    fn rejects_what_the_spl_rejects() {
        assert!(RtosHeader::parse(&[0u8; 512]).is_err());
        let mut b = sample();
        b[8..12].copy_from_slice(&TAG_SECURE.to_le_bytes());
        assert!(RtosHeader::parse(&b).unwrap_err().to_string().contains("secure boot"));
        let mut b = sample();
        b[20..24].copy_from_slice(&0x8600_0000u32.to_le_bytes());
        assert!(RtosHeader::parse(&b).is_err());
        assert!(RtosHeader::parse(&sample()[..16]).is_err());
    }
}
