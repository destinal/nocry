//! Whole-disk operations: backup to an image file, write an image back (reimage).

use crate::gpt::{Gpt, GPT_AREA};
use crate::restore::Target;
use crate::snmac::{SnMac, RECORD_AREA};
use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;
use std::time::Instant;

const MIB: u64 = 1 << 20;
const CHUNK: u64 = 4 * MIB;

/// "45 s", "2 min 40 s", "1 h 05 min"
pub fn fmt_duration(secs: f64) -> String {
    let s = secs.max(0.0).round() as u64;
    if s < 60 {
        format!("{s} s")
    } else if s < 3600 {
        format!("{} min {:02} s", s / 60, s % 60)
    } else {
        format!("{} h {:02} min", s / 3600, (s % 3600) / 60)
    }
}

struct Progress {
    what: &'static str,
    total: u64,
    t0: Instant,
    last: Instant,
}

impl Progress {
    fn new(what: &'static str, total: u64) -> Self {
        let now = Instant::now();
        Progress { what, total, t0: now, last: now }
    }
    fn tick(&mut self, done: u64, extra: &str) {
        if self.last.elapsed().as_millis() < 1000 && done < self.total {
            return;
        }
        self.last = Instant::now();
        let s = self.t0.elapsed().as_secs_f64().max(0.001);
        let rate = done as f64 / s / 1e6;
        let eta = if rate > 0.0 { (self.total - done) as f64 / 1e6 / rate } else { 0.0 };
        crate::events::progress(self.what, done, self.total, &format!("  {rate:5.1} MB/s  {} left{extra}", fmt_duration(eta)));
    }
    fn done(&self) -> f64 {
        crate::events::progress_end();
        self.t0.elapsed().as_secs_f64()
    }
}

fn identity(mut read: impl FnMut(u64, usize) -> Result<Vec<u8>>) -> Option<SnMac> {
    let g = Gpt::parse(&read(0, GPT_AREA).ok()?).ok()?;
    let p = g.find("sn_mac")?;
    SnMac::parse(&read(p.offset(), RECORD_AREA).ok()?)
}

fn hash_device(t: &Target, label: &'static str) -> Result<String> {
    t.drop_cache(0, t.size);
    let mut h = Sha256::new();
    let mut p = Progress::new(label, t.size);
    let mut off = 0;
    while off < t.size {
        let n = CHUNK.min(t.size - off) as usize;
        h.update(t.read(off, n)?);
        t.drop_cache(off, n as u64);
        off += n as u64;
        p.tick(off, "");
    }
    p.done();
    Ok(format!("{:x}", h.finalize()))
}

/// Full-disk image of `t` into `out` (+ `out.sha256`). Returns the sha256.
pub fn backup(t: &Target, out: &Path, verify: bool, force: bool) -> Result<String> {
    if out.exists() && !force {
        bail!("{} exists; pass --force to overwrite", out.display());
    }
    let part = out.with_extension(format!(
        "{}part",
        out.extension().map(|e| format!("{}.", e.to_string_lossy())).unwrap_or_default()
    ));
    let r = backup_inner(t, out, &part, verify);
    if r.is_err() {
        let _ = std::fs::remove_file(&part); // never leave a partial image behind
    }
    r
}

fn backup_inner(t: &Target, out: &Path, part: &Path, verify: bool) -> Result<String> {
    if let Some(id) = identity(|o, n| t.read(o, n)) {
        crate::say!("device identity: model {:?}, board {:?}, SN valid: {}", id.model(), id.board(), id.sn_valid());
    } else {
        crate::say!("device identity: no valid GPT/sn_mac (blank or damaged board?) - backing up anyway");
    }
    let mut f = File::create(part).with_context(|| format!("creating {}", part.display()))?;
    let mut p = Progress::new("backup", t.size);
    t.drop_cache(0, t.size);
    let step = if t.is_fastboot() { 32 * MIB } else { 8 * MIB };
    let want_local = t.is_fastboot();
    // The device is read on this thread; hashing and file writes run on a second one, so
    // the USB link never waits for them.
    let (tx, rx) = std::sync::mpsc::sync_channel::<Vec<u8>>(4);
    let (h, local) = std::thread::scope(|sc| -> Result<(Sha256, Vec<Vec<u8>>)> {
        let f = &mut f;
        let worker = sc.spawn(move || -> Result<(Sha256, Vec<Vec<u8>>)> {
            let mut h = Sha256::new();
            let mut local = vec![]; // xxh64 per CHUNK, for the on-board verify
            for b in rx {
                h.update(&b);
                if want_local {
                    local.extend(b.chunks(CHUNK as usize).map(Target::local_hash));
                }
                f.write_all(&b).with_context(|| format!("writing {}", part.display()))?;
            }
            f.sync_all()?;
            Ok((h, local))
        });
        let mut off = 0;
        let mut err = None;
        while off < t.size {
            if let Err(e) = crate::events::check_cancelled() {
                err = Some(e);
                break;
            }
            let n = step.min(t.size - off) as usize;
            match t.read(off, n) {
                Ok(b) => {
                    if tx.send(b).is_err() {
                        break; // worker failed; its error is reported below
                    }
                }
                Err(e) => {
                    err = Some(e);
                    break;
                }
            }
            t.drop_cache(off, n as u64);
            off += n as u64;
            p.tick(off, "");
        }
        drop(tx);
        let r = worker.join().map_err(|_| anyhow::anyhow!("backup writer thread panicked"))?;
        match err {
            Some(e) => Err(e),
            None => r,
        }
    })?;
    drop(f);
    let secs = p.done();
    let sum = format!("{:x}", h.finalize());
    std::fs::rename(part, out)?;
    let name = out.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default();
    std::fs::write(format!("{}.sha256", out.display()), format!("{sum}  {name}\n"))?;
    crate::say!(
        "wrote {} ({} bytes) in {:.0}s ({:.1} MB/s)\nsha256 {sum}",
        out.display(),
        t.size,
        secs,
        t.size as f64 / secs / 1e6
    );
    if verify && t.is_fastboot() {
        let mut p = Progress::new("verify (hashing on the board)", t.size);
        let (mut off, mut i) = (0u64, 0usize);
        while off < t.size {
            crate::events::check_cancelled()
                .context("verification stopped: the image file is complete but was not verified")?;
            let n = (256 * MIB).min(t.size - off);
            let dev = t.chunk_hashes(off, n, CHUNK)?.context("no on-board hashing")?;
            if dev[..] != local[i..i + dev.len()] {
                bail!("VERIFY FAILED: the board's data differs from what was read, within {off:#x}..{:#x}", off + n);
            }
            i += dev.len();
            off += n;
            p.tick(off, "");
        }
        p.done();
        crate::say!("verified: on-board hashes of the whole eMMC match the image");
    } else if verify {
        let again = hash_device(t, "verify (re-reading the device)")?;
        if again != sum {
            bail!("VERIFY FAILED: the device read back differently ({again}); the image may be bad");
        }
        crate::say!("verified: a second read of the device has the same sha256");
    }
    Ok(sum)
}

pub struct WriteImageOptions {
    pub yes: bool,
    pub allow_identity_change: bool,
}

/// Write a full-disk image back. Only chunks that differ are written; each batch is
/// flushed, re-read with the page cache dropped, and compared.
/// Returns (bytes that differed, bytes written).
pub fn write_image(t: &Target, img_path: &Path, o: &WriteImageOptions) -> Result<(u64, u64)> {
    let mut img = File::open(img_path).with_context(|| format!("opening {}", img_path.display()))?;
    let isize = img.seek(SeekFrom::End(0))?;
    if isize != t.size {
        bail!(
            "{} is {isize} bytes but the device is {} bytes; a full-disk image must match exactly",
            img_path.display(),
            t.size
        );
    }
    let read_img = |f: &mut File, off: u64, n: usize| -> Result<Vec<u8>> {
        let mut v = vec![0u8; n];
        f.seek(SeekFrom::Start(off))?;
        f.read_exact(&mut v)?;
        Ok(v)
    };
    let head = read_img(&mut img, 0, GPT_AREA)?;
    Gpt::parse(&head).context("the image has no valid GPT; refusing to write it")?;
    let img_id = {
        let mut f2 = File::open(img_path)?;
        identity(move |off, n| read_img(&mut f2, off, n))
    };
    let dev_id = identity(|off, n| t.read(off, n));
    match (&img_id, &dev_id) {
        (Some(i), Some(d)) if i.identity_valid() && d.identity_valid() && i.sn() != d.sn() => {
            crate::say!("image identity:  model {:?} board {:?} SN ...{}", i.model(), i.board(), &i.sn()[i.sn().len().saturating_sub(4)..]);
            crate::say!("device identity: model {:?} board {:?} SN ...{}", d.model(), d.board(), &d.sn()[d.sn().len().saturating_sub(4)..]);
            if !o.allow_identity_change {
                bail!(
                    "the image comes from a different board (SN differs). Writing it would clone that board's \
                     SN/MAC onto this one. Pass --allow-identity-change if that is really what you want, or use \
                     `nocry restore` (keeps this board's identity)"
                );
            }
            crate::say!("note: --allow-identity-change: this board will get the image's SN/MAC");
        }
        (None, Some(_)) => crate::say!("note: the image has no valid sn_mac; this board's identity will be overwritten by it"),
        _ => {}
    }
    let mut p = Progress::new(if o.yes { "reimage" } else { "comparing" }, t.size);
    t.drop_cache(0, t.size);
    let (mut off, mut differ, mut written) = (0u64, 0u64, 0u64);
    const WIN: u64 = 256 * MIB;
    let mut win: Option<Vec<Vec<u8>>> = None;
    // batch of consecutive differing chunks, written together
    let mut batch: Vec<u8> = vec![];
    let mut batch_off = 0u64;
    let flush = |batch: &mut Vec<u8>, at: u64, written: &mut u64| -> Result<()> {
        if !batch.is_empty() {
            t.write_verified(at, batch, "image")?;
            *written += batch.len() as u64;
            batch.clear();
        }
        Ok(())
    };
    while off < t.size {
        let n = CHUNK.min(t.size - off) as usize;
        let want = read_img(&mut img, off, n)?;
        if off % WIN == 0 {
            win = t.chunk_hashes(off, WIN.min(t.size - off), CHUNK)?;
        }
        let same = match &win {
            Some(h) => h[((off % WIN) / CHUNK) as usize] == Target::local_hash(&want),
            None => {
                let have = t.read(off, n)?;
                t.drop_cache(off, n as u64);
                have == want
            }
        };
        if !same {
            differ += n as u64;
            if o.yes {
                if batch.is_empty() {
                    batch_off = off;
                }
                batch.extend_from_slice(&want);
                if batch.len() as u64 >= 64 * MIB {
                    flush(&mut batch, batch_off, &mut written)?;
                }
            }
        } else if o.yes {
            flush(&mut batch, batch_off, &mut written)?;
        }
        off += n as u64;
        p.tick(off, &format!("  differ {} MiB", differ / MIB));
    }
    if o.yes {
        flush(&mut batch, batch_off, &mut written)?;
    }
    let secs = p.done();
    if !o.yes {
        crate::say!(
            "{} MiB of {} MiB differ ({:.0}s). Dry run; re-run with --yes to write them",
            differ / MIB,
            t.size / MIB,
            secs
        );
    } else if differ == 0 {
        crate::say!("nothing to do: the device already matches the image");
    } else {
        crate::say!(
            "wrote and verified {} MiB in {:.0}s. Power-cycle the board to boot it",
            written / MIB,
            secs
        );
    }
    Ok((differ, written))
}

/// What a backup image is, compared with the board it is about to be written to (cheap).
#[derive(Debug, Clone, Default)]
pub struct ImageCheck {
    pub image_bytes: u64,
    pub device_bytes: u64,
    /// product names ("Retail Nebula Pad"), "" when there is no valid identity
    pub image_model: String,
    pub device_model: String,
    /// last 4 characters of the serial numbers, "" when none
    pub image_sn_tail: String,
    pub device_sn_tail: String,
    /// both identities valid and the serial numbers differ: writing clones the image's SN/MAC
    pub different_board: bool,
    /// a .sha256 file sits next to the image
    pub has_checksum: bool,
}

pub fn check_image(t: &Target, img_path: &Path) -> Result<ImageCheck> {
    let mut img = File::open(img_path).with_context(|| format!("opening {}", img_path.display()))?;
    let mut c = ImageCheck { image_bytes: img.seek(SeekFrom::End(0))?, device_bytes: t.size, ..Default::default() };
    if c.image_bytes != c.device_bytes {
        bail!(
            "this file is {} bytes but the board's storage is {} bytes. A backup can only be restored to a board with the same storage size",
            c.image_bytes,
            c.device_bytes
        );
    }
    let mut rd = |off: u64, n: usize| -> Result<Vec<u8>> {
        let mut v = vec![0u8; n];
        img.seek(SeekFrom::Start(off))?;
        img.read_exact(&mut v)?;
        Ok(v)
    };
    Gpt::parse(&rd(0, GPT_AREA)?).context("this file is not a board backup (no valid partition table in it)")?;
    let tail = |s: &str| s[s.len().saturating_sub(4)..].to_string();
    let ii = identity(&mut rd);
    let di = identity(|off, n| t.read(off, n));
    if let Some(i) = &ii {
        c.image_model = crate::snmac::product_name(i.model());
        c.image_sn_tail = tail(i.sn());
    }
    if let Some(d) = &di {
        c.device_model = crate::snmac::product_name(d.model());
        c.device_sn_tail = tail(d.sn());
    }
    if let (Some(i), Some(d)) = (&ii, &di) {
        c.different_board = i.identity_valid() && d.identity_valid() && i.sn() != d.sn();
    }
    c.has_checksum = Path::new(&format!("{}.sha256", img_path.display())).exists();
    Ok(c)
}

/// Checks the image file against its `.sha256` neighbour, if there is one.
pub fn verify_checksum_file(img_path: &Path) -> Result<bool> {
    let Ok(txt) = std::fs::read_to_string(format!("{}.sha256", img_path.display())) else { return Ok(false) };
    let want = txt.split_whitespace().next().unwrap_or("").to_lowercase();
    let mut f = File::open(img_path)?;
    let total = f.seek(SeekFrom::End(0))?;
    f.seek(SeekFrom::Start(0))?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; (8 * MIB) as usize];
    let mut p = Progress::new("checking the backup file", total);
    let mut done = 0u64;
    loop {
        crate::events::check_cancelled()?;
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
        done += n as u64;
        p.tick(done, "");
    }
    p.done();
    if format!("{:x}", h.finalize()) != want {
        bail!("the backup file is damaged: it does not match its checksum file ({}.sha256)", img_path.display());
    }
    crate::say!("the backup file matches its checksum");
    Ok(true)
}

#[cfg(test)]
mod tests {
    #[test]
    fn durations() {
        assert_eq!(super::fmt_duration(45.2), "45 s");
        assert_eq!(super::fmt_duration(160.0), "2 min 40 s");
        assert_eq!(super::fmt_duration(65.0), "1 min 05 s");
        assert_eq!(super::fmt_duration(3900.0), "1 h 05 min");
    }
}
