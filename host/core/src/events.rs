//! How the library reports what it is doing. Front ends install a sink (`set_sink`); the
//! default prints to the terminal like the CLI always did. One device operation runs at a
//! time, so the sink is process-wide.

use std::sync::{Arc, RwLock};

#[derive(Debug, Clone)]
pub enum Event {
    /// A line of status or result text.
    Log(String),
    /// A long step is at `done` of `total` bytes. `note` is extra text (rate, eta...).
    Progress { what: String, done: u64, total: u64, note: String },
    /// The current progress display is finished.
    ProgressEnd,
}

type Sink = Arc<dyn Fn(Event) + Send + Sync>;
static SINK: RwLock<Option<Sink>> = RwLock::new(None);

/// Route all events to `f` (replaces the terminal output).
pub fn set_sink(f: impl Fn(Event) + Send + Sync + 'static) {
    *SINK.write().unwrap() = Some(Arc::new(f));
}

pub fn emit(e: Event) {
    let sink = SINK.read().unwrap().clone();
    match sink {
        Some(f) => f(e),
        None => terminal(e),
    }
}

fn terminal(e: Event) {
    match e {
        Event::Log(l) => println!("{l}"),
        Event::Progress { what, done, total, note } => {
            eprint!("\r  {what}: {:>5}/{} MiB{note}   ", done >> 20, total >> 20)
        }
        Event::ProgressEnd => eprintln!(),
    }
}

pub fn progress(what: &str, done: u64, total: u64, note: &str) {
    emit(Event::Progress { what: what.into(), done, total, note: note.into() });
}

pub fn progress_end() {
    emit(Event::ProgressEnd);
}

/// `println!` for the library: goes to the event sink.
#[macro_export]
macro_rules! say {
    ($($a:tt)*) => { $crate::events::emit($crate::events::Event::Log(format!($($a)*))) };
}

static CANCEL: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Ask the running operation to stop at its next safe point (long read/compare loops).
pub fn cancel() {
    CANCEL.store(true, std::sync::atomic::Ordering::SeqCst);
}

/// Front ends call this before starting an operation.
pub fn reset_cancel() {
    CANCEL.store(false, std::sync::atomic::Ordering::SeqCst);
}

pub fn check_cancelled() -> anyhow::Result<()> {
    if CANCEL.load(std::sync::atomic::Ordering::SeqCst) {
        anyhow::bail!("cancelled");
    }
    Ok(())
}
