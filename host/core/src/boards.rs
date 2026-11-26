//! Which boards nocry knows, and what their firmware expects.
//!
//! All of them are Creality boards with an Ingenic X2000E, the same loader (SPL), slot scheme
//! and `sn_mac` identity record. They differ in what the fields of that record mean:
//!
//! - **Nebula family**: one firmware per printer type. Its `ota_board_name` (NEBULA, F003...)
//!   names the printer; the mainboard field is always "NEBULA V1.0.0.1".
//! - **K1 family**: one firmware line for several printer types and mainboard revisions. The
//!   firmware picks the printer's configuration from the model field ("CR-K1", "K1C"...) and
//!   the mainboard field ("CR4CU220812S10/S11/S12").
//!
//! Which model/mainboard combinations a given firmware supports is read from the firmware
//! itself (`firmware_support`), not from a table here.

use crate::squashfs;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FamilyId {
    Nebula,
    K1,
}

/// Where the bootloader and partition table for a blank board of this family come from:
/// an official factory package. Update files do not contain them.
#[derive(Debug)]
pub struct BootSource {
    pub url: &'static str,
    pub file: &'static str,
    pub size: u64,
    /// sha256 of `images/u-boot-with-spl-mbr-gpt.bin` inside it
    pub boot_sha256: &'static str,
    pub cache_name: &'static str,
}

#[derive(Debug)]
pub struct Family {
    pub id: FamilyId,
    pub name: &'static str,
    /// values of the sn_mac mainboard field that mean this family (prefix match)
    pub mainboard_prefixes: &'static [&'static str],
    /// mainboard field written when a board has none and nothing else decides it
    pub default_mainboard: Option<&'static str>,
    pub boot: BootSource,
}

pub const FAMILIES: &[Family] = &[
    Family {
        id: FamilyId::Nebula,
        name: "Nebula Pad family",
        mainboard_prefixes: &["NEBULA"],
        default_mainboard: Some("NEBULA V1.0.0.1"),
        boot: BootSource {
            url: "https://github.com/CrealityOfficial/Ender-3_V3_KE_Klipper/releases/download/V1.1.0.12/Ender-3_V3_KE_1.1.0.12.ingenic",
            file: "Ender-3_V3_KE_1.1.0.12.ingenic",
            size: 130364076,
            boot_sha256: "8dc843d276694e0996062ac599a0c57134f0db065728536a03ac276aadc0af9a",
            cache_name: "nebula-boot-area.bin",
        },
    },
    Family {
        id: FamilyId::K1,
        name: "K1 family",
        mainboard_prefixes: &["CR4CU220812S1"],
        default_mainboard: None, // S10, S11 or S12: printed on the board; the user has to say
        boot: BootSource {
            url: "https://github.com/CrealityOfficial/K1_Series_Klipper/releases/download/V1.3.3.5/CR4CU220812S11_1.3.3.5.ingenic",
            file: "CR4CU220812S11_1.3.3.5.ingenic",
            size: 142951851,
            boot_sha256: "1ccac1a13a8c3c96b718731403c6555a0d127881c7ec5923d35f7476088399eb",
            cache_name: "k1-boot-area.bin",
        },
    },
];

pub fn family(id: FamilyId) -> &'static Family {
    FAMILIES.iter().find(|f| f.id == id).unwrap()
}

/// A firmware line: what an `ota_board_name` stands for.
#[derive(Debug)]
pub struct Line {
    pub ota_board: &'static str,
    pub family: FamilyId,
    /// what to call the firmware in dialogs
    pub name: &'static str,
    /// the one model field this firmware is for (Nebula family); None when the firmware serves
    /// several printer types and the model field chooses (K1 family)
    pub model: Option<&'static str>,
}

pub const LINES: &[Line] = &[
    Line { ota_board: "NEBULA", family: FamilyId::Nebula, name: "Retail Nebula Pad", model: Some("Nebula Pad") },
    Line { ota_board: "F003", family: FamilyId::Nebula, name: "CR-10 SE", model: Some("F003") },
    Line { ota_board: "F004", family: FamilyId::Nebula, name: "Ender-5 Max", model: Some("F004") },
    Line { ota_board: "F005", family: FamilyId::Nebula, name: "Ender-3 V3 KE", model: Some("F005") },
    // the K1 line is published under the S11 mainboard's name and serves S10, S11 and S12
    Line { ota_board: "CR4CU220812S10", family: FamilyId::K1, name: "K1 series", model: None },
    Line { ota_board: "CR4CU220812S11", family: FamilyId::K1, name: "K1 series", model: None },
    Line { ota_board: "CR4CU220812S12", family: FamilyId::K1, name: "K1 series", model: None },
    // Ender-3 V3 and V3 Plus: K1-family mainboard and panels, own firmware line (not tried on hardware)
    Line { ota_board: "F001", family: FamilyId::K1, name: "Ender-3 V3 series", model: None },
];

pub fn line(ota_board: &str) -> Option<&'static Line> {
    LINES.iter().find(|l| l.ota_board == ota_board)
}

/// Printer names for model-field values. Display only: unknown values are shown as they are.
const PRODUCTS: &[(&str, &str)] = &[
    ("Nebula Pad", "Retail Nebula Pad"),
    ("F003", "CR-10 SE"),
    ("F004", "Ender-5 Max"),
    ("F005", "Ender-3 V3 KE"),
    ("CR-K1", "K1"),
    ("CR-K1 Max", "K1 Max"),
    ("K1C", "K1C"),
    ("K1 SE", "K1 SE"),
    ("K1 Max SE", "K1 Max SE"),
    ("K2 SE", "K2 SE"),
    ("K2 SE Combo", "K2 SE Combo"),
    ("F001", "Ender-3 V3"),
    ("F002", "Ender-3 V3 Plus"),
];

pub fn product_name(model: &str) -> String {
    PRODUCTS.iter().find(|(m, _)| *m == model).map(|(_, n)| n.to_string()).unwrap_or_else(|| model.to_string())
}

/// Name of a firmware for dialogs, from its `ota_board_name`.
pub fn firmware_name(ota_board: &str) -> String {
    line(ota_board).map(|l| l.name.to_string()).unwrap_or_else(|| ota_board.to_string())
}

/// The family a board belongs to, from its sn_mac mainboard field.
pub fn family_of_mainboard(mainboard: &str) -> Option<&'static Family> {
    FAMILIES.iter().find(|f| f.mainboard_prefixes.iter().any(|p| !mainboard.is_empty() && mainboard.starts_with(p)))
}

/// Describes firmware nocry has no line for, for refusal messages.
pub fn unknown_firmware(ota_board: &str) -> String {
    format!("a printer nocry does not know; firmware id {ota_board}")
}

/// One printer type a firmware can run, and on which mainboards.
#[derive(Debug, Clone, PartialEq)]
pub struct Support {
    pub model: String,
    /// mainboard field values the firmware names for this model; empty = it does not look at it
    pub mainboards: Vec<String>,
}

/// What a firmware says it supports: the `case $model in` of `copy_config` in its
/// /etc/init.d/S55klipper_service, which picks the printer's configuration at every start.
/// Empty if the script is missing or reads differently.
pub fn firmware_support(rootfs: &[u8]) -> Vec<Support> {
    // the second path is where the K1 family's newer generation keeps it
    for path in ["etc/init.d/S55klipper_service", "apps/etc/init.d/CS55klipper_service"] {
        if let Some(script) = squashfs::read_file(rootfs, path, 1 << 20) {
            let s = parse_support(&String::from_utf8_lossy(&script));
            if !s.is_empty() {
                return s;
            }
        }
    }
    vec![]
}

pub fn parse_support(script: &str) -> Vec<Support> {
    let mut out: Vec<Support> = vec![];
    // older firmware: `case $model in` inside copy_config(); the K1 family's newer generation:
    // `case $product in` at the top of the script
    let (body, case) = match script.find("copy_config()").and_then(|s| script[s..].find("case $model in").map(|c| (s, c))) {
        Some((s, c)) => (&script[s..], c),
        None => match script.find("case $product in") {
            Some(c) => (script, c),
            None => return out,
        },
    };
    let mut cur: Option<Support> = None;
    for raw in body[case..].lines().skip(1) {
        let l = raw.trim();
        if l == "esac" && cur.is_none() {
            break;
        }
        // a case label: "CR-K1")   (possibly several separated by |)
        if cur.is_none() && l.ends_with(')') && l.starts_with('"') {
            let label = l.trim_end_matches(')').split('|').next().unwrap_or("").trim().trim_matches('"');
            if !label.is_empty() {
                cur = Some(Support { model: label.to_string(), mainboards: vec![] });
            }
            continue;
        }
        if l.starts_with("*)") {
            break; // the catch-all ends the list
        }
        if let Some(c) = cur.as_mut() {
            // [ "$board" = "CR4CU220812S11" ...
            let mut rest = l;
            while let Some(i) = rest.find("\"$board\" = \"") {
                let after = &rest[i + 12..];
                if let Some(j) = after.find('"') {
                    let b = &after[..j];
                    if !c.mainboards.iter().any(|x| x == b) {
                        c.mainboards.push(b.to_string());
                    }
                    rest = &after[j..];
                } else {
                    break;
                }
            }
            if l == ";;" {
                out.push(cur.take().unwrap());
            }
        }
    }
    out
}

/// Does `support` (from `firmware_support`) cover this printer type on this mainboard?
pub fn supports(support: &[Support], model: &str, mainboard: &str) -> bool {
    support.iter().any(|s| s.model == model && (s.mainboards.is_empty() || s.mainboards.iter().any(|b| b == mainboard)))
}

#[cfg(test)]
mod tests {
    use super::*;

    const K1: &str = r#"
copy_config()
{
    local model=$(get_sn_mac.sh model)
    local board=$(get_sn_mac.sh board)
    case $model in
        "CR-K1")
            if [ "$board" = "CR4CU220812S10" ]; then
                config_dir=K1_CR4CU220812S10
            elif [ "$board" = "CR4CU220812S12" -a "$structure_version" = "0" ]; then
                config_dir=K1_CR4CU220812S12
            elif [ "$board" = "CR4CU220812S12" -a "$structure_version" = "1" ]; then
                config_dir=K1_CR4CU220812S12_1
            fi
            gcode_dir=K1
            ;;
        "K1C")
            if [ "$board" = "CR4CU220812S12" ]; then
                config_dir=K1C_CR4CU220812S12
            fi
            ;;
        *)
            config_dir=
            ;;
    esac
}
"#;

    #[test]
    fn reads_k1_support() {
        let s = parse_support(K1);
        assert_eq!(s.len(), 2);
        assert_eq!(s[0], Support { model: "CR-K1".into(), mainboards: vec!["CR4CU220812S10".into(), "CR4CU220812S12".into()] });
        assert!(supports(&s, "K1C", "CR4CU220812S12"));
        assert!(!supports(&s, "K1C", "CR4CU220812S11"));
        assert!(!supports(&s, "K1 SE", "CR4CU220812S12"));
    }

    #[test]
    fn reads_single_model_firmware() {
        let s = parse_support("copy_config()\n{\n case $model in\n \"Nebula Pad\")\n  config_dir=Nebula/Ender-3\\ Pro\n  ;;\n\n *)\n  config_dir=\n  ;;\n esac\n}\n");
        assert_eq!(s, vec![Support { model: "Nebula Pad".into(), mainboards: vec![] }]);
        assert!(supports(&s, "Nebula Pad", "NEBULA V1.0.0.1"));
    }

    #[test]
    fn reads_newer_generation() {
        let s = parse_support("product=$(x2000_get_sn_mac.sh model)\nif [ -n \"$product\" ]; then\n case $product in\n \"K1C\")\n  model=\"k1c\"\n  ;;\n \"CR-K1 Max\")\n  model=\"k1max\"\n  gcode=\"k1max\"\n  ;;\n *)\n  model=\"k1c\"\n  ;;\n esac\nfi\n");
        assert_eq!(s.iter().map(|x| x.model.as_str()).collect::<Vec<_>>(), ["K1C", "CR-K1 Max"]);
    }

    #[test]
    fn families_and_names() {
        assert_eq!(family_of_mainboard("CR4CU220812S12").unwrap().id, FamilyId::K1);
        assert_eq!(family_of_mainboard("NEBULA V1.0.0.1").unwrap().id, FamilyId::Nebula);
        assert!(family_of_mainboard("").is_none());
        assert_eq!(product_name("CR-K1 Max"), "K1 Max");
        assert_eq!(product_name("GS-01"), "GS-01");
        assert_eq!(firmware_name("CR4CU220812S11"), "K1 series");
        assert_eq!(line("F005").unwrap().model, Some("F005"));
    }
}
