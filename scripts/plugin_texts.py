"""Keeps the plugin manager's translations of the plugins' own texts in step with Plugins/*/*/plugin.toml.

tools/plugin-manager/crates/ro-plugins/lang/plugins/en.lang is a snapshot of every plugin's English texts (its
description, each setting's label and description, each choice's label and description), one `key=value` line each:

    <Plugin>.description
    <Plugin>.<Setting>.label / .description
    <Plugin>.<Setting>.<choice>.label / .description   (a choice value that isn't [A-Za-z0-9_] is written x<hex>)

The other `<code>.lang` files there hold the same keys, translated. The manager shows a translation only while the
plugin's current English equals the snapshot's, so a changed text shows in English until it is translated again.

    python scripts/plugin_texts.py          rewrite en.lang from the plugin.toml files and drop the translations
                                            whose English changed or whose text is gone (they are reported)
    python scripts/plugin_texts.py --check  exit 1 when en.lang is out of date (nothing is written)
"""
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLUGINS = ROOT / "Plugins"
LANG = ROOT / "tools" / "plugin-manager" / "crates" / "ro-plugins" / "lang" / "plugins"
ENGLISH = LANG / "en.lang"
HEADER = """# The plugins' own texts (plugin.toml), {what}.
# Written by scripts/plugin_texts.py: {how}
# One `key=value` line per text; `\\n` is a line break. A text missing here shows in English.
"""


def segment(value: str) -> str:
    """A choice value as a key segment."""
    return value if re.fullmatch(r"[A-Za-z0-9_]+", value) else "x" + value.encode("utf-8").hex()


def escape(text: str) -> str:
    return text.replace("\\", "\\\\").replace("\n", "\\n")


def unescape(text: str) -> str:
    out, chars = [], iter(text)
    for c in chars:
        if c != "\\":
            out.append(c)
            continue
        nxt = next(chars, "")
        out.append("\n" if nxt == "n" else nxt if nxt else "\\")
    return "".join(out)


def english() -> list[tuple[str, str, str]]:
    """(plugin, key, text) of every plugin text, by category then plugin, settings in file order."""
    entries = []
    for path in sorted(PLUGINS.glob("*/*/plugin.toml"), key=lambda p: (p.parent.parent.name.lower(), p.parent.name.lower())):
        data = tomllib.loads(path.read_text(encoding="utf-8"))
        name = data["name"]
        add = lambda key, text: entries.append((name, f"{name}.{key}", text)) if text.strip() else None
        add("description", data.get("description", ""))
        for key, setting in data.get("settings", {}).items():
            add(f"{key}.label", setting.get("label", ""))
            add(f"{key}.description", setting.get("description", ""))
            for value, choice in (setting.get("choices") or {}).items():
                add(f"{key}.{segment(str(value))}.label", choice.get("label", ""))
                add(f"{key}.{segment(str(value))}.description", choice.get("description", ""))
    return entries


def read(path: Path) -> dict[str, str]:
    table = {}
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, value = line.split("=", 1)
                table[key.strip()] = unescape(value.strip())
    return table


def render(entries: list[tuple[str, str, str]], texts: dict[str, str], header: str) -> str:
    lines, plugin = [header], None
    for name, key, _ in entries:
        if key not in texts:
            continue
        if name != plugin:
            lines.append(f"\n# {name}")
            plugin = name
        lines.append(f"{key}={escape(texts[key])}")
    return "\n".join(lines) + "\n"


def main() -> int:
    entries = english()
    new = {key: text for _, key, text in entries}
    old = read(ENGLISH)
    snapshot = render(entries, new, HEADER.format(what="in English", how="the snapshot the translations follow."))
    if "--check" in sys.argv:
        current = ENGLISH.read_text(encoding="utf-8") if ENGLISH.is_file() else ""
        if current != snapshot:
            print(f"{ENGLISH.relative_to(ROOT)} is out of date: run python scripts/plugin_texts.py")
            return 1
        print("plugin texts: up to date")
        return 0
    ENGLISH.write_text(snapshot, encoding="utf-8", newline="\n")
    for path in sorted(LANG.glob("*.lang")):
        if path == ENGLISH:
            continue
        translated = read(path)
        kept = {key: text for key, text in translated.items() if key in new and old.get(key) == new[key]}
        dropped = sorted(set(translated) - set(kept))
        header = HEADER.format(what=f"in {path.stem}", how="translations of en.lang; a changed English drops its line.")
        path.write_text(render(entries, kept, header), encoding="utf-8", newline="\n")
        missing = len(new) - len(kept)
        print(f"{path.name}: {len(kept)} translated, {missing} to translate" + (f", dropped {', '.join(dropped)}" if dropped else ""))
    print(f"{ENGLISH.name}: {len(new)} texts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
