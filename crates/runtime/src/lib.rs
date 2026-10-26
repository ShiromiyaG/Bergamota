//! A lean 3DS system for the recompiled ports (Solstice, Duality,
//! Tidefall): the kernel calls and services these games ask for, and
//! nothing else. What each game uses, and why the list is what it is:
//! Solstice's docs/lean-runtime.md.
//!
//! Threads run cooperatively on one host thread for now, as on the 3DS's
//! application core.

pub mod kernel;
pub mod services;
