//! C interface to nocry-core for the Qt front end (see include/nocry.h).

use nocry_core::events::{self, Event};
use nocry_core::session::{self, BoardKind, Via};
use std::ffi::{c_char, c_int, c_void, CStr, CString};

type EventCb = extern "C" fn(c_int, *const c_char, u64, u64, *const c_char, *mut c_void);

struct UserPtr(*mut c_void);
// The front end promises its callback and user pointer are usable from the worker thread.
unsafe impl Send for UserPtr {}
unsafe impl Sync for UserPtr {}

fn cstr(s: &str) -> CString {
    CString::new(s.replace('\0', " ")).unwrap()
}

fn out(s: String) -> *mut c_char {
    cstr(&s).into_raw()
}

fn esc(s: &str) -> String {
    let mut o = String::with_capacity(s.len() + 2);
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            '\n' => o.push_str("\\n"),
            '\r' => o.push_str("\\r"),
            '\t' => o.push_str("\\t"),
            c if (c as u32) < 0x20 => o.push_str(&format!("\\u{:04x}", c as u32)),
            c => o.push(c),
        }
    }
    o
}

#[no_mangle]
pub extern "C" fn nocry_set_event_callback(cb: Option<EventCb>, user: *mut c_void) {
    let Some(cb) = cb else { return };
    let user = UserPtr(user);
    events::set_sink(move |e| {
        let u = &user;
        let empty = cstr("");
        match e {
            Event::Log(l) => cb(0, cstr(&l).as_ptr(), 0, 0, empty.as_ptr(), u.0),
            Event::Progress { what, done, total, note } => {
                cb(1, cstr(&what).as_ptr(), done, total, cstr(&note).as_ptr(), u.0)
            }
            Event::ProgressEnd => cb(2, empty.as_ptr(), 0, 0, empty.as_ptr(), u.0),
        }
    });
}

/// # Safety
/// Pointers must be NULL or valid NUL-terminated strings.
#[no_mangle]
pub unsafe extern "C" fn nocry_set_paths(images_dir: *const c_char, recovery_image: *const c_char) {
    if !images_dir.is_null() {
        std::env::set_var("NOCRY_IMAGES", CStr::from_ptr(images_dir).to_string_lossy().as_ref());
    }
    if !recovery_image.is_null() {
        std::env::set_var("NOCRY_RECOVERY", CStr::from_ptr(recovery_image).to_string_lossy().as_ref());
    }
}

#[no_mangle]
pub extern "C" fn nocry_board_state_json() -> *mut c_char {
    let r = std::panic::catch_unwind(session::board_state);
    out(match r {
        Ok(Ok(s)) => {
            let k = match s.kind {
                BoardKind::None => "none",
                BoardKind::UsbBoot => "usb-boot",
                BoardKind::Fastboot => "fastboot",
                BoardKind::Recovery => "recovery",
                BoardKind::UbootDisk => "uboot-disk",
            };
            format!("{{\"state\":\"{k}\",\"port\":\"{}\"}}", esc(&s.port))
        }
        Ok(Err(e)) => format!("{{\"state\":\"error\",\"error\":\"{}\"}}", esc(&format!("{e:#}"))),
        Err(_) => "{\"state\":\"error\",\"error\":\"internal error\"}".into(),
    })
}

#[no_mangle]
pub extern "C" fn nocry_device_info_json() -> *mut c_char {
    let r = std::panic::catch_unwind(|| {
        let soc = &nocry_core::bootrom::SOCS[0];
        session::device_info(soc, Via::Auto, test_target().as_ref())
    });
    out(match r {
        Ok(Ok(d)) => {
            let slot = |s: Option<&session::SlotInfo>| match s {
                Some(s) => format!(
                    "{{\"bootable\":{},\"firmware_name\":\"{}\",\"firmware_version\":\"{}\"}}",
                    s.bootable(), esc(&s.firmware_name), esc(&s.firmware_version)
                ),
                None => "null".into(),
            };
            let parts: Vec<String> = d
                .partitions
                .iter()
                .map(|(n, o, s)| format!("{{\"name\":\"{}\",\"offset\":{o},\"size\":{s}}}", esc(n)))
                .collect();
            format!(
                "{{\"ok\":true,\"access\":\"{}\",\"emmc_bytes\":{},\"sn\":\"{}\",\"mac\":\"{}\",\"model\":\"{}\",\
                 \"model_description\":\"{}\",\"board\":\"{}\",\"identity_valid\":{},\"active_slot\":\"{}\",\
                 \"partitions\":[{}],\"slot_a\":{},\"slot_b\":{}}}",
                esc(&d.access),
                d.emmc_bytes,
                esc(&d.sn),
                esc(&d.mac),
                esc(&d.model),
                esc(&d.model_description),
                esc(&d.board),
                d.identity_valid,
                esc(&d.active_slot),
                parts.join(","),
                slot(d.slots.as_ref().map(|s| &s.a)),
                slot(d.slots.as_ref().map(|s| &s.b))
            )
        }
        Ok(Err(e)) => format!("{{\"ok\":false,\"error\":\"{}\"}}", esc(&format!("{e:#}"))),
        Err(_) => "{\"ok\":false,\"error\":\"internal error\"}".into(),
    })
}

/// # Safety
/// `output_path` must be a valid NUL-terminated UTF-8 path.
#[no_mangle]
pub unsafe extern "C" fn nocry_backup_json(output_path: *const c_char, verify: c_int, overwrite: c_int) -> *mut c_char {
    let path = std::path::PathBuf::from(CStr::from_ptr(output_path).to_string_lossy().as_ref());
    events::reset_cancel();
    let r = std::panic::catch_unwind(|| {
        session::backup(&nocry_core::bootrom::SOCS[0], Via::Auto, &path, verify != 0, overwrite != 0)
    });
    out(match r {
        Ok(Ok(b)) => format!(
            "{{\"ok\":true,\"sha256\":\"{}\",\"bytes\":{},\"seconds\":{:.1}}}",
            b.sha256, b.bytes, b.seconds
        ),
        Ok(Err(e)) => {
            let msg = format!("{e:#}");
            format!("{{\"ok\":false,\"cancelled\":{},\"error\":\"{}\"}}", msg.contains("cancelled") || msg.contains("verification stopped"), esc(&msg))
        }
        Err(_) => "{\"ok\":false,\"error\":\"internal error\"}".into(),
    })
}

unsafe fn opt_path(p: *const c_char) -> Option<std::path::PathBuf> {
    if p.is_null() {
        None
    } else {
        Some(std::path::PathBuf::from(CStr::from_ptr(p).to_string_lossy().as_ref()))
    }
}

/// # Safety
/// `os_path` must be a valid NUL-terminated UTF-8 path; `safety_dir` NULL or likewise.
#[no_mangle]
pub unsafe extern "C" fn nocry_install_json(os_path: *const c_char, keep_data: c_int, execute: c_int, safety_dir: *const c_char) -> *mut c_char {
    let os = opt_path(os_path).unwrap_or_default();
    let safety = opt_path(safety_dir);
    // development/self-test hook: operate on an image file instead of a board
    let target = std::env::var("NOCRY_TEST_TARGET").ok().map(std::path::PathBuf::from);
    events::reset_cancel();
    let r = std::panic::catch_unwind(|| {
        session::install(&nocry_core::bootrom::SOCS[0], Via::Auto, &os, keep_data != 0, execute != 0, safety.as_deref(), target.as_ref())
    });
    out(match r {
        Ok(Ok(i)) => {
            let notes: Vec<String> = i.notes.iter().map(|n| format!("\"{}\"", esc(n))).collect();
            format!(
                "{{\"ok\":true,\"firmware_board\":\"{}\",\"firmware_version\":\"{}\",\"has_boot_area\":{},\
                 \"device_firmware\":\"{}\",\"model_before\":\"{}\",\"model_after\":\"{}\",\"identity_generated\":{},\
                 \"notes\":[{}],\"details\":\"{}\",\"bytes_to_write\":{},\"executed\":{},\"safety_copy\":\"{}\",\
                 \"model_before_name\":\"{}\",\"model_after_name\":\"{}\",\"firmware_name\":\"{}\",\"device_firmware_name\":\"{}\"}}",
                esc(&i.firmware_board), esc(&i.firmware_version), i.has_boot_area, esc(&i.device_firmware),
                esc(&i.model_before), esc(&i.model_after), i.identity_generated, notes.join(","), esc(&i.details),
                i.bytes_to_write, i.executed, esc(&i.safety_copy), esc(&i.model_before_name),
                esc(&i.model_after_name), esc(&i.firmware_name), esc(&i.device_firmware_name)
            )
        }
        Ok(Err(e)) => format!("{{\"ok\":false,\"error\":\"{}\"}}", esc(&format!("{e:#}"))),
        Err(_) => "{\"ok\":false,\"error\":\"internal error\"}".into(),
    })
}

fn slots_json(r: std::thread::Result<anyhow::Result<session::Slots>>) -> *mut c_char {
    let one = |s: &session::SlotInfo| {
        format!(
            "{{\"bootable\":{},\"firmware_name\":\"{}\",\"firmware_version\":\"{}\"}}",
            s.bootable(), esc(&s.firmware_name), esc(&s.firmware_version)
        )
    };
    out(match r {
        Ok(Ok(s)) => format!("{{\"ok\":true,\"active\":\"{}\",\"a\":{},\"b\":{}}}", s.active, one(&s.a), one(&s.b)),
        Ok(Err(e)) => format!("{{\"ok\":false,\"error\":\"{}\"}}", esc(&format!("{e:#}"))),
        Err(_) => "{\"ok\":false,\"error\":\"internal error\"}".into(),
    })
}

fn test_target() -> Option<std::path::PathBuf> {
    std::env::var("NOCRY_TEST_TARGET").ok().map(std::path::PathBuf::from)
}

#[no_mangle]
pub extern "C" fn nocry_slots_json() -> *mut c_char {
    events::reset_cancel();
    slots_json(std::panic::catch_unwind(|| session::slots(&nocry_core::bootrom::SOCS[0], Via::Auto, test_target().as_ref())))
}

/// # Safety
/// `slot` must be "a" or "b" (NUL-terminated); `safety_dir` NULL or a valid path.
#[no_mangle]
pub unsafe extern "C" fn nocry_set_active_slot_json(slot: *const c_char, safety_dir: *const c_char) -> *mut c_char {
    let slot = CStr::from_ptr(slot).to_string_lossy().to_string();
    let safety = opt_path(safety_dir);
    events::reset_cancel();
    slots_json(std::panic::catch_unwind(|| {
        session::set_active_slot(&nocry_core::bootrom::SOCS[0], Via::Auto, &slot, safety.as_deref(), test_target().as_ref())
    }))
}

/// # Safety
/// `safety_dir` must be NULL or a valid NUL-terminated path.
#[no_mangle]
pub unsafe extern "C" fn nocry_factory_reset_json(keep_userdata: c_int, execute: c_int, safety_dir: *const c_char) -> *mut c_char {
    let safety = opt_path(safety_dir);
    events::reset_cancel();
    let r = std::panic::catch_unwind(|| {
        session::factory_reset(&nocry_core::bootrom::SOCS[0], Via::Auto, keep_userdata != 0, execute != 0, safety.as_deref(), test_target().as_ref())
    });
    out(match r {
        Ok(Ok(x)) => {
            let parts: Vec<String> = x.partitions.iter().map(|(n, s)| format!("{{\"name\":\"{}\",\"size\":{s}}}", esc(n))).collect();
            format!("{{\"ok\":true,\"partitions\":[{}],\"bytes_to_write\":{},\"executed\":{}}}", parts.join(","), x.bytes_to_write, x.executed)
        }
        Ok(Err(e)) => format!("{{\"ok\":false,\"error\":\"{}\"}}", esc(&format!("{e:#}"))),
        Err(_) => "{\"ok\":false,\"error\":\"internal error\"}".into(),
    })
}

/// # Safety
/// `image_path` must be a valid NUL-terminated path; `safety_dir` NULL or likewise.
#[no_mangle]
pub unsafe extern "C" fn nocry_restore_backup_json(image_path: *const c_char, execute: c_int, allow_identity_change: c_int, safety_dir: *const c_char) -> *mut c_char {
    let img = opt_path(image_path).unwrap_or_default();
    let safety = opt_path(safety_dir);
    events::reset_cancel();
    let r = std::panic::catch_unwind(|| {
        session::restore_backup(&nocry_core::bootrom::SOCS[0], Via::Auto, &img, execute != 0, allow_identity_change != 0, safety.as_deref(), test_target().as_ref())
    });
    out(match r {
        Ok(Ok(x)) => format!(
            "{{\"ok\":true,\"image_model\":\"{}\",\"device_model\":\"{}\",\"image_sn_tail\":\"{}\",\"device_sn_tail\":\"{}\",\
             \"different_board\":{},\"has_checksum\":{},\"image_bytes\":{},\"bytes_differed\":{},\"bytes_written\":{},\"executed\":{}}}",
            esc(&x.check.image_model), esc(&x.check.device_model), esc(&x.check.image_sn_tail), esc(&x.check.device_sn_tail),
            x.check.different_board, x.check.has_checksum, x.check.image_bytes, x.bytes_differed, x.bytes_written, x.executed
        ),
        Ok(Err(e)) => {
            let m = format!("{e:#}");
            format!("{{\"ok\":false,\"cancelled\":{},\"error\":\"{}\"}}", m.contains("cancelled"), esc(&m))
        }
        Err(_) => "{\"ok\":false,\"error\":\"internal error\"}".into(),
    })
}

/// Ask the running operation to stop at its next safe point.
#[no_mangle]
pub extern "C" fn nocry_cancel() {
    events::cancel();
}

/// # Safety
/// `s` must come from this library and not have been freed.
#[no_mangle]
pub unsafe extern "C" fn nocry_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}
