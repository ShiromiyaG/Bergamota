//! the services the games talk to. Native: what they need to run. Offline:
//! what they only probe (online play, local wireless, SpotPass, camera),
//! answered as a 3DS with no network and no camera would.

/// how the runtime answers a service.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Answer {
    /// implemented: the game needs it to run
    Native,
    /// a fixed "not connected" or "not present" answer
    Offline,
}

/// every service the games open, and how it is answered.
pub const SERVICES: &[(&str, Answer)] = &[
    ("srv:", Answer::Native),
    ("APT:A", Answer::Native),
    ("APT:U", Answer::Native),
    ("cfg:u", Answer::Native),
    ("cfg:s", Answer::Native),
    ("cfg:i", Answer::Native),
    ("fs:USER", Answer::Native),
    ("ldr:ro", Answer::Native),
    ("hid:USER", Answer::Native),
    ("gsp::Gpu", Answer::Native),
    ("dsp::DSP", Answer::Native),
    ("y2r:u", Answer::Native),
    ("ptm:u", Answer::Native),
    ("ac:u", Answer::Offline),
    ("ndm:u", Answer::Offline),
    ("frd:u", Answer::Offline),
    ("nwm::UDS", Answer::Offline),
    ("soc:U", Answer::Offline),
    ("http:C", Answer::Offline),
    ("ssl:C", Answer::Offline),
    ("boss:U", Answer::Offline),
    ("ir:USER", Answer::Offline),
    ("cam:u", Answer::Offline),
];
