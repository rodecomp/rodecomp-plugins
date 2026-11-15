"""Semantic versions (MAJOR.MINOR.PATCH[-prerelease]) and npm-style ranges for plugin manifests.

A range is comparator sets joined by `||`; a set is space-separated comparators that must all hold. Accepted:
exact `1.2.3` / `=1.2.3`, `^1.2.3`, `~1.2.3`, `>=` `>` `<` `<=` comparators, `*` (also `x`, empty), and partial
versions (`1`, `1.2`, `1.x`) where npm allows them. normalise() desugars a range into plain comparators
(`^1.2.0` -> `>=1.2.0 <2.0.0-0`) so a consumer only evaluates `op version`. Versions compare by semver
precedence; npm's rule that excludes prereleases from ranges without one is not applied.
"""

import re

_VERSION = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$")
_PARTIAL = re.compile(r"^(\d+|[xX*])(?:\.(\d+|[xX*]))?(?:\.(\d+|[xX*]))?(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$")
_COMPARATOR = re.compile(r"^(\^|~|>=|<=|>|<|=)?(.*)$")


class SemverError(ValueError):
    pass


class Version:
    __slots__ = ("major", "minor", "patch", "prerelease")

    def __init__(self, major, minor, patch, prerelease=()):
        self.major, self.minor, self.patch, self.prerelease = major, minor, patch, tuple(prerelease)

    @classmethod
    def parse(cls, text):
        match = _VERSION.match(text)
        if not match:
            raise SemverError(f"invalid version {text!r} (expected MAJOR.MINOR.PATCH[-prerelease])")
        prerelease = match.group(4).split(".") if match.group(4) else []
        for part in prerelease:
            if part.isdigit() and len(part) > 1 and part[0] == "0":
                raise SemverError(f"invalid version {text!r}: numeric prerelease {part!r} has a leading zero")
        return cls(int(match.group(1)), int(match.group(2)), int(match.group(3)), prerelease)

    def _key(self):
        # A release ranks above its prereleases; numeric identifiers rank below alphanumeric ones.
        pre = tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in self.prerelease)
        return (self.major, self.minor, self.patch, 1 if not pre else 0, pre)

    def __lt__(self, other):
        return self._key() < other._key()

    def __eq__(self, other):
        return isinstance(other, Version) and self._key() == other._key()

    def __le__(self, other):
        return self._key() <= other._key()

    def __hash__(self):
        return hash(self._key())

    def __str__(self):
        core = f"{self.major}.{self.minor}.{self.patch}"
        return f"{core}-{'.'.join(self.prerelease)}" if self.prerelease else core


def _floor(major, minor=0, patch=0):
    """The lowest version of a release line: X.Y.Z-0, below every prerelease of X.Y.Z."""
    return Version(major, minor, patch, ["0"])


def _desugar(token):
    """One range token as a list of (op, Version)."""
    op, rest = _COMPARATOR.match(token).groups()
    op = op or ""
    if rest in ("", "*", "x", "X"):
        if op in ("", "=", ">=", "^", "~", "<="):
            return [(">=", _floor(0))]
        raise SemverError(f"{token!r} matches nothing")
    match = _PARTIAL.match(rest)
    if not match:
        raise SemverError(f"invalid comparator {token!r}")
    parts = [None if p is None or p in "xX*" else int(p) for p in match.groups()[:3]]
    major, minor, patch = parts
    prerelease = match.group(4).split(".") if match.group(4) else []
    if major is None or (minor is None and patch is not None):
        raise SemverError(f"invalid comparator {token!r}")
    if prerelease and patch is None:
        raise SemverError(f"invalid comparator {token!r}: prerelease needs a full version")
    full = Version(major, minor or 0, patch or 0, prerelease)

    if op == "^":
        if major > 0 or minor is None:
            upper = _floor(major + 1)
        elif minor > 0 or patch is None:
            upper = _floor(0, minor + 1)
        else:
            upper = _floor(0, 0, patch + 1)
        return [(">=", full), ("<", upper)]
    if op == "~" or (op in ("", "=") and patch is None):
        upper = _floor(major + 1) if minor is None else _floor(major, minor + 1)
        return [(">=", full), ("<", upper)]
    if op in ("", "="):
        return [("=", full)]
    if patch is not None:
        return [(op, full)]
    # Partial comparators: >1.2 means >=1.3.0, <=1.2 means <1.3.0-0.
    next_line = _floor(major + 1) if minor is None else _floor(major, minor + 1)
    if op == ">=":
        return [(">=", full)]
    if op == "<":
        return [("<", _floor(major, minor or 0))]
    if op == ">":
        return [(">=", Version(next_line.major, next_line.minor, 0))]
    return [("<", next_line)]  # <=


def parse_range(text):
    """A range as a list of comparator sets, each a list of (op, Version)."""
    if not isinstance(text, str):
        raise SemverError(f"range must be a string, got {text!r}")
    sets = []
    for alternative in text.split("||"):
        # `>= 1.2.0` is accepted as `>=1.2.0`.
        tokens = re.sub(r"(\^|~|>=|<=|>|<|=)\s+", r"\1", alternative.strip()).split()
        comparators = []
        for token in tokens or ["*"]:
            comparators += _desugar(token)
        sets.append(comparators)
    return sets


_TESTS = {
    "=": lambda a, b: a == b,
    ">=": lambda a, b: b <= a,
    ">": lambda a, b: b < a,
    "<=": lambda a, b: a <= b,
    "<": lambda a, b: a < b,
}


def satisfies(version, sets):
    return any(all(_TESTS[op](version, bound) for op, bound in comparators) for comparators in sets)


def normalise(sets):
    return " || ".join(" ".join(f"{op}{bound}" for op, bound in comparators) for comparators in sets)
