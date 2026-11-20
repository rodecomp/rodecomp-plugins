//! The plugin list's pure logic: the category tree, the case-insensitive search and the description excerpt.

use std::collections::BTreeMap;
use std::ops::Range;

/// The group of plugins without a manifest (refused DLLs); always last.
pub const UNACCEPTED: &str = "Not accepted";

/// Characters of description kept on each side of a match in an excerpt.
const EXCERPT_CONTEXT: usize = 20;

/// Where a search matched a plugin; `None` ranges mean the field did not match. Byte ranges into the original text.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Hit {
    pub name: Option<Range<usize>>,
    pub description: Option<Range<usize>>,
    pub category: bool,
}

/// One category of the tree: `None` is the group of refused DLLs.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Group {
    pub category: Option<String>,
    pub names: Vec<String>,
}

impl Group {
    pub fn title(&self) -> &str {
        self.category.as_deref().unwrap_or(UNACCEPTED)
    }
}

/// The first case-insensitive occurrence of `needle` in `haystack`, as a byte range of `haystack`. Each char is
/// lowercased on its own, so a char whose lowercase is longer (in bytes or chars) still maps back to its own bytes.
pub fn find(haystack: &str, needle: &str) -> Option<Range<usize>> {
    let needle: Vec<char> = needle.chars().flat_map(char::to_lowercase).collect();
    if needle.is_empty() {
        return None;
    }
    // Each lowercased char with the byte range of the original char it came from.
    let folded: Vec<(char, usize, usize)> = haystack
        .char_indices()
        .flat_map(|(start, c)| {
            let end = start + c.len_utf8();
            let lower: Vec<char> = if c.is_ascii() { vec![c.to_ascii_lowercase()] } else { c.to_lowercase().collect() };
            lower.into_iter().map(move |l| (l, start, end))
        })
        .collect();
    folded.windows(needle.len()).find(|window| window.iter().zip(&needle).all(|((c, _, _), n)| c == n)).map(|window| window[0].1..window[needle.len() - 1].2)
}

/// Matches a plugin against a search; `None` when nothing matches. An empty (or blank) needle matches everything
/// without ranges.
pub fn hit(needle: &str, name: &str, description: &str, category: &str) -> Option<Hit> {
    let needle = needle.trim();
    if needle.is_empty() {
        return Some(Hit::default());
    }
    let hit = Hit { name: find(name, needle), description: find(description, needle), category: find(category, needle).is_some() };
    (hit.name.is_some() || hit.description.is_some() || hit.category).then_some(hit)
}

/// A one-line slice of `text` around `range` (about `EXCERPT_CONTEXT` chars each side, ellipsis where cut, line breaks
/// and tabs as spaces), with the range moved into the excerpt.
pub fn excerpt(text: &str, range: Range<usize>) -> (String, Range<usize>) {
    let before: Vec<usize> = text[..range.start].char_indices().map(|(i, _)| i).collect();
    let start = before.len().checked_sub(EXCERPT_CONTEXT).map_or(0, |index| before[index]);
    let end = text[range.end..].char_indices().nth(EXCERPT_CONTEXT).map_or(text.len(), |(i, _)| range.end + i);
    let (lead, tail) = (if start > 0 { "…" } else { "" }, if end < text.len() { "…" } else { "" });
    // ASCII whitespace to a space keeps every byte offset.
    let body: String = text[start..end].chars().map(|c| if c.is_ascii_whitespace() { ' ' } else { c }).collect();
    let shift = lead.len() as isize - start as isize;
    let moved = (range.start as isize + shift) as usize..(range.end as isize + shift) as usize;
    (format!("{lead}{body}{tail}"), moved)
}

/// Groups names by category: categories sorted case-insensitively, names sorted case-insensitively inside, names
/// without a category (`None`) in a last group of their own. Empty groups are left out.
pub fn tree<'a>(names: impl IntoIterator<Item = &'a str>, category: impl Fn(&str) -> Option<&'a str>) -> Vec<Group> {
    let mut groups: Vec<Group> = Vec::new();
    for name in names {
        let key = category(name).map(str::to_owned);
        match groups.iter_mut().find(|group| group.category == key) {
            Some(group) => group.names.push(name.to_owned()),
            None => groups.push(Group { category: key, names: vec![name.to_owned()] }),
        }
    }
    for group in &mut groups {
        group.names.sort_by_key(|name| name.to_lowercase());
    }
    groups.sort_by_key(|group| (group.category.is_none(), group.category.as_ref().map(|c| (c.to_lowercase(), c.clone()))));
    groups
}

/// The list as shown for a search: the flat attention group on top, then the category tree without those plugins.
/// Only matching plugins are kept (all of them for an empty search), and groups left empty are dropped.
#[derive(Debug, Default)]
pub struct Listing {
    pub attention: Vec<String>,
    pub groups: Vec<Group>,
    pub hits: BTreeMap<String, Hit>,
}

impl Listing {
    /// `manifest` gives a plugin's (category, description); `None` for a refused DLL. `keep` filters further (the
    /// All / On / ★ view); a plugin that needs attention is always kept, so a problem never hides.
    pub fn build<'a>(
        needle: &str,
        names: &'a [String],
        needs_attention: impl Fn(&str) -> bool,
        keep: impl Fn(&str) -> bool,
        manifest: impl Fn(&str) -> Option<(&'a str, &'a str)>,
    ) -> Listing {
        let hits: BTreeMap<String, Hit> = names
            .iter()
            .filter(|name| needs_attention(name) || keep(name))
            .filter_map(|name| {
                let (category, description) = manifest(name).unwrap_or((UNACCEPTED, ""));
                hit(needle, name, description, category).map(|hit| (name.clone(), hit))
            })
            .collect();
        let shown = names.iter().filter(|name| hits.contains_key(*name));
        let (attention, rest): (Vec<&String>, Vec<&String>) = shown.partition(|name| needs_attention(name));
        let mut attention: Vec<String> = attention.into_iter().cloned().collect();
        attention.sort_by_key(|name| name.to_lowercase());
        let groups = tree(rest.into_iter().map(String::as_str), |name| manifest(name).map(|(category, _)| category));
        Listing { attention, groups, hits }
    }

    pub fn is_empty(&self) -> bool {
        self.hits.is_empty()
    }

    /// The names in display order, skipping groups `open` says are folded; for keyboard navigation.
    pub fn order(&self, open: impl Fn(&Group) -> bool) -> Vec<String> {
        let groups = self.groups.iter().filter(|group| open(group)).flat_map(|group| group.names.iter());
        self.attention.iter().chain(groups).cloned().collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn find_is_case_insensitive() {
        assert_eq!(find("WARP_GrfsEmbedded", "grfs"), Some(5..9));
        assert_eq!(find("Reads DATA.INI first", "data.ini"), Some(6..14));
        assert_eq!(find("abc", "x"), None);
        assert_eq!(find("abc", ""), None);
    }

    #[test]
    fn find_maps_non_ascii_to_original_bytes() {
        // 'İ' (2 bytes) lowercases to "i̇" (2 chars, 3 bytes); 'É' to 'é'.
        let text = "xİÉy";
        assert_eq!(find(text, "é"), Some(3..5));
        assert_eq!(&text[3..5], "É");
        assert_eq!(find(text, "i̇é"), Some(1..5));
        assert_eq!(find(text, "y"), Some(5..6));
        assert_eq!(find("ÀBC", "bc"), Some(2..4));
    }

    #[test]
    fn hit_checks_name_description_category() {
        let hit = hit("data.ini", "WARP_GrfsEmbedded", "Loads GRFs listed in DATA.INI", "Client").unwrap();
        assert_eq!(hit.name, None);
        assert_eq!(hit.description, Some(21..29));
        assert!(!hit.category);
        let by_category = super::hit("clie", "X", "", "Client").unwrap();
        assert!(by_category.category && by_category.name.is_none());
        assert_eq!(super::hit("warp", "WARP_x", "", "").unwrap().name, Some(0..4));
        assert_eq!(super::hit("zzz", "a", "b", "c"), None);
        assert_eq!(super::hit("  ", "a", "b", "c"), Some(Hit::default()));
    }

    #[test]
    fn excerpt_cuts_around_the_match() {
        let text = "This plugin reads the client's DATA.INI and mounts every GRF listed in it, in order.";
        let range = find(text, "data.ini").unwrap();
        let (short, moved) = excerpt(text, range);
        assert_eq!(&short[moved], "DATA.INI");
        assert!(short.starts_with('…') && short.ends_with('…'));
        assert_eq!(short, "… reads the client's DATA.INI and mounts every GR…");
    }

    #[test]
    fn excerpt_keeps_short_text_whole_and_flattens_lines() {
        let (short, moved) = excerpt("é\nDATA.INI", 3..11);
        assert_eq!(short, "é DATA.INI");
        assert_eq!(&short[moved], "DATA.INI");
    }

    #[test]
    fn tree_sorts_and_puts_unaccepted_last() {
        let categories = [("b", Some("graphics")), ("A", Some("Graphics")), ("z", Some("Client")), ("bad", None), ("c", Some("graphics"))];
        let groups = tree(categories.iter().map(|(n, _)| *n), |name| categories.iter().find(|(n, _)| *n == name).unwrap().1);
        let shape: Vec<(&str, Vec<&str>)> = groups.iter().map(|g| (g.title(), g.names.iter().map(String::as_str).collect())).collect();
        assert_eq!(
            shape,
            vec![("Client", vec!["z"]), ("Graphics", vec!["A"]), ("graphics", vec!["b", "c"]), (UNACCEPTED, vec!["bad"])]
        );
    }

    #[test]
    fn listing_keeps_attention_out_of_the_tree_and_filters() {
        let names: Vec<String> = ["WARP_GrfsEmbedded", "broken", "Alpha", "refused"].map(str::to_owned).to_vec();
        let manifest = |name: &str| match name {
            "WARP_GrfsEmbedded" => Some(("Data", "Mounts the GRFs of DATA.INI")),
            "broken" => Some(("Data", "")),
            "Alpha" => Some(("Client", "")),
            _ => None,
        };
        let any = |_: &str| true;
        let all = Listing::build("", &names, |name| name == "broken", any, manifest);
        assert_eq!(all.attention, vec!["broken"]);
        let titles: Vec<&str> = all.groups.iter().map(Group::title).collect();
        assert_eq!(titles, vec!["Client", "Data", UNACCEPTED]);
        assert_eq!(all.groups[1].names, vec!["WARP_GrfsEmbedded"]);
        assert_eq!(all.order(|group| group.title() != "Client"), vec!["broken", "WARP_GrfsEmbedded", "refused"]);

        let found = Listing::build("data.ini", &names, |name| name == "broken", any, manifest);
        assert!(found.attention.is_empty());
        assert_eq!(found.groups.len(), 1);
        assert_eq!(found.hits["WARP_GrfsEmbedded"].description, Some(19..27));
        assert!(Listing::build("nothing", &names, |_| false, any, manifest).is_empty());
        assert_eq!(Listing::build("data", &names, |_| false, any, manifest).groups[0].names, vec!["broken", "WARP_GrfsEmbedded"]);

        // A view that keeps only Alpha: the rest drops out, a plugin that needs attention stays whatever the view says.
        let enabled = Listing::build("", &names, |name| name == "broken", |name| name == "Alpha", manifest);
        assert_eq!(enabled.order(|_| true), vec!["broken", "Alpha"]);
    }
}
