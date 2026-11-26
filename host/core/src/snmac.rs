//! `sn_mac` partition record (X2000E Creality boards, see docs/sn_mac.md).
//!
//! Plain ASCII, NUL-terminated, read from the first 512 bytes of the partition:
//!   SN;MAC;MODEL;BOARD;PCBA_TEST;MACHINE_SN;STRUCTURE_VERSION;IC_LC;
//! Exactly 8 ';' (Creality's clear_sn_mac.sh refuses anything else). Older
//! firmware treats fields 6-8 as reserved.

use anyhow::{bail, Context, Result};
use std::fmt;

pub const RECORD_AREA: usize = 512;
pub const NFIELDS: usize = 8;

#[derive(Debug, Clone, PartialEq, Default)]
pub struct SnMac {
    pub fields: [String; NFIELDS],
}

/// Model/board pairs verified from each firmware's S13mcu_update / S55klipper_service.
pub const NEBULA_BOARD: &str = "NEBULA V1.0.0.1";
pub const KNOWN_MODELS: &[(&str, &str, &str)] = &[
    // (ota_board_name, model field, description)
    ("NEBULA", "Nebula Pad", "retail Nebula Pad / Nebula Smart Kit"),
    ("F003", "F003", "CR-10 SE"),
    ("F004", "F004", "Ender-5 Max"),
    ("F005", "F005", "Ender-3 V3 KE"),
];

pub fn model_for_ota_board(ota_board: &str) -> Option<&'static str> {
    KNOWN_MODELS.iter().find(|(b, _, _)| *b == ota_board).map(|(_, m, _)| *m)
}

pub fn describe_model(model: &str) -> &'static str {
    KNOWN_MODELS.iter().find(|(_, m, _)| *m == model).map(|(_, _, d)| *d).unwrap_or("unknown model")
}

/// Name a person would recognise, for dialogs: "Retail Nebula Pad", "Ender-3 V3 KE"...
/// Falls back to the raw value for models nocry does not know.
pub fn product_name(model: &str) -> String {
    match model {
        "Nebula Pad" => "Retail Nebula Pad".into(),
        m => match KNOWN_MODELS.iter().find(|(_, k, _)| *k == m) {
            Some((_, _, d)) => d.to_string(),
            None => m.to_string(),
        },
    }
}

/// The same, from a firmware's ota_board_name.
pub fn product_name_for_ota_board(board: &str) -> String {
    model_for_ota_board(board).map(product_name).unwrap_or_else(|| board.to_string())
}

fn is_hex(s: &str, n: usize) -> bool {
    s.len() == n && s.bytes().all(|c| c.is_ascii_hexdigit())
}

impl SnMac {
    pub fn sn(&self) -> &str {
        &self.fields[0]
    }
    pub fn mac(&self) -> &str {
        &self.fields[1]
    }
    pub fn model(&self) -> &str {
        &self.fields[2]
    }
    pub fn board(&self) -> &str {
        &self.fields[3]
    }
    pub fn sn_valid(&self) -> bool {
        is_hex(self.sn(), 14)
    }
    pub fn mac_valid(&self) -> bool {
        is_hex(self.mac(), 12)
    }
    pub fn identity_valid(&self) -> bool {
        self.sn_valid() && self.mac_valid()
    }

    /// Parse the first 512 bytes of the partition. Returns None for a blank
    /// (all 0x00 / 0xff) or unparseable area.
    pub fn parse(area: &[u8]) -> Option<SnMac> {
        let a = &area[..area.len().min(RECORD_AREA)];
        let end = a.iter().position(|&c| c == 0).unwrap_or(a.len());
        let s = std::str::from_utf8(&a[..end]).ok()?;
        if s.is_empty() || s.bytes().all(|c| c == 0xff) || !s.contains(';') {
            return None;
        }
        let mut f: [String; NFIELDS] = Default::default();
        for (i, part) in s.split(';').take(NFIELDS).enumerate() {
            f[i] = part.to_string();
        }
        Some(SnMac { fields: f })
    }

    pub fn semicolons(area: &[u8]) -> usize {
        let a = &area[..area.len().min(RECORD_AREA)];
        let end = a.iter().position(|&c| c == 0).unwrap_or(a.len());
        a[..end].iter().filter(|&&c| c == b';').count()
    }

    pub fn validate_for_write(&self) -> Result<()> {
        if !self.sn_valid() {
            bail!("SN {:?} must be exactly 14 hex digits", self.sn());
        }
        if !self.mac_valid() {
            bail!("MAC {:?} must be exactly 12 hex digits", self.mac());
        }
        for (i, f) in self.fields.iter().enumerate() {
            if f.contains(';') || f.bytes().any(|c| c < 0x20 || c > 0x7e) {
                bail!("field {} {:?} must be printable ASCII without ';'", i + 1, f);
            }
        }
        if self.model().is_empty() || self.board().is_empty() {
            bail!("model and board must not be empty");
        }
        Ok(())
    }

    /// "f1;f2;...;f8;" + NUL padding to 512 bytes.
    pub fn to_area(&self) -> Result<Vec<u8>> {
        self.validate_for_write()?;
        let mut s = String::new();
        for f in &self.fields {
            s.push_str(f);
            s.push(';');
        }
        if s.len() >= RECORD_AREA {
            bail!("sn_mac record too long ({} bytes)", s.len());
        }
        let mut v = s.into_bytes();
        v.resize(RECORD_AREA, 0);
        Ok(v)
    }
}

impl fmt::Display for SnMac {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(
            f,
            "SN={} MAC={} model={:?} board={:?}",
            self.sn(),
            self.mac(),
            self.model(),
            self.board()
        )?;
        let extra: Vec<String> = self.fields[4..]
            .iter()
            .enumerate()
            .filter(|(_, v)| !v.is_empty())
            .map(|(i, v)| format!("f{}={v:?}", i + 5))
            .collect();
        if !extra.is_empty() {
            write!(f, " {}", extra.join(" "))?;
        }
        Ok(())
    }
}

fn random_bytes<const N: usize>() -> Result<[u8; N]> {
    let mut b = [0u8; N];
    getrandom::getrandom(&mut b).context("OS random generator")?;
    Ok(b)
}

/// Random MAC: locally administered, unicast (bit1 of octet0 set, bit0 clear), so
/// it cannot collide with any vendor-assigned address.
pub fn generate_mac() -> Result<String> {
    let mut b: [u8; 6] = random_bytes()?;
    b[0] = (b[0] & 0xfc) | 0x02;
    Ok(b.iter().map(|x| format!("{x:02X}")).collect())
}

/// SN = 6 hex prefix + last 8 hex of the MAC (the pattern factory units use).
pub fn generate_sn(mac: &str, prefix: Option<&str>) -> Result<String> {
    let prefix = match prefix {
        Some(p) => {
            if !is_hex(p, 6) {
                bail!("SN prefix {p:?} must be 6 hex digits");
            }
            p.to_ascii_uppercase()
        }
        None => random_bytes::<3>()?.iter().map(|x| format!("{x:02X}")).collect(),
    };
    if !is_hex(mac, 12) {
        bail!("MAC {mac:?} must be 12 hex digits");
    }
    Ok(format!("{prefix}{}", &mac[4..].to_ascii_uppercase()))
}

#[cfg(test)]
mod tests {
    use super::*;

    const RETAIL: &[u8] = b"112233CC001234;AABBCC001234;Nebula Pad;NEBULA V1.0.0.1;;;;;\0\0\0";

    #[test]
    fn parses_retail_record() {
        let r = SnMac::parse(RETAIL).unwrap();
        assert_eq!(r.sn(), "112233CC001234");
        assert_eq!(r.mac(), "AABBCC001234");
        assert_eq!(r.model(), "Nebula Pad");
        assert_eq!(r.board(), NEBULA_BOARD);
        assert!(r.identity_valid());
        assert_eq!(SnMac::semicolons(RETAIL), 8);
    }

    #[test]
    fn round_trip_keeps_creality_format() {
        let r = SnMac::parse(RETAIL).unwrap();
        let a = r.to_area().unwrap();
        assert_eq!(a.len(), RECORD_AREA);
        assert_eq!(&a[..RETAIL.len() - 3], &RETAIL[..RETAIL.len() - 3]);
        assert_eq!(SnMac::semicolons(&a), 8);
    }

    #[test]
    fn blank_is_none() {
        assert!(SnMac::parse(&[0u8; 512]).is_none());
        assert!(SnMac::parse(&[0xffu8; 512]).is_none());
    }

    #[test]
    fn generated_identity_is_valid_and_local() {
        let mac = generate_mac().unwrap();
        let o0 = u8::from_str_radix(&mac[..2], 16).unwrap();
        assert_eq!(o0 & 0x03, 0x02, "locally administered unicast");
        let sn = generate_sn(&mac, Some("ABCDEF")).unwrap();
        assert_eq!(sn.len(), 14);
        assert!(sn.starts_with("ABCDEF") && sn.ends_with(&mac[4..]));
        let r = SnMac {
            fields: [sn, mac, "F005".into(), NEBULA_BOARD.into(), "".into(), "".into(), "".into(), "".into()],
        };
        r.validate_for_write().unwrap();
        assert_eq!(SnMac::semicolons(&r.to_area().unwrap()), 8);
    }

    #[test]
    fn rejects_bad_fields() {
        let mut r = SnMac::parse(RETAIL).unwrap();
        r.fields[2] = "a;b".into();
        assert!(r.to_area().is_err());
        r = SnMac::parse(RETAIL).unwrap();
        r.fields[1] = "XYZ".into();
        assert!(r.to_area().is_err());
    }
}
