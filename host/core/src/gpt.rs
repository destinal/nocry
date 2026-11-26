//! Minimal primary-GPT reader/patcher for the factory boot area.
//! LBA0 = protective MBR, LBA1 = header, entries from LBA2; SPL starts at 0x4400.

use anyhow::{bail, Result};

pub const SECTOR: u64 = 512;
pub const GPT_AREA: usize = 0x4400; // MBR + header + 128 entries * 128 B

#[derive(Debug, Clone, PartialEq)]
pub struct Part {
    pub name: String,
    pub first_lba: u64,
    pub last_lba: u64,
}

impl Part {
    pub fn offset(&self) -> u64 {
        self.first_lba * SECTOR
    }
    pub fn size(&self) -> u64 {
        (self.last_lba - self.first_lba + 1) * SECTOR
    }
}

#[derive(Debug, Clone)]
#[allow(dead_code)] // alt_lba/last_usable: informational
pub struct Gpt {
    pub parts: Vec<Part>,
    pub alt_lba: u64,
    pub last_usable: u64,
    entries_lba: u64,
    nentries: u32,
    esize: u32,
}

fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64le(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

impl Gpt {
    /// Parse from the first GPT_AREA bytes of a disk/boot image. Verifies CRCs.
    pub fn parse(b: &[u8]) -> Result<Gpt> {
        if b.len() < GPT_AREA {
            bail!("need {GPT_AREA:#x} bytes for GPT");
        }
        let h = &b[512..];
        if &h[..8] != b"EFI PART" {
            bail!("no GPT signature at LBA1");
        }
        let hsize = u32le(h, 12) as usize;
        let mut hc = h[..hsize].to_vec();
        hc[16..20].fill(0);
        if crc32fast::hash(&hc) != u32le(h, 16) {
            bail!("GPT header CRC mismatch");
        }
        let entries_lba = u64le(h, 72);
        let nentries = u32le(h, 80);
        let esize = u32le(h, 84);
        let eoff = (entries_lba * SECTOR) as usize;
        let elen = (nentries * esize) as usize;
        if eoff + elen > b.len() {
            bail!("GPT entries outside the buffer");
        }
        if crc32fast::hash(&b[eoff..eoff + elen]) != u32le(h, 88) {
            bail!("GPT entries CRC mismatch");
        }
        let mut parts = vec![];
        for i in 0..nentries as usize {
            let e = &b[eoff + i * esize as usize..eoff + (i + 1) * esize as usize];
            if e[..16].iter().all(|&x| x == 0) {
                continue;
            }
            let name16: Vec<u16> = e[56..128].chunks(2).map(|c| u16::from_le_bytes([c[0], c[1]])).collect();
            let name = String::from_utf16_lossy(&name16).trim_end_matches('\0').to_string();
            parts.push(Part { name, first_lba: u64le(e, 32), last_lba: u64le(e, 40) });
        }
        Ok(Gpt { parts, alt_lba: u64le(h, 32), last_usable: u64le(h, 48), entries_lba, nentries, esize })
    }

    pub fn find(&self, name: &str) -> Option<&Part> {
        self.parts.iter().find(|p| p.name == name)
    }

    pub fn same_layout(&self, o: &Gpt) -> bool {
        self.parts == o.parts
    }

    /// Bytes the layout needs (end of the last partition).
    pub fn required_size(&self) -> u64 {
        self.parts.iter().map(|p| (p.last_lba + 1) * SECTOR).max().unwrap_or(0)
    }

    /// Give the disk and every partition a fresh random unique GUID and fix both CRCs.
    pub fn randomize_guids(&self, b: &mut [u8]) -> Result<()> {
        let mut rnd = vec![0u8; 16 * (1 + self.nentries as usize)];
        getrandom::getrandom(&mut rnd)?;
        let v4 = |g: &mut [u8], r: &[u8]| {
            g.copy_from_slice(r);
            g[7] = (g[7] & 0x0f) | 0x40; // version 4 (mixed-endian GUID layout)
            g[8] = (g[8] & 0x3f) | 0x80; // RFC 4122 variant
        };
        v4(&mut b[512 + 56..512 + 72], &rnd[..16]);
        let eoff = (self.entries_lba * SECTOR) as usize;
        for i in 0..self.nentries as usize {
            let e = eoff + i * self.esize as usize;
            if b[e..e + 16].iter().all(|&x| x == 0) {
                continue;
            }
            let r = &rnd[16 * (i + 1)..16 * (i + 2)];
            v4(&mut b[e + 16..e + 32], r);
        }
        let elen = (self.nentries * self.esize) as usize;
        let ecrc = crc32fast::hash(&b[eoff..eoff + elen]);
        b[512 + 88..512 + 92].copy_from_slice(&ecrc.to_le_bytes());
        b[512 + 16..512 + 20].fill(0);
        let hsize = u32le(&b[512..], 12) as usize;
        let hcrc = crc32fast::hash(&b[512..512 + hsize]);
        b[512 + 16..512 + 20].copy_from_slice(&hcrc.to_le_bytes());
        Ok(())
    }
}
