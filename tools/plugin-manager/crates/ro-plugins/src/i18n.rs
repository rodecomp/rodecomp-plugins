//! The window's texts in several languages. Each language is one `lang/<code>.lang` file of `key=value` lines,
//! embedded in the binary, so the exe stays a single file. A key missing from the chosen language falls back to
//! English, and a key missing from English shows the key itself: a gap never breaks the window.
//!
//! The first start uses the operating system's language when it is one of [`Lang::ALL`], else English; a language
//! the user picks in the window is remembered and wins from then on (see `gui::run`). The CLI stays in English: its
//! output is the same as `scripts/plugins.py`'s and scripts read it.
//!
//! Code uses the macros: `t!("key")` (a `&'static str`), `t!("key", name = value)` (fills `{name}`), and
//! `tn!("key", count)` (`key.one` for 1, `key.other` otherwise, `{count}` filled). `key!("key")` marks a key kept
//! for later; the tests check that every key the code names exists in every file.

use std::collections::HashMap;
use std::fmt::Display;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicU8, Ordering};

/// A supported language.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Lang {
    En,
    Es,
    PtBr,
    PtPt,
}

impl Lang {
    /// In the order the language menu lists them.
    pub const ALL: [Lang; 4] = [Lang::En, Lang::Es, Lang::PtBr, Lang::PtPt];
    /// Used when the system language is not supported, and for keys a language lacks.
    pub const FALLBACK: Lang = Lang::En;

    pub fn code(self) -> &'static str {
        match self {
            Lang::En => "en",
            Lang::Es => "es",
            Lang::PtBr => "pt-BR",
            Lang::PtPt => "pt-PT",
        }
    }

    pub fn from_code(code: &str) -> Option<Lang> {
        Lang::ALL.into_iter().find(|lang| lang.code().eq_ignore_ascii_case(code.trim()))
    }

    /// The language's name in that language, as the menu shows it.
    pub fn native_name(self) -> &'static str {
        match self {
            Lang::En => "English",
            Lang::Es => "Español",
            Lang::PtBr => "Português (Brasil)",
            Lang::PtPt => "Português (Portugal)",
        }
    }

    /// The supported language of a system locale: `pt-BR`, `pt_PT.UTF-8`, `es-419`, `en-US`. Portuguese outside
    /// Brazil reads as European Portuguese. `None` for any other language.
    pub fn from_locale(locale: &str) -> Option<Lang> {
        let tag = locale.split(['.', '@']).next().unwrap_or_default().replace('_', "-");
        let mut parts = tag.split('-');
        let language = parts.next().unwrap_or_default().to_ascii_lowercase();
        // The region is the first 2-letter or 3-digit subtag (a 4-letter one is a script: `zh-Hant-TW`).
        let region = parts.find(|part| part.len() == 2 || (part.len() == 3 && part.bytes().all(|b| b.is_ascii_digit())));
        match language.as_str() {
            "en" => Some(Lang::En),
            "es" => Some(Lang::Es),
            "pt" if region.is_some_and(|region| region.eq_ignore_ascii_case("BR")) => Some(Lang::PtBr),
            "pt" => Some(Lang::PtPt),
            _ => None,
        }
    }

    fn index(self) -> usize {
        Lang::ALL.iter().position(|lang| *lang == self).unwrap_or(0)
    }

    fn source(self) -> &'static str {
        match self {
            Lang::En => include_str!("../lang/en.lang"),
            Lang::Es => include_str!("../lang/es.lang"),
            Lang::PtBr => include_str!("../lang/pt-BR.lang"),
            Lang::PtPt => include_str!("../lang/pt-PT.lang"),
        }
    }

    /// The plugins' own texts (plugin.toml): `en` is the English they translate, written by scripts/plugin_texts.py.
    fn plugin_source(self) -> &'static str {
        match self {
            Lang::En => include_str!("../lang/plugins/en.lang"),
            Lang::Es => include_str!("../lang/plugins/es.lang"),
            Lang::PtBr => include_str!("../lang/plugins/pt-BR.lang"),
            Lang::PtPt => include_str!("../lang/plugins/pt-PT.lang"),
        }
    }
}

/// The language a start uses: the user's remembered choice, else the system's when supported, else English.
pub fn initial(chosen: Option<Lang>, system: Option<Lang>) -> Lang {
    chosen.or(system).unwrap_or(Lang::FALLBACK)
}

/// The operating system's language as a supported language, if it is one. `RO_PLUGINS_SYSTEM_LOCALE` (a locale
/// such as `es-ES`) stands in for the system's, to try another language without changing Windows.
pub fn system_language() -> Option<Lang> {
    let locale = std::env::var("RO_PLUGINS_SYSTEM_LOCALE").ok().filter(|locale| !locale.trim().is_empty()).or_else(os_locale)?;
    Lang::from_locale(&locale)
}

/// Windows: the first display language of the user (the language Windows itself is shown in), else the locale of
/// the regional format.
#[cfg(windows)]
fn os_locale() -> Option<String> {
    use windows_sys::Win32::Globalization::{GetUserDefaultLocaleName, GetUserPreferredUILanguages, MUI_LANGUAGE_NAME};
    let (mut count, mut length) = (0u32, 0u32);
    // SAFETY: a null buffer asks for the needed length; both out pointers are valid.
    if unsafe { GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &mut count, std::ptr::null_mut(), &mut length) } != 0 && length > 0 {
        let mut buffer = vec![0u16; length as usize];
        // SAFETY: the buffer holds `length` UTF-16 units, as the first call asked for.
        if unsafe { GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &mut count, buffer.as_mut_ptr(), &mut length) } != 0 {
            if let Some(first) = buffer.split(|unit| *unit == 0).find(|name| !name.is_empty()) {
                return Some(String::from_utf16_lossy(first));
            }
        }
    }
    let mut name = [0u16; 85]; // LOCALE_NAME_MAX_LENGTH
    // SAFETY: the buffer is valid for its length.
    let written = unsafe { GetUserDefaultLocaleName(name.as_mut_ptr(), name.len() as i32) };
    (written > 1).then(|| String::from_utf16_lossy(&name[..written as usize - 1]))
}

/// Elsewhere: the usual locale variables.
#[cfg(not(windows))]
fn os_locale() -> Option<String> {
    ["LC_ALL", "LC_MESSAGES", "LANG"].into_iter().filter_map(|name| std::env::var(name).ok()).find(|value| !value.is_empty())
}

/// One `.lang` file: `key=value` per line, `#` comments and blank lines skipped. In a value `\n` is a line break and
/// `\\` a backslash. Errors name the line.
pub fn parse(source: &str) -> Result<Vec<(&str, String)>, String> {
    let mut entries = Vec::new();
    for (index, line) in source.lines().enumerate() {
        let line = line.trim_start_matches('\u{feff}').trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let Some((key, value)) = line.split_once('=') else { return Err(format!("line {}: no `=`", index + 1)) };
        let key = key.trim();
        if key.is_empty() || !key.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'_') {
            return Err(format!("line {}: bad key `{key}`", index + 1));
        }
        entries.push((key, unescape(value.trim())));
    }
    Ok(entries)
}

fn unescape(value: &str) -> String {
    let mut text = String::with_capacity(value.len());
    let mut chars = value.chars();
    while let Some(c) = chars.next() {
        match (c, chars.clone().next()) {
            ('\\', Some('n')) => {
                text.push('\n');
                chars.next();
            }
            ('\\', Some('\\')) => {
                text.push('\\');
                chars.next();
            }
            _ => text.push(c),
        }
    }
    text
}

type Table = HashMap<&'static str, String>;

/// The lines of a file that parse; a malformed file is caught by the tests, and at run time its good lines count.
fn table(source: &'static str) -> Table {
    source.lines().filter_map(|line| parse(line).ok()).flatten().collect()
}

fn tables() -> &'static [Table; 4] {
    static TABLES: OnceLock<[Table; 4]> = OnceLock::new();
    TABLES.get_or_init(|| Lang::ALL.map(|lang| table(lang.source())))
}

fn plugin_tables() -> &'static [Table; 4] {
    static TABLES: OnceLock<[Table; 4]> = OnceLock::new();
    TABLES.get_or_init(|| Lang::ALL.map(|lang| table(lang.plugin_source())))
}

/// A plugin's own text (its description, a setting's or a choice's label and description) in the current language.
/// `key` is `<Plugin>.description`, `<Plugin>.<Setting>.label`… (see [`choice_key`]); `english` is the text in the
/// installed plugin.toml. The translation is used only while the English it was made from is still `english`, so a
/// plugin whose text changed shows its new English, never an outdated translation.
pub fn plugin_text<'a>(key: &str, english: &'a str) -> &'a str {
    let lang = current();
    if lang == Lang::En {
        return english;
    }
    let tables = plugin_tables();
    match (tables[Lang::En.index()].get(key), tables[lang.index()].get(key)) {
        (Some(source), Some(text)) if source == english => text,
        _ => english,
    }
}

/// A choice value as a key segment, as scripts/plugin_texts.py writes it: as is when it is `[A-Za-z0-9_]`, else `x`
/// and its UTF-8 bytes in hex.
pub fn choice_key(value: &str) -> String {
    if !value.is_empty() && value.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_') {
        value.to_owned()
    } else {
        std::iter::once("x".to_owned()).chain(value.bytes().map(|b| format!("{b:02x}"))).collect()
    }
}

static CURRENT: AtomicU8 = AtomicU8::new(0);

/// Switches every text to `lang`.
pub fn set(lang: Lang) {
    CURRENT.store(lang.index() as u8, Ordering::Relaxed);
}

pub fn current() -> Lang {
    Lang::ALL.get(CURRENT.load(Ordering::Relaxed) as usize).copied().unwrap_or(Lang::FALLBACK)
}

/// The text of `key` in `lang`, else in English, else the key itself.
pub fn lookup(lang: Lang, key: &'static str) -> &'static str {
    let tables = tables();
    tables[lang.index()].get(key).or_else(|| tables[Lang::FALLBACK.index()].get(key)).map_or(key, String::as_str)
}

/// The text of `key` in the current language.
pub fn text(key: &'static str) -> &'static str {
    lookup(current(), key)
}

/// The text of `key` with each `{name}` replaced by its argument. An unknown `{name}` stays as written.
pub fn format(key: &'static str, args: &[(&str, &dyn Display)]) -> String {
    fill(text(key), args)
}

/// `one` when `count` is 1, else `other`, with `{count}` and the other arguments filled.
pub fn plural(one: &'static str, other: &'static str, count: usize, args: &[(&str, &dyn Display)]) -> String {
    let mut all: Vec<(&str, &dyn Display)> = vec![("count", &count)];
    all.extend_from_slice(args);
    fill(text(if count == 1 { one } else { other }), &all)
}

fn fill(template: &str, args: &[(&str, &dyn Display)]) -> String {
    let mut out = String::with_capacity(template.len() + 16);
    let mut rest = template;
    while let Some(open) = rest.find('{') {
        out.push_str(&rest[..open]);
        let after = &rest[open + 1..];
        match after.find('}').map(|close| (&after[..close], close)) {
            Some((name, close)) if args.iter().any(|(arg, _)| *arg == name) => {
                let (_, value) = args.iter().find(|(arg, _)| *arg == name).expect("checked above");
                out.push_str(&value.to_string());
                rest = &after[close + 1..];
            }
            _ => {
                out.push('{');
                rest = after;
            }
        }
    }
    out.push_str(rest);
    out
}

/// `t!("key")` is the text of a key; `t!("key", name = value, …)` fills `{name}` placeholders.
macro_rules! t {
    ($key:literal) => {
        $crate::i18n::text($key)
    };
    ($key:literal, $($name:ident = $value:expr),+ $(,)?) => {
        $crate::i18n::format($key, &[$((stringify!($name), &$value as &dyn ::std::fmt::Display)),+])
    };
}

/// `tn!("key", count)`: `key.one` or `key.other` by count, `{count}` and any `name = value` filled.
macro_rules! tn {
    ($key:literal, $count:expr $(, $name:ident = $value:expr)* $(,)?) => {
        $crate::i18n::plural(concat!($key, ".one"), concat!($key, ".other"), $count, &[$((stringify!($name), &$value as &dyn ::std::fmt::Display)),*])
    };
}

/// A key stored now and shown later with [`text`]; the tests check it exists like the other macros' keys.
macro_rules! key {
    ($key:literal) => {
        $key
    };
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::{BTreeMap, BTreeSet};
    use std::path::Path;

    fn keys(lang: Lang) -> BTreeSet<&'static str> {
        parse(lang.source()).unwrap().into_iter().map(|(key, _)| key).collect()
    }

    /// `{name}` placeholders of a text, sorted.
    fn placeholders(text: &str) -> BTreeSet<String> {
        let mut found = BTreeSet::new();
        let mut rest = text;
        while let Some(open) = rest.find('{') {
            let after = &rest[open + 1..];
            let Some(close) = after.find('}') else { break };
            found.insert(after[..close].to_owned());
            rest = &after[close + 1..];
        }
        found
    }

    #[test]
    fn every_file_parses_with_the_same_keys_and_placeholders() {
        let english: BTreeMap<_, _> = parse(Lang::En.source()).unwrap().into_iter().collect();
        for lang in Lang::ALL {
            let entries = parse(lang.source()).unwrap_or_else(|error| panic!("{}.lang: {error}", lang.code()));
            let mut seen = BTreeSet::new();
            for (key, value) in &entries {
                assert!(seen.insert(*key), "{}.lang: `{key}` twice", lang.code());
                assert!(!value.is_empty(), "{}.lang: `{key}` is empty", lang.code());
                let base = english.get(key).unwrap_or_else(|| panic!("{}.lang: `{key}` is not in en.lang", lang.code()));
                assert_eq!(placeholders(value), placeholders(base), "{}.lang: `{key}` placeholders", lang.code());
            }
            assert_eq!(keys(lang), keys(Lang::En), "{}.lang and en.lang keys differ", lang.code());
        }
    }

    /// Every `t!("…")`, `tn!("…")` and `key!("…")` in the sources names keys that exist.
    #[test]
    fn every_key_the_code_uses_exists() {
        let english = keys(Lang::En);
        let mut used = BTreeSet::new();
        let mut stack = vec![Path::new(env!("CARGO_MANIFEST_DIR")).join("src")];
        while let Some(dir) = stack.pop() {
            for entry in std::fs::read_dir(&dir).unwrap().flatten() {
                let path = entry.path();
                if path.is_dir() {
                    stack.push(path);
                    continue;
                }
                if path.extension().is_none_or(|ext| ext != "rs") || path.ends_with("i18n.rs") {
                    continue;
                }
                let source = std::fs::read_to_string(&path).unwrap();
                for (macro_name, suffixes) in [("t!(\"", &[""][..]), ("key!(\"", &[""][..]), ("tn!(\"", &[".one", ".other"][..])] {
                    for (at, _) in source.match_indices(macro_name) {
                        // `format!("` ends in `t!("`: only a macro name that starts a word counts.
                        if source[..at].bytes().next_back().is_some_and(|b| b.is_ascii_alphanumeric() || b == b'_') {
                            continue;
                        }
                        let start = at + macro_name.len();
                        let end = start + source[start..].find('"').unwrap();
                        for suffix in suffixes {
                            used.insert(format!("{}{suffix}", &source[start..end]));
                        }
                    }
                }
            }
        }
        assert!(used.len() > 100, "found only {} keys: is the scan broken?", used.len());
        let missing: Vec<_> = used.iter().filter(|key| !english.contains(key.as_str())).collect();
        assert!(missing.is_empty(), "keys used in the code but not in en.lang: {missing:?}");
        let unused: Vec<_> = english.iter().filter(|key| !used.contains(**key)).collect();
        assert!(unused.is_empty(), "keys in en.lang that the code never uses: {unused:?}");
    }

    /// The plugin texts: every file parses, translations only name texts of the English snapshot, nothing is empty,
    /// and a translation applies only while its English is the plugin's current one.
    #[test]
    fn plugin_texts_follow_the_english_snapshot() {
        let english: BTreeMap<_, _> = parse(Lang::En.plugin_source()).unwrap().into_iter().collect();
        assert!(english.len() > 100, "the English snapshot is nearly empty");
        for lang in Lang::ALL {
            let entries = parse(lang.plugin_source()).unwrap_or_else(|error| panic!("plugins/{}.lang: {error}", lang.code()));
            let mut seen = BTreeSet::new();
            for (key, value) in &entries {
                assert!(seen.insert(*key), "plugins/{}.lang: `{key}` twice", lang.code());
                assert!(!value.is_empty(), "plugins/{}.lang: `{key}` is empty", lang.code());
                assert!(english.contains_key(key), "plugins/{}.lang: `{key}` is not in plugins/en.lang", lang.code());
            }
        }
        // A translation applies only to the English it was made from (lookup by language, without the global one).
        let tables = plugin_tables();
        let pick = |lang: Lang, key: &str, current: &'static str| -> &'static str {
            match (tables[Lang::En.index()].get(key), tables[lang.index()].get(key)) {
                (Some(source), Some(text)) if source == current => text.as_str(),
                _ => current,
            }
        };
        let (key, source) = english.iter().next().unwrap();
        let source: &'static str = Box::leak(source.clone().into_boxed_str());
        for lang in [Lang::PtBr, Lang::PtPt, Lang::Es] {
            assert_ne!(pick(lang, key, source), source, "{key} is not translated in {}", lang.code());
            assert_eq!(pick(lang, key, "A changed English text"), "A changed English text");
        }
        assert_eq!(choice_key("Custom"), "Custom");
        assert_eq!(choice_key("%"), "x25");
        assert_eq!(choice_key("a b"), "x612062");
    }

    #[test]
    fn system_locales_map_to_supported_languages() {
        for (locale, lang) in [
            ("pt-BR", Some(Lang::PtBr)),
            ("pt_BR.UTF-8", Some(Lang::PtBr)),
            ("pt-PT", Some(Lang::PtPt)),
            ("pt-AO", Some(Lang::PtPt)),
            ("pt", Some(Lang::PtPt)),
            ("en-US", Some(Lang::En)),
            ("en-GB", Some(Lang::En)),
            ("es-ES", Some(Lang::Es)),
            ("es-419", Some(Lang::Es)),
            ("es_MX.UTF-8@euro", Some(Lang::Es)),
            ("fr-FR", None),
            ("ko-KR", None),
            ("zh-Hant-TW", None),
            ("", None),
        ] {
            assert_eq!(Lang::from_locale(locale), lang, "{locale}");
        }
    }

    #[test]
    fn a_remembered_choice_wins_over_the_system_and_english_is_the_fallback() {
        assert_eq!(initial(None, Some(Lang::PtBr)), Lang::PtBr);
        assert_eq!(initial(None, Lang::from_locale("pt-PT")), Lang::PtPt);
        assert_eq!(initial(None, Lang::from_locale("es-ES")), Lang::Es);
        assert_eq!(initial(None, Lang::from_locale("en-US")), Lang::En);
        assert_eq!(initial(None, Lang::from_locale("ja-JP")), Lang::En);
        assert_eq!(initial(Some(Lang::Es), Some(Lang::PtBr)), Lang::Es);
        assert_eq!(initial(Some(Lang::En), None), Lang::En);
        for lang in Lang::ALL {
            assert_eq!(Lang::from_code(lang.code()), Some(lang));
        }
        assert_eq!(Lang::from_code("PT-br"), Some(Lang::PtBr));
        assert_eq!(Lang::from_code("fr"), None);
        assert_eq!(Lang::from_code(""), None);
    }

    #[test]
    fn lookups_fall_back_to_english_then_to_the_key() {
        assert_eq!(lookup(Lang::PtBr, "common.cancel"), "Cancelar");
        assert_eq!(lookup(Lang::Es, "common.cancel"), "Cancelar");
        assert_eq!(lookup(Lang::En, "common.cancel"), "Cancel");
        assert_eq!(lookup(Lang::PtPt, "no.such.key"), "no.such.key");
    }

    #[test]
    fn placeholders_are_filled_and_unknown_ones_kept() {
        let name = "Zoom";
        assert_eq!(fill("{name} turned on", &[("name", &name)]), "Zoom turned on");
        assert_eq!(fill("{a}{b} {c}", &[("a", &1), ("b", &2)]), "12 {c}");
        assert_eq!(fill("no braces", &[]), "no braces");
        assert_eq!(fill("{unclosed", &[]), "{unclosed");
        assert_eq!(unescape(r"a\nb \\ c"), "a\nb \\ c");
        assert!(parse("good=1\nbad line").is_err());
        assert!(parse("bad key=1").is_err());
        assert_eq!(parse("# comment\n\n a.b = x = y ").unwrap(), [("a.b", "x = y".to_owned())]);
    }
}
