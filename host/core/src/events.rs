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

/// Progress of one whole operation, possibly spanning several partitions and steps: one bar
/// from start to end, with speed and time left.
pub struct Meter {
    label: String,
    total: u64,
    done: u64,
    t0: std::time::Instant,
    last: Option<std::time::Instant>,
}

impl Meter {
    pub fn new(label: impl Into<String>, total: u64) -> Self {
        let m = Meter { label: label.into(), total: total.max(1), done: 0, t0: std::time::Instant::now(), last: None };
        progress(&m.label, 0, m.total, "");
        m
    }
    /// `n` more bytes are finished.
    pub fn add(&mut self, n: u64) {
        self.done = (self.done + n).min(self.total);
        if self.done < self.total && self.last.is_some_and(|l| l.elapsed().as_millis() < 250) {
            return;
        }
        self.last = Some(std::time::Instant::now());
        let s = self.t0.elapsed().as_secs_f64();
        let note = if s >= 2.0 && self.done > 0 && self.done < self.total {
            let rate = self.done as f64 / s;
            format!("  {:.1} MB/s  {} left", rate / 1e6, crate::diskimage::fmt_duration((self.total - self.done) as f64 / rate))
        } else {
            String::new()
        };
        progress(&self.label, self.done, self.total, &note);
    }
    pub fn end(self) {
        progress_end();
    }
}
