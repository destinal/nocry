//! nocry-core: everything the nocry front ends (CLI, GUI) share.
//!
//! - `bootrom`, `usb`, `image`: load stage1/stage2 through the SoC bootrom (requests 0x00-0x05 only)
//! - `fastboot`: client for the nocry fastboot stage2 and the recovery's nocry-fbd
//! - `session`: detect the board's state, boot what is needed, open the eMMC
//! - `events`: status and progress reporting for the front ends
//! - `gpt`, `snmac`: on-disk formats
//! - `restore`, `diskimage`: provisioning/repair plans, full-disk backup and reimage

pub mod boards;
pub mod bootrom;
pub mod catalog;
pub mod diskimage;
pub mod events;
pub mod fastboot;
pub mod gpt;
pub mod image;
pub mod loader;
pub mod ota;
pub mod picture;
pub mod restore;
pub mod rtos;
pub mod session;
pub mod snmac;
pub mod squashfs;
pub mod usb;
