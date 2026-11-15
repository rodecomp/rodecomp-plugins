//! Opening a folder in the system file manager.

use std::io;
use std::path::Path;

/// Shows `path` in the file manager. Fails when it is not an existing folder or the file manager cannot be asked.
pub fn open_folder(path: &Path) -> io::Result<()> {
    let folder = std::path::absolute(path)?;
    if !std::fs::metadata(&folder)?.is_dir() {
        return Err(io::ErrorKind::NotADirectory.into());
    }
    launch(&folder)
}

/// Through the shell rather than by spawning `explorer`: no child process is created, so nothing of this one (std
/// handles, working directory) has to be inheritable, and a failure comes back as an error code.
#[cfg(windows)]
fn launch(folder: &Path) -> io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::UI::Shell::{SEE_MASK_FLAG_NO_UI, SEE_MASK_NOASYNC, SHELLEXECUTEINFOW, ShellExecuteExW};
    use windows_sys::Win32::UI::WindowsAndMessaging::SW_SHOWNORMAL;

    const SEPARATOR: u16 = b'\\' as u16;
    // The trailing separator names the folder itself: without it the shell prefers a sibling `<name>.bat`.
    let folder = folder.as_os_str();
    let mut file = Vec::with_capacity(folder.len() + 2);
    file.extend(folder.encode_wide());
    if file.last() != Some(&SEPARATOR) {
        file.push(SEPARATOR);
    }
    file.push(0);

    let mut info = SHELLEXECUTEINFOW {
        cbSize: size_of::<SHELLEXECUTEINFOW>() as u32,
        // Synchronous and without message boxes, so the outcome is the return value.
        fMask: SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI,
        // "explore" only ever shows a folder, where "open" would run a file.
        lpVerb: windows_sys::w!("explore"),
        lpFile: file.as_ptr(),
        nShow: SW_SHOWNORMAL,
        ..Default::default()
    };
    // SAFETY: `info` is initialised, and its strings are NUL-terminated and outlive the call.
    if unsafe { ShellExecuteExW(&mut info) } == 0 { Err(io::Error::last_os_error()) } else { Ok(()) }
}

#[cfg(not(windows))]
fn launch(folder: &Path) -> io::Result<()> {
    use std::process::{Command, Stdio};

    let program = if cfg!(target_os = "macos") { "open" } else { "xdg-open" };
    let mut opener = Command::new(program)
        .arg(folder)
        .current_dir("/")
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()?;
    // Waited for on a thread: an unreaped child stays a zombie, and how long the opener runs is not ours to bound.
    std::thread::spawn(move || drop(opener.wait()));
    Ok(())
}
