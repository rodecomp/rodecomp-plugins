"""Build Ragexe.exe from the rodecomp submodule, add the plugin loader, and build RoPluginHost.dll and every plugin.

rodecomp/ is never modified:
  1. rodecomp/scripts/exe/build_exe.py builds and patches the exe in rodecomp/build/exe (skip with --no-exe).
  2. The exe, .pdb and .map are copied to build/exe and patches/NNNN_*.py are applied there with rodecomp's
     postprocess Image; 0100_plugin_loader writes Ragexe.def and Ragexe.globals.
  3. RoPluginHost.dll and every plugin Plugins/<Name>/ (*.cpp plus plugin.json) are compiled with the exe's own
     flags (data/compile.json shape 0) plus Plugins/SDK, so they share the exe's struct layouts, STL and CRT
     (msvcp140/vcruntime140, one heap).

Every plugin has a manifest Plugins/<Name>/plugin.json:
    {"name": "DX12Effects", "version": "0.1.0", "description": "...",
     "dependencies": {"RendererDX12": "^1.0.0"}, "libs": ["d3d12.lib"]}
name equals the directory, version is semver, dependencies map plugin names to npm-style ranges (scripts/semver.py).
Dependencies are resolved against the local plugins (missing, out-of-range and cyclic ones fail the build) and
plugins build in dependency order. A plugin compiles with /D<NAME>_EXPORTS (upper case) for its API macro and with
/I on each dependency's directory, and links each dependency's import library. Next to each DLL goes
plugins/<Name>.plugin, the manifest the host reads:
    name DX12Effects
    version 0.1.0
    depends RendererDX12 >=1.0.0 <2.0.0-0
with every range normalised to comparator sets (`||`-separated, space-joined `op version`).

    python scripts/build.py                       # everything into build/
    python scripts/build.py --no-exe              # reuse rodecomp/build/exe/Ragexe.exe
    python scripts/build.py --install DIR         # also copy exe, host and plugins into DIR
    python scripts/build.py --install DIR --exe-name Ragexe.plugins.exe   # install the exe under another name
    python scripts/build.py --plugin NAME         # only NAME and its dependencies (plus exe and host)
"""

import argparse
import importlib.util
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import semver

ROOT = Path(__file__).resolve().parent.parent
RODECOMP = ROOT / "rodecomp"
sys.path.insert(0, str(RODECOMP / "scripts" / "exe"))
import build_exe  # noqa: E402
import postprocess  # noqa: E402

PLUGINS = ROOT / "Plugins"
PATCHES = ROOT / "patches"
HOST_DIR = PLUGINS / "Host"
SDK_DIR = PLUGINS / "SDK"
IMAGE_SYM_ABSOLUTE = 0xFFFF
IMAGE_SYM_CLASS_EXTERNAL = 2


def apply_patches(exe: Path):
    image = postprocess.Image(exe)
    for path in sorted(PATCHES.glob("*.py")):
        spec = importlib.util.spec_from_file_location(f"plugin_patch_{path.stem}", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        print(f"patch {path.stem}: {module.apply(image)}", flush=True)
    image.save()


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


class Plugin:
    def __init__(self, directory: Path, version, ranges: dict, libs: list):
        self.directory = directory
        self.name = directory.name
        self.version = version
        self.ranges = ranges  # name -> range as written
        self.dependencies = {name: semver.parse_range(text) for name, text in ranges.items()}
        self.libs = libs


def load_manifest(directory: Path) -> Plugin:
    path = directory / "plugin.json"
    where = path.relative_to(ROOT)
    if not path.exists():
        sys.exit(f"{where}: missing (every plugin needs a manifest)")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        sys.exit(f"{where}: {error}")
    if not isinstance(data, dict):
        sys.exit(f"{where}: expected an object")
    unknown = data.keys() - {"name", "version", "description", "dependencies", "libs"}
    if unknown:
        sys.exit(f"{where}: unknown keys {sorted(unknown)}")
    if data.get("name") != directory.name:
        sys.exit(f"{where}: name {data.get('name')!r} must equal the directory name {directory.name!r}")
    if not isinstance(data.get("description", ""), str):
        sys.exit(f"{where}: description must be a string")
    dependencies = data.get("dependencies", {})
    libs = data.get("libs", [])
    if not isinstance(dependencies, dict):
        sys.exit(f"{where}: dependencies must map plugin names to version ranges")
    if not isinstance(libs, list) or not all(isinstance(lib, str) for lib in libs):
        sys.exit(f"{where}: libs must be a list of strings")
    version = data.get("version")
    try:
        return Plugin(directory, semver.Version.parse(version if isinstance(version, str) else repr(version)), dependencies, libs)
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


def installed_manifest(plugin: Plugin) -> str:
    lines = [f"name {plugin.name}", f"version {plugin.version}"]
    lines += [f"depends {name} {semver.normalise(sets)}" for name, sets in sorted(plugin.dependencies.items())]
    return "\n".join(lines) + "\n"


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

    plugins = {p.name: p for p in map(load_manifest, sorted(d for d in PLUGINS.iterdir() if d.is_dir() and d not in (HOST_DIR, SDK_DIR)))}
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
    exe_dir = out / "exe"
    exe_dir.mkdir(parents=True, exist_ok=True)
    for name in ("Ragexe.exe", "Ragexe.pdb", "Ragexe.map"):
        shutil.copy2(RODECOMP / "build" / "exe" / name, exe_dir / name)
    try:
        apply_patches(exe_dir / "Ragexe.exe")
    except postprocess.PatchError as error:
        (exe_dir / "Ragexe.exe").unlink(missing_ok=True)
        sys.exit(f"patch failed: {error}")
    definition, listing = exe_dir / "Ragexe.def", exe_dir / "Ragexe.globals"

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

    sdk = out / "sdk"
    sdk.mkdir(parents=True, exist_ok=True)
    result = tc.run("lib.exe", ["/nologo", "/MACHINE:X86", f"/DEF:{tc.path(definition)}", f"/OUT:{tc.path(sdk / 'Ragexe.lib')}"])
    if result.returncode != 0:
        sys.exit(result.stdout)
    exe_globals = dict(reversed(line.split(" ", 1)) for line in listing.read_text(encoding="ascii").splitlines())

    host = compile_sources(tc, env, flags, sorted(HOST_DIR.glob("*.cpp")), out / "obj" / "Host")
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
        referenced = undefined_symbols(tc, objects) & exe_globals.keys()
        globals_obj = out / "obj" / plugin.name / "RagexeGlobals.obj"
        globals_object({name: int(exe_globals[name], 16) for name in referenced}, globals_obj)
        import_libs = [out / "plugins" / f"{d.name}.lib" for d in dependencies]
        # Ragexe.exe is delay-loaded and bound by RoPluginDelayLoad.cpp to the running exe, whatever its file name
        # (LNK4199: a plugin may import nothing from the exe).
        link_dll(tc, [*objects, sdk / "Ragexe.lib", globals_obj, *import_libs],
                 [*plugin.libs, "delayimp.lib", "/DELAYLOAD:Ragexe.exe", "/IGNORE:4199"], dll)
        dll.with_suffix(".plugin").write_text(installed_manifest(plugin), encoding="ascii", newline="\n")
        built.append(dll)

    if args.install:
        target = Path(args.install)
        (target / "plugins").mkdir(parents=True, exist_ok=True)
        exe_name = Path(args.exe_name)
        for name in (exe_name.name, exe_name.with_suffix(".pdb").name):
            shutil.copy2(exe_dir / ("Ragexe" + Path(name).suffix), target / name)
        shutil.copy2(out / "RoPluginHost.dll", target / "RoPluginHost.dll")
        for dll in built:
            for path in (dll, dll.with_suffix(".pdb"), dll.with_suffix(".plugin")):
                shutil.copy2(path, target / "plugins" / path.name)
        print(f"installed into {target}")


if __name__ == "__main__":
    main()
