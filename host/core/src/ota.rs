//! Creality OTA update files for Ingenic boards (K1 family, Nebula family, Ender-3 V3...).
//!
//! The file is a solid, header-encrypted 7z archive (LZMA2 + AES-256). Its password is
//! `md5_crypt(SEED + "C3_7e_bz", salt "cxswfile")`, where SEED is the product code in the file
//! name ("F005") or the board id ("CR4CU220812S11"). Inside, `ota_update.in` lists the images
//! (kernel `xImage`, `rootfs.squashfs`, rtos `zero.bin`), each stored as 1 MiB fragments
//! named `<name>.<NNNN>.<md5>` and verified against a whole-image md5.
//! (Format worked out in the user's extract_creality_ota.py; Allwinner-based printers use a
//! different, unencrypted container that this module does not handle.)

use anyhow::{anyhow, bail, Context, Result};
use md5::{Digest, Md5};
use std::collections::BTreeMap;
use std::path::Path;

pub const SEVENZ_MAGIC: &[u8] = b"7z\xbc\xaf\x27\x1c";
const SUFFIX: &str = "C3_7e_bz";
const SALT: &[u8] = b"cxswfile";
/// Seeds tried when the file name gives none that works.
const KNOWN_SEEDS: &[&str] = &["NEBULA", "F001", "F002", "F003", "F004", "F005", "CR4CU220812S10", "CR4CU220812S11", "CR4CU220812S12"];

pub fn is_ota(path: &Path) -> bool {
    let mut b = [0u8; 6];
    std::fs::File::open(path).and_then(|mut f| std::io::Read::read_exact(&mut f, &mut b)).is_ok() && b == SEVENZ_MAGIC
}

/// md5-crypt ("$1$", Poul-Henning Kamp), as `openssl passwd -1`.
pub fn md5_crypt(password: &[u8], salt: &[u8]) -> String {
    const ITOA64: &[u8] = b"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    let salt = &salt[..salt.len().min(8)];
    let alt = Md5::new().chain_update(password).chain_update(salt).chain_update(password).finalize();
    let mut ctx = Md5::new().chain_update(password).chain_update(b"$1$").chain_update(salt);
    let mut i = password.len();
    while i > 0 {
        ctx.update(&alt[..i.min(16)]);
        i = i.saturating_sub(16);
    }
    let mut i = password.len();
    while i > 0 {
        if i & 1 == 1 {
            ctx.update([0u8]);
        } else {
            ctx.update(&password[..1]);
        }
        i >>= 1;
    }
    let mut fin = ctx.finalize();
    for r in 0..1000 {
        let mut c = Md5::new();
        if r & 1 == 1 {
            c.update(password);
        } else {
            c.update(fin);
        }
        if r % 3 != 0 {
            c.update(salt);
        }
        if r % 7 != 0 {
            c.update(password);
        }
        if r & 1 == 1 {
            c.update(fin);
        } else {
            c.update(password);
        }
        fin = c.finalize();
    }
    let mut out = format!("$1${}$", String::from_utf8_lossy(salt));
    let mut b64 = |mut v: u32, n: usize| {
        for _ in 0..n {
            out.push(ITOA64[(v & 0x3f) as usize] as char);
            v >>= 6;
        }
    };
    let f = |a: usize, b: usize, c: usize| (fin[a] as u32) << 16 | (fin[b] as u32) << 8 | fin[c] as u32;
    b64(f(0, 6, 12), 4);
    b64(f(1, 7, 13), 4);
    b64(f(2, 8, 14), 4);
    b64(f(3, 9, 15), 4);
    b64(f(4, 10, 5), 4);
    b64(fin[11] as u32, 2);
    out
}

pub fn password_for(seed: &str) -> String {
    md5_crypt(format!("{seed}{SUFFIX}").as_bytes(), SALT)
}

/// Password seeds to try, most likely first: `Fnnn` in the file name, a leading `CR...`
/// board id, the parent directory name, then the known list.
pub fn seed_candidates(path: &Path) -> Vec<String> {
    let name = path.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default();
    let mut v: Vec<String> = vec![];
    let b = name.as_bytes();
    for i in 0..b.len() {
        if b[i] == b'F' && (i == 0 || !b[i - 1].is_ascii_alphanumeric()) {
            let d = b[i + 1..].iter().take_while(|c| c.is_ascii_digit()).count();
            if (3..=4).contains(&d) {
                v.push(name[i..i + 1 + d].to_string());
            }
        }
    }
    if let Some(i) = name.find("CR") {
        let id: String = name[i..].chars().take_while(|c| c.is_ascii_uppercase() || c.is_ascii_digit()).collect();
        if id.len() >= 8 {
            v.push(id);
        }
    }
    // "<SEED>_ota_img_V..." (e.g. NEBULA_ota_img_V1.1.0.30.img), also behind a "<md5>_" cache prefix
    if let Some(i) = name.find("_ota") {
        let head = &name[..i];
        let seed = head.rsplit('_').next().unwrap_or(head);
        if seed.len() >= 4 && seed.bytes().all(|c| c.is_ascii_uppercase() || c.is_ascii_digit()) {
            v.push(seed.to_string());
        }
    }
    if let Some(d) = path.parent().and_then(|p| p.file_name()) {
        v.push(d.to_string_lossy().to_string());
    }
    v.extend(KNOWN_SEEDS.iter().map(|s| s.to_string()));
    let mut seen = std::collections::HashSet::new();
    v.retain(|s| !s.is_empty() && seen.insert(s.clone()));
    v
}

/// The reassembled, md5-verified images of an OTA.
#[derive(Debug, Default)]
pub struct Ota {
    pub version: String,
    /// the seed whose password opened the archive
    pub seed: String,
    pub kernel: Option<Vec<u8>>,
    pub rootfs: Option<Vec<u8>>,
    pub rtos: Option<Vec<u8>>,
}

fn open(path: &Path, seed: &str) -> Result<BTreeMap<String, Vec<u8>>> {
    let pw = password_for(seed);
    let mut rd = sevenz_rust2::ArchiveReader::open(path, sevenz_rust2::Password::from(pw.as_str()))?;
    let mut files = BTreeMap::new();
    rd.for_each_entries(|e, r| {
        if !e.is_directory() {
            let mut v = Vec::with_capacity(e.size() as usize);
            std::io::Read::read_to_end(r, &mut v)?;
            // keep the base name only: fragments and the manifest share one directory
            let base = e.name().rsplit('/').next().unwrap_or("").to_string();
            files.insert(base, v);
        }
        Ok(true)
    })?;
    Ok(files)
}

pub fn load(path: &Path) -> Result<Ota> {
    let mut last = anyhow!("no password candidates");
    let mut opened = None;
    for seed in seed_candidates(path) {
        match open(path, &seed) {
            Ok(f) => {
                opened = Some((seed, f));
                break;
            }
            Err(e) => last = e,
        }
    }
    let (seed, files) = opened.with_context(|| {
        format!(
            "{}: could not open this update file with any known password (last error: {last}). \
             It may be for a printer nocry does not know, or the file is damaged",
            path.display()
        )
    })?;
    let manifest = files.get("ota_update.in").context("the update file has no ota_update.in manifest")?;
    let text = String::from_utf8_lossy(manifest).replace("\r\n", "\n");
    let mut ota = Ota { seed, ..Default::default() };
    for stanza in text.split("\n\n") {
        let kv: BTreeMap<&str, &str> =
            stanza.lines().filter_map(|l| l.split_once('=')).map(|(k, v)| (k.trim(), v.trim())).collect();
        if let Some(v) = kv.get("ota_version") {
            ota.version = v.to_string();
        }
        let (Some(ty), Some(name)) = (kv.get("img_type"), kv.get("img_name")) else { continue };
        let size: usize = kv.get("img_size").and_then(|s| s.parse().ok()).with_context(|| format!("{name}: bad img_size"))?;
        let want = kv.get("img_md5").copied().unwrap_or("").to_lowercase();
        // fragments: <name>.<NNNN>.<32 hex>
        let prefix = format!("{name}.");
        let mut frags: Vec<(u32, &Vec<u8>)> = files
            .iter()
            .filter_map(|(k, v)| {
                let rest = k.strip_prefix(&prefix)?;
                let (idx, md5) = rest.split_once('.')?;
                (md5.len() == 32 && md5.bytes().all(|c| c.is_ascii_hexdigit())).then_some(())?;
                Some((idx.parse().ok()?, v))
            })
            .collect();
        frags.sort_by_key(|f| f.0);
        if frags.is_empty() {
            bail!("the update file lists {name} but contains no pieces of it");
        }
        let mut img = Vec::with_capacity(size);
        for (_, f) in frags {
            img.extend_from_slice(f);
        }
        let got = format!("{:x}", Md5::digest(&img));
        if img.len() != size || got != want {
            bail!("{name} in the update file is damaged (size {} of {size}, md5 {got}, expected {want})", img.len());
        }
        match *ty {
            "kernel" => ota.kernel = Some(img),
            "rootfs" => ota.rootfs = Some(img),
            "rtos" => ota.rtos = Some(img),
            _ => {}
        }
    }
    Ok(ota)
}

// ---------------------------------------------------------------------------
// The newer, unencrypted container ("..._ota_X2000E_V...img", K1-family S12 firmware from late 2025)
//
//   0x000  file name (0x80)      0x080  description (0x40)     0x0c0  build time, text (0x40)
//   0x100  version, text (0x20)  0x120  u32 time               0x128  u64 file size
//   0x130  u32 number of entries, u32 1
//   0x138  entries, 0xe0 bytes each: u32 id, name (0x80), target device "mmcblk0pN" (0x40),
//          u64 size, u64 offset in the file, 8 zero bytes, u32 crc32 of the data
// Each image is listed twice, once per slot (p3/p4 rtos, p5/p6 kernel, p7/p8 rootfs).

const PLAIN_ENTRIES: usize = 0x138;
const PLAIN_ENTRY: usize = 0xe0;

fn plain_header(h: &[u8], file_len: u64) -> Option<usize> {
    if h.len() < PLAIN_ENTRIES {
        return None;
    }
    let name_ok = h[..0x80].iter().take_while(|&&c| c != 0).all(|&c| (0x20..0x7f).contains(&c)) && h[0] != 0;
    let total = u64::from_le_bytes(h[0x128..0x130].try_into().unwrap());
    let n = u32::from_le_bytes(h[0x130..0x134].try_into().unwrap()) as usize;
    (name_ok && total == file_len && (1..=64).contains(&n)).then_some(n)
}

pub fn is_plain_ota(path: &Path) -> bool {
    let Ok(md) = std::fs::metadata(path) else { return false };
    let mut h = [0u8; PLAIN_ENTRIES];
    std::fs::File::open(path).and_then(|mut f| std::io::Read::read_exact(&mut f, &mut h)).is_ok() && plain_header(&h, md.len()).is_some()
}

pub fn load_plain(path: &Path) -> Result<Ota> {
    let d = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
    let n = plain_header(&d, d.len() as u64).context("not an update file of the unencrypted kind")?;
    let text = |b: &[u8]| String::from_utf8_lossy(&b[..b.iter().position(|&c| c == 0).unwrap_or(b.len())]).to_string();
    let mut ota = Ota { version: text(&d[0x100..0x120]).trim_start_matches(['V', 'v']).to_string(), seed: String::new(), ..Default::default() };
    for i in 0..n {
        let e = PLAIN_ENTRIES + i * PLAIN_ENTRY;
        if e + PLAIN_ENTRY > d.len() {
            bail!("the update file's table of contents is cut short");
        }
        let (name, dev) = (text(&d[e + 4..e + 0x84]), text(&d[e + 0x84..e + 0xc4]));
        let size = u64::from_le_bytes(d[e + 0xc4..e + 0xcc].try_into().unwrap()) as usize;
        let off = u64::from_le_bytes(d[e + 0xcc..e + 0xd4].try_into().unwrap()) as usize;
        let crc = u32::from_le_bytes(d[e + 0xdc..e + 0xe0].try_into().unwrap());
        let slot = match dev.as_str() {
            "mmcblk0p3" => &mut ota.rtos,
            "mmcblk0p5" => &mut ota.kernel,
            "mmcblk0p7" => &mut ota.rootfs,
            _ => continue, // the second slot's copies, or something this board layout has no place for
        };
        let img = d.get(off..off.saturating_add(size)).with_context(|| format!("{name}: lies outside the update file"))?;
        if crc32fast::hash(img) != crc {
            bail!("{name} in the update file is damaged (checksum mismatch)");
        }
        *slot = Some(img.to_vec());
    }
    Ok(ota)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn md5_crypt_matches_openssl() {
        // openssl passwd -1 -salt cxswfile F005C3_7e_bz / password
        assert_eq!(md5_crypt(b"password", b"saltsalt"), "$1$saltsalt$qjXMvbEw8oaL.CzflDtaK/");
        assert_eq!(md5_crypt(b"", b"cxswfile").len(), 34);
    }

    #[test]
    fn seeds_from_names() {
        let s = seed_candidates(Path::new("/x/HW/Ender-3_V3_KE_F005_ota_img_V1.1.0.17.img"));
        assert_eq!(s[0], "F005");
        let s = seed_candidates(Path::new("/x/CR4CU220812S12K1/CR4CU220812S11_ota_img_V1.3.3.46.img"));
        assert_eq!(s[0], "CR4CU220812S11");
        assert!(s.contains(&"F001".to_string()));
        let s = seed_candidates(Path::new("/dl/NEBULA_ota_img_V1.1.0.30.img"));
        assert_eq!(s[0], "NEBULA");
        let s = seed_candidates(Path::new("update.img"));
        assert!(s.contains(&"F005".to_string()));
    }
}
