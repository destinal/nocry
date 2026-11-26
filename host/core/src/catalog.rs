//! The list of Creality firmware nocry can install, and downloading from it.
//!
//! A catalog is built into the program (data/catalog.json, made by
//! tools/catalog/build_creality_catalog.py) so the list works offline and at once.
//! `refresh` asks the API behind crealitycloud.com/downloads/firmware for the current list
//! and saves it next to the user's data; `load` prefers that copy when it is newer.
//! Downloads are verified: the 32-hex path segment of a Creality CDN URL is the file's md5.

use crate::events;
use anyhow::{bail, Context, Result};
use md5::{Digest, Md5};
use serde_json::{json, Value};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

const BUILTIN: &str = include_str!("../data/catalog.json");
const API: &str = "https://admin.crealitycloud.com";
/// (ota_board_name, product name, where the API lists it)
const MODELS: &[(&str, &str, Source)] = &[
    ("NEBULA", "Retail Nebula Pad", Source::Type(24)),
    ("F005", "Ender-3 V3 KE", Source::Series("2")),
    ("F003", "CR-10 SE", Source::Series("1")),
    ("F004", "Ender-5 Max", Source::Series("2")),
];

#[derive(Clone, Copy)]
enum Source {
    /// accessory firmware type
    Type(u32),
    /// printer series id; the model is matched by its internal name
    Series(&'static str),
}

#[derive(Debug, Clone)]
pub struct Firmware {
    pub version: String,
    pub file: String,
    pub size: u64,
    pub released: String,
    pub url: String,
    pub notes: Vec<String>,
}

#[derive(Debug, Clone)]
pub struct Model {
    /// the firmware's ota_board_name: NEBULA, F005...
    pub key: String,
    pub name: String,
    pub firmwares: Vec<Firmware>,
}

#[derive(Debug, Clone)]
pub struct Catalog {
    /// date the list was built (YYYY-MM-DD)
    pub built: String,
    /// true when this is a refreshed copy, false for the built-in list
    pub updated: bool,
    pub models: Vec<Model>,
}

fn s(v: &Value, k: &str) -> String {
    v.get(k).and_then(|x| x.as_str()).unwrap_or("").to_string()
}

pub fn parse(text: &str) -> Result<Catalog> {
    let v: Value = serde_json::from_str(text).context("catalog is not valid JSON")?;
    let mut models = vec![];
    for m in v.get("models").and_then(|m| m.as_array()).context("catalog has no models")? {
        let firmwares = m
            .get("firmwares")
            .and_then(|f| f.as_array())
            .map(|a| {
                a.iter()
                    .map(|f| Firmware {
                        version: s(f, "version"),
                        file: s(f, "file"),
                        size: f.get("size").and_then(|x| x.as_u64()).unwrap_or(0),
                        released: s(f, "released"),
                        url: s(f, "url"),
                        notes: f
                            .get("notes")
                            .and_then(|n| n.as_array())
                            .map(|n| n.iter().filter_map(|x| x.as_str().map(String::from)).collect())
                            .unwrap_or_default(),
                    })
                    .filter(|f| f.url.starts_with("https://") && !f.file.is_empty())
                    .collect()
            })
            .unwrap_or_default();
        models.push(Model { key: s(m, "key"), name: s(m, "name"), firmwares });
    }
    Ok(Catalog { built: s(&v, "built"), updated: false, models })
}

fn to_value(c: &Catalog) -> Value {
    json!({
        "built": c.built,
        "models": c.models.iter().map(|m| json!({
            "key": m.key, "name": m.name,
            "firmwares": m.firmwares.iter().map(|f| json!({
                "version": f.version, "file": f.file, "size": f.size, "released": f.released,
                "url": f.url, "notes": f.notes,
            })).collect::<Vec<_>>(),
        })).collect::<Vec<_>>(),
    })
}

/// The refreshed copy in `data_dir` if there is one at least as new as the built-in list.
pub fn load(data_dir: Option<&Path>) -> Catalog {
    let builtin = parse(BUILTIN).expect("built-in catalog");
    if let Some(d) = data_dir {
        if let Ok(text) = std::fs::read_to_string(d.join("catalog.json")) {
            if let Ok(mut c) = parse(&text) {
                if c.built >= builtin.built && !c.models.is_empty() {
                    c.updated = true;
                    return c;
                }
            }
        }
    }
    builtin
}

/// Per-user data folder (same place the GUI uses): refreshed catalog and downloads live here.
pub fn default_data_dir() -> PathBuf {
    let home = || PathBuf::from(std::env::var_os("HOME").unwrap_or_default());
    if cfg!(windows) {
        PathBuf::from(std::env::var_os("LOCALAPPDATA").unwrap_or_default()).join("nocry")
    } else if cfg!(target_os = "macos") {
        home().join("Library/Application Support/nocry")
    } else {
        std::env::var_os("XDG_DATA_HOME").map(PathBuf::from).unwrap_or_else(|| home().join(".local/share")).join("nocry")
    }
}

/// Finds a catalog entry by model key (or product name) and version ("latest" = newest).
pub fn find<'a>(c: &'a Catalog, model: &str, version: &str) -> Result<(&'a Model, &'a Firmware)> {
    let m = c
        .models
        .iter()
        .find(|m| m.key.eq_ignore_ascii_case(model) || m.name.eq_ignore_ascii_case(model))
        .with_context(|| format!("no model {model:?} in the firmware list (known: {})", c.models.iter().map(|m| m.key.as_str()).collect::<Vec<_>>().join(", ")))?;
    let f = if version.eq_ignore_ascii_case("latest") {
        m.firmwares.first()
    } else {
        m.firmwares.iter().find(|f| f.version == version.trim_start_matches(['V', 'v']))
    }
    .with_context(|| format!("no version {version} for {} (have: {})", m.name, m.firmwares.iter().map(|f| f.version.as_str()).collect::<Vec<_>>().join(", ")))?;
    Ok((m, f))
}

/// YYYY-MM-DD for a Unix time (UTC).
fn date(unix: i64) -> String {
    let z = unix.div_euclid(86400) + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let (d, m) = (doy - (153 * mp + 2) / 5 + 1, if mp < 10 { mp + 3 } else { mp - 9 });
    format!("{:04}-{:02}-{:02}", yoe + era * 400 + i64::from(m <= 2), m, d)
}

fn post(path: &str, body: Value) -> Result<Value> {
    let rid = format!("{:032x}", std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_nanos());
    let resp = ureq::post(&format!("{API}{path}"))
        .timeout(std::time::Duration::from_secs(30))
        .set("Content-Type", "application/json")
        .set("User-Agent", "Mozilla/5.0 nocry")
        // the search endpoints answer "Invalid Parameter" without the site's own headers
        .set("__CXY_OS_VER_", "not set")
        .set("__CXY_PLATFORM_", "2")
        .set("__CXY_BRAND_", "creality")
        .set("__CXY_APP_CH_", "not set")
        .set("__CXY_DUID_", "not set")
        .set("__CXY_APP_ID_", "cxy-gen2")
        .set("__CXY_APP_VER_", "7.3.28")
        .set("__CXY_OS_LANG_", "0")
        .set("__CXY_TIMEZONE_", "0")
        .set("__CXY_REQUESTID_", &rid)
        .send_string(&body.to_string())
        .context("could not reach Creality's server (no internet connection?)")?;
    let v: Value = serde_json::from_reader(resp.into_reader()).context("unexpected answer from Creality's server")?;
    if v.get("code").and_then(|c| c.as_i64()) != Some(0) {
        bail!("Creality's server refused the request: {}", s(&v, "msg"));
    }
    Ok(v.get("result").cloned().unwrap_or(Value::Null))
}

fn search(mut params: Value) -> Result<Vec<Value>> {
    let mut out = vec![];
    for page in 1..=20 {
        params["page"] = json!(page);
        params["pageSize"] = json!(100);
        let r = post("/api/cxy/search/firmwareSearch", params.clone())?;
        let list = r.get("list").and_then(|l| l.as_array()).cloned().unwrap_or_default();
        let n = list.len();
        out.extend(list);
        if n == 0 || out.len() as u64 >= r.get("count").and_then(|c| c.as_u64()).unwrap_or(0) {
            break;
        }
    }
    Ok(out)
}

/// Ask Creality for the current list and save it in `data_dir`.
pub fn refresh(data_dir: &Path) -> Result<Catalog> {
    crate::say!("asking Creality for the current firmware list");
    let mut series: std::collections::BTreeMap<&str, Vec<Value>> = Default::default();
    let mut models = vec![];
    for (key, name, src) in MODELS {
        let recs: Vec<Value> = match src {
            Source::Type(t) => search(json!({ "type": t }))?,
            Source::Series(id) => {
                if !series.contains_key(id) {
                    series.insert(id, search(json!({ "type": 2, "seriesId": id }))?);
                }
                series[id]
                    .iter()
                    .filter(|r| {
                        r.get("subType").and_then(|x| x.as_array()).is_some_and(|a| a.iter().any(|x| s(x, "internalName") == *key))
                    })
                    .cloned()
                    .collect()
            }
        };
        let mut fws: Vec<(i64, Firmware)> = vec![];
        for r in &recs {
            let (file, url) = (s(r, "name"), s(r, "downloadUrl"));
            if !file.ends_with(".img") || !file.contains("_ota_") || !url.starts_with("https://") {
                continue;
            }
            if fws.iter().any(|(_, f)| f.url == url) {
                continue; // one record per hardware version
            }
            let t = r.get("createTime").and_then(|x| x.as_i64()).unwrap_or(0);
            fws.push((
                t,
                Firmware {
                    version: s(r, "versionNumber").trim_start_matches(['V', 'v']).to_string(),
                    file,
                    size: r.get("firmwareSize").and_then(|x| x.as_u64()).unwrap_or(0),
                    released: date(t),
                    url,
                    notes: r
                        .get("updateDescription")
                        .and_then(|n| n.as_array())
                        .map(|n| n.iter().filter_map(|x| x.as_str()).map(|x| x.trim().to_string()).filter(|x| !x.is_empty()).collect())
                        .unwrap_or_default(),
                },
            ));
        }
        fws.sort_by_key(|f| std::cmp::Reverse(f.0));
        models.push(Model { key: key.to_string(), name: name.to_string(), firmwares: fws.into_iter().map(|f| f.1).collect() });
    }
    if models.iter().all(|m| m.firmwares.is_empty()) {
        bail!("Creality's server returned no firmware; keeping the current list");
    }
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_secs() as i64;
    let cat = Catalog { built: date(now), updated: true, models };
    std::fs::create_dir_all(data_dir)?;
    std::fs::write(data_dir.join("catalog.json"), serde_json::to_string_pretty(&to_value(&cat))?)?;
    let n: usize = cat.models.iter().map(|m| m.firmwares.len()).sum();
    crate::say!("firmware list updated: {n} versions for {} models", cat.models.len());
    Ok(cat)
}

/// The bootloader and partition table for a blank board of `family`. Update files do not contain
/// them, so they are taken from one of Creality's own factory packages for that family
/// (`boards::BootSource`), downloaded once and checked against a known sha256.
pub fn boot_area(data_dir: &Path, family: crate::boards::FamilyId) -> Result<Vec<u8>> {
    use sha2::Digest as _;
    let src = &crate::boards::family(family).boot;
    let sha = |b: &[u8]| format!("{:x}", sha2::Sha256::digest(b));
    let cache = data_dir.join("boot").join(src.cache_name);
    if let Ok(b) = std::fs::read(&cache) {
        if sha(&b) == src.boot_sha256 {
            return Ok(b);
        }
    }
    crate::say!("this board needs a bootloader and partition table; fetching them from Creality's factory package");
    let pkg = download(src.url, src.file, src.size, &data_dir.join("firmware"))?;
    let b = crate::restore::load_boot(&pkg)?;
    if sha(&b) != src.boot_sha256 {
        let _ = std::fs::remove_file(&pkg);
        bail!("the downloaded factory package does not contain the expected bootloader (checksum mismatch); nothing was installed");
    }
    std::fs::create_dir_all(cache.parent().unwrap())?;
    std::fs::write(&cache, &b)?;
    Ok(b)
}

/// md5 a Creality CDN URL promises (its 32-hex path segment), if any.
pub fn url_md5(url: &str) -> Option<String> {
    url.split('/').find(|p| p.len() == 32 && p.bytes().all(|c| c.is_ascii_hexdigit())).map(|p| p.to_lowercase())
}

/// Where a download is kept: `<cache>/<md5>_<file>` (or `<file>` without a known md5).
pub fn cached_path(cache_dir: &Path, url: &str, file: &str) -> PathBuf {
    let safe: String = file.chars().map(|c| if c.is_ascii_alphanumeric() || "._-".contains(c) { c } else { '_' }).collect();
    match url_md5(url) {
        Some(m) => cache_dir.join(format!("{m}_{safe}")),
        None => cache_dir.join(safe),
    }
}

fn file_md5(p: &Path) -> Result<String> {
    let mut f = std::fs::File::open(p)?;
    let mut h = Md5::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(format!("{:x}", h.finalize()))
}

/// Download `url` into the cache (or reuse a verified copy). Returns the local path.
pub fn download(url: &str, file: &str, size: u64, cache_dir: &Path) -> Result<PathBuf> {
    if !url.starts_with("https://") {
        bail!("refusing to download from a non-https address");
    }
    let dest = cached_path(cache_dir, url, file);
    let want = url_md5(url);
    if dest.exists() {
        match &want {
            Some(w) if &file_md5(&dest)? == w => {
                crate::say!("already downloaded: {}", dest.display());
                return Ok(dest);
            }
            None => return Ok(dest),
            _ => {
                let _ = std::fs::remove_file(&dest); // damaged or incomplete copy
            }
        }
    }
    std::fs::create_dir_all(cache_dir)?;
    crate::say!("downloading {file}");
    let resp = ureq::get(url)
        .set("User-Agent", "Mozilla/5.0 nocry")
        .timeout(std::time::Duration::from_secs(3600))
        .call()
        .with_context(|| format!("could not download {file} (no internet connection?)"))?;
    let total = resp.header("Content-Length").and_then(|l| l.parse().ok()).unwrap_or(size);
    let part = dest.with_extension("part");
    let r = (|| -> Result<String> {
        let mut out = std::fs::File::create(&part)?;
        let mut rd = resp.into_reader();
        let mut h = Md5::new();
        let mut buf = vec![0u8; 256 * 1024];
        let (mut done, t0, mut last) = (0u64, std::time::Instant::now(), std::time::Instant::now());
        loop {
            events::check_cancelled()?;
            let n = rd.read(&mut buf).context("the download was interrupted")?;
            if n == 0 {
                break;
            }
            out.write_all(&buf[..n])?;
            h.update(&buf[..n]);
            done += n as u64;
            if last.elapsed().as_millis() >= 500 {
                last = std::time::Instant::now();
                let rate = done as f64 / t0.elapsed().as_secs_f64().max(0.001);
                let left = if rate > 0.0 && total > done { (total - done) as f64 / rate } else { 0.0 };
                events::progress("downloading", done, total.max(done), &format!("  {:5.1} MB/s  {} left", rate / 1e6, crate::diskimage::fmt_duration(left)));
            }
        }
        out.sync_all()?;
        events::progress_end();
        if size > 0 && done != size {
            bail!("the download is incomplete ({done} of {size} bytes)");
        }
        Ok(format!("{:x}", h.finalize()))
    })();
    let got = match r {
        Ok(g) => g,
        Err(e) => {
            events::progress_end();
            let _ = std::fs::remove_file(&part);
            return Err(e);
        }
    };
    if let Some(w) = &want {
        if &got != w {
            let _ = std::fs::remove_file(&part);
            bail!("the downloaded file is damaged (checksum {got}, expected {w}); nothing was installed");
        }
    }
    std::fs::rename(&part, &dest)?;
    crate::say!("downloaded and verified: {}", dest.display());
    Ok(dest)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn builtin_catalog_is_usable() {
        let c = parse(BUILTIN).unwrap();
        assert!(!c.updated && c.built.len() == 10);
        let neb = c.models.iter().find(|m| m.key == "NEBULA").unwrap();
        assert!(neb.firmwares.len() >= 6);
        for m in &c.models {
            for f in &m.firmwares {
                assert!(f.url.starts_with("https://") && url_md5(&f.url).is_some(), "{}", f.url);
                assert!(f.size > 50 << 20 && !f.version.is_empty());
            }
        }
    }

    #[test]
    fn helpers() {
        assert_eq!(date(0), "1970-01-01");
        assert_eq!(date(1768382929), "2026-01-14");
        assert_eq!(url_md5("https://x/file/A92BA54773af1a1da4416759f324ada8/n.img").as_deref(), Some("a92ba54773af1a1da4416759f324ada8"));
        assert!(url_md5("https://x/file/short/n.img").is_none());
        let p = cached_path(Path::new("/c"), "https://x/file/a92ba54773af1a1da4416759f324ada8/n.img", "a b/../n.img");
        assert_eq!(p, Path::new("/c/a92ba54773af1a1da4416759f324ada8_a_b_.._n.img"));
    }

    #[test]
    fn newer_cache_wins_older_is_ignored() {
        let d = std::env::temp_dir().join(format!("nocry-cat-{}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        let mut c = parse(BUILTIN).unwrap();
        c.built = "2099-01-01".into();
        c.models.truncate(1);
        std::fs::write(d.join("catalog.json"), to_value(&c).to_string()).unwrap();
        let l = load(Some(&d));
        assert!(l.updated && l.models.len() == 1);
        c.built = "2000-01-01".into();
        std::fs::write(d.join("catalog.json"), to_value(&c).to_string()).unwrap();
        assert!(!load(Some(&d)).updated);
        std::fs::write(d.join("catalog.json"), "garbage").unwrap();
        assert!(!load(Some(&d)).updated);
        let _ = std::fs::remove_dir_all(&d);
    }
}
