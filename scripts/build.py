"""Build Ragexe.exe from the rodecomp submodule with a native plugin loader, then RoPluginHost.dll and every plugin.

rodecomp/ is never modified:
  1. rodecomp/scripts/exe/build_exe.py compiles and links the exe in rodecomp/build/exe (skip with --no-exe).
  2. scripts/plugin_exe.py links rodecomp's objects again into build/exe with loader/RoLoaderEntry.cpp as entry
     point, an export table (/DEF) and a fixed base, then runs rodecomp's post-link patches on it. It writes
     Ragexe.lib (exe functions for plugins) and Ragexe.globals (exe globals at their fixed address).
  3. RoPluginHost.dll and every plugin Plugins/<Category>/<Name>/ (*.cpp plus plugin.toml) are compiled with the exe's own
     flags (data/compile.json shape 0) plus Plugins/SDK, so they share the exe's struct layouts, STL and CRT
     (msvcp140/vcruntime140, one heap). Each plugin *.rc is compiled with rc.exe to a .res linked into its DLL.

Every plugin lives in a category directory and has Plugins/<Category>/<Name>/plugin.toml (build/plugin-toml-spec.md):
    name = "MyEffects"
    category = "Graphics"
    version = "0.1.0"
    libs = ["d3d12.lib"]
    [dependencies]
    RendererDX12 = "^1.1.0"
    [settings.Strength]
    type = "float"
    default = 0.5
name equals the directory, category equals the parent directory (required), version is semver, dependencies map
plugin names to npm-style ranges (scripts/semver.py), settings are validated by scripts/plugin_toml.py. Names are
unique across categories. Dependencies are resolved against the local plugins (missing, out-of-range and cyclic
ones fail the build) and plugins build in dependency order. A plugin compiles with /D<NAME>_EXPORTS (upper case) for
its API macro and with /I on each dependency's directory, and links each dependency's import library. Next to each
DLL goes plugins/<Name>.toml, the file the host reads: plugin.toml with every range normalised to comparator sets
(`||`-separated, space-joined `op version`, e.g. ">=1.1.0 <2.0.0-0"). --install merges it with the client's
installed plugins/<Name>.toml: each user `value` still valid under the new declaration is kept, the others are
dropped and printed; a leftover <Name>.plugin of the old format is deleted.

Change detection: the exe build writes build/exe/Ragexe.relsig (scripts/relsig.py), and each plugin's <Name>.toml gets
an [exe] table with the relsig of every exe function the plugin uses (the exe symbols its objects reference, plus the
decorated names its sources pass as string literals to ro::HookVirtual / Resolve / GetProcAddress). The host and the
plugin manager compare them with the installed exe's Ragexe.relsig and warn about changed or missing functions.

    python scripts/build.py                       # everything into build/
    python scripts/build.py --no-exe              # reuse rodecomp/build/exe/Ragexe.exe
    python scripts/build.py --install DIR         # also copy exe, host and plugins into DIR
    python scripts/build.py --install DIR --exe-name Ragexe.plugins.exe   # install the exe under another name
    python scripts/build.py --plugin NAME         # only NAME and its dependencies (plus exe and host)
"""

import argparse
import json
import shutil
import struct
import subprocess
import sys
import tomllib
from pathlib import Path

import plugin_exe
import plugin_toml
import relsig
import semver

ROOT = Path(__file__).resolve().parent.parent
RODECOMP = ROOT / "rodecomp"
sys.path.insert(0, str(RODECOMP / "scripts" / "exe"))
import build_exe  # noqa: E402
import postprocess  # noqa: E402

PLUGINS = ROOT / "Plugins"
LOADER = ROOT / "loader" / "RoLoaderEntry.cpp"
HOST_DIR = PLUGINS / "Host"
SDK_DIR = PLUGINS / "SDK"
IMAGE_SYM_ABSOLUTE = 0xFFFF
IMAGE_SYM_CLASS_EXTERNAL = 2


def undefined_symbols(tc, objects):
    """Names the objects reference without defining (dumpbin /symbols: UNDEF External)."""
    result = tc.run("dumpbin.exe", ["/nologo", "/symbols", *map(tc.path, objects)])
    if result.returncode != 0:
        sys.exit(result.stdout)
    return {line.split("| ", 1)[1].split(" ", 1)[0] for line in result.stdout.splitlines()
            if " UNDEF " in line and " External " in line and "| " in line}


def globals_object(globals_: dict, out: Path):
    """A COFF object defining exe globals as absolute symbols at their fixed VA (the exe is not relocatable once
    patched). The linker resolves plugin references to them without a base relocation. Only the globals a plugin
    references go in: link.exe /DEBUG:FULL hangs on some unreferenced absolute symbols (e.g. ?Rect@@3UtagRECT@@A)."""
    entries = sorted(globals_.items())
    strings = bytearray()
    symbols = bytearray()
    for name, value in entries:
        encoded = name.encode("ascii")
        if len(encoded) <= 8:
            field = encoded.ljust(8, b"\0")
        else:
            field = struct.pack("<II", 0, 4 + len(strings))
            strings += encoded + b"\0"
        symbols += field + struct.pack("<IHHBB", value, IMAGE_SYM_ABSOLUTE, 0, IMAGE_SYM_CLASS_EXTERNAL, 0)
    header = struct.pack("<HHIIIHH", 0x14C, 0, 0, 20, len(entries), 0, 0)
    out.write_bytes(header + symbols + struct.pack("<I", 4 + len(strings)) + strings)


def compile_flags(tc, config):
    flags = [tc.expand(a) for a in config["shapes"][0]]
    return [*flags, f"/I{tc.path(SDK_DIR)}", "/Z7", "/W3", "/wd4273"]


def compile_sources(tc, env, flags, sources, obj_dir):
    obj_dir.mkdir(parents=True, exist_ok=True)
    objects = []
    for source in sources:
        obj = obj_dir / (source.stem + ".obj")
        result = tc.run("cl.exe", [*flags, f"/Fo{tc.path(obj)}", tc.path(source)], env=env, cwd=RODECOMP)
        output = "\n".join(line for line in result.stdout.splitlines() if line.strip() and line.strip() != source.name)
        if output:
            print(output)
        if result.returncode != 0:
            sys.exit(f"compile failed: {source}")
        objects.append(obj)
    return objects


def link_dll(tc, objects, libs, out: Path):
    """/DEBUG:FASTLINK: link.exe's /DEBUG:FULL PDB writer hangs on the absolute symbols of RagexeGlobals.obj. A
    fastlink PDB references the debug info in the plugin's objects (build/obj), which debuggers read from there."""
    out.parent.mkdir(parents=True, exist_ok=True)
    args = ["/nologo", "/DLL", "/MACHINE:X86", "/DEBUG:FASTLINK", f"/PDB:{tc.path(out.with_suffix('.pdb'))}",
            "/INCREMENTAL:NO", "/OPT:REF", f"/OUT:{tc.path(out)}", *map(tc.path, objects), *libs]
    result = tc.run("link.exe", args)
    if result.stdout.strip():
        print(result.stdout.strip())
    if result.returncode != 0:
        sys.exit(f"link failed: {out.name}")
    print(f"built {out}")


def find_rc(tc) -> Path:
    """rc.exe of the toolchain's Windows SDK, else the newest x86 rc.exe of the installed Windows Kits."""
    candidate = tc.kits / "bin" / build_exe.SDK_VERSION / "x86" / "rc.exe"
    if candidate.exists():
        return candidate
    found = [rc for root in build_exe._windows_kits_roots() for rc in root.glob("bin/*/x86/rc.exe")]
    if not found:
        sys.exit(f"no rc.exe: not at {candidate} nor in any installed Windows Kits bin/*/x86 (install a Windows SDK)")
    return max(found, key=lambda rc: tuple(int(part) for part in rc.parent.parent.name.split(".") if part.isdigit()))


def compile_resources(tc, resources, obj_dir):
    """Each .rc runs from its own directory so relative file names (icon.ico) resolve. tc.run only runs tc.bin."""
    if not resources:
        return []
    rc = find_rc(tc)
    obj_dir.mkdir(parents=True, exist_ok=True)
    compiled = []
    for resource in resources:
        res = obj_dir / (resource.stem + ".res")
        command = [str(rc), "/nologo", "/fo", tc.path(res), tc.path(resource)]
        if tc.kind != "native":
            command = ["wine", *command]
        result = subprocess.run(command, env=tc.env, cwd=resource.parent, capture_output=True, text=True, errors="replace")
        if result.returncode != 0:
            sys.exit(f"rc failed: {resource}\n{result.stdout}{result.stderr}")
        compiled.append(res)
    return compiled


class Plugin:
    def __init__(self, directory: Path, data: dict, version, ranges: dict):
        self.directory = directory
        self.name = directory.name
        self.category = data["category"]
        self.data = data  # the plugin.toml document
        self.version = version
        self.ranges = ranges  # name -> range as written
        self.dependencies = {name: semver.parse_range(text) for name, text in ranges.items()}
        self.libs = data.get("libs", [])
        self.conflicts = sorted(set(data.get("conflicts", [])))


def load_manifest(directory: Path) -> Plugin:
    path = directory / "plugin.toml"
    where = path.relative_to(ROOT)
    if not path.exists():
        sys.exit(f"{where}: missing (every plugin needs a plugin.toml)")
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as error:
        sys.exit(f"{where}: {error}")
    unknown = data.keys() - {"name", "category", "version", "description", "dependencies", "conflicts", "libs", "settings"}
    if unknown:
        sys.exit(f"{where}: unknown keys {sorted(unknown)}")
    if data.get("name") != directory.name:
        sys.exit(f"{where}: name {data.get('name')!r} must equal the directory name {directory.name!r}")
    if data.get("category") != directory.parent.name:
        sys.exit(f"{where}: category {data.get('category')!r} must equal the parent directory name {directory.parent.name!r}")
    if not isinstance(data.get("description", ""), str):
        sys.exit(f"{where}: description must be a string")
    dependencies = data.get("dependencies", {})
    libs = data.get("libs", [])
    conflicts = data.get("conflicts", [])
    if not isinstance(dependencies, dict) or not all(isinstance(text, str) for text in dependencies.values()):
        sys.exit(f"{where}: dependencies must map plugin names to version ranges")
    if not isinstance(libs, list) or not all(isinstance(lib, str) for lib in libs):
        sys.exit(f"{where}: libs must be a list of strings")
    if not isinstance(conflicts, list) or not all(isinstance(name, str) and name and " " not in name for name in conflicts):
        sys.exit(f"{where}: conflicts must be a list of plugin names")
    if set(conflicts) & dependencies.keys():
        sys.exit(f"{where}: {sorted(set(conflicts) & dependencies.keys())} both a dependency and a conflict")
    settings = data.get("settings", {})
    if not isinstance(settings, dict):
        sys.exit(f"{where}: settings must be a table of [settings.<key>] tables")
    for key, declaration in settings.items():
        why = plugin_toml.check_declaration(declaration, allow_value=False)
        if why:
            sys.exit(f"{where}: setting {key}: {why}")
    version = data.get("version")
    try:
        return Plugin(directory, data, semver.Version.parse(version if isinstance(version, str) else repr(version)),
                      dependencies)
    except semver.SemverError as error:
        sys.exit(f"{where}: {error}")


def resolve(plugins: dict, wanted: list) -> list:
    """`wanted` and their dependencies in dependency order; exits on a missing, out-of-range or cyclic dependency."""
    for plugin in plugins.values():
        for name, sets in plugin.dependencies.items():
            needs = f"{plugin.name} {plugin.version} needs {name} {plugin.ranges[name]}"
            if name not in plugins:
                sys.exit(f"dependency error: {needs}, which is not a plugin in Plugins/")
            found = plugins[name].version
            if not semver.satisfies(found, sets):
                sys.exit(f"dependency error: {needs} ({semver.normalise(sets)}), found {found}")
    order, done, path = [], set(), []

    def visit(name):
        if name in path:
            cycle = path[path.index(name):] + [name]
            sys.exit(f"dependency error: cycle {' -> '.join(cycle)}")
        if name in done:
            return
        path.append(name)
        for dependency in sorted(plugins[name].dependencies):
            visit(dependency)
        path.pop()
        done.add(name)
        order.append(plugins[name])

    # Every plugin is checked for cycles, even under --plugin, so a broken tree never builds partially.
    for name in sorted(plugins):
        visit(name)
    needed, stack = set(), list(wanted)
    while stack:
        name = stack.pop()
        if name not in needed:
            needed.add(name)
            stack += plugins[name].dependencies
    return [plugin for plugin in order if plugin.name in needed]


def installed_manifest(plugin: Plugin, exe_functions: dict) -> dict:
    """build/plugins/<Name>.toml: plugin.toml with normalised ranges, the settings in source order (no values) and the
    [exe] relsigs (`relsig-id` first) of the exe functions it uses."""
    data = plugin.data
    document = {"name": plugin.name, "category": plugin.category, "version": str(plugin.version)}
    for key in ("description", "conflicts", "libs"):
        if key in data:
            document[key] = data[key]
    document["dependencies"] = {name: semver.normalise(plugin.dependencies[name]) for name in plugin.ranges}
    document["settings"] = data.get("settings", {})
    document["exe"] = exe_functions
    return document


def merge_installed(built: Path, installed: Path) -> str:
    """The build's <Name>.toml plus each `value` of the old installed copy still valid under the new declaration;
    prints the values kept and dropped."""
    document = tomllib.loads(built.read_text(encoding="utf-8"))
    name = document["name"]
    old = {}
    if installed.exists():
        try:
            old = tomllib.loads(installed.read_text(encoding="utf-8")).get("settings", {})
        except tomllib.TOMLDecodeError as error:
            print(f"{name}: old {installed.name} is not valid TOML ({error}); its values are dropped")
        if not isinstance(old, dict):
            old = {}
    settings = document.get("settings", {})
    for key, declaration in old.items():
        if not isinstance(declaration, dict) or "value" not in declaration:
            continue
        value = declaration["value"]
        if key not in settings:
            print(f"{name}: dropped value of setting {key}: the setting no longer exists")
            continue
        why = plugin_toml.check_value(settings[key], value)
        if why:
            print(f"{name}: dropped value of setting {key}: {why} under the new declaration")
            continue
        settings[key]["value"] = value
        print(f"{name}: kept value of setting {key}")
    return plugin_toml.dumps(document)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--no-exe", action="store_true", help="reuse rodecomp/build/exe instead of rebuilding it")
    parser.add_argument("--out", default=str(ROOT / "build"), help="output directory")
    parser.add_argument("--install", help="client directory to copy Ragexe.exe, RoPluginHost.dll and plugins/ into")
    parser.add_argument("--toolchain", help="toolchain directory (td layout); default: RODECOMP_TOOLCHAIN, ...")
    parser.add_argument("--plugin", help="build only this plugin and its dependencies")
    parser.add_argument("--exe-name", default="Ragexe.exe",
                        help="file name of the installed exe (default Ragexe.exe); plugins bind to any name")
    args = parser.parse_args()

    plugins = {}
    for category in sorted(d for d in PLUGINS.iterdir() if d.is_dir() and d not in (HOST_DIR, SDK_DIR)):
        if (category / "plugin.toml").exists():
            sys.exit(f"{category.relative_to(ROOT)}: a plugin needs a category: move it to Plugins/<Category>/{category.name}/")
        for directory in sorted(d for d in category.iterdir() if d.is_dir()):
            plugin = load_manifest(directory)
            if plugin.name in plugins:
                sys.exit(f"{directory.relative_to(ROOT)}: plugin name {plugin.name} is also used by "
                         f"{plugins[plugin.name].directory.relative_to(ROOT)}")
            plugins[plugin.name] = plugin
    if args.plugin and args.plugin not in plugins:
        sys.exit(f"--plugin {args.plugin}: no such plugin (have {', '.join(sorted(plugins))})")
    order = resolve(plugins, [args.plugin] if args.plugin else list(plugins))
    print("plugins: " + ", ".join(f"{p.name} {p.version}" for p in order), flush=True)

    out = Path(args.out)
    if not args.no_exe:
        command = [sys.executable, str(RODECOMP / "scripts" / "exe" / "build_exe.py")]
        if args.toolchain:
            command += ["--toolchain", args.toolchain]
        if subprocess.run(command, cwd=RODECOMP).returncode != 0:
            sys.exit("rodecomp exe build failed")
    try:
        tc = build_exe.find_toolchain(args.toolchain)
    except build_exe.ToolchainNotFound as error:
        sys.exit(str(error))
    config = json.loads((RODECOMP / "data" / "compile.json").read_text(encoding="utf-8"))
    env = dict(tc.env)
    for key, value in config["env"].items():
        env[key] = tc.expand(value)
    tc.env["LIB"] = tc.lib_env()
    flags = compile_flags(tc, config)

    # The entry point runs before the CRT has set up the /GS security cookie.
    exe_dir = out / "exe"
    loader = compile_sources(tc, env, [*flags, "/GS-"], [LOADER], out / "obj" / "loader")
    try:
        plugin_exe.build(tc, build_exe, postprocess, RODECOMP / "build" / "exe" / "Ragexe.exe", exe_dir, loader[0])
    except (RuntimeError, postprocess.PatchError) as error:
        (exe_dir / "Ragexe.exe").unlink(missing_ok=True)
        sys.exit(f"plugin exe failed: {error}")

    sdk = out / "sdk"
    sdk.mkdir(parents=True, exist_ok=True)
    shutil.copy2(exe_dir / "Ragexe.lib", sdk / "Ragexe.lib")
    listing = exe_dir / "Ragexe.globals"
    exe_globals = dict(reversed(line.split(" ", 1)) for line in listing.read_text(encoding="ascii").splitlines())
    exe_relsig_id, exe_relsigs = relsig.read(exe_dir / "Ragexe.relsig")

    # MinHook is C: the exe's flags force C++ (/TP), so its sources build as C (/TC) with the same CRT.
    minhook = HOST_DIR / "minhook" / "src"
    c_flags = [flag for flag in flags if flag not in ("/TP", "-std:c++20")] + ["/TC"]
    host = compile_sources(tc, env, flags, sorted(HOST_DIR.glob("*.cpp")), out / "obj" / "Host")
    host += compile_sources(tc, env, c_flags, [*sorted(minhook.glob("*.c")), minhook / "hde" / "hde32.c"], out / "obj" / "Host")
    link_dll(tc, host, [], out / "RoPluginHost.dll")

    built = []
    for plugin in order:
        directory = plugin.directory
        dependencies = [plugins[name] for name in sorted(plugin.dependencies)]
        plugin_flags = [*flags, f"/I{tc.path(directory)}", f"/D{plugin.name.upper()}_EXPORTS",
                        *(f"/I{tc.path(d.directory)}" for d in dependencies)]
        sources = [*sorted(directory.glob("*.cpp")), SDK_DIR / "RoPluginDelayLoad.cpp"]
        objects = compile_sources(tc, env, plugin_flags, sources, out / "obj" / plugin.name)
        dll = out / "plugins" / f"{plugin.name}.dll"
        undefined = undefined_symbols(tc, objects)
        referenced = undefined & exe_globals.keys()
        used = sorted((undefined | relsig.decorated_literals(sources)) & exe_relsigs.keys(), key=lambda n: n.encode("latin1"))
        exe_functions = {relsig.ID_KEY: exe_relsig_id, **{name: exe_relsigs[name] for name in used}}
        globals_obj = out / "obj" / plugin.name / "RagexeGlobals.obj"
        globals_object({name: int(exe_globals[name], 16) for name in referenced}, globals_obj)
        import_libs = [out / "plugins" / f"{d.name}.lib" for d in dependencies]
        resources = compile_resources(tc, sorted(directory.glob("*.rc")), out / "obj" / plugin.name)
        # Ragexe.exe is delay-loaded and bound by RoPluginDelayLoad.cpp to the running exe, whatever its file name
        # (LNK4199: a plugin may import nothing from the exe).
        link_dll(tc, [*objects, *resources, sdk / "Ragexe.lib", globals_obj, *import_libs],
                 [*plugin.libs, "delayimp.lib", "/DELAYLOAD:Ragexe.exe", "/IGNORE:4199"], dll)
        manifest_text = plugin_toml.dumps(installed_manifest(plugin, exe_functions))
        dll.with_suffix(".toml").write_text(manifest_text, encoding="utf-8", newline="\n")
        built.append(dll)

    if args.install:
        target = Path(args.install)
        (target / "plugins").mkdir(parents=True, exist_ok=True)
        exe_name = Path(args.exe_name)
        for name in (exe_name.name, exe_name.with_suffix(".pdb").name, exe_name.with_suffix(".relsig").name):
            shutil.copy2(exe_dir / ("Ragexe" + Path(name).suffix), target / name)
        shutil.copy2(out / "RoPluginHost.dll", target / "RoPluginHost.dll")
        for dll in built:
            for path in (dll, dll.with_suffix(".pdb")):
                shutil.copy2(path, target / "plugins" / path.name)
            installed = target / "plugins" / f"{dll.stem}.toml"
            merged = merge_installed(dll.with_suffix(".toml"), installed)
            installed.write_text(merged, encoding="utf-8", newline="\n")
            (target / "plugins" / f"{dll.stem}.plugin").unlink(missing_ok=True)
        print(f"installed into {target}")


if __name__ == "__main__":
    main()
