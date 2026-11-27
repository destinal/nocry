//! The boot picture image of a firmware slot (partition `rtos` / `rtos2`).
//!
//! It is a small program the loader runs before Linux: it switches the display on and shows
//! the picture seen for the first seconds after power-on. Every Creality firmware brings its
//! own (the Creality logo). nocry can put its open one (`rtos/`, "nocry-rtos") in its place.
//!
//! The firmware's own image is not thrown away: it is kept in the unused second half of the
//! same 4 MiB partition, so the choice can be undone without the firmware file.
//!
//!   0          the image the loader runs
//!   2 MiB-512  one sector of text: `nocry:firmware-picture size=<n> sha256=<hex>`
//!   2 MiB      the firmware's own image, <n> bytes
//!
//! A firmware update from the printer itself rewrites the start of the partition of the slot it
//! updates, which brings Creality's picture back there.

use crate::rtos::RtosHeader;
use sha2::Digest as _;

pub const KEPT_OFFSET: u64 = 2 << 20;
pub const KEPT_MARK_OFFSET: u64 = KEPT_OFFSET - 512;
const KEPT_TAG: &str = "nocry:firmware-picture";
/// what identifies nocry's image (a text its start-up log begins with)
const NOCRY_MARK: &[u8] = b"nocry-rtos";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    /// nocry's open boot picture image
    Nocry,
    /// the one that came with the firmware
    Firmware,
    /// no valid image
    None,
}

/// Length on disk of the image whose first sector is `head` (whole sectors), or None.
pub fn image_len(head: &[u8]) -> Option<usize> {
    let h = RtosHeader::parse(head).ok()?;
    Some((h.image_len() as usize).div_ceil(512) * 512)
}

/// Tells which image this is. `image` should hold the whole image (see `image_len`).
pub fn kind(image: &[u8]) -> Kind {
    if RtosHeader::parse(image).is_err() {
        return Kind::None;
    }
    if image.windows(NOCRY_MARK.len()).any(|w| w == NOCRY_MARK) {
        Kind::Nocry
    } else {
        Kind::Firmware
    }
}

fn sha256(b: &[u8]) -> String {
    format!("{:x}", sha2::Sha256::digest(b))
}

/// The sector that marks a kept firmware image.
pub fn kept_mark(image: &[u8]) -> Vec<u8> {
    let mut v = vec![0u8; 512];
    let t = format!("{KEPT_TAG} size={} sha256={}\n", image.len(), sha256(image));
    v[..t.len()].copy_from_slice(t.as_bytes());
    v
}

/// Size of the kept image according to its mark sector.
pub fn kept_size(mark: &[u8]) -> Option<usize> {
    let end = mark.iter().position(|&c| c == b'\n')?;
    let t = std::str::from_utf8(&mark[..end]).ok()?.strip_prefix(KEPT_TAG)?;
    t.split_whitespace().find_map(|w| w.strip_prefix("size=")).and_then(|x| x.parse().ok()).filter(|n| *n > 0 && *n <= (2usize << 20))
}

/// Checks a kept firmware image (mark sector + the bytes behind it) and returns it.
pub fn kept_image(mark: &[u8], data: &[u8]) -> Option<Vec<u8>> {
    let size = kept_size(mark)?;
    let end = mark.iter().position(|&c| c == b'\n')?;
    let t = std::str::from_utf8(&mark[..end]).ok()?;
    let sha = t.split_whitespace().find_map(|w| w.strip_prefix("sha256="))?;
    if data.len() < size || sha256(&data[..size]) != sha {
        return None;
    }
    let img = data[..size].to_vec();
    (kind(&img) == Kind::Firmware && image_len(&img) == Some(size)).then_some(img)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn image(len: usize, nocry: bool) -> Vec<u8> {
        let mut v = vec![0u8; len];
        let w = [0x1000_0008u32, 0, crate::rtos::TAG, 1, 0x8600_0000, 0x8600_0000 + len as u32, 0x8607_0400, 0x86fa_1200, 0x8600_0040];
        for (i, x) in w.iter().enumerate() {
            v[i * 4..i * 4 + 4].copy_from_slice(&x.to_le_bytes());
        }
        if nocry {
            v[0x100..0x10a].copy_from_slice(b"nocry-rtos");
        }
        v
    }

    #[test]
    fn tells_images_apart() {
        assert_eq!(kind(&image(4096, true)), Kind::Nocry);
        assert_eq!(kind(&image(4096, false)), Kind::Firmware);
        assert_eq!(kind(&[0u8; 4096]), Kind::None);
        assert_eq!(image_len(&image(4096, false)[..512]), Some(4096));
        assert_eq!(image_len(&[0u8; 512]), None);
    }

    #[test]
    fn kept_image_round_trip() {
        let f = image(8192, false);
        let m = kept_mark(&f);
        assert_eq!(kept_size(&m), Some(8192));
        assert_eq!(kept_image(&m, &f), Some(f.clone()));
        let mut bad = f.clone();
        bad[5000] ^= 1;
        assert_eq!(kept_image(&m, &bad), None);
        assert_eq!(kept_image(&[0u8; 512], &f), None);
        // nocry's own image is never "the firmware's picture"
        let n = image(8192, true);
        assert_eq!(kept_image(&kept_mark(&n), &n), None);
    }
}
