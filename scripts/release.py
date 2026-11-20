"""Package a release: one .zip with everything a player needs to add plugins to their Ragnarok client.

    python scripts/release.py [--version 2026.11.20] [--out dist]

It takes the current build output (run scripts/build.py first) and the release build of the plugin manager, and
writes dist/rodecomp-plugins-<version>.zip:

    rodecomp-plugins-<version>/
        RO Plugins.exe              the plugin manager (GUI; also the CLI with a command)
        README.txt                  how to use it
        plugins-build/              what the manager installs, in the layout of build/:
            exe/Ragexe.exe          the game exe with the plugin loader
            RoPluginHost.dll
            plugins/<Name>.dll, <Name>.toml

The manager finds plugins-build/ next to itself. Debug files (.pdb), import libraries and the build's
intermediate files are left out.
"""

import argparse
import datetime
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
MANAGER = ROOT / "tools" / "plugin-manager" / "target" / "release" / "ro-plugins.exe"

README = """RO Plugins {version}
====================

Plugins for Ragnarok Online, and the program that manages them.

Requirements
------------
- Windows 10 or 11.
- A Ragnarok Online client folder (the one with data.grf).
- Microsoft Visual C++ 2015-2022 Redistributable, x86 AND x64:
  https://aka.ms/vs/17/release/vc_redist.x86.exe
  https://aka.ms/vs/17/release/vc_redist.x64.exe

Getting started
---------------
1. Unzip this folder anywhere. Keep "RO Plugins.exe" and "plugins-build" together.
2. Start "RO Plugins.exe" and choose your game's .exe (for example Ragexe.exe in your client folder).
3. Click "Turn on plugins". Your original game is kept as a backup (Ragexe.original.exe).
   The recommended plugins are turned on; turn on the others you want in the list.
4. Click Run.

Going back
----------
Settings menu (gear, top right) > "Go back to the original game". Nothing is deleted.

Updating
--------
Unzip a newer release, start its "RO Plugins.exe", and click "Update available" at the top.

Built from rodecomp-plugins {commit} ({date}).
"""


def git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True, check=True).stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", default=datetime.date.today().strftime("%Y.%m.%d"))
    parser.add_argument("--out", default=str(ROOT / "dist"))
    args = parser.parse_args()

    exe, host, plugins = BUILD / "exe" / "Ragexe.exe", BUILD / "RoPluginHost.dll", BUILD / "plugins"
    for path in (exe, host, plugins, MANAGER):
        if not path.exists():
            sys.exit(f"missing {path}: run scripts/build.py and `cargo build --release` in tools/plugin-manager first")
    if git("status", "--porcelain"):
        sys.exit("the working tree has uncommitted changes: commit them so the release matches a commit")

    files = [(MANAGER, "RO Plugins.exe"), (exe, "plugins-build/exe/Ragexe.exe"), (host, "plugins-build/RoPluginHost.dll")]
    for toml in sorted(plugins.glob("*.toml")):
        dll = toml.with_suffix(".dll")
        if not dll.is_file():
            sys.exit(f"{toml.name} has no {dll.name}: rebuild the plugins")
        files += [(dll, f"plugins-build/plugins/{dll.name}"), (toml, f"plugins-build/plugins/{toml.name}")]

    commit = git("rev-parse", "--short", "HEAD")
    readme = README.format(version=args.version, commit=commit, date=git("log", "-1", "--format=%cs"))
    name = f"rodecomp-plugins-{args.version}"
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    archive = out / f"{name}.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zip_file:
        zip_file.writestr(f"{name}/README.txt", readme.replace("\n", "\r\n"))
        for source, target in files:
            zip_file.write(source, f"{name}/{target}")
    plugin_count = len(list(plugins.glob("*.toml")))
    print(f"{archive} ({archive.stat().st_size / 1e6:.1f} MB): manager, plugin exe, host and {plugin_count} plugins, commit {commit}")


if __name__ == "__main__":
    main()
