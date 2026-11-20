//! `[settings.<Key>]` of a plugin's `<Name>.toml`: typed declarations, value validation, editing a `value` with
//! `toml_edit` (comments and layout stay), and the merge of the user's values on install.

use std::fmt;
use std::fs;
use std::path::{Component, Path};

use toml_edit::{DocumentMut, Item, TableLike};

/// A validated setting value.
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Bool(bool),
    Int(i64),
    Float(f64),
    /// string, path and enum.
    Str(String),
    List(Vec<Value>),
}

impl Value {
    pub fn to_toml(&self) -> toml_edit::Value {
        match self {
            Value::Bool(value) => (*value).into(),
            Value::Int(value) => (*value).into(),
            Value::Float(value) => (*value).into(),
            Value::Str(value) => value.as_str().into(),
            Value::List(items) => toml_edit::Value::Array(items.iter().map(Value::to_toml).collect()),
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        if let Value::Str(text) = self { Some(text) } else { None }
    }
}

/// The TOML form: `true`, `3`, `0.5`, `"text"`, `["a", "b"]`.
impl fmt::Display for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.to_toml())
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct PathKind {
    /// `kind = "directory"`; a file otherwise.
    pub directory: bool,
    /// File dialog filters such as `*.ico`.
    pub filter: Vec<String>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Element {
    String,
    Path(PathKind),
    Int,
}

/// One value of an enum with the text the manager shows for it.
#[derive(Debug, Clone, PartialEq)]
pub struct Choice {
    /// The stored value, one of `values`.
    pub value: String,
    /// `choices.<value>.label`, else the value.
    pub label: String,
    /// `choices.<value>.description`, else empty.
    pub description: String,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Kind {
    Bool,
    Int { min: Option<i64>, max: Option<i64> },
    Float { min: Option<f64>, max: Option<f64>, step: Option<f64> },
    String,
    Path(PathKind),
    Enum(Vec<Choice>),
    List(Element),
}

fn got(item: &Item) -> &'static str {
    match item {
        Item::Value(value) => match value {
            toml_edit::Value::String(_) => "a string",
            toml_edit::Value::Integer(_) => "an integer",
            toml_edit::Value::Float(_) => "a float",
            toml_edit::Value::Boolean(_) => "a boolean",
            toml_edit::Value::Datetime(_) => "a date-time",
            toml_edit::Value::Array(_) => "an array",
            toml_edit::Value::InlineTable(_) => "a table",
        },
        Item::Table(_) => "a table",
        Item::ArrayOfTables(_) => "an array of tables",
        Item::None => "nothing",
    }
}

fn string(item: &Item) -> Result<String, String> {
    item.as_str().map(str::to_owned).ok_or_else(|| format!("expected a string, got {}", got(item)))
}

fn integer(item: &Item) -> Result<i64, String> {
    item.as_integer().ok_or_else(|| format!("expected an integer, got {}", got(item)))
}

fn number(item: &Item) -> Result<f64, String> {
    item.as_float()
        .or_else(|| item.as_integer().map(|value| value as f64))
        .ok_or_else(|| format!("expected a number, got {}", got(item)))
}

/// An array of strings.
pub(crate) fn strings(item: &Item) -> Result<Vec<String>, String> {
    let array = item.as_array().ok_or_else(|| format!("expected an array of strings, got {}", got(item)))?;
    array
        .iter()
        .enumerate()
        .map(|(index, value)| {
            value.as_str().map(str::to_owned).ok_or_else(|| format!("element {}: expected a string", index + 1))
        })
        .collect()
}

impl Kind {
    pub fn name(&self) -> &'static str {
        match self {
            Kind::Bool => "bool",
            Kind::Int { .. } => "int",
            Kind::Float { .. } => "float",
            Kind::String => "string",
            Kind::Path(_) => "path",
            Kind::Enum(_) => "enum",
            Kind::List(Element::String) => "list of string",
            Kind::List(Element::Path(_)) => "list of path",
            Kind::List(Element::Int) => "list of int",
        }
    }

    /// Validates a value of this type: the type, `min`/`max`, the enum's `values`, each list element.
    pub fn check(&self, item: &Item) -> Result<Value, String> {
        match self {
            Kind::Bool => item.as_bool().map(Value::Bool).ok_or_else(|| format!("expected a boolean, got {}", got(item))),
            Kind::Int { min, max } => {
                let value = integer(item)?;
                if min.is_some_and(|min| value < min) {
                    return Err(format!("{value} is below the minimum {}", min.unwrap_or_default()));
                }
                if max.is_some_and(|max| value > max) {
                    return Err(format!("{value} is above the maximum {}", max.unwrap_or_default()));
                }
                Ok(Value::Int(value))
            }
            Kind::Float { min, max, .. } => {
                let value = number(item)?;
                if min.is_some_and(|min| value < min) {
                    return Err(format!("{value} is below the minimum {}", min.unwrap_or_default()));
                }
                if max.is_some_and(|max| value > max) {
                    return Err(format!("{value} is above the maximum {}", max.unwrap_or_default()));
                }
                Ok(Value::Float(value))
            }
            Kind::String | Kind::Path(_) => string(item).map(Value::Str),
            Kind::Enum(choices) => {
                let value = string(item)?;
                if choices.iter().any(|choice| choice.value == value) {
                    Ok(Value::Str(value))
                } else {
                    let values: Vec<&str> = choices.iter().map(|choice| choice.value.as_str()).collect();
                    Err(format!("\"{value}\" is not one of {}", values.join(", ")))
                }
            }
            Kind::List(element) => {
                let array = item.as_array().ok_or_else(|| format!("expected an array, got {}", got(item)))?;
                array
                    .iter()
                    .enumerate()
                    .map(|(index, value)| {
                        let item = Item::Value(value.clone());
                        match element {
                            Element::Int => integer(&item).map(Value::Int),
                            Element::String | Element::Path(_) => string(&item).map(Value::Str),
                        }
                        .map_err(|why| format!("element {}: {why}", index + 1))
                    })
                    .collect::<Result<_, _>>()
                    .map(Value::List)
            }
        }
    }
}

/// `when = { <key> = "<value>" }` or `{ <key> = ["<value>", …] }`: the setting applies, and is shown, only while the
/// enum setting `key` of the same plugin holds one of `values`.
#[derive(Debug, Clone, PartialEq)]
pub struct Condition {
    pub key: String,
    pub values: Vec<String>,
}

impl Condition {
    fn parse(item: &Item) -> Result<Condition, String> {
        let table = item.as_table_like().ok_or_else(|| format!("when: expected a table, got {}", got(item)))?;
        let mut entries = table.iter();
        let (Some((key, values)), None) = (entries.next(), entries.next()) else {
            return Err("when: expected exactly one setting".to_owned());
        };
        let values = match values.as_str() {
            Some(value) => vec![value.to_owned()],
            None => strings(values).map_err(|why| format!("when.{key}: {why}"))?,
        };
        if values.is_empty() {
            return Err(format!("when.{key}: must not be empty"));
        }
        Ok(Condition { key: key.to_owned(), values })
    }
}

/// Checks that each setting's `when` names another enum setting of `settings` and only values of it.
pub(crate) fn check_conditions(settings: &[Setting]) -> Result<(), String> {
    for setting in settings {
        let Some(condition) = &setting.when else { continue };
        let key = &setting.key;
        let target = settings
            .iter()
            .find(|other| other.key == condition.key && other.key != *key)
            .ok_or_else(|| format!("settings.{key}: when: {} is not another setting", condition.key))?;
        let Kind::Enum(choices) = &target.kind else {
            return Err(format!("settings.{key}: when: {} is not an enum", condition.key));
        };
        if let Some(value) = condition.values.iter().find(|value| !choices.iter().any(|choice| &choice.value == *value)) {
            return Err(format!("settings.{key}: when: \"{value}\" is not one of the values of {}", condition.key));
        }
    }
    Ok(())
}

/// The order the manager shows settings in: file order, except that each setting with `when` comes right after its
/// controlling setting (after that one's earlier dependents).
pub fn display_order(settings: &[Setting]) -> Vec<&Setting> {
    fn place<'a>(settings: &'a [Setting], index: usize, placed: &mut [bool], order: &mut Vec<&'a Setting>) {
        placed[index] = true;
        order.push(&settings[index]);
        for dependent in 0..settings.len() {
            if !placed[dependent] && settings[dependent].when.as_ref().is_some_and(|when| when.key == settings[index].key) {
                place(settings, dependent, placed, order);
            }
        }
    }
    let mut placed = vec![false; settings.len()];
    let mut order = Vec::with_capacity(settings.len());
    // Roots first; the second pass places settings whose conditions form a cycle.
    for pass in [false, true] {
        for index in 0..settings.len() {
            if !placed[index] && (pass || settings[index].when.is_none()) {
                place(settings, index, &mut placed, &mut order);
            }
        }
    }
    order
}

#[derive(Debug, Clone, PartialEq)]
pub struct Setting {
    pub key: String,
    pub kind: Kind,
    /// `label`, else the key.
    pub label: String,
    pub description: String,
    pub default: Value,
    /// The installed `value`, when present and valid.
    pub value: Option<Value>,
    /// Why the installed `value` is invalid (the default applies then).
    pub error: Option<String>,
    /// `when`: the setting applies only for some values of another setting.
    pub when: Option<Condition>,
}

impl Setting {
    /// Whether the setting applies, given the value of each other setting (`value_of(key)`): always without `when`,
    /// else while the controlling setting holds one of the listed values.
    pub fn is_shown<'a>(&self, value_of: impl Fn(&str) -> Option<&'a Value>) -> bool {
        self.when.as_ref().is_none_or(|condition| {
            value_of(&condition.key)
                .and_then(Value::as_str)
                .is_some_and(|value| condition.values.iter().any(|shown| shown == value))
        })
    }

    /// What the host uses: the valid `value`, else `default`.
    pub fn current(&self) -> &Value {
        self.value.as_ref().unwrap_or(&self.default)
    }

    /// Whether the file has a `value` (valid or not), so there is something to reset.
    pub fn is_set(&self) -> bool {
        self.value.is_some() || self.error.is_some()
    }

    /// Whether what the host uses differs from the declared default, or the file holds an invalid `value`: the
    /// setting shows as changed and can be reset. A `value` equal to the default is not a change.
    pub fn is_changed(&self) -> bool {
        self.error.is_some() || self.current() != &self.default
    }

    /// Parses `[settings.<key>]`; an error makes the whole file invalid.
    pub(crate) fn parse(key: &str, table: &dyn TableLike) -> Result<Setting, String> {
        let field = |name: &str| table.get(name);
        let kind_name = field("type").ok_or("needs a type")?.as_str().ok_or("type: expected a string")?;
        let path_kind = || -> Result<PathKind, String> {
            let directory = match field("kind") {
                None => false,
                Some(item) => match item.as_str() {
                    Some("file") => false,
                    Some("directory") => true,
                    _ => return Err("kind: expected \"file\" or \"directory\"".to_owned()),
                },
            };
            let filter = field("filter").map(strings).transpose().map_err(|why| format!("filter: {why}"))?.unwrap_or_default();
            Ok(PathKind { directory, filter })
        };
        let int = |name: &str| field(name).map(integer).transpose().map_err(|why| format!("{name}: {why}"));
        let float = |name: &str| field(name).map(number).transpose().map_err(|why| format!("{name}: {why}"));
        let (kind, extra): (Kind, &[&str]) = match kind_name {
            "bool" => (Kind::Bool, &[]),
            "int" => (Kind::Int { min: int("min")?, max: int("max")? }, &["min", "max"]),
            "float" => (Kind::Float { min: float("min")?, max: float("max")?, step: float("step")? }, &["min", "max", "step"]),
            "string" => (Kind::String, &[]),
            "path" => (Kind::Path(path_kind()?), &["kind", "filter"]),
            "enum" => {
                let values = strings(field("values").ok_or("an enum needs values")?).map_err(|why| format!("values: {why}"))?;
                if values.is_empty() {
                    return Err("values: must not be empty".to_owned());
                }
                (Kind::Enum(enum_choices(values, field("choices"))?), &["values", "choices"])
            }
            "list" => match field("of").and_then(Item::as_str) {
                Some("string") => (Kind::List(Element::String), &["of"]),
                Some("int") => (Kind::List(Element::Int), &["of"]),
                Some("path") => (Kind::List(Element::Path(path_kind()?)), &["of", "kind", "filter"]),
                _ => return Err("a list needs of = \"string\", \"path\" or \"int\"".to_owned()),
            },
            other => return Err(format!("unknown type \"{other}\"")),
        };
        for (name, _) in table.iter() {
            if !["type", "label", "description", "default", "value", "when"].contains(&name) && !extra.contains(&name) {
                return Err(format!("unknown key {name} for type {kind_name}"));
            }
        }
        let text = |name: &str| field(name).map(string).transpose().map_err(|why| format!("{name}: {why}"));
        let label = text("label")?.unwrap_or_else(|| key.to_owned());
        let description = text("description")?.unwrap_or_default();
        let when = field("when").map(Condition::parse).transpose()?;
        let default = kind.check(field("default").ok_or("needs a default")?).map_err(|why| format!("default: {why}"))?;
        let (value, error) = match field("value") {
            None => (None, None),
            Some(item) => match kind.check(item) {
                Ok(value) => (Some(value), None),
                Err(why) => (None, Some(why)),
            },
        };
        Ok(Setting { key: key.to_owned(), kind, label, description, default, value, error, when })
    }
}

/// The choices of an enum: each of `values` with its text from the optional `choices` table
/// (`<value> = { label = "…", description = "…" }`), whose keys must all be values.
fn enum_choices(values: Vec<String>, table: Option<&Item>) -> Result<Vec<Choice>, String> {
    let mut choices: Vec<Choice> =
        values.into_iter().map(|value| Choice { label: value.clone(), value, description: String::new() }).collect();
    let Some(item) = table else { return Ok(choices) };
    let table = item.as_table_like().ok_or_else(|| format!("choices: expected a table, got {}", got(item)))?;
    for (value, entry) in table.iter() {
        let choice = choices
            .iter_mut()
            .find(|choice| choice.value == value)
            .ok_or_else(|| format!("choices: \"{value}\" is not one of values"))?;
        let entry = entry.as_table_like().ok_or_else(|| format!("choices.{value}: expected a table, got {}", got(entry)))?;
        for (name, text) in entry.iter() {
            let text = string(text).map_err(|why| format!("choices.{value}.{name}: {why}"))?;
            match name {
                "label" => choice.label = text,
                "description" => choice.description = text,
                _ => return Err(format!("choices.{value}: unknown key {name}")),
            }
        }
    }
    Ok(choices)
}

/// The table of `[settings.<key>]` in a document.
fn setting_table<'a>(document: &'a mut DocumentMut, key: &str) -> Option<&'a mut dyn TableLike> {
    document.get_mut("settings")?.as_table_like_mut()?.get_mut(key)?.as_table_like_mut()
}

/// Sets (`Some`) or removes (`None`) the `value` of `[settings.<key>]`. Only that key changes; a replaced value keeps
/// its comment.
pub fn edit_value(document: &mut DocumentMut, key: &str, value: Option<toml_edit::Value>) -> Result<(), String> {
    let table = setting_table(document, key).ok_or_else(|| format!("no [settings.{key}]"))?;
    match value {
        Some(mut value) => {
            if let Some(old) = table.get("value").and_then(Item::as_value) {
                *value.decor_mut() = old.decor().clone();
            } else {
                value.decor_mut().clear();
            }
            table.insert("value", Item::Value(value));
        }
        None => {
            table.remove("value");
        }
    }
    Ok(())
}

/// Writes `text` to `path` through a temporary file, so a reader never sees half a file.
pub(crate) fn write_atomically(path: &Path, text: &str) -> std::io::Result<()> {
    let temporary = path.with_extension("toml.tmp");
    fs::write(&temporary, text)?;
    fs::rename(&temporary, path)
}

/// A value of the old installed file that the new one does not keep.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Dropped {
    pub key: String,
    /// The value in TOML form.
    pub value: String,
    pub why: String,
}

/// The new installed file: `new_text` (the build output) plus each `value` of `old_text` (the old installed file)
/// that is valid under the new declaration. Returns the text and the values that were dropped.
pub fn merge(new_text: &str, new_settings: &[Setting], old_text: Option<&str>) -> Result<(String, Vec<Dropped>), String> {
    let mut document: DocumentMut = new_text.parse().map_err(|error: toml_edit::TomlError| error.message().to_owned())?;
    let mut dropped = Vec::new();
    let old: Option<DocumentMut> = old_text.and_then(|text| text.parse().ok());
    let old_settings = old.as_ref().and_then(|old| old.get("settings")).and_then(Item::as_table_like);
    for (key, item) in old_settings.into_iter().flat_map(|settings| settings.iter()) {
        let Some(value) = item.as_table_like().and_then(|table| table.get("value")) else { continue };
        let shown = value.as_value().map_or_else(|| got(value).to_owned(), |value| value.to_string().trim().to_owned());
        let why = match new_settings.iter().find(|setting| setting.key == key) {
            None => "no longer a setting".to_owned(),
            Some(setting) => match setting.kind.check(value) {
                Ok(_) => {
                    edit_value(&mut document, key, value.as_value().cloned())?;
                    continue;
                }
                Err(why) => why,
            },
        };
        dropped.push(Dropped { key: key.to_owned(), value: shown, why });
    }
    Ok((document.to_string(), dropped))
}

/// A picked path as stored: relative to `client` with `/` separators when inside it, else as picked.
pub fn client_relative(client: &Path, picked: &Path) -> String {
    let absolute = |path: &Path| std::path::absolute(path).unwrap_or_else(|_| path.to_owned());
    let (client, picked) = (absolute(client), absolute(picked));
    let same = |a: &Component, b: &Component| {
        let (a, b) = (a.as_os_str().to_string_lossy(), b.as_os_str().to_string_lossy());
        if cfg!(windows) { a.eq_ignore_ascii_case(&b) } else { a == b }
    };
    let base: Vec<Component> = client.components().collect();
    let all: Vec<Component> = picked.components().collect();
    if all.len() > base.len() && base.iter().zip(&all).all(|(a, b)| same(a, b)) {
        let rest: Vec<String> = all[base.len()..].iter().map(|part| part.as_os_str().to_string_lossy().into_owned()).collect();
        return rest.join("/");
    }
    picked.to_string_lossy().into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn declared(text: &str) -> Result<Setting, String> {
        let document: DocumentMut = text.parse().unwrap();
        Setting::parse("K", document.as_table())
    }

    #[test]
    fn parses_every_type() {
        let int = declared("type = \"int\"\nmin = 1\nmax = 5\ndefault = 2\nvalue = 4\nlabel = \"Count\"").unwrap();
        assert_eq!(int.kind, Kind::Int { min: Some(1), max: Some(5) });
        assert_eq!((int.label.as_str(), int.current()), ("Count", &Value::Int(4)));
        let float = declared("type = \"float\"\nmin = 0\nmax = 1.5\nstep = 0.1\ndefault = 1").unwrap();
        assert_eq!(float.default, Value::Float(1.0));
        assert_eq!(float.label, "K");
        let path = declared("type = \"path\"\nkind = \"directory\"\ndefault = \"x\"").unwrap();
        assert_eq!(path.kind, Kind::Path(PathKind { directory: true, filter: vec![] }));
        let list = declared("type = \"list\"\nof = \"path\"\nfilter = [\"*.grf\"]\ndefault = []\nvalue = [\"a.grf\"]").unwrap();
        assert_eq!(list.kind, Kind::List(Element::Path(PathKind { directory: false, filter: vec!["*.grf".into()] })));
        assert_eq!(list.current(), &Value::List(vec![Value::Str("a.grf".into())]));
        let choice = declared("type = \"enum\"\nvalues = [\"a\", \"b\"]\ndefault = \"b\"").unwrap();
        assert_eq!(choice.current().as_str(), Some("b"));
        assert_eq!(declared("type = \"bool\"\ndefault = true").unwrap().default, Value::Bool(true));
    }

    #[test]
    fn parses_enum_choices() {
        let setting = declared(
            "type = \"enum\"\nvalues = [\"A\", \"B\"]\ndefault = \"A\"\n[choices]\nA = { label = \"First\", description = \"The first one.\" }",
        )
        .unwrap();
        assert_eq!(
            setting.kind,
            Kind::Enum(vec![
                Choice { value: "A".into(), label: "First".into(), description: "The first one.".into() },
                Choice { value: "B".into(), label: "B".into(), description: String::new() },
            ])
        );
        for (text, error) in [
            ("type = \"enum\"\nvalues = [\"A\"]\ndefault = \"A\"\n[choices]\nZ = { label = \"Z\" }", "choices: \"Z\" is not one of values"),
            ("type = \"enum\"\nvalues = [\"A\"]\ndefault = \"A\"\nchoices = 1", "choices: expected a table, got an integer"),
            ("type = \"enum\"\nvalues = [\"A\"]\ndefault = \"A\"\n[choices]\nA = \"x\"", "choices.A: expected a table, got a string"),
            ("type = \"enum\"\nvalues = [\"A\"]\ndefault = \"A\"\n[choices]\nA = { icon = \"x\" }", "choices.A: unknown key icon"),
            ("type = \"enum\"\nvalues = [\"A\"]\ndefault = \"A\"\n[choices]\nA = { label = 1 }", "choices.A.label: expected a string, got an integer"),
            ("type = \"int\"\ndefault = 1\n[choices]\nA = {}", "unknown key choices for type int"),
        ] {
            assert_eq!(declared(text).unwrap_err(), error, "{text}");
        }
    }

    #[test]
    fn rejects_bad_declarations() {
        for (text, error) in [
            ("default = 1", "needs a type"),
            ("type = \"color\"\ndefault = 1", "unknown type \"color\""),
            ("type = \"int\"", "needs a default"),
            ("type = \"int\"\ndefault = \"1\"", "default: expected an integer, got a string"),
            ("type = \"int\"\nmax = 3\ndefault = 4", "default: 4 is above the maximum 3"),
            ("type = \"int\"\nmin = 1.5\ndefault = 4", "min: expected an integer, got a float"),
            ("type = \"enum\"\ndefault = \"a\"", "an enum needs values"),
            ("type = \"enum\"\nvalues = []\ndefault = \"a\"", "values: must not be empty"),
            ("type = \"enum\"\nvalues = [\"a\"]\ndefault = \"b\"", "default: \"b\" is not one of a"),
            ("type = \"list\"\ndefault = []", "a list needs of = \"string\", \"path\" or \"int\""),
            ("type = \"list\"\nof = \"list\"\ndefault = []", "a list needs of = \"string\", \"path\" or \"int\""),
            ("type = \"list\"\nof = \"int\"\ndefault = [1, \"x\"]", "default: element 2: expected an integer, got a string"),
            ("type = \"list\"\nof = \"string\"\nfilter = []\ndefault = []", "unknown key filter for type list"),
            ("type = \"string\"\nmin = 1\ndefault = \"\"", "unknown key min for type string"),
            ("type = \"path\"\nkind = \"folder\"\ndefault = \"\"", "kind: expected \"file\" or \"directory\""),
            ("type = \"bool\"\nlabel = 1\ndefault = true", "label: expected a string, got an integer"),
        ] {
            assert_eq!(declared(text).unwrap_err(), error, "{text}");
        }
    }

    fn plugin(text: &str) -> Result<Vec<Setting>, String> {
        let document: DocumentMut = text.parse().unwrap();
        let settings: Vec<Setting> = document["settings"]
            .as_table_like()
            .unwrap()
            .iter()
            .map(|(key, item)| Setting::parse(key, item.as_table_like().unwrap()))
            .collect::<Result<_, _>>()?;
        check_conditions(&settings).map(|()| settings)
    }

    const MODE: &str = "[settings.Mode]\ntype = \"enum\"\nvalues = [\"Off\", \"Beep\", \"Icon\"]\ndefault = \"Off\"\n";

    #[test]
    fn parses_conditions() {
        let one = declared("type = \"int\"\ndefault = 1\nwhen = { Mode = \"Beep\" }").unwrap();
        assert_eq!(one.when, Some(Condition { key: "Mode".into(), values: vec!["Beep".into()] }));
        let two = declared("type = \"int\"\ndefault = 1\n[when]\nMode = [\"Beep\", \"Icon\"]").unwrap();
        assert_eq!(two.when, Some(Condition { key: "Mode".into(), values: vec!["Beep".into(), "Icon".into()] }));
        for (text, error) in [
            ("type = \"int\"\ndefault = 1\nwhen = \"Mode\"", "when: expected a table, got a string"),
            ("type = \"int\"\ndefault = 1\nwhen = {}", "when: expected exactly one setting"),
            ("type = \"int\"\ndefault = 1\nwhen = { Mode = \"Beep\", Other = \"A\" }", "when: expected exactly one setting"),
            ("type = \"int\"\ndefault = 1\nwhen = { Mode = [] }", "when.Mode: must not be empty"),
            ("type = \"int\"\ndefault = 1\nwhen = { Mode = 1 }", "when.Mode: expected an array of strings, got an integer"),
        ] {
            assert_eq!(declared(text).unwrap_err(), error, "{text}");
        }
        assert!(plugin(&format!("{MODE}[settings.N]\ntype = \"int\"\ndefault = 1\nwhen = {{ Mode = [\"Beep\", \"Icon\"] }}\n")).is_ok());
        for (text, error) in [
            ("[settings.N]\ntype = \"int\"\ndefault = 1\nwhen = { Nope = \"Beep\" }\n", "settings.N: when: Nope is not another setting"),
            ("[settings.N]\ntype = \"int\"\ndefault = 1\nwhen = { N = \"Beep\" }\n", "settings.N: when: N is not another setting"),
            (
                "[settings.B]\ntype = \"bool\"\ndefault = true\n[settings.N]\ntype = \"int\"\ndefault = 1\nwhen = { B = \"true\" }\n",
                "settings.N: when: B is not an enum",
            ),
            ("[settings.N]\ntype = \"int\"\ndefault = 1\nwhen = { Mode = \"Loud\" }\n", "settings.N: when: \"Loud\" is not one of the values of Mode"),
        ] {
            let text = format!("{MODE}{text}");
            assert_eq!(plugin(&text).unwrap_err(), error, "{text}");
        }
    }

    #[test]
    fn shown_only_for_the_listed_values() {
        let settings = plugin(&format!(
            "{MODE}value = \"Beep\"\n[settings.Sound]\ntype = \"string\"\ndefault = \"\"\nwhen = {{ Mode = [\"Beep\", \"Icon\"] }}\n[settings.Icon]\ntype = \"string\"\ndefault = \"\"\nwhen = {{ Mode = \"Icon\" }}\n"
        ))
        .unwrap();
        let current = |key: &str| settings.iter().find(|setting| setting.key == key).map(Setting::current);
        assert!(settings[0].is_shown(current));
        assert!(settings[1].is_shown(current));
        assert!(!settings[2].is_shown(current));
        let icon = Value::Str("Icon".into());
        assert!(settings[2].is_shown(|key| (key == "Mode").then_some(&icon)));
        assert!(!settings[2].is_shown(|_| None));
    }

    #[test]
    fn dependents_follow_their_controller() {
        let settings = plugin(&format!(
            "[settings.Sound]\ntype = \"string\"\ndefault = \"\"\nwhen = {{ Mode = \"Beep\" }}\n[settings.Other]\ntype = \"bool\"\ndefault = true\n{MODE}[settings.Icon]\ntype = \"string\"\ndefault = \"\"\nwhen = {{ Mode = \"Icon\" }}\n"
        ))
        .unwrap();
        let keys: Vec<&str> = display_order(&settings).iter().map(|setting| setting.key.as_str()).collect();
        assert_eq!(keys, ["Other", "Mode", "Sound", "Icon"]);
    }

    #[test]
    fn changed_means_different_from_the_default() {
        assert!(!declared("type = \"int\"\ndefault = 1").unwrap().is_changed());
        assert!(declared("type = \"int\"\ndefault = 1\nvalue = 2").unwrap().is_changed());
        // A value equal to the default is set (there is a line to remove) but not changed.
        let same = declared("type = \"bool\"\ndefault = true\nvalue = true").unwrap();
        assert!(same.is_set() && !same.is_changed());
        assert!(declared("type = \"list\"\nof = \"string\"\ndefault = []\nvalue = [\"a\"]").unwrap().is_changed());
        // An invalid value applies the default, but is still a change to reset.
        assert!(declared("type = \"int\"\nmin = 0\ndefault = 1\nvalue = -1").unwrap().is_changed());
    }

    #[test]
    fn invalid_values_fall_back_to_the_default() {
        for (text, error) in [
            ("type = \"int\"\nmin = 0\ndefault = 1\nvalue = -1", "-1 is below the minimum 0"),
            ("type = \"bool\"\ndefault = true\nvalue = \"yes\"", "expected a boolean, got a string"),
            ("type = \"enum\"\nvalues = [\"a\"]\ndefault = \"a\"\nvalue = \"z\"", "\"z\" is not one of a"),
            ("type = \"list\"\nof = \"path\"\ndefault = []\nvalue = [\"a\", 2]", "element 2: expected a string, got an integer"),
        ] {
            let setting = declared(text).unwrap();
            assert_eq!(setting.error.as_deref(), Some(error), "{text}");
            assert_eq!(setting.current(), &setting.default);
            assert!(setting.is_set());
        }
    }

    #[test]
    fn edits_keep_comments_and_layout() {
        let text = "# top\nname = \"A\"\n\n[settings.Title]  # the title\ntype = \"string\"\ndefault = \"x\" # default\n\n[settings.N]\ntype = \"int\"\ndefault = 1\nvalue = 2 # mine\n";
        let mut document: DocumentMut = text.parse().unwrap();
        edit_value(&mut document, "Title", Some("hello".into())).unwrap();
        edit_value(&mut document, "N", Some(5.into())).unwrap();
        assert_eq!(
            document.to_string(),
            "# top\nname = \"A\"\n\n[settings.Title]  # the title\ntype = \"string\"\ndefault = \"x\" # default\nvalue = \"hello\"\n\n[settings.N]\ntype = \"int\"\ndefault = 1\nvalue = 5 # mine\n"
        );
        edit_value(&mut document, "Title", None).unwrap();
        edit_value(&mut document, "N", None).unwrap();
        assert_eq!(document.to_string(), "# top\nname = \"A\"\n\n[settings.Title]  # the title\ntype = \"string\"\ndefault = \"x\" # default\n\n[settings.N]\ntype = \"int\"\ndefault = 1\n");
        assert_eq!(edit_value(&mut document, "Nope", None).unwrap_err(), "no [settings.Nope]");
    }

    #[test]
    fn relative_inside_the_client_only() {
        let client = std::env::temp_dir().join("client");
        assert_eq!(client_relative(&client, &client.join("plugins").join("a.ico")), "plugins/a.ico");
        let outside = std::env::temp_dir().join("other").join("a.ico");
        assert_eq!(client_relative(&client, &outside), outside.to_string_lossy());
    }
}
