//! Minimal squashfs 4.0 reader: fetch one small file by path from a gzip-compressed image
//! held in memory. Enough to read `etc/ota_info` from a Creality rootfs on every platform.

use std::io::Read;

fn u16le(b: &[u8], o: usize) -> Option<u16> {
    Some(u16::from_le_bytes(b.get(o..o + 2)?.try_into().ok()?))
}
fn u32le(b: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(o..o + 4)?.try_into().ok()?))
}
fn u64le(b: &[u8], o: usize) -> Option<u64> {
    Some(u64::from_le_bytes(b.get(o..o + 8)?.try_into().ok()?))
}

fn inflate(data: &[u8], limit: usize) -> Option<Vec<u8>> {
    let mut out = Vec::new();
    flate2::read::ZlibDecoder::new(data).take(limit as u64 + 1).read_to_end(&mut out).ok()?;
    (out.len() <= limit).then_some(out)
}

fn unlzo(data: &[u8], limit: usize) -> Option<Vec<u8>> {
    let out = lzokay_native::decompress_all(data, Some(limit)).ok()?;
    (out.len() <= limit).then_some(out)
}

const COMP_GZIP: u16 = 1;
const COMP_LZO: u16 = 3;

struct Sq<'a> {
    img: &'a [u8],
    comp: u16,
    block_size: usize,
    inode_table: u64,
    dir_table: u64,
    frag_table: u64,
}

impl<'a> Sq<'a> {
    fn unpack(&self, data: &[u8], limit: usize) -> Option<Vec<u8>> {
        match self.comp {
            COMP_LZO => unlzo(data, limit),
            _ => inflate(data, limit),
        }
    }

    /// One metadata block at absolute `pos`: (data, position of the next block).
    fn meta_block(&self, pos: u64) -> Option<(Vec<u8>, u64)> {
        let p = pos as usize;
        let h = u16le(self.img, p)?;
        let n = (h & 0x7fff) as usize;
        let raw = self.img.get(p + 2..p + 2 + n)?;
        let data = if h & 0x8000 != 0 { raw.to_vec() } else { self.unpack(raw, 8192)? };
        Some((data, pos + 2 + n as u64))
    }

    /// `len` bytes of a metadata stream starting at block `pos`, offset `off` inside it.
    fn meta(&self, mut pos: u64, off: usize, len: usize) -> Option<Vec<u8>> {
        let mut out = Vec::with_capacity(len);
        let mut skip = off;
        while out.len() < len {
            let (b, next) = self.meta_block(pos)?;
            let part = b.get(skip..)?;
            let take = part.len().min(len - out.len());
            out.extend_from_slice(&part[..take]);
            skip = 0;
            pos = next;
        }
        Some(out)
    }

    fn inode(&self, block: u64, off: usize, len: usize) -> Option<Vec<u8>> {
        self.meta(self.inode_table + block, off, len)
    }

    /// Directory inode at (block, off): (inode block, inode offset) of the entry `name`.
    fn lookup(&self, block: u64, off: usize, name: &[u8]) -> Option<(u64, usize)> {
        let h = self.inode(block, off, 40)?;
        let (start, size, doff) = match u16le(&h, 0)? {
            1 => (u32le(&h, 16)? as u64, u16le(&h, 24)? as usize, u16le(&h, 26)? as usize),
            8 => (u32le(&h, 24)? as u64, u32le(&h, 20)? as usize, u16le(&h, 34)? as usize),
            _ => return None,
        };
        if size < 3 {
            return None;
        }
        let d = self.meta(self.dir_table + start, doff, size - 3)?;
        let mut p = 0;
        while p + 12 <= d.len() {
            let count = u32le(&d, p)? as usize + 1;
            let iblock = u32le(&d, p + 4)? as u64;
            p += 12;
            for _ in 0..count {
                let ioff = u16le(&d, p)? as usize;
                let nlen = u16le(&d, p + 6)? as usize + 1;
                let n = d.get(p + 8..p + 8 + nlen)?;
                if n == name {
                    return Some((iblock, ioff));
                }
                p += 8 + nlen;
            }
        }
        None
    }

    fn data_block(&self, pos: u64, word: u32) -> Option<Vec<u8>> {
        let n = (word & 0xff_ffff) as usize;
        let raw = self.img.get(pos as usize..pos as usize + n)?;
        if word & (1 << 24) != 0 {
            Some(raw.to_vec())
        } else {
            self.unpack(raw, self.block_size)
        }
    }

    /// Contents of the regular file whose inode is at (block, off); refuses files over `max`.
    fn file(&self, block: u64, off: usize, max: usize) -> Option<Vec<u8>> {
        let h = self.inode(block, off, 56)?;
        let (start, frag, frag_off, size, hdr) = match u16le(&h, 0)? {
            2 => (u32le(&h, 16)? as u64, u32le(&h, 20)?, u32le(&h, 24)? as usize, u32le(&h, 28)? as usize, 32),
            9 => (u64le(&h, 16)?, u32le(&h, 44)?, u32le(&h, 48)? as usize, u64le(&h, 24)? as usize, 56),
            _ => return None,
        };
        if size > max {
            return None;
        }
        let nblocks = if frag == u32::MAX { size.div_ceil(self.block_size) } else { size / self.block_size };
        let words = self.inode(block, off, hdr + 4 * nblocks)?;
        let mut out = Vec::with_capacity(size);
        let mut pos = start;
        for i in 0..nblocks {
            let w = u32le(&words, hdr + 4 * i)?;
            if w & 0xff_ffff == 0 {
                out.resize(out.len() + self.block_size.min(size - out.len()), 0); // sparse
            } else {
                out.extend_from_slice(&self.data_block(pos, w)?);
                pos += (w & 0xff_ffff) as u64;
            }
        }
        if frag != u32::MAX {
            // fragment table: u64 pointers to metadata blocks of 512 16-byte entries
            let ptr = u64le(self.img, self.frag_table as usize + 8 * (frag as usize / 512))?;
            let e = self.meta(ptr, 16 * (frag as usize % 512), 16)?;
            let fb = self.data_block(u64le(&e, 0)?, u32le(&e, 8)?)?;
            out.extend_from_slice(fb.get(frag_off..frag_off + (size - out.len()))?);
        }
        out.truncate(size);
        (out.len() == size).then_some(out)
    }
}

/// Reads the regular file at `path` (e.g. "etc/ota_info", no symlinks followed) from a
/// gzip- or lzo-compressed squashfs image. None if the image is not such a squashfs 4, or the file is missing
/// or larger than `max` bytes.
pub fn read_file(img: &[u8], path: &str, max: usize) -> Option<Vec<u8>> {
    let comp = u16le(img, 20)?;
    if img.get(..4)? != b"hsqs" || !(comp == COMP_GZIP || comp == COMP_LZO) || u16le(img, 28)? != 4 {
        return None;
    }
    let sq = Sq {
        img,
        comp,
        block_size: u32le(img, 12)? as usize,
        inode_table: u64le(img, 64)?,
        dir_table: u64le(img, 72)?,
        frag_table: u64le(img, 80)?,
    };
    if !(4096..=1 << 20).contains(&sq.block_size) {
        return None;
    }
    let root = u64le(img, 32)?;
    let (mut block, mut off) = (root >> 16, (root & 0xffff) as usize);
    for part in path.split('/').filter(|p| !p.is_empty()) {
        (block, off) = sq.lookup(block, off, part.as_bytes())?;
    }
    sq.file(block, off, max)
}

#[cfg(test)]
mod tests {
    use super::read_file;
    use sha2::{Digest, Sha256};
    // mksquashfs -comp gzip of: etc/ota_info, etc/zeros (5000 zero bytes), usr/lib/big.bin (300000 random bytes)
    const IMG: &[u8] = include_bytes!("../tests/data/small.sqfs");

    #[test]
    fn reads_files() {
        assert_eq!(read_file(IMG, "etc/ota_info", 4096).unwrap(), b"ota_board_name=TEST\nota_version=9.8.7.6\n");
        assert_eq!(read_file(IMG, "/etc/zeros", 8192).unwrap(), vec![0u8; 5000]);
        let big = read_file(IMG, "usr/lib/big.bin", 1 << 20).unwrap(); // several data blocks + a fragment
        assert_eq!(big.len(), 300000);
        assert_eq!(format!("{:x}", Sha256::digest(&big)), "0c9fb8d6f2d7d7849914c6668dd54ebbe751407aa66a2c27007d0889d281b526");
    }

    #[test]
    fn refuses_what_it_should() {
        assert!(read_file(IMG, "etc/missing", 4096).is_none());
        assert!(read_file(IMG, "etc", 4096).is_none()); // a directory
        assert!(read_file(IMG, "usr/lib/big.bin", 1000).is_none()); // over the size limit
        assert!(read_file(b"not a squashfs image", "etc/ota_info", 4096).is_none());
        assert!(read_file(&IMG[..200], "etc/ota_info", 4096).is_none()); // truncated
    }
}
