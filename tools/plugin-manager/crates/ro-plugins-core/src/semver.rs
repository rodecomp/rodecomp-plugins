//! Semantic versions (MAJOR.MINOR.PATCH[-prerelease]) and npm-style ranges, a port of scripts/semver.py.
//!
//! A range is comparator sets joined by `||`; a set is space-separated comparators that must all hold. Sugar
//! (`^`, `~`, `*`, partial versions) desugars into plain comparators with `-0` floors (`^1.2.0` ->
//! `>=1.2.0 <2.0.0-0`), the form the build writes into `.plugin` manifests and Host.cpp evaluates.

use std::cmp::Ordering;
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SemverError(pub String);

impl fmt::Display for SemverError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for SemverError {}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct Version {
    pub major: u64,
    pub minor: u64,
    pub patch: u64,
    pub prerelease: Vec<String>,
}

fn is_number(text: &str) -> bool {
    !text.is_empty() && text.bytes().all(|b| b.is_ascii_digit())
}

fn is_identifier(text: &str) -> bool {
    !text.is_empty() && text.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')
}

/// `0|[1-9]\d*`
fn core_number(text: &str) -> Option<u64> {
    if !is_number(text) || (text.len() > 1 && text.starts_with('0')) {
        return None;
    }
    text.parse().ok()
}

/// Numeric identifiers compare by value; leading zeros (only possible in range comparators) do not matter.
fn compare_numeric(x: &str, y: &str) -> Ordering {
    let x = x.trim_start_matches('0');
    let y = y.trim_start_matches('0');
    x.len().cmp(&y.len()).then_with(|| x.cmp(y))
}

impl Version {
    pub fn new(major: u64, minor: u64, patch: u64) -> Self {
        Version { major, minor, patch, prerelease: Vec::new() }
    }

    pub fn parse(text: &str) -> Result<Version, SemverError> {
        let invalid = || SemverError(format!("invalid version {} (expected MAJOR.MINOR.PATCH[-prerelease])", repr(text)));
        let (core, prerelease) = match text.split_once('-') {
            Some((core, pre)) => (core, Some(pre)),
            None => (text, None),
        };
        let mut parts = core.split('.');
        let (Some(major), Some(minor), Some(patch), None) = (parts.next(), parts.next(), parts.next(), parts.next()) else {
            return Err(invalid());
        };
        let (Some(major), Some(minor), Some(patch)) = (core_number(major), core_number(minor), core_number(patch)) else {
            return Err(invalid());
        };
        let mut version = Version::new(major, minor, patch);
        if let Some(pre) = prerelease {
            for part in pre.split('.') {
                if !is_identifier(part) {
                    return Err(invalid());
                }
                if is_number(part) && part.len() > 1 && part.starts_with('0') {
                    return Err(SemverError(format!(
                        "invalid version {}: numeric prerelease {} has a leading zero",
                        repr(text),
                        repr(part)
                    )));
                }
                version.prerelease.push(part.to_owned());
            }
        }
        Ok(version)
    }

    /// The lowest version of a release line: X.Y.Z-0, below every prerelease of X.Y.Z.
    fn floor(major: u64, minor: u64, patch: u64) -> Version {
        Version { major, minor, patch, prerelease: vec!["0".to_owned()] }
    }
}

impl Ord for Version {
    // A release ranks above its prereleases; numeric identifiers rank below alphanumeric ones; a shorter
    // prerelease ranks below a longer one it prefixes.
    fn cmp(&self, other: &Self) -> Ordering {
        (self.major, self.minor, self.patch)
            .cmp(&(other.major, other.minor, other.patch))
            .then_with(|| match (self.prerelease.is_empty(), other.prerelease.is_empty()) {
                (true, true) => Ordering::Equal,
                (true, false) => Ordering::Greater,
                (false, true) => Ordering::Less,
                (false, false) => {
                    for (x, y) in self.prerelease.iter().zip(&other.prerelease) {
                        let order = match (is_number(x), is_number(y)) {
                            (true, true) => compare_numeric(x, y),
                            (true, false) => Ordering::Less,
                            (false, true) => Ordering::Greater,
                            (false, false) => x.cmp(y),
                        };
                        if order != Ordering::Equal {
                            return order;
                        }
                    }
                    self.prerelease.len().cmp(&other.prerelease.len())
                }
            })
    }
}

impl PartialOrd for Version {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl fmt::Display for Version {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}.{}.{}", self.major, self.minor, self.patch)?;
        if !self.prerelease.is_empty() {
            write!(f, "-{}", self.prerelease.join("."))?;
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Op {
    Eq,
    Ge,
    Gt,
    Le,
    Lt,
}

impl Op {
    pub fn as_str(self) -> &'static str {
        match self {
            Op::Eq => "=",
            Op::Ge => ">=",
            Op::Gt => ">",
            Op::Le => "<=",
            Op::Lt => "<",
        }
    }

    fn test(self, version: &Version, bound: &Version) -> bool {
        let order = version.cmp(bound);
        match self {
            Op::Eq => order == Ordering::Equal,
            Op::Ge => order != Ordering::Less,
            Op::Gt => order == Ordering::Greater,
            Op::Le => order != Ordering::Greater,
            Op::Lt => order == Ordering::Less,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Comparator {
    pub op: Op,
    pub version: Version,
}

/// Comparator sets joined by `||`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Range(pub Vec<Vec<Comparator>>);

impl Range {
    pub fn parse(text: &str) -> Result<Range, SemverError> {
        let mut sets = Vec::new();
        for alternative in text.split("||") {
            let mut comparators = Vec::new();
            let tokens = tokens(alternative.trim());
            if tokens.is_empty() {
                comparators.extend(desugar("*")?);
            }
            for token in &tokens {
                comparators.extend(desugar(token)?);
            }
            sets.push(comparators);
        }
        Ok(Range(sets))
    }

    pub fn satisfied_by(&self, version: &Version) -> bool {
        self.0.iter().any(|set| set.iter().all(|c| c.op.test(version, &c.version)))
    }
}

impl fmt::Display for Range {
    /// The normalised form, as written into `.plugin` manifests.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for (i, set) in self.0.iter().enumerate() {
            if i > 0 {
                f.write_str(" || ")?;
            }
            for (j, c) in set.iter().enumerate() {
                if j > 0 {
                    f.write_str(" ")?;
                }
                write!(f, "{}{}", c.op.as_str(), c.version)?;
            }
        }
        Ok(())
    }
}

/// Python's repr() of a plain string, for identical error messages.
fn repr(text: &str) -> String {
    if text.contains('\'') && !text.contains('"') {
        format!("\"{text}\"")
    } else {
        format!("'{}'", text.replace('\'', "\\'"))
    }
}

const OPERATORS: [&str; 7] = [">=", "<=", "^", "~", ">", "<", "="];

fn split_operator(token: &str) -> (&str, &str) {
    for op in OPERATORS {
        if let Some(rest) = token.strip_prefix(op) {
            return (op, rest);
        }
    }
    ("", token)
}

/// Whitespace-separated tokens, with `>= 1.2.0` joined into `>=1.2.0`.
fn tokens(text: &str) -> Vec<String> {
    let mut result: Vec<String> = Vec::new();
    let mut glue = false;
    for word in text.split_whitespace() {
        if glue {
            result.last_mut().expect("glue follows a token").push_str(word);
        } else {
            result.push(word.to_owned());
        }
        // A token that is, or ends with, an operator joins the next one (the regex `(op)\s+` -> `\1`).
        let last = result.last().expect("just pushed");
        glue = OPERATORS.iter().any(|op| last.ends_with(op));
    }
    result
}

fn is_wildcard(text: &str) -> bool {
    matches!(text, "x" | "X" | "*")
}

/// `\d+|[xX*]` as a number or None for a wildcard.
fn partial_part(text: &str) -> Result<Option<u64>, ()> {
    if is_wildcard(text) {
        Ok(None)
    } else if is_number(text) {
        Ok(Some(text.parse().unwrap_or(u64::MAX)))
    } else {
        Err(())
    }
}

fn desugar(token: &str) -> Result<Vec<Comparator>, SemverError> {
    let (op, rest) = split_operator(token);
    let comparator = |op, version| Comparator { op, version };
    if rest.is_empty() || is_wildcard(rest) {
        return if matches!(op, "" | "=" | ">=" | "^" | "~" | "<=") {
            Ok(vec![comparator(Op::Ge, Version::floor(0, 0, 0))])
        } else {
            Err(SemverError(format!("{} matches nothing", repr(token))))
        };
    }
    let invalid = || SemverError(format!("invalid comparator {}", repr(token)));
    let (core, prerelease) = match rest.split_once('-') {
        Some((core, pre)) => (core, Some(pre)),
        None => (rest, None),
    };
    let prerelease: Vec<String> = match prerelease {
        Some(pre) if pre.split('.').all(is_identifier) => pre.split('.').map(str::to_owned).collect(),
        Some(_) => return Err(invalid()),
        None => Vec::new(),
    };
    let fields: Vec<&str> = core.split('.').collect();
    if fields.len() > 3 {
        return Err(invalid());
    }
    let mut parts = [None; 3];
    for (slot, field) in parts.iter_mut().zip(&fields) {
        *slot = partial_part(field).map_err(|_| invalid())?;
    }
    // A missing part counts like a wildcard.
    let present = |i: usize| i < fields.len() && parts[i].is_some();
    let (major, minor, patch) = (parts[0], if present(1) { parts[1] } else { None }, if present(2) { parts[2] } else { None });
    let Some(major) = major else { return Err(invalid()) };
    if minor.is_none() && patch.is_some() {
        return Err(invalid());
    }
    if !prerelease.is_empty() && patch.is_none() {
        return Err(SemverError(format!("invalid comparator {}: prerelease needs a full version", repr(token))));
    }
    let full = Version { major, minor: minor.unwrap_or(0), patch: patch.unwrap_or(0), prerelease };
    let next_line = match minor {
        None => Version::floor(major + 1, 0, 0),
        Some(minor) => Version::floor(major, minor + 1, 0),
    };

    if op == "^" {
        let upper = if major > 0 || minor.is_none() {
            Version::floor(major + 1, 0, 0)
        } else if minor.unwrap_or(0) > 0 || patch.is_none() {
            Version::floor(0, minor.unwrap_or(0) + 1, 0)
        } else {
            Version::floor(0, 0, patch.unwrap_or(0) + 1)
        };
        return Ok(vec![comparator(Op::Ge, full), comparator(Op::Lt, upper)]);
    }
    if op == "~" || (matches!(op, "" | "=") && patch.is_none()) {
        return Ok(vec![comparator(Op::Ge, full), comparator(Op::Lt, next_line)]);
    }
    let op = match op {
        "" | "=" => return Ok(vec![comparator(Op::Eq, full)]),
        ">=" => Op::Ge,
        "<=" => Op::Le,
        ">" => Op::Gt,
        _ => Op::Lt,
    };
    if patch.is_some() {
        return Ok(vec![comparator(op, full)]);
    }
    // Partial comparators: >1.2 means >=1.3.0, <=1.2 means <1.3.0-0.
    Ok(vec![match op {
        Op::Ge => comparator(Op::Ge, full),
        Op::Lt => comparator(Op::Lt, Version::floor(major, minor.unwrap_or(0), 0)),
        Op::Gt => comparator(Op::Ge, Version::new(next_line.major, next_line.minor, 0)),
        _ => comparator(Op::Lt, next_line),
    }])
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v(text: &str) -> Version {
        Version::parse(text).unwrap()
    }

    fn norm(text: &str) -> String {
        Range::parse(text).unwrap().to_string()
    }

    fn sat(version: &str, range: &str) -> bool {
        Range::parse(range).unwrap().satisfied_by(&v(version))
    }

    #[test]
    fn parses_versions() {
        assert_eq!(v("1.2.3").to_string(), "1.2.3");
        assert_eq!(v("1.0.0-alpha.1").prerelease, vec!["alpha", "1"]);
        for bad in ["1.2", "01.2.3", "1.2.3-", "1.2.3-a..b", "1.2.3.4", "v1.2.3", "", "1.2.3-01"] {
            assert!(Version::parse(bad).is_err(), "{bad}");
        }
        assert_eq!(
            Version::parse("1.2").unwrap_err().0,
            "invalid version '1.2' (expected MAJOR.MINOR.PATCH[-prerelease])"
        );
        assert_eq!(
            Version::parse("1.2.3-01").unwrap_err().0,
            "invalid version '1.2.3-01': numeric prerelease '01' has a leading zero"
        );
    }

    #[test]
    fn orders_by_precedence() {
        let chain = [
            "1.0.0-0", "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
            "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "2.0.0",
        ];
        for pair in chain.windows(2) {
            assert!(v(pair[0]) < v(pair[1]), "{} < {}", pair[0], pair[1]);
        }
        assert_eq!(v("1.0.0").cmp(&v("1.0.0")), Ordering::Equal);
    }

    #[test]
    fn normalises_like_semver_py() {
        let cases = [
            ("^1.2.3", ">=1.2.3 <2.0.0-0"),
            ("^1.0.0", ">=1.0.0 <2.0.0-0"),
            ("^0.2.3", ">=0.2.3 <0.3.0-0"),
            ("^0.0.3", ">=0.0.3 <0.0.4-0"),
            ("^0.0", ">=0.0.0 <0.1.0-0"),
            ("^0", ">=0.0.0 <1.0.0-0"),
            ("^1.x", ">=1.0.0 <2.0.0-0"),
            ("~1.2.3", ">=1.2.3 <1.3.0-0"),
            ("~1.2", ">=1.2.0 <1.3.0-0"),
            ("~1", ">=1.0.0 <2.0.0-0"),
            ("1.2.3", "=1.2.3"),
            ("=1.2.3", "=1.2.3"),
            ("1.2", ">=1.2.0 <1.3.0-0"),
            ("1.x", ">=1.0.0 <2.0.0-0"),
            ("1", ">=1.0.0 <2.0.0-0"),
            ("*", ">=0.0.0-0"),
            ("", ">=0.0.0-0"),
            ("x", ">=0.0.0-0"),
            (">=1.2", ">=1.2.0"),
            (">1.2", ">=1.3.0"),
            (">1", ">=2.0.0"),
            ("<1.2", "<1.2.0-0"),
            ("<=1.2", "<1.3.0-0"),
            ("<=1", "<2.0.0-0"),
            (">=1.0.0 <2.0.0", ">=1.0.0 <2.0.0"),
            (">= 1.0.0 < 2.0.0", ">=1.0.0 <2.0.0"),
            ("^1.0.0 || ^2.0.0", ">=1.0.0 <2.0.0-0 || >=2.0.0 <3.0.0-0"),
            ("1.0.0-rc.1", "=1.0.0-rc.1"),
            ("^1.2.3-beta.2", ">=1.2.3-beta.2 <2.0.0-0"),
            (">=1.0.0 <2.0.0-0", ">=1.0.0 <2.0.0-0"),
        ];
        for (range, expected) in cases {
            assert_eq!(norm(range), expected, "{range}");
        }
    }

    #[test]
    fn rejects_like_semver_py() {
        assert_eq!(Range::parse(">*").unwrap_err().0, "'>*' matches nothing");
        assert_eq!(Range::parse("<x").unwrap_err().0, "'<x' matches nothing");
        assert_eq!(Range::parse("x.1").unwrap_err().0, "invalid comparator 'x.1'");
        assert_eq!(Range::parse("1.x.3").unwrap_err().0, "invalid comparator '1.x.3'");
        assert_eq!(Range::parse("abc").unwrap_err().0, "invalid comparator 'abc'");
        assert_eq!(Range::parse("1.2.3.4").unwrap_err().0, "invalid comparator '1.2.3.4'");
        assert_eq!(
            Range::parse("1.2-beta").unwrap_err().0,
            "invalid comparator '1.2-beta': prerelease needs a full version"
        );
    }

    #[test]
    fn satisfies_with_prerelease_floors() {
        assert!(sat("1.0.0", "^1.0.0"));
        assert!(sat("1.9.9", "^1.0.0"));
        assert!(!sat("2.0.0", "^1.0.0"));
        // The -0 upper bound excludes 2.0.0 prereleases; npm's prerelease exclusion is not applied.
        assert!(!sat("2.0.0-alpha", "^1.0.0"));
        assert!(!sat("2.0.0-0", "^1.0.0"));
        assert!(sat("1.5.0-beta", "^1.0.0"));
        assert!(!sat("1.0.0-beta", "^1.0.0"));
        assert!(sat("0.0.0-0", "*"));
        assert!(sat("1.2.3", "1.2.3"));
        assert!(!sat("1.2.4", "1.2.3"));
        assert!(sat("1.3.0", ">1.2"));
        assert!(!sat("1.2.9", ">1.2"));
        assert!(sat("1.2.9", "<=1.2"));
        assert!(!sat("1.3.0-0", "<=1.2"));
        assert!(sat("3.1.0", "^1.0.0 || ^3.0.0"));
        assert!(!sat("2.1.0", "^1.0.0 || ^3.0.0"));
        assert!(sat("1.0.0", ">=1.0.0 <2.0.0-0"));
    }
}
