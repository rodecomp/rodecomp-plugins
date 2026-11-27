//! ro-plugins: the rodecomp plugin manager. With a command it is the CLI that replaced scripts/plugins.py; without
//! one (or with `gui`) it opens the window.

// First: its t!/tn!/key! macros are used by the modules after it.
#[macro_use]
mod i18n;

mod cli;
mod elevate;
mod gui;
mod open;
mod paths;

use clap::Parser;

fn main() {
    let mut args = match cli::Args::try_parse() {
        Ok(args) => args,
        Err(error) => {
            let code = if error.use_stderr() { ro_plugins_core::ops::EXIT_USAGE } else { 0 };
            let _ = error.print();
            std::process::exit(code);
        }
    };
    match args.command.take() {
        Some(cli::Command::Gui) | None => {
            hide_own_console();
            if let Err(error) = gui::run(args.client, args.exe, args.build) {
                eprintln!("ro-plugins: {error}");
                std::process::exit(1);
            }
        }
        Some(command) => std::process::exit(cli::run(args, command)),
    }
}

/// The console subsystem keeps CLI output in the calling shell; started from Explorer the GUI drops the console
/// Windows created for it. FreeConsole closes the console's std handles but leaves their values in the process;
/// those values are later reused by unrelated kernel objects, and a child process that inherits stdio then fails
/// (ERROR_NOT_SUPPORTED), so they are cleared.
#[cfg(windows)]
fn hide_own_console() {
    use windows_sys::Win32::System::Console::{
        FreeConsole, GetConsoleProcessList, STD_ERROR_HANDLE, STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, SetStdHandle,
    };
    let mut processes = [0u32; 2];
    // SAFETY: the buffer is valid for its length.
    if unsafe { GetConsoleProcessList(processes.as_mut_ptr(), processes.len() as u32) } == 1 {
        // SAFETY: no console handles are in use yet.
        unsafe { FreeConsole() };
        for id in [STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE] {
            // SAFETY: a null std handle is valid and means "none".
            unsafe { SetStdHandle(id, std::ptr::null_mut()) };
        }
    }
}

#[cfg(not(windows))]
fn hide_own_console() {}
