/* C interface to nocry-core (see host/ffi). Strings are UTF-8; every char* returned by the
 * library must be released with nocry_string_free(). Functions that talk to the board block:
 * call them from a worker thread. One device operation at a time. */
#ifndef NOCRY_H
#define NOCRY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NOCRY_EVENT_LOG = 0, NOCRY_EVENT_PROGRESS = 1, NOCRY_EVENT_PROGRESS_END = 2 };

/* text: the log line, or the step name for progress; note: rate/eta text for progress.
 * Called on the thread that runs the operation. */
typedef void (*nocry_event_cb)(int kind, const char *text, uint64_t done, uint64_t total,
                               const char *note, void *user);
void nocry_set_event_callback(nocry_event_cb cb, void *user);

/* Where the stage1/stage2 images and the recovery image are (NULL keeps the default search). */
void nocry_set_paths(const char *images_dir, const char *recovery_image);

/* Cheap USB scan. {"state":"none|usb-boot|fastboot|recovery|uboot-disk","port":"1-7"} */
char *nocry_board_state_json(void);

/* Boots the recovery if needed (RAM only) and reads identity, slot and partitions.
 * {"ok":true,...} or {"ok":false,"error":"..."} */
char *nocry_device_info_json(void);

/* Full-disk backup to output_path (+ .sha256). Boots the recovery if needed; the board is only
 * read. {"ok":true,"sha256":"...","bytes":N,"seconds":S} or {"ok":false,"cancelled":bool,"error":"..."} */
char *nocry_backup_json(const char *output_path, int verify, int overwrite);

/* Plan (execute=0) or carry out (execute=1) a firmware installation from os_path: a .ingenic
 * file, or a folder with xImage, rootfs.squashfs and zero.bin.
 * keep_data=1: repair (identity, settings, files, other slot kept); 0: clean install (data
 * partitions erased, identity kept). safety_dir: where to save the first 3 MiB before writing
 * (NULL: no copy). Returns the plan/result; "bytes_to_write":0 means nothing to do. */
char *nocry_install_json(const char *os_path, int keep_data, int execute, const char *safety_dir);

/* The two system slots: {"ok":true,"active":"a|b","a":{"bootable":bool,"firmware_name":"...",
 * "firmware_version":"..."},"b":{...}}. Reads the board (boots the recovery if needed). */
char *nocry_slots_json(void);

/* Make slot "a" or "b" start at the next power-on. Refuses a slot without a complete system.
 * Returns the same JSON as nocry_slots_json with the new state. */
char *nocry_set_active_slot_json(const char *slot, const char *safety_dir);

/* Copy the firmware of slot `from` ("a"|"b") into the other slot, verified. The active slot,
 * settings and identity are not changed. Returns the slots JSON with the new state. */
char *nocry_copy_slot_json(const char *from, const char *safety_dir);

/* Factory reset: blank the data partitions the firmware recreates on first boot (rootfs_data,
 * and userdata unless keep_userdata). execute=0 only checks. Firmware, slots and identity stay.
 * {"ok":true,"partitions":[{"name":..,"size":..}],"bytes_to_write":N,"executed":bool} */
char *nocry_factory_reset_json(int keep_userdata, int execute, const char *safety_dir);

/* Restore a full backup image. execute=0: quick check (size, valid backup, whose identity).
 * execute=1: check the file against its .sha256 if present, then make the board identical to
 * it (only differing parts written, verified). A backup of a different board is refused
 * unless allow_identity_change. Fields: image_model, device_model, image_sn_tail,
 * device_sn_tail, different_board, has_checksum, bytes_differed, bytes_written, executed. */
char *nocry_restore_backup_json(const char *image_path, int execute, int allow_identity_change, const char *safety_dir);

/* The list of downloadable firmware: {"ok":true,"built":"YYYY-MM-DD","updated":bool,
 * "models":[{"key":"NEBULA","name":"Retail Nebula Pad","firmwares":[{"version","file","size",
 * "released","url","downloaded":bool,"notes":[...]}]}]}. refresh=1 asks Creality's server
 * first and saves the result in data_dir. */
char *nocry_catalog_json(const char *data_dir, int refresh);

/* Download one firmware into <data_dir>/firmware, verify it, reuse an existing verified copy.
 * Cancellable. {"ok":true,"path":"..."} */
char *nocry_download_firmware_json(const char *url, const char *file, uint64_t size, const char *data_dir);

/* Shell access to the running recovery, as far as this computer can tell:
 * {"adb_installed":bool,"adb_ready":bool,"adb_serial":"...","adb_state":"...","console_port":"..."}
 * serial: the board's serial number (may be NULL). Runs `adb devices`; call from a worker thread. */
char *nocry_shell_status_json(const char *serial);

/* Advanced. All return {"ok":true,"name","bytes","bytes_written","sha256"} or {"ok":false,"error"}.
 * Writes are verified; a copy of the first 3 MiB goes to safety_dir first (NULL: none). */
char *nocry_partition_backup_json(const char *name, const char *out_path);   /* + out_path.sha256 */
char *nocry_partition_restore_json(const char *name, const char *file, const char *safety_dir);
char *nocry_partition_erase_json(const char *name, const char *safety_dir);  /* fills with zeros */
char *nocry_erase_disk_json(const char *safety_dir);                         /* the board is blank afterwards */

/* Identity record. sn: 14 hex digits; mac: 12 hex digits (":" and "-" accepted); empty model
 * or board keep the current value. {"ok":true,"sn","mac","model","board"} */
char *nocry_set_identity_json(const char *sn, const char *mac, const char *model, const char *board, const char *safety_dir);
char *nocry_generate_identity_json(void);   /* {"ok":true,"sn":"...","mac":"..."}: random, valid pair */

/* Install into the inactive firmware slot only (same printer type as the board); nothing else
 * is touched. Same JSON as nocry_install_json plus "target_slot":"a|b". */
char *nocry_install_slot_json(const char *os_path, int execute, const char *safety_dir);

/* Factory reset that also makes old settings and files unrecoverable.
 * {"ok":true,"bytes":N,"partitions":[{"name":"userdata","method":"secure-discard|discard|overwrite"}]} */
char *nocry_secure_wipe_json(const char *safety_dir);

/* The board's log of its current (recovery) or previous start: {"ok":true,"kind":"recovery|previous","text":"..."} */
char *nocry_boot_log_json(void);

/* Ask the running operation to stop at its next safe point (callable from any thread). */
void nocry_cancel(void);

void nocry_string_free(char *s);

#ifdef __cplusplus
}
#endif
#endif
