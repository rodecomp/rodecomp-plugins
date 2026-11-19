//! setup / update / restore, run here or, when the client folder is not writable (e.g. under Program Files), in an
//! elevated copy of this exe: `ro-plugins elevated <op> --exe FILE [--build DIR] --grant SID --result FILE`, started
//! through ShellExecuteExW "runas" with a hidden window and waited for. The elevated run writes its exit code and
//! report lines to the result file; it grants the non-elevated user (by SID) modify rights on `<client>/plugins`, so
//! switches and settings never need elevation afterwards. Declining the UAC prompt changes nothing.

use std::path::{Path, PathBuf};

use ro_plugins_core::ops::{EXIT_REFUSED, EXIT_USAGE};
use ro_plugins_core::setup::{self, PluginSet};
use ro_plugins_core::status::Processes;

#[derive(Debug, Clone, Copy, PartialEq, Eq, clap::ValueEnum)]
pub enum Op {
    Setup,
    Update,
    Restore,
}

impl Op {
    pub fn name(self) -> &'static str {
        match self {
            Op::Setup => "setup",
            Op::Update => "update",
            Op::Restore => "restore",
        }
    }
}

#[derive(Debug, Clone)]
pub struct Request {
    pub op: Op,
    pub exe: PathBuf,
    /// The build output (`<repo>/build`); unused by restore.
    pub build: Option<PathBuf>,
}

/// What an operation reported: its exit code, report lines, and the refusal when it failed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Reply {
    pub code: i32,
    pub out: Vec<String>,
    pub error: Option<String>,
}

impl Reply {
    fn failed(code: i32, error: impl Into<String>) -> Reply {
        Reply { code, out: Vec::new(), error: Some(error.into()) }
    }
}

/// Runs `request` in this process; `grant` is the account given modify rights on the plugins folder.
pub fn run_here(request: &Request, grant: Option<&str>) -> Reply {
    let processes = Processes::new();
    let mut out = Vec::new();
    let set = match (request.op, &request.build) {
        (Op::Restore, _) => None,
        (_, Some(build)) => match PluginSet::find(build) {
            Ok(set) => Some(set),
            Err(refused) => return Reply::failed(refused.code, refused.message),
        },
        (_, None) => return Reply::failed(EXIT_USAGE, "no build output found; pass --build DIR"),
    };
    let outcome = match (request.op, &set) {
        (Op::Setup, Some(set)) => setup::setup(&request.exe, set, grant, &processes, &mut out),
        (Op::Update, Some(set)) => setup::update(&request.exe, set, grant, &processes, &mut out),
        _ => setup::restore(&request.exe, &processes, &mut out),
    };
    match outcome {
        Ok(code) => Reply { code, out, error: None },
        Err(refused) => Reply { code: refused.code, out, error: Some(refused.message) },
    }
}

/// Runs `request` here when the client folder is writable, else through `elevator` (the real one is [`elevated`];
/// tests pass a mock).
pub fn perform(request: &Request, elevator: impl FnOnce(&Request) -> Reply) -> Reply {
    if setup::needs_elevation(&request.exe) { elevator(request) } else { run_here(request, None) }
}

/// The result file of an elevated run: the exit code, then `o\t<line>` per report line and `e\t<error>`.
pub fn write_result(path: &Path, reply: &Reply) -> std::io::Result<()> {
    let mut text = format!("{}\n", reply.code);
    for line in &reply.out {
        text += &format!("o\t{line}\n");
    }
    if let Some(error) = &reply.error {
        text += &format!("e\t{error}\n");
    }
    std::fs::write(path, text)
}

fn read_result(path: &Path) -> Option<Reply> {
    let text = std::fs::read_to_string(path).ok()?;
    let mut lines = text.lines();
    let code = lines.next()?.trim().parse().ok()?;
    let mut reply = Reply { code, out: Vec::new(), error: None };
    for line in lines {
        match line.split_once('\t') {
            Some(("o", text)) => reply.out.push(text.to_owned()),
            Some(("e", text)) => reply.error = Some(text.to_owned()),
            _ => {}
        }
    }
    Some(reply)
}

/// One argument quoted for CommandLineToArgvW / the Rust runtime's parser.
fn quote(arg: &str) -> String {
    if !arg.is_empty() && !arg.contains([' ', '\t', '"']) {
        return arg.to_owned();
    }
    let mut quoted = String::from('"');
    let mut backslashes = 0;
    for c in arg.chars() {
        match c {
            '\\' => backslashes += 1,
            '"' => {
                quoted.extend(std::iter::repeat_n('\\', backslashes * 2 + 1));
                quoted.push('"');
                backslashes = 0;
            }
            _ => {
                quoted.extend(std::iter::repeat_n('\\', backslashes));
                quoted.push(c);
                backslashes = 0;
            }
        }
    }
    quoted.extend(std::iter::repeat_n('\\', backslashes * 2));
    quoted.push('"');
    quoted
}

/// The arguments of the elevated run.
pub fn elevated_args(request: &Request, sid: &str, result: &Path) -> String {
    let mut args = vec!["elevated".to_owned(), request.op.name().to_owned(), "--exe".to_owned(), request.exe.display().to_string()];
    if let Some(build) = &request.build {
        args.extend(["--build".to_owned(), build.display().to_string()]);
    }
    args.extend(["--grant".to_owned(), sid.to_owned(), "--result".to_owned(), result.display().to_string()]);
    args.iter().map(|arg| quote(arg)).collect::<Vec<_>>().join(" ")
}

/// The real elevator: starts this exe elevated (UAC prompt), waits, and reads its result file.
#[cfg(windows)]
pub fn elevated(request: &Request) -> Reply {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Foundation::{CloseHandle, ERROR_CANCELLED, GetLastError};
    use windows_sys::Win32::System::Threading::{GetExitCodeProcess, INFINITE, WaitForSingleObject};
    use windows_sys::Win32::UI::Shell::{SEE_MASK_NOASYNC, SEE_MASK_NOCLOSEPROCESS, SHELLEXECUTEINFOW, ShellExecuteExW};
    use windows_sys::Win32::UI::WindowsAndMessaging::SW_HIDE;

    let wide = |text: &std::ffi::OsStr| text.encode_wide().chain([0]).collect::<Vec<u16>>();
    let Ok(me) = std::env::current_exe() else { return Reply::failed(EXIT_REFUSED, "cannot find ro-plugins.exe to elevate") };
    let sid = match current_user_sid() {
        Ok(sid) => sid,
        Err(why) => return Reply::failed(EXIT_REFUSED, format!("cannot read the current user: {why}")),
    };
    let result = std::env::temp_dir().join(format!("ro-plugins-elevated-{}.txt", std::process::id()));
    let _ = std::fs::remove_file(&result);
    let file = wide(me.as_os_str());
    let parameters = wide(std::ffi::OsStr::new(&elevated_args(request, &sid, &result)));
    let mut info = SHELLEXECUTEINFOW {
        cbSize: size_of::<SHELLEXECUTEINFOW>() as u32,
        fMask: SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC,
        lpVerb: windows_sys::w!("runas"),
        lpFile: file.as_ptr(),
        lpParameters: parameters.as_ptr(),
        nShow: SW_HIDE,
        ..Default::default()
    };
    // SAFETY: `info` is initialised and its strings are NUL-terminated and outlive the call.
    if unsafe { ShellExecuteExW(&mut info) } == 0 {
        // SAFETY: no other call in between.
        let error = unsafe { GetLastError() };
        return if error == ERROR_CANCELLED {
            Reply::failed(EXIT_REFUSED, "administrator rights were declined; nothing was changed")
        } else {
            Reply::failed(EXIT_REFUSED, format!("could not start the elevated run: {}", std::io::Error::from_raw_os_error(error as i32)))
        };
    }
    let mut code = EXIT_REFUSED as u32;
    if !info.hProcess.is_null() {
        // SAFETY: hProcess is a process handle owned here (SEE_MASK_NOCLOSEPROCESS), closed once.
        unsafe {
            WaitForSingleObject(info.hProcess, INFINITE);
            GetExitCodeProcess(info.hProcess, &mut code);
            CloseHandle(info.hProcess);
        }
    }
    let reply = read_result(&result)
        .unwrap_or_else(|| Reply::failed(code as i32, format!("the elevated run ended with code {code} and no result")));
    let _ = std::fs::remove_file(&result);
    reply
}

#[cfg(not(windows))]
pub fn elevated(_request: &Request) -> Reply {
    Reply::failed(EXIT_REFUSED, "the client folder is not writable; run as a user who can write it")
}

/// The SID (`S-1-5-…`) of this process's user, passed to the elevated run so it grants the right account.
#[cfg(windows)]
pub fn current_user_sid() -> Result<String, String> {
    use windows_sys::Win32::Foundation::{CloseHandle, HANDLE, LocalFree};
    use windows_sys::Win32::Security::Authorization::ConvertSidToStringSidW;
    use windows_sys::Win32::Security::{GetTokenInformation, TOKEN_QUERY, TOKEN_USER, TokenUser};
    use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};

    let mut token: HANDLE = std::ptr::null_mut();
    // SAFETY: the pseudo-handle of this process; `token` receives a handle closed below.
    if unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) } == 0 {
        return Err(std::io::Error::last_os_error().to_string());
    }
    // TOKEN_USER plus the SID it points to; u64 for alignment.
    let mut buffer = [0u64; 64];
    let mut size = 0u32;
    // SAFETY: the buffer is valid for its byte length.
    let ok = unsafe { GetTokenInformation(token, TokenUser, buffer.as_mut_ptr().cast(), size_of_val(&buffer) as u32, &mut size) };
    let error = std::io::Error::last_os_error();
    // SAFETY: opened above.
    unsafe { CloseHandle(token) };
    if ok == 0 {
        return Err(error.to_string());
    }
    // SAFETY: GetTokenInformation filled a TOKEN_USER at the start of the buffer.
    let sid = unsafe { (*buffer.as_ptr().cast::<TOKEN_USER>()).User.Sid };
    let mut text: windows_sys::core::PWSTR = std::ptr::null_mut();
    // SAFETY: `sid` points into `buffer`; `text` is freed with LocalFree.
    if unsafe { ConvertSidToStringSidW(sid, &mut text) } == 0 {
        return Err(std::io::Error::last_os_error().to_string());
    }
    // SAFETY: a NUL-terminated string from ConvertSidToStringSidW.
    let sid = unsafe {
        let len = (0..).take_while(|&i| *text.add(i) != 0).count();
        let sid = String::from_utf16_lossy(std::slice::from_raw_parts(text, len));
        LocalFree(text.cast());
        sid
    };
    Ok(sid)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn quoting_round_trips() {
        assert_eq!(quote("plain"), "plain");
        assert_eq!(quote(r"C:\Program Files\RO\"), r#""C:\Program Files\RO\\""#);
        assert_eq!(quote(r#"a"b"#), r#""a\"b""#);
        assert_eq!(quote(""), r#""""#);
    }

    #[test]
    fn result_file_round_trips() {
        let path = std::env::temp_dir().join(format!("ro-plugins-result-test-{}.txt", std::process::id()));
        let reply = Reply { code: 1, out: vec!["a".into(), "b c".into()], error: Some("refused: x".into()) };
        write_result(&path, &reply).unwrap();
        assert_eq!(read_result(&path), Some(reply));
        let _ = std::fs::remove_file(&path);
    }

    /// A folder the current user may not write (an icacls deny entry) goes through the elevator; a writable one does
    /// not. The real UAC prompt is not exercised.
    #[cfg(windows)]
    #[test]
    fn a_read_only_client_asks_the_elevator() {
        use std::process::Command;
        let dir = std::env::temp_dir().join(format!("ro-plugins-elevate-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let exe = dir.join("Game.exe");
        std::fs::write(&exe, "MZ").unwrap();
        let request = Request { op: Op::Restore, exe: exe.clone(), build: None };
        let mut asked = false;
        let reply = perform(&request, |_| {
            asked = true;
            Reply::failed(9, "mock")
        });
        assert!(!asked, "writable: runs here");
        assert!(reply.error.unwrap().contains("no backup"));

        let sid = current_user_sid().unwrap();
        assert!(sid.starts_with("S-1-"));
        let deny = Command::new("icacls").arg(&dir).args(["/deny", &format!("*{sid}:(W)")]).output().unwrap();
        assert!(deny.status.success(), "{}", String::from_utf8_lossy(&deny.stdout));
        let reply = perform(&request, |forwarded| {
            asked = true;
            assert_eq!(forwarded.exe, exe);
            Reply::failed(9, "mock")
        });
        let _ = Command::new("icacls").arg(&dir).args(["/remove:d", &format!("*{sid}")]).output();
        let _ = std::fs::remove_dir_all(&dir);
        assert!(asked, "read-only: asks the elevator");
        assert_eq!(reply.code, 9);
    }
}
