"""The plugin build of Ragexe.exe: rodecomp's own objects, linked again with a native plugin loader.

rodecomp/scripts/exe/build_exe.py compiles and links Ragexe.exe in rodecomp/build/exe. This module links those same
objects once more into <out>/exe with what plugins need, all produced by link.exe:
  - /DEF:Ragexe.def exports every function of the game's own objects (C++ decorated, C without the leading `_`), so
    link.exe writes the export table and Ragexe.lib, through which plugins call exe functions.
  - /DYNAMICBASE:NO: the exe always loads at its preferred base, so a global is at its map address and plugins reach
    it as an absolute symbol (RagexeGlobals.obj, from Ragexe.globals). Source headers declare globals as plain
    `extern`, which a __declspec(dllimport) redeclaration cannot fix (MSVC C2370).
  - /ENTRY:RoLoaderEntry (loader/RoLoaderEntry.cpp) loads RoPluginHost.dll before the CRT starts, then runs the CRT's
    WinMainCRTStartup. Without RoPluginHost.dll the exe behaves as rodecomp's.
Then rodecomp's post-link patches (scripts/exe/patches) run on it, as on rodecomp's exe.

Ragexe.def lists only symbols of rodecomp's link (its map), so nothing /OPT:REF dropped is pulled back in. Exported
code is kept, so addresses can move: Ragexe.globals is read from the map of this link.
"""

import re
from pathlib import Path

ENTRY = "RoLoaderEntry"
EXCLUDED_PREFIXES = ("??_C@", "??_R", "??__E", "??__F", "?$", "??_7", "??_8")
LOCAL_STATIC = re.compile(r"@\?(\d|[A-P]+@)\?\?")
HEADER_DATA = re.compile(r"@std@@|@\?\$")


def map_symbols(map_path: Path) -> dict:
    """export name -> (VA, is_function, linker symbol) of the map's public symbols defined by the game's own objects.

    Symbols of library objects (MSVCRT:x.obj, ucrt:...), absolute symbols, and compiler-generated names (`__` prefix
    or `$`: EH tables, unwind funclets, SSE constants) are left out, as are data that headers define in every module
    that uses it (function-local statics of inline functions, template and std:: variables such as
    std::__DIGIT_TABLE<char>) and C functions inlined from headers (`f i`, e.g. the CRT's _sprintf): a plugin has
    its own copies of those, and an absolute duplicate collides with them (as a link error, or a hang in link.exe's
    PDB writer). C names lose the one leading underscore of the x86 C decoration (`_lua_tolstring` ->
    `lua_tolstring`): the export name GetProcAddress uses, which lib.exe/link.exe turn back into `_lua_tolstring`."""
    symbols = {}
    in_publics = False
    for text in map_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "Publics by Value" in text:
            in_publics = True
            continue
        if not in_publics:
            continue
        if text.startswith(" entry point at"):
            break
        parts = text.split()
        if len(parts) < 3 or ":" not in parts[0]:
            continue
        name = parts[1]
        if ":" in parts[-1] or parts[-1] == "<absolute>":
            continue
        is_function = len(parts) > 3 and parts[3] == "f"
        if name.startswith("?"):
            if name.startswith(EXCLUDED_PREFIXES) or LOCAL_STATIC.search(name):
                continue
        elif not name.startswith("_") or name.startswith("__") or "$" in name or (is_function and parts[4] == "i"):
            continue
        if not is_function and (name.startswith("??$") or HEADER_DATA.search(name)):
            continue
        if name == "_" + ENTRY or name.startswith(("?__global_delete@", "?__empty_global_delete@")):
            continue
        # A __cdecl C name drops its `_` (lib.exe adds it back for the import symbol); a __stdcall one (`@N`) is
        # exported as linked, the form link.exe finds for it in a .def.
        stdcall = re.search(r"@\d+$", name) is not None
        export = name if name.startswith("?") or stdcall else name[1:]
        symbols[export] = (int(parts[2], 16), is_function, name)
    return symbols


ALTERNATE_NAME = re.compile(r'/ALTERNATENAME:_?([^=\s"]+)=([^\s"]+)')


def alternate_names(rodecomp: Path) -> dict:
    """C name -> decorated target of the source's `#pragma comment(linker, "/ALTERNATENAME:...")` aliases. The map
    lists the alias, but a /DEF export of it does not resolve: it is exported as `alias=target`."""
    aliases = {}
    for path in [*rodecomp.glob("Source/**/*.cpp"), *rodecomp.glob("Recovered/**/*.cpp")]:
        text = path.read_text(encoding="utf-8", errors="replace")
        if "/ALTERNATENAME:" in text:
            aliases.update(ALTERNATE_NAME.findall(text))
    return aliases


def write_def(symbols: dict, aliases: dict, path: Path):
    """Exports the functions; globals are reached at their fixed address (Ragexe.globals), not through the table.
    Deleting destructors (??_G, ??_E) are left out: plugins `delete` through the vtable, and link.exe warns that
    exporting them by name may not run correctly (LNK4102)."""
    names = sorted((name for name, (_, is_function, _) in symbols.items()
                    if is_function and not name.startswith(("??_G", "??_E"))), key=lambda n: n.encode("ascii"))
    lines = [f"{name}={aliases[name]}" if name in aliases else name for name in names]
    path.write_text("\n".join(["NAME Ragexe.exe", "EXPORTS", *lines]) + "\n", encoding="ascii")
    return len(names)


def write_globals(symbols: dict, path: Path):
    """`<VA> <linker symbol>` per global, from the map of the exe plugins run against."""
    lines = [f"{va:08x} {symbol}" for _, (va, is_function, symbol) in sorted(symbols.items()) if not is_function]
    path.write_text("\n".join(lines) + "\n", encoding="ascii")
    return len(lines)


def build(tc, build_exe, postprocess, rodecomp_exe: Path, out: Path, loader_obj: Path) -> Path:
    """Links <out>/Ragexe.exe (+ .pdb, .map, .lib, Ragexe.def, Ragexe.globals) from rodecomp_exe's objects."""
    import json

    out.mkdir(parents=True, exist_ok=True)
    rodecomp = rodecomp_exe.parent.parent.parent
    config = json.loads((rodecomp / "data" / "compile.json").read_text(encoding="utf-8"))
    objects = [rodecomp_exe.parent / (unit.replace("/", "_") + ".obj") for unit in build_exe.exe_units(config)]
    missing = [o for o in objects if not o.exists()]
    if missing:
        raise RuntimeError(f"{len(missing)} rodecomp objects missing (e.g. {missing[0]}); build rodecomp's exe first")

    definition = out / "Ragexe.def"
    exports = write_def(map_symbols(rodecomp_exe.with_suffix(".map")), alternate_names(rodecomp), definition)
    extra = [f'"{tc.path(loader_obj)}"', f"/DEF:{tc.path(definition)}", "/DYNAMICBASE:NO", f"/ENTRY:{ENTRY}"]
    code, log, exe = build_exe.link(tc, [*objects, *build_exe.build_import_libs(tc, out)], out, extra)
    if code != 0:
        (out / "link.log").write_text(log, encoding="utf-8")
        raise RuntimeError(f"plugin exe link failed, see {out / 'link.log'}")
    applied = postprocess.run(exe)
    globals_ = write_globals(map_symbols(exe.with_suffix(".map")), out / "Ragexe.globals")
    print(f"plugin exe: {exports} exported functions, {globals_} globals, entry {ENTRY}, fixed base, "
          f"{applied} rodecomp patches", flush=True)
    return exe
