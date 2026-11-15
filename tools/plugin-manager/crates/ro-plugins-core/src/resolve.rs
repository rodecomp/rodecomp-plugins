//! Offline prediction of each plugin's state, resolving dependencies exactly as Host.cpp RoHost_Start does:
//! manifests, refused DLLs, disabled.txt, then a depth-first visit in name order that disables a plugin on a missing,
//! turned-off, disabled, out-of-range or cyclic dependency.

use std::collections::{BTreeMap, HashSet};
use std::path::Path;

use crate::manifest::{self, Manifests};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum State {
    Loaded,
    Off,
    Disabled,
    FailedLoad,
    FailedInit,
    Refused,
    Pending,
}

impl State {
    pub fn as_str(self) -> &'static str {
        match self {
            State::Loaded => "loaded",
            State::Off => "off",
            State::Disabled => "disabled",
            State::FailedLoad => "failed-load",
            State::FailedInit => "failed-init",
            State::Refused => "refused",
            State::Pending => "pending",
        }
    }

    pub fn parse(text: &str) -> Option<State> {
        Some(match text {
            "loaded" => State::Loaded,
            "off" => State::Off,
            "disabled" => State::Disabled,
            "failed-load" => State::FailedLoad,
            "failed-init" => State::FailedInit,
            "refused" => State::Refused,
            "pending" => State::Pending,
            _ => return None,
        })
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Prediction {
    pub state: State,
    pub reason: String,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Mark {
    Pending,
    Visiting,
    Done(State),
}

struct Resolver<'a> {
    found: &'a Manifests,
    marks: BTreeMap<&'a str, Mark>,
    reasons: BTreeMap<&'a str, String>,
    stack: Vec<&'a str>,
}

impl<'a> Resolver<'a> {
    /// The first reason stands; a plugin turned off stays off.
    fn disable(&mut self, name: &'a str, why: String, kind: State) {
        if matches!(self.marks[name], Mark::Done(State::Disabled | State::FailedLoad | State::Off)) {
            return;
        }
        self.marks.insert(name, Mark::Done(kind));
        self.reasons.insert(name, why);
    }

    fn visit(&mut self, name: &'a str) {
        match self.marks[name] {
            Mark::Visiting => {
                let start = self.stack.iter().position(|member| *member == name).unwrap_or(0);
                let members: Vec<&'a str> = self.stack[start..].to_vec();
                let chain = members.iter().chain(std::iter::once(&name)).copied().collect::<Vec<_>>().join(" -> ");
                for member in members {
                    self.disable(member, format!("in dependency cycle {chain}"), State::Disabled);
                }
                return;
            }
            Mark::Done(_) => return,
            Mark::Pending => {}
        }
        self.marks.insert(name, Mark::Visiting);
        self.stack.push(name);
        let manifest = &self.found[name];
        for dependency in &manifest.depends {
            let Some((target_name, target)) = self.found.get_key_value(dependency.name.as_str()) else {
                self.disable(name, format!("needs {} {}, not installed", dependency.name, dependency.text), State::Disabled);
                continue;
            };
            self.visit(target_name);
            match self.marks[target_name.as_str()] {
                Mark::Done(State::Off) => {
                    self.disable(name, format!("needs {}, which is turned off", dependency.name), State::Disabled)
                }
                Mark::Done(State::Disabled | State::FailedLoad) => {
                    self.disable(name, format!("needs {}, which is disabled", dependency.name), State::Disabled)
                }
                _ if !dependency.range.satisfied_by(&target.version) => self.disable(
                    name,
                    format!("needs {} {}, found {}", dependency.name, dependency.text, target.version),
                    State::Disabled,
                ),
                _ => {}
            }
        }
        self.stack.pop();
        if self.marks[name] == Mark::Visiting {
            // Resolved: the host loads it unless LoadLibrary or RoPlugin_Init fails.
            self.marks.insert(name, Mark::Done(State::Loaded));
        }
    }
}

/// Predicts from parsed inputs: valid manifests, DLL stems present, and disabled.txt names.
pub fn predict_from(found: &Manifests, dlls: &[String], disabled: &[String]) -> BTreeMap<String, Prediction> {
    let manifest_names: HashSet<String> = found.keys().map(|name| name.to_lowercase()).collect();
    let dll_names: HashSet<String> = dlls.iter().map(|stem| stem.to_lowercase()).collect();
    let off: HashSet<String> = disabled.iter().map(|name| name.to_lowercase()).collect();
    let mut states = BTreeMap::new();
    for stem in dlls {
        if !manifest_names.contains(&stem.to_lowercase()) {
            states.insert(
                stem.clone(),
                Prediction { state: State::Refused, reason: format!("no valid {stem}.plugin manifest next to it") },
            );
        }
    }
    let mut resolver = Resolver {
        found,
        marks: found.keys().map(|name| (name.as_str(), Mark::Pending)).collect(),
        reasons: BTreeMap::new(),
        stack: Vec::new(),
    };
    for name in found.keys() {
        let lower = name.to_lowercase();
        if off.contains(&lower) {
            resolver.marks.insert(name, Mark::Done(State::Off));
            resolver.reasons.insert(name, "turned off in plugins\\disabled.txt".to_owned());
        } else if !dll_names.contains(&lower) {
            resolver.disable(name, format!("{name}.dll is missing"), State::FailedLoad);
        }
    }
    for name in found.keys() {
        resolver.visit(name);
    }
    for name in found.keys() {
        let Mark::Done(state) = resolver.marks[name.as_str()] else { unreachable!("every plugin is visited") };
        let reason = resolver.reasons.remove(name.as_str()).unwrap_or_default();
        states.insert(name.clone(), Prediction { state, reason });
    }
    states
}

/// Predicts the states for the installed files of a plugins directory.
pub fn predict(plugins: &Path) -> BTreeMap<String, Prediction> {
    let (found, _) = manifest::manifests(plugins);
    predict_from(&found, &manifest::stems(plugins, "dll"), &manifest::read_disabled(plugins))
}

/// Installed plugins that depend on `name`, directly or not, nearest first.
pub fn dependents(found: &Manifests, name: &str) -> Vec<String> {
    let mut result: Vec<String> = Vec::new();
    let mut frontier = std::collections::VecDeque::from([name.to_owned()]);
    while let Some(current) = frontier.pop_front() {
        for (other, manifest) in found {
            if other != name && !result.contains(other) && manifest.depends.iter().any(|d| d.name == current) {
                result.push(other.clone());
                frontier.push_back(other.clone());
            }
        }
    }
    result
}

/// `name` and every installed plugin it needs, directly or not (depth-first, like plugins.py enable).
pub fn requirements(found: &Manifests, name: &str) -> Vec<String> {
    let mut needed: Vec<String> = Vec::new();
    let mut stack = vec![name.to_owned()];
    while let Some(current) = stack.pop() {
        if needed.contains(&current) {
            continue;
        }
        if let Some(manifest) = found.get(&current) {
            stack.extend(manifest.depends.iter().filter(|d| found.contains_key(&d.name)).map(|d| d.name.clone()));
        }
        needed.push(current);
    }
    needed
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::manifest::{Dependency, Manifest};
    use crate::semver::{Range, Version};

    fn plugin(name: &str, version: &str, depends: &[(&str, &str)]) -> (String, Manifest) {
        let depends = depends
            .iter()
            .map(|(n, r)| Dependency { name: (*n).into(), text: (*r).into(), range: Range::parse(r).unwrap() })
            .collect();
        (name.into(), Manifest { path: format!("{name}.plugin").into(), name: name.into(), version: Version::parse(version).unwrap(), depends })
    }

    fn run(plugins: Vec<(String, Manifest)>, dlls: &[&str], disabled: &[&str]) -> BTreeMap<String, (State, String)> {
        let found: Manifests = plugins.into_iter().collect();
        let dlls: Vec<String> = dlls.iter().map(|s| (*s).into()).collect();
        let disabled: Vec<String> = disabled.iter().map(|s| (*s).into()).collect();
        predict_from(&found, &dlls, &disabled).into_iter().map(|(k, p)| (k, (p.state, p.reason))).collect()
    }

    fn get<'a>(states: &'a BTreeMap<String, (State, String)>, name: &str) -> (State, &'a str) {
        let (state, reason) = &states[name];
        (*state, reason.as_str())
    }

    #[test]
    fn loads_satisfied_chain() {
        let states = run(
            vec![plugin("A", "0.1.0", &[("B", ">=1.0.0 <2.0.0-0")]), plugin("B", "1.0.0", &[])],
            &["A", "B"],
            &[],
        );
        assert_eq!(get(&states, "A"), (State::Loaded, ""));
        assert_eq!(get(&states, "B"), (State::Loaded, ""));
    }

    #[test]
    fn missing_dependency_and_dll() {
        let states = run(vec![plugin("A", "1.0.0", &[("Z", ">=1.0.0")]), plugin("C", "1.0.0", &[])], &["A"], &[]);
        assert_eq!(get(&states, "A"), (State::Disabled, "needs Z >=1.0.0, not installed"));
        assert_eq!(get(&states, "C"), (State::FailedLoad, "C.dll is missing"));
    }

    #[test]
    fn version_mismatch() {
        let states = run(
            vec![plugin("A", "1.0.0", &[("B", ">=2.0.0 <3.0.0-0")]), plugin("B", "1.5.0", &[])],
            &["A", "B"],
            &[],
        );
        assert_eq!(get(&states, "A"), (State::Disabled, "needs B >=2.0.0 <3.0.0-0, found 1.5.0"));
        assert_eq!(get(&states, "B"), (State::Loaded, ""));
    }

    #[test]
    fn cycle_disables_members_only() {
        let states = run(
            vec![
                plugin("A", "1.0.0", &[("B", ">=0.0.0-0")]),
                plugin("B", "1.0.0", &[("C", ">=0.0.0-0")]),
                plugin("C", "1.0.0", &[("B", ">=0.0.0-0")]),
                plugin("D", "1.0.0", &[]),
            ],
            &["A", "B", "C", "D"],
            &[],
        );
        assert_eq!(get(&states, "B"), (State::Disabled, "in dependency cycle B -> C -> B"));
        assert_eq!(get(&states, "C"), (State::Disabled, "in dependency cycle B -> C -> B"));
        // A is not in the cycle; it is disabled through B.
        assert_eq!(get(&states, "A"), (State::Disabled, "needs B, which is disabled"));
        assert_eq!(get(&states, "D"), (State::Loaded, ""));
    }

    #[test]
    fn self_cycle() {
        let states = run(vec![plugin("A", "1.0.0", &[("A", ">=0.0.0-0")])], &["A"], &[]);
        assert_eq!(get(&states, "A"), (State::Disabled, "in dependency cycle A -> A"));
    }

    #[test]
    fn off_cascades_through_dependents() {
        let states = run(
            vec![
                plugin("A", "1.0.0", &[("B", ">=0.0.0-0")]),
                plugin("B", "1.0.0", &[("C", ">=0.0.0-0")]),
                plugin("C", "1.0.0", &[]),
            ],
            &["A", "B", "C"],
            &["c"],
        );
        assert_eq!(get(&states, "C"), (State::Off, "turned off in plugins\\disabled.txt"));
        assert_eq!(get(&states, "B"), (State::Disabled, "needs C, which is turned off"));
        assert_eq!(get(&states, "A"), (State::Disabled, "needs B, which is disabled"));
    }

    #[test]
    fn failed_load_cascades_and_first_reason_stands() {
        let states = run(
            vec![plugin("A", "1.0.0", &[("B", ">=0.0.0-0"), ("Z", ">=1.0.0")]), plugin("B", "1.0.0", &[])],
            &["A"],
            &[],
        );
        assert_eq!(get(&states, "B"), (State::FailedLoad, "B.dll is missing"));
        assert_eq!(get(&states, "A"), (State::Disabled, "needs B, which is disabled"));
    }

    #[test]
    fn off_plugin_stays_off() {
        let states = run(vec![plugin("A", "1.0.0", &[("Z", ">=1.0.0")])], &[], &["A"]);
        assert_eq!(get(&states, "A"), (State::Off, "turned off in plugins\\disabled.txt"));
    }

    #[test]
    fn refused_dll_without_manifest() {
        let states = run(vec![], &["Orphan"], &[]);
        assert_eq!(get(&states, "Orphan"), (State::Refused, "no valid Orphan.plugin manifest next to it"));
    }

    #[test]
    fn dependents_and_requirements() {
        let found: Manifests = vec![
            plugin("A", "1.0.0", &[("B", "*")]),
            plugin("B", "1.0.0", &[("C", "*")]),
            plugin("C", "1.0.0", &[]),
            plugin("D", "1.0.0", &[("C", "*")]),
        ]
        .into_iter()
        .collect();
        assert_eq!(dependents(&found, "C"), ["B", "D", "A"]);
        assert!(dependents(&found, "A").is_empty());
        assert_eq!(requirements(&found, "A"), ["A", "B", "C"]);
    }
}
