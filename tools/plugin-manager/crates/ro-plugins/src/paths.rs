//! Default locations: the client directory and the repo's build output.

use std::path::{Path, PathBuf};

pub const DEFAULT_CLIENT: &str = "C:/Games/Ragnarok";

/// The repository root: the first ancestor of the exe or the working directory holding `scripts/build.py`.
pub fn repo_root() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok();
    let cwd = std::env::current_dir().ok();
    [exe, cwd]
        .into_iter()
        .flatten()
        .find_map(|start| start.ancestors().find(|dir| dir.join("scripts").join("build.py").is_file()).map(Path::to_owned))
}

/// `<repo>/build`.
pub fn default_build() -> Option<PathBuf> {
    repo_root().map(|root| root.join("build"))
}

/// `<repo>/Plugins`, whose `<Name>/plugin.json` files carry the descriptions.
pub fn plugin_sources() -> Option<PathBuf> {
    repo_root().map(|root| root.join("Plugins"))
}

/// The directory holding the built `.plugin`/`.dll` files: `<build>/plugins` when the build root is given.
pub fn build_plugins(build: &Path) -> PathBuf {
    let nested = build.join("plugins");
    if nested.is_dir() { nested } else { build.to_owned() }
}

/// The client used when no setting exists: the default one if it is there.
pub fn default_client() -> Option<PathBuf> {
    let path = PathBuf::from(DEFAULT_CLIENT);
    path.is_dir().then_some(path)
}
