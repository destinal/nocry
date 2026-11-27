//! Newer versions of the rescue image and of the nocryboot loader, from this project's
//! releases on GitHub. Used by the rescue system on the board (Updates, on the touch screen
//! and in the web application): `nocry self-update check` says what the newest release has,
//! `nocry self-update fetch` downloads one of its files and checks it.
//!
//! A release carries them as files of their own:
//!   nocry-rescue-<release version>.uImage
//!   nocryboot-nebula-<loader version>.bin, nocryboot-k1-<loader version>.bin
//! Size and SHA-256 of each come from GitHub's own list of the release's files, not from the
//! release's text, and a file is only handed on when both match what was downloaded.

use anyhow::{bail, Context, Result};
use serde_json::Value;
use std::io::{Read, Write};
use std::path::Path;

const RELEASES: &str = "https://api.github.com/repos/destinal/nocry/releases?per_page=30";
/// only files of this project's releases are ever fetched
pub const DOWNLOADS: &str = "https://github.com/destinal/nocry/releases/download/";

#[derive(Debug, Clone, PartialEq)]
pub struct Asset {
    pub name: String,
    pub url: String,
    pub sha256: String,
    pub size: u64,
}

/// What the newest release offers.
#[derive(Debug, Clone, PartialEq)]
pub struct Offer {
    /// the release's version, e.g. "0.2.0-beta.2"
    pub version: String,
    pub rescue: Option<Asset>,
    /// (family, loader version, file): family is "nebula" or "k1"
    pub loaders: Vec<(String, String, Asset)>,
}

/// A version as something that sorts: "0.2.0-beta.2" < "0.2.0-rc.1" < "0.2.0" < "0.2.1";
/// "0.4" (a loader's) works too. Anything unreadable sorts lowest.
pub fn version_key(v: &str) -> (u64, u64, u64, u8, u64) {
    let v = v.trim().trim_start_matches(['v', 'V']);
    let (main, pre) = match v.split_once('-') {
        Some((m, p)) => (m, Some(p)),
        None => (v, None),
    };
    let mut n = main.split('.').map(|x| x.parse::<u64>().unwrap_or(0));
    let (a, b, c) = (n.next().unwrap_or(0), n.next().unwrap_or(0), n.next().unwrap_or(0));
    let (rank, num) = match pre {
        None => (3, 0),
        Some(p) => {
            let (kind, num) = p.split_once('.').unwrap_or((p, "0"));
            let rank = match kind {
                "rc" => 2,
                "beta" => 1,
                _ => 0,
            };
            (rank, num.parse().unwrap_or(0))
        }
    };
    (a, b, c, rank, num)
}

fn s<'a>(v: &'a Value, k: &str) -> &'a str {
    v.get(k).and_then(|x| x.as_str()).unwrap_or("")
}

fn is_sha256(h: &str) -> bool {
    h.len() == 64 && h.bytes().all(|c| c.is_ascii_hexdigit())
}

/// The newest release (by version, drafts left out, pre-releases counted) in the answer of
/// GitHub's releases API that carries at least one of the files.
pub fn from_releases(v: &Value) -> Option<Offer> {
    let mut best: Option<Offer> = None;
    for r in v.as_array().map(|a| a.as_slice()).unwrap_or_default() {
        let tag = s(r, "tag_name");
        if r.get("draft").and_then(|x| x.as_bool()).unwrap_or(true) || !tag.starts_with('v') {
            continue; // "board-images" and the like are not releases of the program
        }
        let version = tag[1..].to_string();
        let mut offer = Offer { version: version.clone(), rescue: None, loaders: vec![] };
        for a in r.get("assets").and_then(|a| a.as_array()).map(|a| a.as_slice()).unwrap_or_default() {
            let (name, url) = (s(a, "name"), s(a, "browser_download_url"));
            let sha256 = s(a, "digest").trim_start_matches("sha256:").to_lowercase();
            let size = a.get("size").and_then(|x| x.as_u64()).unwrap_or(0);
            if !url.starts_with(DOWNLOADS) || !is_sha256(&sha256) || size == 0 {
                continue;
            }
            let asset = Asset { name: name.to_string(), url: url.to_string(), sha256, size };
            if name == format!("nocry-rescue-{version}.uImage") {
                offer.rescue = Some(asset);
            } else if let Some(rest) = name.strip_prefix("nocryboot-").and_then(|n| n.strip_suffix(".bin")) {
                // nocryboot-<family>-<version>.bin; a loader is exactly 24 KiB
                if let Some((family, lv)) = rest.split_once('-') {
                    if (family == "nebula" || family == "k1") && size == 24576 && lv.bytes().all(|c| c.is_ascii_digit() || c == b'.') {
                        offer.loaders.push((family.to_string(), lv.to_string(), asset));
                    }
                }
            }
        }
        if offer.rescue.is_none() && offer.loaders.is_empty() {
            continue;
        }
        if best.as_ref().map_or(true, |b| version_key(&offer.version) > version_key(&b.version)) {
            best = Some(offer);
        }
    }
    best
}

/// Ask GitHub.
pub fn latest() -> Result<Offer> {
    let resp = ureq::get(RELEASES)
        .timeout(std::time::Duration::from_secs(30))
        .set("Accept", "application/vnd.github+json")
        .set("User-Agent", "nocry")
        .call()
        .context("could not reach GitHub for the list of releases (no internet connection?)")?;
    let v: Value = serde_json::from_reader(resp.into_reader()).context("unexpected answer from GitHub")?;
    from_releases(&v).context("no release with a rescue image or a loader was found")
}

/// Download `url` (a file of one of this project's releases) to `dest` and check its size
/// and SHA-256. On any difference the file is removed and nothing is left behind.
pub fn fetch(url: &str, sha256: &str, size: u64, dest: &Path) -> Result<()> {
    use sha2::Digest as _;
    if !url.starts_with(DOWNLOADS) {
        bail!("refusing to download from anywhere but this project's releases");
    }
    if !is_sha256(sha256) || size == 0 {
        bail!("no checksum or size to check the download against; nothing was downloaded");
    }
    if let Some(d) = dest.parent() {
        std::fs::create_dir_all(d)?;
    }
    let part = dest.with_extension("part");
    let r = (|| -> Result<()> {
        let resp = ureq::get(url)
            .set("User-Agent", "nocry")
            .timeout(std::time::Duration::from_secs(3600))
            .call()
            .context("could not download the file (no internet connection?)")?;
        let mut out = std::fs::File::create(&part)?;
        let mut rd = resp.into_reader();
        let mut h = sha2::Sha256::new();
        let mut buf = vec![0u8; 256 * 1024];
        let (mut done, mut last) = (0u64, std::time::Instant::now());
        loop {
            crate::events::check_cancelled()?;
            let n = rd.read(&mut buf).context("the download was interrupted")?;
            if n == 0 {
                break;
            }
            done += n as u64;
            if done > size {
                bail!("the download is larger than the release says ({size} bytes)");
            }
            out.write_all(&buf[..n])?;
            h.update(&buf[..n]);
            if last.elapsed().as_millis() >= 500 {
                last = std::time::Instant::now();
                crate::events::progress("downloading", done, size, "");
            }
        }
        out.sync_all()?;
        crate::events::progress_end();
        if done != size {
            bail!("the download is incomplete ({done} of {size} bytes)");
        }
        let got = format!("{:x}", h.finalize());
        if got != sha256.to_lowercase() {
            bail!("the downloaded file is damaged (SHA-256 {got}, the release says {sha256}); nothing was installed");
        }
        Ok(())
    })();
    if let Err(e) = r {
        crate::events::progress_end();
        let _ = std::fs::remove_file(&part);
        return Err(e);
    }
    std::fs::rename(&part, dest)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn versions_sort() {
        let order = ["0.1.0-alpha.4", "0.2.0-beta.1", "0.2.0-beta.2", "0.2.0-beta.10", "0.2.0-rc.1", "0.2.0", "0.2.1", "0.10.0", "1.0.0"];
        for w in order.windows(2) {
            assert!(version_key(w[0]) < version_key(w[1]), "{} < {}", w[0], w[1]);
        }
        assert!(version_key("0.3") < version_key("0.4") && version_key("0.4") < version_key("0.10"));
        assert_eq!(version_key("v0.2.0"), version_key("0.2.0"));
    }

    #[test]
    fn newest_release_with_checked_files() {
        let sha = "a".repeat(64);
        let asset = |name: &str, size: u64, digest: &str, tag: &str| json!({"name": name, "size": size, "digest": digest,
            "browser_download_url": format!("{DOWNLOADS}{tag}/{name}")});
        let d = format!("sha256:{sha}");
        let v = json!([
            {"tag_name": "board-images", "draft": false, "prerelease": true, "assets": [asset("nocry-rescue-x.uImage", 5, &d, "board-images")]},
            {"tag_name": "v0.2.0-beta.3", "draft": true, "assets": [asset("nocry-rescue-0.2.0-beta.3.uImage", 5, &d, "v0.2.0-beta.3")]},
            {"tag_name": "v0.2.0-beta.1", "draft": false, "prerelease": true, "assets": [asset("nocry-rescue-0.2.0-beta.1.uImage", 5, &d, "v0.2.0-beta.1")]},
            {"tag_name": "v0.2.0-beta.2", "draft": false, "prerelease": true, "assets": [
                asset("nocry-rescue-0.2.0-beta.2.uImage", 18000000, &d, "v0.2.0-beta.2"),
                asset("nocryboot-nebula-0.4.bin", 24576, &d, "v0.2.0-beta.2"),
                asset("nocryboot-k1-0.4.bin", 24576, &d, "v0.2.0-beta.2"),
                asset("nocryboot-nebula-0.5.bin", 24000, &d, "v0.2.0-beta.2"),      // not a loader's size
                asset("nocryboot-other-0.4.bin", 24576, "", "v0.2.0-beta.2"),       // no checksum
                json!({"name": "nocry-rescue-0.2.0-beta.2.uImage", "size": 5, "digest": d, "browser_download_url": "https://example.com/x"}),
            ]},
        ]);
        let o = from_releases(&v).unwrap();
        assert_eq!(o.version, "0.2.0-beta.2");
        assert_eq!(o.rescue.as_ref().unwrap().size, 18000000);
        assert_eq!(o.loaders.iter().map(|l| (l.0.as_str(), l.1.as_str())).collect::<Vec<_>>(), vec![("nebula", "0.4"), ("k1", "0.4")]);
        assert!(from_releases(&json!([])).is_none());
    }

    #[test]
    fn fetch_refuses_other_places_and_missing_checksums() {
        let d = std::env::temp_dir().join("nocry-selfupdate-test");
        assert!(fetch("https://example.com/x", &"a".repeat(64), 5, &d).is_err());
        assert!(fetch(&format!("{DOWNLOADS}v1/x"), "", 5, &d).is_err());
        assert!(fetch(&format!("{DOWNLOADS}v1/x"), &"a".repeat(64), 0, &d).is_err());
    }
}
