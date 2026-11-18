//! Core of the rodecomp plugin manager: `<Name>.toml` plugin files (manifest, settings, values), `disabled.txt`, `status.txt`, semver ranges, the
//! offline prediction of Host.cpp's dependency resolution, game detection, and the install/uninstall/enable/disable
//! rules. Used by the `ro-plugins` GUI and CLI.

pub mod manifest;
pub mod ops;
pub mod resolve;
pub mod semver;
pub mod settings;
pub mod status;

#[cfg(test)]
mod testutil;
