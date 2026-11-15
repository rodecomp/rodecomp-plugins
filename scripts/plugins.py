"""Plugin manager for a client directory: what is installed, what the host loaded or not and why, and switches.

    python scripts/plugins.py list                   # installed plugins, recorded and predicted state
    python scripts/plugins.py info NAME              # manifest, dependents, state and reason
    python scripts/plugins.py disable NAME           # add NAME to plugins/disabled.txt (warns about dependents)
    python scripts/plugins.py enable NAME            # remove NAME and the dependencies it needs from disabled.txt
    python scripts/plugins.py install [NAME...]      # copy from build/plugins (scripts/build.py output), with deps
    python scripts/plugins.py uninstall NAME         # remove .dll .pdb .plugin; --force also removes dependents
Common option: --client DIR (default C:/Games/Ragnarok).

The host (Plugins/Host/Host.cpp) writes plugins/status.txt and reads plugins/disabled.txt; formats are in
Plugins/SDK/RoPlugin.h. `list` predicts each state offline by resolving dependencies exactly as the host does and
flags a recorded state that differs, and whether status.txt is stale (files changed since, or game exited).

Exit codes: 0 success, 1 refused or failed (dependency problem, unknown plugin), 2 usage error,
3 refused because the game is running from this client directory.
"""

import argparse
import ctypes
import ctypes.wintypes
import os
import shutil
import sys
from pathlib import Path

import semver

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CLIENT = Path("C:/Games/Ragnarok")
DEFAULT_BUILD = ROOT / "build" / "plugins"
EXIT_REFUSED, EXIT_RUNNING = 1, 3


class Refused(Exception):
    def __init__(self, message, code=EXIT_REFUSED):
        super().__init__(message)
        self.code = code


class Manifest:
    """An installed <Name>.plugin (the build writes the same file next to each DLL in build/plugins)."""

    def __init__(self, path: Path):
        self.path = path
        self.name, self.version, self.depends, self.error = None, None, [], None
        for number, line in enumerate(path.read_text(encoding="ascii", errors="replace").splitlines(), 1):
            fields = line.split(None, 2)
            if not fields:
                continue
            key, value, rest = fields[0], fields[1] if len(fields) > 1 else "", fields[2].strip() if len(fields) > 2 else ""
            try:
                if key == "name" and value and not rest:
                    self.name = value
                elif key == "version" and not rest:
                    self.version = semver.Version.parse(value)
                elif key == "depends" and value:
                    self.depends.append((value, rest, semver.parse_range(rest)))
                else:
                    raise semver.SemverError(f"not understood: {line}")
            except semver.SemverError as error:
                self.error = f"{path.name}: line {number}: {error}"
                return
        if not self.name or not self.version:
            self.error = f"{path.name}: needs a name and a version line"
        elif (self.name + ".plugin").lower() != path.name.lower():
            self.error = f"{path.name}: names plugin {self.name}; the file must be {self.name}.plugin"

    def text(self):
        return self.path.read_text(encoding="ascii", errors="replace")


def manifests(directory: Path):
    """Valid manifests by name and the error of each invalid one."""
    found, errors = {}, []
    for path in sorted(directory.glob("*.plugin")):
        manifest = Manifest(path)
        if manifest.error:
            errors.append(manifest.error)
        else:
            found[manifest.name] = manifest
    return found, errors


def read_disabled(plugins: Path):
    path = plugins / "disabled.txt"
    if not path.exists():
        return []
    return [line.split("#", 1)[0].split()[0] for line in path.read_text(encoding="utf-8").splitlines()
            if line.split("#", 1)[0].split()]


def write_disabled(plugins: Path, names):
    path = plugins / "disabled.txt"
    lines = [line for line in path.read_text(encoding="utf-8").splitlines() if line.lstrip().startswith("#")] \
        if path.exists() else ["# Plugins turned off: one name per line. Edited by scripts/plugins.py."]
    temporary = path.with_suffix(".tmp")
    temporary.write_text("\n".join(lines + sorted(set(names), key=str.lower)) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def predict(plugins: Path):
    """{name: (state, reason)} computed like Host.cpp RoHost_Start: manifests, refused DLLs, disabled.txt, then a
    depth-first visit in name order that disables on a missing, turned-off, disabled, out-of-range or cyclic dep."""
    found, _ = manifests(plugins)
    dlls = {p.stem.lower(): p.stem for p in plugins.glob("*.dll")}
    states = {}
    for stem in sorted(dlls.values()):
        if stem.lower() not in {name.lower() for name in found}:
            states[stem] = ("refused", f"no valid {stem}.plugin manifest next to it")
    off = {name.lower() for name in read_disabled(plugins)}
    state = {}
    reason = {}

    def disable(name, why, kind="disabled"):
        if state.get(name) in ("disabled", "failed-load", "off"):
            return
        state[name], reason[name] = kind, why

    for name in sorted(found):
        if name.lower() in off:
            state[name], reason[name] = "off", "turned off in plugins\\disabled.txt"
        elif name.lower() not in dlls:
            disable(name, f"{name}.dll is missing", "failed-load")
    stack = []

    def visit(name):
        current = state.get(name, "pending")
        if current == "visiting":
            members = stack[stack.index(name):]
            chain = " -> ".join(members + [name])
            for member in members:
                disable(member, f"in dependency cycle {chain}")
            return
        if current != "pending":
            return
        state[name] = "visiting"
        stack.append(name)
        for dependency, text, sets in found[name].depends:
            if dependency not in found:
                disable(name, f"needs {dependency} {text}, not installed")
                continue
            visit(dependency)
            target = state[dependency]
            if target == "off":
                disable(name, f"needs {dependency}, which is turned off")
            elif target in ("disabled", "failed-load"):
                disable(name, f"needs {dependency}, which is disabled")
            elif not semver.satisfies(found[dependency].version, sets):
                disable(name, f"needs {dependency} {text}, found {found[dependency].version}")
        stack.pop()
        if state[name] == "visiting":
            state[name] = "loaded"  # resolved; the host loads it unless LoadLibrary or RoPlugin_Init fails

    for name in sorted(found):
        visit(name)
    states.update({name: (state[name], reason.get(name, "")) for name in found})
    return states


class Status:
    """plugins/status.txt as written by the host, or empty."""

    def __init__(self, plugins: Path):
        self.path = plugins / "status.txt"
        self.header, self.plugins = {}, {}
        if not self.path.exists():
            return
        lines = self.path.read_text(encoding="utf-8", errors="replace").splitlines()
        if lines and lines[0].startswith("RoPluginHost\t"):
            fields = lines[0].split("\t")
            self.header["host"] = fields[1]
            for field in fields[2:]:
                key, _, value = field.partition(" ")
                self.header[key] = value
        for line in lines[1:]:
            fields = (line.split("\t") + [""] * 5)[:5]
            self.plugins[fields[0]] = {"version": fields[1], "state": fields[2], "reason": fields[3],
                                       "resolved": fields[4]}

    def mtime(self):
        return self.path.stat().st_mtime if self.path.exists() else None


# Processes, through the Win32 API (stdlib ctypes): the game holds its plugin DLLs open, so mutating commands refuse.
_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_psapi = ctypes.WinDLL("psapi", use_last_error=True)
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


def _image(pid):
    handle = _kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return None
    try:
        buffer = ctypes.create_unicode_buffer(32768)
        size = ctypes.wintypes.DWORD(len(buffer))
        if not _kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            return None
        exit_code = ctypes.wintypes.DWORD()
        if _kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code)) and exit_code.value != 259:  # STILL_ACTIVE
            return None
        return Path(buffer.value)
    finally:
        _kernel32.CloseHandle(handle)


def _pids():
    count = 4096
    while True:
        array = (ctypes.wintypes.DWORD * count)()
        needed = ctypes.wintypes.DWORD()
        _psapi.EnumProcesses(array, ctypes.sizeof(array), ctypes.byref(needed))
        if needed.value < ctypes.sizeof(array):
            return list(array[:needed.value // ctypes.sizeof(ctypes.wintypes.DWORD)])
        count *= 2


def _same(a: Path, b: Path):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def game_processes(client: Path, status: Status):
    """(pid, image) of every running exe in `client` that is the exe recorded in status.txt or Ragexe*.exe."""
    names = {"ragexe.exe"}
    if status.header.get("exe"):
        names.add(status.header["exe"].lower())
    running = []
    for pid in _pids():
        image = _image(pid) if pid else None
        if image and _same(image.parent, client) and (image.name.lower() in names or image.name.lower().startswith("ragexe")):
            running.append((pid, image))
    return running


def refuse_while_running(client: Path, status: Status, action):
    running = game_processes(client, status)
    if running:
        listing = ", ".join(f"{image.name} (pid {pid})" for pid, image in running)
        raise Refused(f"refused: cannot {action} while the game is running from {client}: {listing}. "
                      "Close the game first; it keeps the plugin DLLs loaded and reads disabled.txt only at start.",
                      EXIT_RUNNING)


def staleness(client: Path, plugins: Path, status: Status):
    """Why status.txt may not describe the installed files any more, or an empty list."""
    if not status.path.exists():
        return ["no status.txt yet: the game has not run with this host"]
    notes = []
    written = status.mtime()
    # Copies keep the build's mtime (shutil.copy2), so the creation time (st_ctime on Windows) counts too.
    newer = sorted(p.name for pattern in ("*.plugin", "*.dll", "disabled.txt") for p in plugins.glob(pattern)
                   if max(p.stat().st_mtime, p.stat().st_ctime) > written)
    if newer:
        notes.append("changed since it was written: " + ", ".join(newer))
    pid = int(status.header.get("pid", "0") or 0)
    image = _image(pid) if pid else None
    if image and _same(image.parent, client):
        notes.insert(0, f"live: written by {image.name} pid {pid}, still running")
    else:
        notes.insert(0, f"game pid {pid or '?'} has exited; states are from its run started {status.header.get('started', '?')}")
    return notes


def dependents(found, name):
    """Installed plugins that depend on `name`, directly or not, nearest first."""
    result, frontier = [], [name]
    while frontier:
        current = frontier.pop(0)
        for other in sorted(found):
            if other not in result and other != name and any(d == current for d, _, _ in found[other].depends):
                result.append(other)
                frontier.append(other)
    return result


def table(rows):
    widths = [max(len(str(row[i])) for row in rows) for i in range(len(rows[0]))]
    for row in rows:
        print("  ".join(str(cell).ljust(width) for cell, width in zip(row, widths)).rstrip())


def command_list(args, client, plugins):
    found, errors = manifests(plugins)
    status = Status(plugins)
    predicted = predict(plugins)
    header = status.header
    if header:
        print(f"host {header.get('host')}, api {header.get('api')}, exe {header.get('exe', '?')}, "
              f"pid {header.get('pid')}, started {header.get('started')}")
    for note in staleness(client, plugins, status):
        print(f"status.txt: {note}")
    rows = [("NAME", "VERSION", "DEPENDS", "RECORDED", "PREDICTED", "REASON")]
    mismatches = 0
    for name in sorted(set(found) | set(predicted), key=str.lower):
        manifest = found.get(name)
        depends = ", ".join(f"{d} {t}" for d, t, _ in manifest.depends) if manifest else "-"
        recorded = status.plugins.get(name, {})
        state, reason = predicted[name]
        flag = ""
        if recorded and recorded["state"] != state:
            flag = "  <- MISMATCH"
            mismatches += 1
        rows.append((name, manifest.version if manifest else "-", depends or "-", recorded.get("state", "-"),
                     state + flag, recorded.get("reason") or reason))
    table(rows)
    for error in errors:
        print(f"invalid manifest: {error}")
    if mismatches:
        print(f"{mismatches} recorded state(s) differ from the prediction: restart the game to apply the installed "
              "files and disabled.txt, or see RoPluginHost.log for load/init failures.")
    return 0


def command_info(args, client, plugins):
    found, _ = manifests(plugins)
    name = canonical(found, args.name)
    manifest = found[name]
    status = Status(plugins)
    recorded = status.plugins.get(name)
    state, reason = predict(plugins)[name]
    print(f"{name} {manifest.version}")
    print(f"manifest {manifest.path}:")
    for line in manifest.text().splitlines():
        print(f"    {line}")
    print(f"files: {', '.join(p.name for p in sorted(plugins.glob(name + '.*')) if p.suffix in ('.dll', '.pdb', '.plugin'))}")
    print(f"dependents: {', '.join(dependents(found, name)) or '(none)'}")
    print(f"turned off in disabled.txt: {'yes' if name.lower() in map(str.lower, read_disabled(plugins)) else 'no'}")
    if recorded:
        print(f"recorded state: {recorded['state']}" + (f" ({recorded['reason']})" if recorded["reason"] else ""))
        if recorded["resolved"]:
            print(f"resolved dependencies: {recorded['resolved'].replace(',', ', ').replace('=', ' ')}")
    else:
        print("recorded state: - (not in status.txt)")
    print(f"predicted state: {state}" + (f" ({reason})" if reason else ""))
    for note in staleness(client, plugins, status):
        print(f"status.txt: {note}")
    return 0


def canonical(found, name):
    for candidate in found:
        if candidate.lower() == name.lower():
            return candidate
    raise Refused(f"{name}: not installed (installed: {', '.join(sorted(found)) or 'none'})")


def command_disable(args, client, plugins):
    found, _ = manifests(plugins)
    name = canonical(found, args.name)
    refuse_while_running(client, Status(plugins), "change disabled.txt")
    disabled = read_disabled(plugins)
    users = dependents(found, name)
    if users:
        print(f"warning: {', '.join(users)} depend{'s' if len(users) == 1 else ''} on {name} and will be disabled "
              f"(needs {name}, which is turned off)")
    if name.lower() in map(str.lower, disabled):
        print(f"{name} is already turned off")
        return 0
    write_disabled(plugins, disabled + [name])
    print(f"{name} turned off in {plugins / 'disabled.txt'}; takes effect at the next game start")
    return 0


def command_enable(args, client, plugins):
    found, _ = manifests(plugins)
    name = canonical(found, args.name)
    refuse_while_running(client, Status(plugins), "change disabled.txt")
    needed, stack = [], [name]
    while stack:
        current = stack.pop()
        if current not in needed:
            needed.append(current)
            stack += [d for d, _, _ in found[current].depends if d in found]
    disabled = read_disabled(plugins)
    enabled = [n for n in needed if n.lower() in map(str.lower, disabled)]
    if not enabled:
        print(f"{name} is not turned off")
    else:
        write_disabled(plugins, [n for n in disabled if n.lower() not in {e.lower() for e in enabled}])
        for other in enabled:
            print(f"{other} turned on" + ("" if other == name else f" (needed by {name})"))
    state, reason = predict(plugins)[name]
    print(f"{name} at the next game start: {state}" + (f" ({reason})" if reason else ""))
    return 0 if state == "loaded" else EXIT_REFUSED


def command_install(args, client, plugins):
    source = Path(args.build)
    available, errors = manifests(source)
    for error in errors:
        print(f"build: invalid manifest: {error}")
    if not available:
        raise Refused(f"no plugins in {source}; run scripts/build.py first")
    wanted = [canonical(available, n) for n in args.names] if args.names else sorted(available)
    refuse_while_running(client, Status(plugins), "install plugins")
    installed, _ = manifests(plugins)

    # Missing dependencies come from build/ too.
    order = []
    stack = list(reversed(wanted))
    while stack:
        current = stack.pop()
        if current in order:
            continue
        order.append(current)
        for dependency, _, _ in available[current].depends:
            if dependency not in installed and dependency not in order:
                if dependency not in available:
                    continue  # reported below as unsatisfied
                print(f"{dependency}: also installing, needed by {current}")
                stack.append(dependency)

    after = dict(installed)
    after.update({n: available[n] for n in order})
    problems = []
    for name in sorted(after):
        for dependency, text, sets in after[name].depends:
            if name not in order and dependency not in order:
                continue  # unchanged by this install
            if dependency not in after:
                problems.append(f"{name} {after[name].version} needs {dependency} {text}, which is neither installed nor built")
            elif not semver.satisfies(after[dependency].version, sets):
                problems.append(f"{name} {after[name].version} needs {dependency} {text}, would have {after[dependency].version}")
    if problems:
        for problem in problems:
            print(f"{'warning' if args.force else 'incompatible'}: {problem}")
        if not args.force:
            raise Refused("refused: install would leave unsatisfied dependencies (use --force to install anyway)")

    plugins.mkdir(parents=True, exist_ok=True)
    for name in order:
        dll = source / f"{name}.dll"
        if not dll.exists():
            raise Refused(f"{name}: {dll} is missing; rebuild with scripts/build.py")
        for path in (dll, dll.with_suffix(".pdb"), dll.with_suffix(".plugin")):
            if path.exists():
                shutil.copy2(path, plugins / path.name)
        previous = installed.get(name)
        change = f"{previous.version} -> {available[name].version}" if previous else f"{available[name].version} (new)"
        print(f"installed {name} {change}")
    return 0


def command_uninstall(args, client, plugins):
    found, _ = manifests(plugins)
    name = canonical(found, args.name)
    users = dependents(found, name)
    if users and not args.force:
        raise Refused(f"refused: {', '.join(users)} depend{'s' if len(users) == 1 else ''} on {name}; uninstall "
                      f"{'it' if len(users) == 1 else 'them'} first or use --force to remove {name} and its dependents")
    refuse_while_running(client, Status(plugins), "uninstall plugins")
    if users:
        print(f"--force: also removing dependents {', '.join(users)}")
    disabled = read_disabled(plugins)
    for target in [*reversed(users), name]:
        removed = [p.name for suffix in (".dll", ".pdb", ".plugin") for p in [plugins / f"{target}{suffix}"] if p.exists()]
        for file in removed:
            (plugins / file).unlink()
        print(f"uninstalled {target} {found[target].version}: removed {', '.join(removed)}")
    left = [n for n in disabled if n.lower() not in {t.lower() for t in [name, *users]}]
    if len(left) != len(disabled):
        write_disabled(plugins, left)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--client", default=str(DEFAULT_CLIENT), help=f"client directory (default {DEFAULT_CLIENT})")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="installed plugins with recorded and predicted state")
    for command, text in (("info", "manifest, dependents, state and reason"), ("disable", "turn a plugin off"),
                          ("enable", "turn a plugin and its needed dependencies on")):
        commands.add_parser(command, help=text).add_argument("name")
    install = commands.add_parser("install", help="copy plugins (and missing dependencies) from the build output")
    install.add_argument("names", nargs="*", metavar="NAME", help="default: every built plugin")
    install.add_argument("--build", default=str(DEFAULT_BUILD), help=f"build output (default {DEFAULT_BUILD})")
    install.add_argument("--force", action="store_true", help="install despite unsatisfied dependencies")
    uninstall = commands.add_parser("uninstall", help="remove a plugin's .dll, .pdb and .plugin")
    uninstall.add_argument("name")
    uninstall.add_argument("--force", action="store_true", help="also uninstall the plugins that depend on it")
    args = parser.parse_args()

    client = Path(args.client)
    if not client.is_dir():
        parser.error(f"--client {client}: not a directory")
    handler = {"list": command_list, "info": command_info, "disable": command_disable, "enable": command_enable,
               "install": command_install, "uninstall": command_uninstall}[args.command]
    try:
        return handler(args, client, client / "plugins")
    except Refused as refused:
        print(refused, file=sys.stderr)
        return refused.code


if __name__ == "__main__":
    sys.exit(main())
