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
Then rodecomp's post-link patches (scripts/exe/patches) run on it, as on rodecomp's exe. Last, the relsig
(scripts/relsig.py) of every exported function is computed from the same objects, for the plugins' relsig.lock.

Plugins are compiled with every member public (Plugins/SDK/RoPluginAccess.h), and MSVC decorates access into a
member's name. Every private or protected function and static data member is therefore also exported (and listed in
Ragexe.globals) under its public-form name, an alias of the same code or address.

Ragexe.def lists only symbols of rodecomp's link (its map), so nothing /OPT:REF dropped is pulled back in. Exported
code is kept, so addresses can move: Ragexe.globals is read from the map of this link.
"""

import re
from pathlib import Path

import relsig

ENTRY = "RoLoaderEntry"
EXCLUDED_PREFIXES = ("??_C@", "??_R", "??__E", "??__F", "?$", "??_7", "??_8")
LOCAL_STATIC = re.compile(r"@\?(\d|[A-P]+@)\?\?")
# Data that headers define in every module: std:: or template-scoped names. Only the name (before a global's `@@3`
# type code) counts: a game global whose type is a std::vector (`?g_x@@3V?$vector@...@std@@A`) is the game's own.
HEADER_DATA = re.compile(r"@std@@|@\?\$")


def header_data(name: str) -> bool:
    return HEADER_DATA.search(name.split("@@3", 1)[0] + "@@") is not None


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
        if not is_function and (name.startswith("??$") or header_data(name)):
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


def exported_functions(symbols: dict) -> list:
    """Export names of the functions in the .def. Deleting destructors (??_G, ??_E) are left out: plugins `delete`
    through the vtable, and link.exe warns that exporting them by name may not run correctly (LNK4102)."""
    return sorted((name for name, (_, is_function, _) in symbols.items()
                   if is_function and not name.startswith(("??_G", "??_E"))), key=lambda n: n.encode("ascii"))


ACCESS_CODE = re.compile(r"@@(?=[A-P01])")


def undecorate(tc, names: list) -> dict:
    """decorated name -> undname.exe's declaration, in batches under the Windows command-line limit."""
    result = {}
    start = 0
    while start < len(names):
        batch, size = [], 0
        while start < len(names) and (not batch or size + len(names[start]) < 24000):
            batch.append(names[start])
            size += len(names[start]) + 3
            start += 1
        decorated = None
        for line in tc.run("undname.exe", batch).stdout.splitlines():
            if line.startswith('Undecoration of :- "'):
                decorated = line[20:-1]
            elif line.startswith('is :- "') and decorated:
                result[decorated] = line[7:-1]
    return result


def public_aliases(tc, names: list) -> dict:
    """public-form name -> name, for each private/protected member function or static data member in `names`.

    After the qualified name's `@@`, a member function's code is A-P (private A-H, protected I-P) or Q-X (public),
    in groups of 8 by kind (near, near far, static ×2, virtual ×2, thunk ×2): public = 'Q' + code % 8. Static data
    is 0 private, 1 protected, 2 public. Which `@@` ends the qualified name depends on the template arguments
    inside it, so each candidate swap is checked with undname.exe: the one whose declaration is the original's with
    `public: ` for its access is the alias."""
    declarations = undecorate(tc, [name for name in names if name.startswith("?")])
    hidden = {name: text for name, text in declarations.items() if text.startswith(("private: ", "protected: "))}
    candidates = {}
    for name in hidden:
        for match in ACCESS_CODE.finditer(name):
            i = match.end()
            code = "2" if name[i] in "01" else chr(ord("Q") + (ord(name[i]) - ord("A")) % 8)
            candidates.setdefault(name, []).append(name[:i] + code + name[i + 1:])
    checked = undecorate(tc, [c for cs in candidates.values() for c in cs])
    existing = set(names)
    aliases = {}
    for name, text in hidden.items():
        wanted = "public: " + text.split(": ", 1)[1]
        found = [c for c in candidates.get(name, []) if checked.get(c) == wanted]
        if len(found) != 1:
            raise RuntimeError(f"no unique public form of {name} ({text}): {found}")
        if found[0] not in existing:
            aliases[found[0]] = name
    return aliases


def write_def(names: list, aliases: dict, public: dict, path: Path):
    """Exports the functions, then each public-form alias; globals are reached at their fixed address
    (Ragexe.globals), not through the table. The entry point is exported too: an exported RoLoaderEntry that is also
    the PE entry point is how the plugin manager tells a plugin build from Gravity's or rodecomp's plain exe."""
    lines = [f"{name}={aliases[name]}" if name in aliases else name for name in names]
    lines += [f"{alias}={aliases.get(name, name)}" for alias, name in sorted(public.items())]
    lines.append(ENTRY)
    path.write_text("\n".join(["NAME Ragexe.exe", "EXPORTS", *lines]) + "\n", encoding="ascii")
    return len(names)


def exported_relsigs(objects: list, symbols: dict, names: list, aliases: dict, out: Path) -> dict:
    """`linker symbol -> relsig` per exported function, the symbol a plugin's object references. An /ALTERNATENAME
    alias gets its target's value. Per-object results are cached in <out>/relsig.cache."""
    sigs = relsig.exe_relsigs(objects, out / "relsig.cache")
    listed = {}
    for name in names:
        linker = symbols[name][2]
        target = aliases.get(name, linker)
        sig = sigs.get(target) or sigs.get("_" + target)
        if sig:
            listed[linker] = sig
    return listed


def write_globals(symbols: dict, public: dict, path: Path):
    """`<VA> <linker symbol>` per global (and per public-form alias of a static data member), from the map of the
    exe plugins run against."""
    lines = [f"{va:08x} {symbol}" for _, (va, is_function, symbol) in sorted(symbols.items()) if not is_function]
    lines += [f"{symbols[name][0]:08x} {alias}" for alias, name in sorted(public.items())
              if name in symbols and not symbols[name][1]]
    path.write_text("\n".join(lines) + "\n", encoding="ascii")
    return len(lines)


def build(tc, build_exe, postprocess, rodecomp_exe: Path, out: Path, loader_obj: Path):
    """Links <out>/Ragexe.exe (+ .pdb, .map, .lib, Ragexe.def, Ragexe.globals) from rodecomp_exe's objects. Returns
    (exe, relsig id, {linker symbol: relsig} of the exported functions, {public-form alias: linker symbol})."""
    import json

    out.mkdir(parents=True, exist_ok=True)
    rodecomp = rodecomp_exe.parent.parent.parent
    config = json.loads((rodecomp / "data" / "compile.json").read_text(encoding="utf-8"))
    objects = [rodecomp_exe.parent / (unit.replace("/", "_") + ".obj") for unit in build_exe.exe_units(config)]
    missing = [o for o in objects if not o.exists()]
    if missing:
        raise RuntimeError(f"{len(missing)} rodecomp objects missing (e.g. {missing[0]}); build rodecomp's exe first")

    definition = out / "Ragexe.def"
    symbols = map_symbols(rodecomp_exe.with_suffix(".map"))
    aliases = alternate_names(rodecomp)
    names = exported_functions(symbols)
    function_aliases = public_aliases(tc, names)
    exports = write_def(names, aliases, function_aliases, definition)
    extra = [f'"{tc.path(loader_obj)}"', f"/DEF:{tc.path(definition)}", "/DYNAMICBASE:NO", f"/ENTRY:{ENTRY}"]
    code, log, exe = build_exe.link(tc, [*objects, *build_exe.build_import_libs(tc, out)], out, extra)
    if code != 0:
        (out / "link.log").write_text(log, encoding="utf-8")
        raise RuntimeError(f"plugin exe link failed, see {out / 'link.log'}")
    applied = postprocess.run(exe)
    exe_symbols = map_symbols(exe.with_suffix(".map"))
    data_aliases = public_aliases(tc, [name for name, (_, is_function, _) in exe_symbols.items() if not is_function])
    globals_ = write_globals(exe_symbols, data_aliases, out / "Ragexe.globals")
    ident = relsig.relsig_id(build_exe.MSVC_VERSION, config)
    sigs = exported_relsigs(objects, symbols, names, aliases, out)
    print(f"plugin exe: {exports} exported functions ({len(sigs)} with a relsig), {globals_} globals, "
          f"{len(function_aliases)} + {len(data_aliases)} public-form aliases of private/protected functions + "
          f"static data, entry {ENTRY}, fixed base, {applied} rodecomp patches", flush=True)
    public = {alias: symbols[name][2] for alias, name in function_aliases.items()}
    public.update({alias: exe_symbols[name][2] for alias, name in data_aliases.items()})
    return exe, ident, sigs, public
