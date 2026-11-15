//! A small dependency graph: plugins in columns by dependency depth (dependencies left of their dependents), edges
//! as curves coloured by whether the dependency is satisfied, nodes coloured by predicted state.

use std::collections::BTreeMap;

use eframe::egui::{self, Align2, Color32, CornerRadius, FontId, Pos2, Rect, Sense, Stroke, StrokeKind, Ui, Vec2};
use eframe::epaint::CubicBezierShape;
use ro_plugins_core::resolve::State;

use super::model::Snapshot;
use super::theme;

const NODE: Vec2 = Vec2::new(150.0, 40.0);
const GAP: Vec2 = Vec2::new(70.0, 14.0);

/// Longest dependency chain below each plugin; cycles are cut where they close.
fn depths(snapshot: &Snapshot) -> BTreeMap<&str, usize> {
    fn depth<'a>(snapshot: &'a Snapshot, name: &'a str, memo: &mut BTreeMap<&'a str, usize>, path: &mut Vec<&'a str>) -> usize {
        if let Some(&d) = memo.get(name) {
            return d;
        }
        if path.contains(&name) {
            return 0;
        }
        path.push(name);
        let d = snapshot.found.get(name).map_or(0, |m| {
            m.depends
                .iter()
                .filter_map(|dep| snapshot.found.get_key_value(dep.name.as_str()))
                .map(|(dep, _)| depth(snapshot, dep, memo, path) + 1)
                .max()
                .unwrap_or(0)
        });
        path.pop();
        memo.insert(name, d);
        d
    }
    let mut memo = BTreeMap::new();
    for name in &snapshot.names {
        depth(snapshot, name, &mut memo, &mut Vec::new());
    }
    memo
}

/// Draws the graph; returns the plugin clicked.
pub fn show(ui: &mut Ui, snapshot: &Snapshot, selected: Option<&str>) -> Option<String> {
    let depths = depths(snapshot);
    let columns = depths.values().max().map_or(0, |d| d + 1);
    let mut column_items: Vec<Vec<&str>> = vec![Vec::new(); columns];
    for name in &snapshot.names {
        column_items[depths[name.as_str()]].push(name);
    }
    let rows = column_items.iter().map(Vec::len).max().unwrap_or(0);
    let size = Vec2::new(
        (columns as f32 * (NODE.x + GAP.x) - GAP.x).max(0.0),
        (rows as f32 * (NODE.y + GAP.y) - GAP.y).max(0.0),
    );
    let width = ui.available_width();
    let (area, _) = ui.allocate_exact_size(Vec2::new(width, size.y + 8.0), Sense::hover());
    let origin = Pos2::new(area.left() + ((width - size.x) / 2.0).max(0.0), area.top() + 4.0);
    let mut rects: BTreeMap<&str, Rect> = BTreeMap::new();
    for (column, items) in column_items.iter().enumerate() {
        let offset = (size.y - (items.len() as f32 * (NODE.y + GAP.y) - GAP.y)) / 2.0;
        for (row, name) in items.iter().enumerate() {
            let min = origin + Vec2::new(column as f32 * (NODE.x + GAP.x), offset + row as f32 * (NODE.y + GAP.y));
            rects.insert(name, Rect::from_min_size(min, NODE));
        }
    }

    let painter = ui.painter_at(area.expand(4.0));
    for (name, manifest) in &snapshot.found {
        let Some(to) = rects.get(name.as_str()) else { continue };
        for dependency in &manifest.depends {
            let Some(from) = rects.get(dependency.name.as_str()) else { continue };
            let ok = snapshot
                .found
                .get(&dependency.name)
                .is_some_and(|target| dependency.range.satisfied_by(&target.version));
            let color = if ok { theme::BORDER.gamma_multiply(1.6) } else { theme::RED.gamma_multiply(0.8) };
            let start = from.right_center();
            let end = to.left_center();
            let bend = ((end.x - start.x).abs() * 0.5).max(30.0);
            painter.add(CubicBezierShape::from_points_stroke(
                [start, start + Vec2::new(bend, 0.0), end - Vec2::new(bend, 0.0), end],
                false,
                Color32::TRANSPARENT,
                Stroke::new(1.5, color),
            ));
            painter.circle_filled(end, 3.0, color);
        }
    }

    let mut clicked = None;
    for (name, rect) in &rects {
        let state = snapshot.predicted.get(*name).map_or(State::Pending, |p| p.state);
        let color = theme::state_color(state);
        let response = ui.interact(*rect, ui.id().with(("graph-node", *name)), Sense::click());
        let hovered = response.hovered();
        let is_selected = selected == Some(*name);
        let fill = if is_selected {
            theme::ACCENT_DIM
        } else if hovered {
            theme::CARD_HOVER
        } else {
            theme::BG
        };
        let stroke = if is_selected { Stroke::new(1.5, theme::ACCENT) } else { Stroke::new(1.0, color.gamma_multiply(0.55)) };
        painter.rect_filled(*rect, CornerRadius::same(9), fill);
        painter.rect_stroke(*rect, CornerRadius::same(9), stroke, StrokeKind::Inside);
        let bar = Rect::from_min_size(rect.min + Vec2::new(0.0, 8.0), Vec2::new(3.0, rect.height() - 16.0));
        painter.rect_filled(bar, CornerRadius::same(2), color);
        let version = snapshot.found.get(*name).map_or_else(|| "no manifest".to_owned(), |m| m.version.to_string());
        painter.text(rect.left_center() + Vec2::new(12.0, -7.0), Align2::LEFT_CENTER, *name, FontId::new(12.5, theme::semibold()), theme::TEXT);
        painter.text(
            rect.left_center() + Vec2::new(12.0, 9.0),
            Align2::LEFT_CENTER,
            format!("{version} · {}", state.as_str()),
            FontId::proportional(11.0),
            theme::MUTED,
        );
        if response.on_hover_cursor(egui::CursorIcon::PointingHand).clicked() {
            clicked = Some((*name).to_owned());
        }
    }
    clicked
}
