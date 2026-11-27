//! Where the parts of a firmware are held while nocry works with them.
//!
//! On a computer they are simply in memory. On the board itself (nocry running in the rescue
//! system, about 230 MB of RAM for everything) a 140 MB system image cannot be: with
//! `NOCRY_STAGE_DIR` set, parts are unpacked into files in that folder and mapped instead, so
//! the kernel keeps in memory only the pages being read. Staged files are removed when the
//! part is no longer used.

use anyhow::{Context, Result};
use std::io::{Read, Write};
use std::ops::Deref;
use std::path::{Path, PathBuf};

pub enum Blob {
    Mem(Vec<u8>),
    Map { map: memmap2::Mmap, start: usize, len: usize, delete: Option<PathBuf> },
}

/// A part of a firmware, shared between the package and the plan made from it.
pub type Part = std::sync::Arc<Blob>;

impl Deref for Blob {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        match self {
            Blob::Mem(v) => v,
            Blob::Map { map, start, len, .. } => &map[*start..*start + *len],
        }
    }
}

impl std::fmt::Debug for Blob {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Blob({} bytes, {})", self.len(), if matches!(self, Blob::Mem(_)) { "in memory" } else { "mapped" })
    }
}

impl Drop for Blob {
    fn drop(&mut self) {
        if let Blob::Map { delete: Some(p), .. } = self {
            let _ = std::fs::remove_file(p);
        }
    }
}

impl From<Vec<u8>> for Blob {
    fn from(v: Vec<u8>) -> Self {
        Blob::Mem(v)
    }
}

/// The folder to unpack into instead of memory, when one is set and exists.
pub fn stage_dir() -> Option<PathBuf> {
    std::env::var_os("NOCRY_STAGE_DIR").map(PathBuf::from).filter(|p| p.is_dir())
}

fn safe(name: &str) -> String {
    name.chars().map(|c| if c.is_ascii_alphanumeric() || "._-".contains(c) { c } else { '_' }).collect()
}

impl Blob {
    /// `len` bytes of an existing file from `start`, mapped; the file is left where it is.
    pub fn map_range(path: &Path, start: u64, len: u64) -> Result<Blob> {
        Self::map(path, start, len, false)
    }

    fn map(path: &Path, start: u64, len: u64, delete: bool) -> Result<Blob> {
        if len == 0 {
            if delete {
                let _ = std::fs::remove_file(path);
            }
            return Ok(Blob::Mem(Vec::new()));
        }
        let f = std::fs::File::open(path).with_context(|| format!("opening {}", path.display()))?;
        let size = f.metadata()?.len();
        if start.checked_add(len).is_none_or(|e| e > size) {
            anyhow::bail!("{}: the part lies outside the file", path.display());
        }
        // read-only and private: nothing here writes through the mapping
        let map = unsafe { memmap2::Mmap::map(&f) }.with_context(|| format!("mapping {}", path.display()))?;
        Ok(Blob::Map { map, start: start as usize, len: len as usize, delete: delete.then(|| path.to_path_buf()) })
    }

    /// A whole file: mapped when staging is on, read into memory otherwise.
    pub fn from_file(path: &Path) -> Result<Blob> {
        if stage_dir().is_some() {
            let len = std::fs::metadata(path).with_context(|| format!("reading {}", path.display()))?.len();
            Self::map(path, 0, len, false)
        } else {
            Ok(Blob::Mem(std::fs::read(path).with_context(|| format!("reading {}", path.display()))?))
        }
    }

    /// Everything `reader` gives: into a staged file when staging is on, else into memory.
    pub fn collect(name: &str, reader: &mut dyn Read, size_hint: usize) -> Result<Blob> {
        let Some(dir) = stage_dir() else {
            let mut v = Vec::with_capacity(size_hint);
            reader.read_to_end(&mut v)?;
            return Ok(Blob::Mem(v));
        };
        let path = dir.join(format!("{}.{}.part", safe(name), std::process::id()));
        let r = (|| -> Result<u64> {
            let mut f = std::fs::File::create(&path).with_context(|| format!("creating {}", path.display()))?;
            let n = std::io::copy(reader, &mut f)?;
            f.flush()?;
            Ok(n)
        })();
        match r {
            Ok(n) => Self::map(&path, 0, n, true),
            Err(e) => {
                let _ = std::fs::remove_file(&path);
                Err(e)
            }
        }
    }

    /// The pieces one after another, as one part.
    pub fn concat<'a>(name: &str, pieces: impl Iterator<Item = &'a [u8]>, size_hint: usize) -> Result<Blob> {
        struct Chain<'a, I: Iterator<Item = &'a [u8]>> {
            it: I,
            cur: &'a [u8],
        }
        impl<'a, I: Iterator<Item = &'a [u8]>> Read for Chain<'a, I> {
            fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
                while self.cur.is_empty() {
                    match self.it.next() {
                        Some(p) => self.cur = p,
                        None => return Ok(0),
                    }
                }
                let n = self.cur.len().min(buf.len());
                buf[..n].copy_from_slice(&self.cur[..n]);
                self.cur = &self.cur[n..];
                Ok(n)
            }
        }
        Self::collect(name, &mut Chain { it: pieces, cur: &[] }, size_hint)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mapped_ranges_and_staged_parts_read_like_memory() {
        // one test: the environment variable is shared by the whole test program
        let dir = std::env::temp_dir().join(format!("nocry-blob-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let data: Vec<u8> = (0..300_000u32).map(|i| (i * 7 + i / 251) as u8).collect();
        let file = dir.join("whole.bin");
        std::fs::write(&file, &data).unwrap();

        assert_eq!(&Blob::map_range(&file, 1000, 5000).unwrap()[..], &data[1000..6000]);
        assert!(Blob::map_range(&file, 299_000, 2000).is_err());
        assert_eq!(Blob::map_range(&file, 10, 0).unwrap().len(), 0);

        // in memory
        std::env::remove_var("NOCRY_STAGE_DIR");
        assert!(matches!(Blob::collect("x", &mut &data[..], 0).unwrap(), Blob::Mem(_)));
        let joined = Blob::concat("y", [&data[..100], &data[100..]].into_iter(), 0).unwrap();
        assert_eq!(&joined[..], &data[..]);

        // staged: same bytes, a file while in use, gone afterwards
        std::env::set_var("NOCRY_STAGE_DIR", &dir);
        let staged = Blob::concat("root fs/..", [&data[..70_000], &data[70_000..]].into_iter(), 0).unwrap();
        assert_eq!(&staged[..], &data[..]);
        let Blob::Map { delete: Some(p), .. } = &staged else { panic!("not staged") };
        let p = p.clone();
        assert!(p.starts_with(&dir) && p.exists());
        assert!(matches!(Blob::from_file(&file).unwrap(), Blob::Map { delete: None, .. }));
        drop(staged);
        assert!(!p.exists() && file.exists());
        std::env::remove_var("NOCRY_STAGE_DIR");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
