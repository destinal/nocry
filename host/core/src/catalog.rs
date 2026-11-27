//! The list of Creality firmware nocry can install, and downloading from it.
//!
//! A catalog is built into the program (data/catalog.json, made by
//! tools/catalog/build_creality_catalog.py) so the list works offline and at once.
//! `refresh` asks the API behind crealitycloud.com/downloads/firmware for the current list
//! and saves it next to the user's data; `load` prefers that copy when it is newer.
//! Downloads are verified: the 32-hex path segment of a Creality CDN URL is the file's md5.
//!
//! NebulaOS (github.com/NebulaOS-KE/NebulaOS), an independent open firmware for the Nebula Pad
//! family, is listed too: from its GitHub releases, each file verified against the sha256 the
//! release publishes for it. Its three .img files per release hold the same system under
//! different wrapper names (for its own updater, Creality's updater on a KE, and on a retail
//! pad); nocry lists the plain one.

use crate::events;
use anyhow::{bail, Context, Result};
use md5::{Digest, Md5};
use serde_json::{json, Value};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

const BUILTIN: &str = include_str!("../data/catalog.json");
const API: &str = "https://admin.crealitycloud.com";
const NEBULAOS_RELEASES: &str = "https://api.github.com/repos/NebulaOS-KE/NebulaOS/releases?per_page=30";
const NEBULAOS_DOWNLOADS: &str = "https://github.com/NebulaOS-KE/NebulaOS/releases/download/";
const NEBULAOS_NAME: &str = "NebulaOS (Ender-3 V3 KE, Nebula Pad)";
/// A list of firmware in the catalog: one per firmware line (`boards::LINES`).
struct Listing {
    /// the firmware's ota_board_name
    key: &'static str,
    name: &'static str,
    source: Source,
    /// printers (internal names) whose records carry this line's files
    printers: &'static [&'static str],
    /// file names of this line start with this ("" = any)
    file_prefix: &'static str,
}

const MODELS: &[Listing] = &[
    Listing { key: "NEBULA", name: "Retail Nebula Pad", source: Source::Type(24), printers: &[], file_prefix: "" },
    Listing { key: "F005", name: "Ender-3 V3 KE", source: Source::Series("2"), printers: &["F005"], file_prefix: "" },
    Listing { key: "F003", name: "CR-10 SE", source: Source::Series("1"), printers: &["F003"], file_prefix: "" },
    Listing { key: "F004", name: "Ender-5 Max", source: Source::Series("2"), printers: &["F004"], file_prefix: "" },
    // one firmware for the whole K1 family, listed under each of its printers
    Listing {
        key: "CR4CU220812S11",
        name: "K1 series (K1, K1 Max, K1C, K1 SE)",
        source: Source::Series("9"),
        printers: &["CR-K1", "CR-K1 Max", "K1C", "K1 SE"],
        file_prefix: "CR4CU220812S11_",
    },
    Listing {
        key: "F001",
        name: "Ender-3 V3 series (V3, V3 Plus)",
        source: Source::Series("2"),
        printers: &["F001", "F002"],
        file_prefix: "F001_",
    },
];

#[derive(Clone, Copy)]
enum Source {
    /// accessory firmware type
    Type(u32),
    /// printer series id; the printers are matched by their internal names
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
    /// the file's sha256 where the publisher gives one (NebulaOS); "" otherwise
    pub sha256: String,
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
                        sha256: s(f, "sha256").to_lowercase(),
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
                "url": f.url, "notes": f.notes, "sha256": f.sha256,
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

fn is_sha256(h: &str) -> bool {
    h.len() == 64 && h.bytes().all(|c| c.is_ascii_hexdigit())
}

/// The NebulaOS versions in the answer of GitHub's releases API, newest first. A release
/// counts when it is final, has the plain image `NebulaOS-Ender3V3KE-<version>.img` with a
/// sha256, and is also published for the retail Nebula Pad (from 1.0.4, the first version
/// that sets up an erased data partition by itself, which a clean install needs).
pub fn nebulaos_from_releases(v: &Value) -> Vec<Firmware> {
    let mut out: Vec<Firmware> = vec![];
    for r in v.as_array().map(|a| a.as_slice()).unwrap_or_default() {
        let flag = |k: &str| r.get(k).and_then(|x| x.as_bool()).unwrap_or(true);
        let version = s(r, "tag_name").trim_start_matches(['V', 'v']).to_string();
        if flag("draft") || flag("prerelease") || version.is_empty() || !version.bytes().all(|c| c.is_ascii_digit() || c == b'.') {
            continue;
        }
        let assets = r.get("assets").and_then(|a| a.as_array()).cloned().unwrap_or_default();
        let want = format!("NebulaOS-Ender3V3KE-{version}.img");
        let Some(a) = assets.iter().find(|a| s(a, "name") == want) else { continue };
        if !assets.iter().any(|a| s(a, "name").starts_with("NebulaOS-NebulaPad-") && s(a, "name").ends_with(".img")) {
            continue;
        }
        let (url, sha256) = (s(a, "browser_download_url"), s(a, "digest").trim_start_matches("sha256:").to_lowercase());
        let size = a.get("size").and_then(|x| x.as_u64()).unwrap_or(0);
        if !url.starts_with(NEBULAOS_DOWNLOADS) || !is_sha256(&sha256) || size == 0 {
            continue;
        }
        // the first paragraph of the release text says what the version is about
        let body = s(r, "body").replace('\r', "");
        let about = body.split("\n\n").map(|p| p.split_whitespace().collect::<Vec<_>>().join(" ")).find(|p| !p.is_empty() && !p.starts_with('#') && !p.starts_with('|'));
        let mut notes: Vec<String> = about.into_iter().collect();
        let page = s(r, "html_url");
        if page.starts_with("https://github.com/NebulaOS-KE/NebulaOS/") {
            notes.push(format!("Release notes: {page}"));
        }
        out.push(Firmware { version, file: want, size, released: s(r, "published_at").chars().take(10).collect(), url, notes, sha256 });
    }
    out.sort_by(|a, b| b.released.cmp(&a.released));
    out
}

fn nebulaos_firmwares() -> Result<Vec<Firmware>> {
    let resp = ureq::get(NEBULAOS_RELEASES)
        .timeout(std::time::Duration::from_secs(30))
        .set("Accept", "application/vnd.github+json")
        .set("User-Agent", "nocry")
        .call()
        .context("could not reach GitHub for the NebulaOS list")?;
    let v: Value = serde_json::from_reader(resp.into_reader()).context("unexpected answer from GitHub")?;
    Ok(nebulaos_from_releases(&v))
}

/// The sha256 the firmware list promises for `url`, looked up in the list kept in `data_dir`
/// (or the built-in one).
pub fn listed_sha256(data_dir: Option<&Path>, url: &str) -> Option<String> {
    load(data_dir).models.iter().flat_map(|m| &m.firmwares).find(|f| f.url == url && is_sha256(&f.sha256)).map(|f| f.sha256.clone())
}

/// Ask Creality (and GitHub, for NebulaOS) for the current list and save it in `data_dir`.
pub fn refresh(data_dir: &Path) -> Result<Catalog> {
    crate::say!("asking Creality for the current firmware list");
    let mut series: std::collections::BTreeMap<&str, Vec<Value>> = Default::default();
    let mut models = vec![];
    for Listing { key, name, source, printers, file_prefix } in MODELS {
        let recs: Vec<Value> = match source {
            Source::Type(t) => search(json!({ "type": t }))?,
            Source::Series(id) => {
                if !series.contains_key(id) {
                    series.insert(id, search(json!({ "type": 2, "seriesId": id }))?);
                }
                series[id]
                    .iter()
                    .filter(|r| {
                        r.get("subType").and_then(|x| x.as_array()).is_some_and(|a| a.iter().any(|x| printers.contains(&s(x, "internalName").as_str())))
                    })
                    .cloned()
                    .collect()
            }
        };
        let mut fws: Vec<(i64, Firmware)> = vec![];
        for r in &recs {
            let (file, url) = (s(r, "name"), s(r, "downloadUrl"));
            if !file.ends_with(".img") || !file.contains("_ota_") || !url.starts_with("https://") || !file.starts_with(file_prefix) {
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
                    sha256: String::new(),
                },
            ));
        }
        fws.sort_by_key(|f| std::cmp::Reverse(f.0));
        models.push(Model { key: key.to_string(), name: name.to_string(), firmwares: fws.into_iter().map(|f| f.1).collect() });
    }
    if models.iter().all(|m| m.firmwares.is_empty()) {
        bail!("Creality's server returned no firmware; keeping the current list");
    }
    // NebulaOS: its own source. If that cannot be reached, what the current list has is kept.
    crate::say!("asking GitHub for the current NebulaOS versions");
    let nebulaos = match nebulaos_firmwares() {
        Ok(f) if !f.is_empty() => f,
        r => {
            crate::say!("NebulaOS list not updated ({}); keeping the versions already known", r.err().map(|e| format!("{e:#}")).unwrap_or_else(|| "no usable release found".into()));
            load(Some(data_dir)).models.into_iter().find(|m| m.key == crate::boards::NEBULAOS).map(|m| m.firmwares).unwrap_or_default()
        }
    };
    if !nebulaos.is_empty() {
        models.push(Model { key: crate::boards::NEBULAOS.into(), name: NEBULAOS_NAME.into(), firmwares: nebulaos });
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

fn file_sha256(p: &Path) -> Result<String> {
    use sha2::Digest as _;
    let mut f = std::fs::File::open(p)?;
    let mut h = sha2::Sha256::new();
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
    // what the file has to be: Creality's md5 in the address, or the sha256 the list gives
    let want_sha = listed_sha256(cache_dir.parent(), url);
    if url.starts_with(NEBULAOS_DOWNLOADS) && want_sha.is_none() {
        bail!("the firmware list has no checksum for {file}; update the list and try again. Nothing was downloaded");
    }
    let want = url_md5(url);
    if dest.exists() {
        if let Some(w) = &want_sha {
            if &file_sha256(&dest)? == w {
                crate::say!("already downloaded: {}", dest.display());
                return Ok(dest);
            }
            let _ = std::fs::remove_file(&dest); // damaged or incomplete copy
        }
    }
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
    let r = (|| -> Result<(String, String)> {
        use sha2::Digest as _;
        let mut out = std::fs::File::create(&part)?;
        let mut rd = resp.into_reader();
        let mut h = Md5::new();
        let mut h256 = sha2::Sha256::new();
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
            h256.update(&buf[..n]);
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
        Ok((format!("{:x}", h.finalize()), format!("{:x}", h256.finalize())))
    })();
    let (got, got_sha) = match r {
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
    if let Some(w) = &want_sha {
        if &got_sha != w {
            let _ = std::fs::remove_file(&part);
            bail!("the downloaded file is damaged (sha256 {got_sha}, expected {w}); nothing was installed");
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
                assert!(f.url.starts_with("https://") && (url_md5(&f.url).is_some() || is_sha256(&f.sha256)), "{}", f.url);
                assert!(f.size > 50 << 20 && !f.version.is_empty());
            }
        }
    }

    #[test]
    fn nebulaos_releases() {
        let asset = |name: &str, digest: &str| json!({"name": name, "size": 148815426u64, "digest": digest,
            "browser_download_url": format!("https://github.com/NebulaOS-KE/NebulaOS/releases/download/v1.0.5/{name}")});
        let sha = "sha256:1314cc5494a64b571e48ce78692b8b8843ee5deea2c905474ae695811f3d918f";
        let rel = |tag: &str, pre: bool, assets: Vec<Value>| json!({"tag_name": tag, "draft": false, "prerelease": pre,
            "published_at": "2026-11-26T13:27:03Z", "html_url": "https://github.com/NebulaOS-KE/NebulaOS/releases/tag/v1.0.5",
            "body": "NebulaOS 1.0.5 for the Creality\r\nEnder-3 V3 KE.\r\n\r\n## At a glance\r\n", "assets": assets});
        let both = |v: &str| vec![asset(&format!("NebulaOS-Ender3V3KE-{v}.img"), sha), asset(&format!("NebulaOS-NebulaPad-NEBULA_ota_img_V9.{v}.img"), sha),
            asset(&format!("NebulaOS-Ender3V3KE-{v}.ingenic"), sha)];
        let v = json!([
            rel("v1.0.5", false, both("1.0.5")),
            rel("v1.0.6-rc1", false, both("1.0.6-rc1")),                                   // not a plain version
            rel("v1.0.6", true, both("1.0.6")),                                            // prerelease
            rel("v1.0.3", false, vec![asset("NebulaOS-Ender3V3KE-1.0.3.img", sha)]),       // not published for the pad
            rel("v1.0.4", false, vec![asset("NebulaOS-Ender3V3KE-1.0.4.img", "md5:abc"), asset("NebulaOS-NebulaPad-x.img", sha)]), // no sha256
        ]);
        let f = nebulaos_from_releases(&v);
        assert_eq!(f.len(), 1);
        assert_eq!((f[0].version.as_str(), f[0].file.as_str(), f[0].released.as_str()), ("1.0.5", "NebulaOS-Ender3V3KE-1.0.5.img", "2026-11-26"));
        assert_eq!(f[0].sha256, &sha[7..]);
        assert_eq!(f[0].notes[0], "NebulaOS 1.0.5 for the Creality Ender-3 V3 KE.");
        assert!(f[0].notes.last().unwrap().starts_with("Release notes: https://github.com/NebulaOS-KE/"));
        // the list keeps the checksum through saving and loading
        let c = Catalog { built: "2026-11-27".into(), updated: false, models: vec![Model { key: "NEBULAOS".into(), name: "x".into(), firmwares: f.clone() }] };
        assert_eq!(parse(&to_value(&c).to_string()).unwrap().models[0].firmwares[0].sha256, f[0].sha256);
        assert!(nebulaos_from_releases(&json!({"message": "rate limited"})).is_empty());
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
