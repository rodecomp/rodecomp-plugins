"""plugin.toml settings: declaration and value validation (the rules of RoPluginHost and the plugin manager) and a
deterministic TOML writer for the installed <Name>.toml (build/plugin-toml-spec.md)."""

import math
import re

TYPES = ("bool", "int", "float", "string", "path", "enum", "list")
_BARE_KEY = re.compile(r"^[A-Za-z0-9_-]+$")


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def _is_number(value):
    return _is_int(value) or isinstance(value, float)


def _is_strings(value):
    return isinstance(value, list) and all(isinstance(item, str) for item in value)


def check_value(declaration: dict, value):
    """Why `value` is not valid under `declaration` (a valid declaration), or None."""
    kind = declaration["type"]
    if kind == "bool":
        return None if isinstance(value, bool) else "not a boolean"
    if kind in ("int", "float"):
        if not (_is_int(value) if kind == "int" else _is_number(value)):
            return "not an integer" if kind == "int" else "not a number"
        if "min" in declaration and value < declaration["min"]:
            return f"{value} is below min {declaration['min']}"
        if "max" in declaration and value > declaration["max"]:
            return f"{value} is above max {declaration['max']}"
        return None
    if kind in ("string", "path", "enum"):
        if not isinstance(value, str):
            return "not a string"
        if kind == "enum" and value not in declaration["values"]:
            return f'"{value}" is not one of its values'
        return None
    if not isinstance(value, list):
        return "not a list"
    for index, item in enumerate(value):
        if not (_is_int(item) if declaration["of"] == "int" else isinstance(item, str)):
            return f"element {index} is not {'an integer' if declaration['of'] == 'int' else 'a string'}"
    return None


def check_declaration(declaration, allow_value: bool):
    """Why a [settings.K] table is invalid, or None. `value` is allowed only in an installed file."""
    if not isinstance(declaration, dict):
        return "must be a table"
    kind = declaration.get("type")
    if kind not in TYPES:
        return f"type must be one of {', '.join(TYPES)}"
    allowed = {"type", "label", "description", "default"} | ({"value"} if allow_value else set())
    if kind == "int":
        allowed |= {"min", "max"}
        for key in ("min", "max"):
            if key in declaration and not _is_int(declaration[key]):
                return f"{key} must be an integer"
    elif kind == "float":
        allowed |= {"min", "max", "step"}
        for key in ("min", "max", "step"):
            if key in declaration and not _is_number(declaration[key]):
                return f"{key} must be a number"
    elif kind == "enum":
        allowed.add("values")
        if not _is_strings(declaration.get("values")) or not declaration["values"]:
            return "values must be a non-empty list of strings"
    elif kind == "list":
        allowed.add("of")
        if declaration.get("of") not in ("string", "path", "int"):
            return "of must be string, path or int"
    if kind == "path" or (kind == "list" and declaration["of"] == "path"):
        allowed |= {"kind", "filter"}
        if declaration.get("kind", "file") not in ("file", "directory"):
            return 'kind must be "file" or "directory"'
        if "filter" in declaration and not _is_strings(declaration["filter"]):
            return "filter must be a list of strings"
    unknown = declaration.keys() - allowed
    if unknown:
        return f"unknown keys {sorted(unknown)}"
    for key in ("label", "description"):
        if key in declaration and not isinstance(declaration[key], str):
            return f"{key} must be a string"
    if "default" not in declaration:
        return "default is missing"
    why = check_value(declaration, declaration["default"])
    return f"default: {why}" if why else None


def _key(key: str) -> str:
    return key if _BARE_KEY.match(key) else _string(key)


def _string(text: str) -> str:
    escaped = {'"': '\\"', "\\": "\\\\", "\b": "\\b", "\t": "\\t", "\n": "\\n", "\f": "\\f", "\r": "\\r"}
    out = []
    for char in text:
        if char in escaped:
            out.append(escaped[char])
        elif ord(char) < 0x20 or ord(char) == 0x7F:
            out.append(f"\\u{ord(char):04X}")
        else:
            out.append(char)
    return '"' + "".join(out) + '"'


def _value(value) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if _is_int(value):
        return str(value)
    if isinstance(value, float):
        if math.isnan(value):
            return "nan"
        if math.isinf(value):
            return "inf" if value > 0 else "-inf"
        text = repr(value)
        return text if any(c in text for c in ".en") else text + ".0"
    if isinstance(value, str):
        return _string(value)
    if isinstance(value, list):
        return "[" + ", ".join(_value(item) for item in value) + "]"
    raise TypeError(f"no TOML form for {value!r}")


def dumps(document: dict) -> str:
    """Top-level keys whose values are scalars or arrays, then each table (one level, or `settings` of tables), in
    insertion order. Covers exactly the plugin.toml schema."""
    lines = [f"{_key(key)} = {_value(value)}" for key, value in document.items() if not isinstance(value, dict)]
    for key, table in document.items():
        if not isinstance(table, dict):
            continue
        if key == "settings":
            for name, declaration in table.items():
                lines += ["", f"[settings.{_key(name)}]"]
                lines += [f"{_key(k)} = {_value(v)}" for k, v in declaration.items()]
        elif table:
            lines += ["", f"[{_key(key)}]"]
            lines += [f"{_key(k)} = {_value(v)}" for k, v in table.items()]
    return "\n".join(lines) + "\n"
