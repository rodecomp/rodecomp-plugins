"""Plugin loader: export the exe's C++ symbols and load RoPluginHost.dll before the CRT starts.

Applied by scripts/build.py to a copy of rodecomp's patched Ragexe.exe. Without RoPluginHost.dll next to the exe it
behaves as before, except:
  1. An export directory (in .patch) names every C++ function and global of the game's own objects. Ragexe.def is
     written next to the exe; scripts/build.py turns it into Ragexe.lib, so plugins call exe functions through
     ordinary import thunks.
  2. IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE is cleared: the exe always loads at its preferred base, so a global
     is at its map address and plugins reach it as an absolute symbol (RagexeGlobals.obj). Source headers declare
     globals as plain `extern`, which a __declspec(dllimport) redeclaration cannot fix (MSVC C2370).
  3. A new entry point that runs, before mainCRTStartup:
         h = LoadLibraryA("RoPluginHost.dll"); if (h) { f = GetProcAddress(h, "RoHost_Start"); if (f) f(); }
         jmp <original entry point>
"""

import re
import struct

from postprocess import PatchError

HOST_DLL = b"RoPluginHost.dll\0"
HOST_START = b"RoHost_Start\0"
EXCLUDED_PREFIXES = ("??_C@", "??_R", "??__E", "??__F", "?$", "??_7", "??_8")
LOCAL_STATIC = re.compile(r"@\?(\d|[A-P]+@)\?\?")
HEADER_DATA = re.compile(r"@std@@|@\?\$")


def exported_symbols(map_path):
    """(name, VA, is_function) of the map's public C++ symbols defined by the game's own objects, by name.

    Symbols of library objects (MSVCRT:x.obj, ucrt:...) and data that headers define in every module that uses it
    (function-local statics of inline functions, template and std:: variables such as std::__DIGIT_TABLE<char>) are
    left out: a plugin has its own copies of those, and an absolute duplicate collides with them (as a link error,
    or a hang in link.exe's PDB writer)."""
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
        if not name.startswith("?") or name.startswith(EXCLUDED_PREFIXES) or LOCAL_STATIC.search(name):
            continue
        if ":" in parts[-1]:
            continue
        is_function = len(parts) > 3 and parts[3] == "f"
        if not is_function and (name.startswith("??$") or HEADER_DATA.search(name)):
            continue
        symbols[name] = (int(parts[2], 16), is_function)
    return symbols


def export_directory(image, symbols, rva):
    """IMAGE_EXPORT_DIRECTORY + tables + strings laid out at rva; names sorted as the loader binary-searches them."""
    names = sorted(symbols, key=lambda n: n.encode("ascii"))
    count = len(names)
    module = b"Ragexe.exe\0"
    header = 40
    functions_rva = rva + header
    names_rva = functions_rva + 4 * count
    ordinals_rva = names_rva + 4 * count
    strings_rva = ordinals_rva + 2 * count
    strings = bytearray(module)
    name_rvas = []
    for name in names:
        name_rvas.append(strings_rva + len(strings))
        strings += name.encode("ascii") + b"\0"
    blob = bytearray(struct.pack("<IIHHIIIIIII", 0, 0, 0, 0, strings_rva, 1, count, count,
                                 functions_rva, names_rva, ordinals_rva))
    blob += b"".join(struct.pack("<I", symbols[n][0] - image.base) for n in names)
    blob += b"".join(struct.pack("<I", r) for r in name_rvas)
    blob += b"".join(struct.pack("<H", i) for i in range(count))
    blob += strings
    return bytes(blob), names


def apply(image):
    symbols = exported_symbols(image.exe.with_suffix(".map"))
    if not symbols:
        raise PatchError("no C++ symbols in the linker map")

    imports = {(entry.dll.lower(), imp.name): imp.address
               for entry in image.pe.DIRECTORY_ENTRY_IMPORT for imp in entry.imports}
    try:
        load_library = imports[(b"kernel32.dll", b"LoadLibraryA")]
        get_proc = imports[(b"kernel32.dll", b"GetProcAddress")]
    except KeyError as missing:
        raise PatchError(f"import {missing} not found") from None

    header = image.pe.OPTIONAL_HEADER
    entry = image.base + header.AddressOfEntryPoint
    host_name = image.add(HOST_DLL)
    start_name = image.add(HOST_START)
    stub_va = image.allocate(48)
    stub = bytearray()
    stub += b"\x68" + struct.pack("<I", host_name)            # 00 push "RoPluginHost.dll"
    stub += b"\xff\x15" + struct.pack("<I", load_library)     # 05 call [LoadLibraryA]
    stub += b"\x85\xc0\x74\x12"                               # 0B test eax,eax; jz done
    stub += b"\x68" + struct.pack("<I", start_name)           # 0F push "RoHost_Start"
    stub += b"\x50"                                           # 14 push eax
    stub += b"\xff\x15" + struct.pack("<I", get_proc)         # 15 call [GetProcAddress]
    stub += b"\x85\xc0\x74\x02"                               # 1B test eax,eax; jz done
    stub += b"\xff\xd0"                                       # 1F call eax
    done = len(stub)
    stub += b"\xe9" + struct.pack("<i", entry - (stub_va + done + 5))  # 21 done: jmp original entry
    if done != 0x21 or stub[14] != done - 15 or stub[30] != done - 31:
        raise PatchError("entry stub branch offsets are wrong")
    image.write(stub_va, bytes(stub), bytes(len(stub)), relocs=(1, 7, 16, 23))
    struct.pack_into("<I", image.data, header.get_field_absolute_offset("AddressOfEntryPoint"), stub_va - image.base)

    directory_va = image.allocate(0)
    blob, names = export_directory(image, symbols, directory_va - image.base)
    if image.add(blob, align=16) != directory_va:
        raise PatchError("export directory moved while being placed")
    exports = header.DATA_DIRECTORY[0]
    struct.pack_into("<II", image.data, exports.get_file_offset(), directory_va - image.base, len(blob))

    characteristics = header.get_field_absolute_offset("DllCharacteristics")
    struct.pack_into("<H", image.data, characteristics, header.DllCharacteristics & ~0x0040)

    lines = ["NAME Ragexe.exe", "EXPORTS"] + [name for name in names if symbols[name][1]]
    image.exe.with_suffix(".def").write_text("\n".join(lines) + "\n", encoding="ascii")
    globals_ = [f"{symbols[name][0]:08x} {name}" for name in names if not symbols[name][1]]
    image.exe.with_suffix(".globals").write_text("\n".join(globals_) + "\n", encoding="ascii")
    return (f"entry {entry:#x} -> loader stub {stub_va:#x}; exported {len(names) - len(globals_)} functions and "
            f"{len(globals_)} globals ({len(blob) // 1024} KiB); fixed base; wrote Ragexe.def, Ragexe.globals")
