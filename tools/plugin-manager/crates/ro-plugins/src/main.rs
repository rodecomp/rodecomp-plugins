//! ro-plugins: the rodecomp plugin manager. With a command it is the CLI that replaced scripts/plugins.py; without
//! one (or with `gui`) it opens the window.

mod cli;
mod gui;
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
            if let Err(error) = gui::run(args.client, args.build) {
                eprintln!("ro-plugins: {error}");
                std::process::exit(1);
            }
        }
        Some(command) => std::process::exit(cli::run(args, command)),
    }
}

/// The console subsystem keeps CLI output in the calling shell; started from Explorer the GUI drops the console
/// Windows created for it.
#[cfg(windows)]
fn hide_own_console() {
    use windows_sys::Win32::System::Console::{FreeConsole, GetConsoleProcessList};
    let mut processes = [0u32; 2];
    // SAFETY: the buffer is valid for its length.
    if unsafe { GetConsoleProcessList(processes.as_mut_ptr(), processes.len() as u32) } == 1 {
        // SAFETY: no console handles are in use yet.
        unsafe { FreeConsole() };
    }
}

#[cfg(not(windows))]
fn hide_own_console() {}
