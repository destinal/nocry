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

    /// Adapt the partition table in a boot area to the storage it goes onto, the way Creality's
    /// factory tool does: the usable size is the storage size rounded down to whole MiB, the
    /// last partition (userdata) reaches to its end, and the header and the protective MBR
    /// say so. The other partitions stay where the loader expects them. Returns the new table.
    pub fn fit_to_disk(b: &mut [u8], disk_bytes: u64) -> Result<Gpt> {
        let g = Gpt::parse(b)?;
        let usable = (disk_bytes >> 20) << 11; // sectors
        let (eoff, esize) = ((g.entries_lba * SECTOR) as usize, g.esize as usize);
        // the entry that starts last
        let mut last: Option<(usize, u64)> = None;
        for i in 0..g.nentries as usize {
            let e = &b[eoff + i * esize..eoff + (i + 1) * esize];
            if e[..16].iter().all(|&x| x == 0) {
                continue;
            }
            let first = u64le(e, 32);
            if last.is_none_or(|(_, f)| first > f) {
                last = Some((i, first));
            }
        }
        let (idx, first) = last.ok_or_else(|| anyhow::anyhow!("partition table has no partitions"))?;
        const MIN_LAST: u64 = 64 << 11; // at least 64 MiB for the last partition
        if usable < first + MIN_LAST {
            bail!(
                "the storage ({} MiB) is too small for this partition layout: its last partition starts at {} MiB",
                disk_bytes >> 20,
                first >> 11
            );
        }
        let end = usable - 1;
        let e = eoff + idx * esize;
        b[e + 40..e + 48].copy_from_slice(&end.to_le_bytes());
        b[512 + 32..512 + 40].copy_from_slice(&end.to_le_bytes()); // "alternate" LBA, as the factory tool sets it
        b[512 + 48..512 + 56].copy_from_slice(&end.to_le_bytes()); // last usable LBA
        // protective MBR: number of sectors after LBA 0
        let n = end.min(0xffff_ffff) as u32;
        b[0x1ca..0x1ce].copy_from_slice(&n.to_le_bytes());
        let elen = (g.nentries * g.esize) as usize;
        let ecrc = crc32fast::hash(&b[eoff..eoff + elen]);
        b[512 + 88..512 + 92].copy_from_slice(&ecrc.to_le_bytes());
        b[512 + 16..512 + 20].fill(0);
        let hsize = u32le(&b[512..], 12) as usize;
        let hcrc = crc32fast::hash(&b[512..512 + hsize]);
        b[512 + 16..512 + 20].copy_from_slice(&hcrc.to_le_bytes());
        Gpt::parse(b)
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

#[cfg(test)]
mod tests {
    use super::*;

    /// A small table: three partitions, the last one open-ended, laid out like the boards'.
    fn sample(disk_sectors: u64) -> Vec<u8> {
        let mut b = vec![0u8; GPT_AREA];
        b[0x1fe] = 0x55;
        b[0x1ff] = 0xaa;
        b[0x1c2] = 0xee;
        let parts: [(&str, u64, u64); 3] = [("ota", 0x800, 0xfff), ("rootfs", 0x1000, 0x4fff), ("userdata", 0x5000, disk_sectors - 1)];
        for (i, (name, first, last)) in parts.iter().enumerate() {
            let e = 1024 + i * 128;
            b[e] = 1; // any non-zero type GUID
            b[e + 32..e + 40].copy_from_slice(&first.to_le_bytes());
            b[e + 40..e + 48].copy_from_slice(&last.to_le_bytes());
            for (k, c) in name.encode_utf16().enumerate() {
                b[e + 56 + 2 * k..e + 58 + 2 * k].copy_from_slice(&c.to_le_bytes());
            }
        }
        let h = 512;
        b[h..h + 8].copy_from_slice(b"EFI PART");
        b[h + 12..h + 16].copy_from_slice(&92u32.to_le_bytes());
        b[h + 24..h + 32].copy_from_slice(&1u64.to_le_bytes());
        b[h + 72..h + 80].copy_from_slice(&2u64.to_le_bytes());
        b[h + 80..h + 84].copy_from_slice(&11u32.to_le_bytes());
        b[h + 84..h + 88].copy_from_slice(&128u32.to_le_bytes());
        let ecrc = crc32fast::hash(&b[1024..1024 + 11 * 128]);
        b[h + 88..h + 92].copy_from_slice(&ecrc.to_le_bytes());
        let hcrc = crc32fast::hash(&b[h..h + 92]);
        b[h + 16..h + 20].copy_from_slice(&hcrc.to_le_bytes());
        b
    }

    #[test]
    fn fits_to_larger_and_smaller_storage() {
        let mut b = sample(15269888);
        // 7457.8 MiB of storage: usable is 7457 MiB
        let g = Gpt::fit_to_disk(&mut b, 15273600 * 512).unwrap();
        assert_eq!(g.find("userdata").unwrap().last_lba, 15271935);
        assert_eq!(g.alt_lba, 15271935);
        assert_eq!(g.find("rootfs").unwrap().last_lba, 0x4fff);
        assert_eq!(u32le(&b, 0x1ca), 15271935);
        // a 32 GiB part
        let g = Gpt::fit_to_disk(&mut b, 31 << 30).unwrap();
        assert_eq!(g.find("userdata").unwrap().size(), (31u64 << 30) - 0x5000 * 512);
        // and back down
        let g = Gpt::fit_to_disk(&mut b, 4 << 30).unwrap();
        assert_eq!(g.required_size(), 4 << 30);
        // too small for the fixed part
        assert!(Gpt::fit_to_disk(&mut b, 16 << 20).is_err());
    }
}
