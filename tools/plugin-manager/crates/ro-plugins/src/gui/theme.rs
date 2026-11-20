//! The look: a calm dark theme with one indigo accent, Inter text, Phosphor icons, and the few painted widgets
//! (switch, quiet button, chip). Areas are separated by tone and spacing, not borders.

use eframe::egui::{
    self, Align2, Color32, CornerRadius, CursorIcon, FontData, FontDefinitions, FontFamily, FontId, Id, Rect, Response,
    Sense, Shadow, Stroke, TextStyle, Ui, Vec2, WidgetInfo, WidgetType,
};

use super::model::Tone;

/// Window chrome: the top bar and the plugin list.
pub const CHROME: Color32 = Color32::from_rgb(0x0f, 0x11, 0x15);
/// The content area.
pub const BG: Color32 = Color32::from_rgb(0x14, 0x16, 0x1b);
/// The one card level: grouped rows, dialogs, fields.
pub const SURFACE: Color32 = Color32::from_rgb(0x1b, 0x1e, 0x25);
pub const SURFACE_HOVER: Color32 = Color32::from_rgb(0x23, 0x27, 0x30);
pub const ROW_HOVER: Color32 = Color32::from_rgb(0x19, 0x1c, 0x22);
pub const FIELD: Color32 = Color32::from_rgb(0x17, 0x1a, 0x20);
pub const DIVIDER: Color32 = Color32::from_rgb(0x27, 0x2b, 0x34);
pub const TEXT: Color32 = Color32::from_rgb(0xec, 0xee, 0xf2);
pub const SOFT: Color32 = Color32::from_rgb(0xb6, 0xbb, 0xc6);
pub const MUTED: Color32 = Color32::from_rgb(0x88, 0x8e, 0x9b);
pub const FAINT: Color32 = Color32::from_rgb(0x5c, 0x62, 0x6e);
pub const ACCENT: Color32 = Color32::from_rgb(0x7b, 0x89, 0xff);
pub const ACCENT_FILL: Color32 = Color32::from_rgb(0x58, 0x65, 0xf2);
pub const GREEN: Color32 = Color32::from_rgb(0x3e, 0xcf, 0x8e);
pub const AMBER: Color32 = Color32::from_rgb(0xf0, 0xb3, 0x5a);
pub const RED: Color32 = Color32::from_rgb(0xf0, 0x61, 0x6d);
pub const BLUE: Color32 = Color32::from_rgb(0x5a, 0xa9, 0xf5);

pub fn semibold() -> FontFamily {
    FontFamily::Name("semibold".into())
}

pub fn tone_color(tone: Tone) -> Color32 {
    match tone {
        Tone::Good => GREEN,
        Tone::Off => FAINT,
        Tone::Warn => AMBER,
        Tone::Bad => RED,
        Tone::Info => BLUE,
    }
}

pub fn install(ctx: &egui::Context) {
    let mut fonts = FontDefinitions::default();
    fonts.font_data.insert("inter".into(), FontData::from_static(include_bytes!("../../assets/Inter-Regular.ttf")).into());
    fonts
        .font_data
        .insert("inter-semibold".into(), FontData::from_static(include_bytes!("../../assets/Inter-SemiBold.ttf")).into());
    let proportional = fonts.families.entry(FontFamily::Proportional).or_default();
    proportional.insert(0, "inter".into());
    let mut bold = vec!["inter-semibold".to_owned()];
    bold.extend(proportional.iter().skip(1).cloned());
    fonts.families.insert(semibold(), bold);
    // The embedded Inter is subset without the Private Use Area (its own glyphs there would shadow the icons), and
    // Phosphor comes second because it maps a-z to blank ligature glyphs.
    fonts.font_data.insert("phosphor".into(), FontData::from_static(egui_phosphor::Variant::Regular.font_bytes()).into());
    for family in [FontFamily::Proportional, semibold(), FontFamily::Monospace] {
        fonts.families.entry(family).or_default().insert(1, "phosphor".into());
    }
    // Inter has no Hangul, and settings quote Korean data paths (몸\body_…). Fall back to Windows' Korean UI font
    // when it is there rather than embedding megabytes of CJK glyphs.
    let windows = std::env::var_os("WINDIR").map_or_else(|| "C:\\Windows".into(), std::path::PathBuf::from);
    if let Ok(bytes) = std::fs::read(windows.join("Fonts").join("malgun.ttf")) {
        fonts.font_data.insert("malgun".into(), FontData::from_owned(bytes).into());
        for family in [FontFamily::Proportional, semibold(), FontFamily::Monospace] {
            fonts.families.entry(family).or_default().push("malgun".into());
        }
    }
    ctx.set_fonts(fonts);

    ctx.all_styles_mut(|style| {
        style.text_styles = [
            (TextStyle::Heading, FontId::new(20.0, semibold())),
            (TextStyle::Body, FontId::new(13.5, FontFamily::Proportional)),
            (TextStyle::Button, FontId::new(13.5, FontFamily::Proportional)),
            (TextStyle::Small, FontId::new(12.0, FontFamily::Proportional)),
            (TextStyle::Monospace, FontId::new(12.5, FontFamily::Monospace)),
        ]
        .into();
        style.spacing.item_spacing = Vec2::new(8.0, 8.0);
        style.spacing.button_padding = Vec2::new(12.0, 6.0);
        style.spacing.interact_size.y = 30.0;
        style.spacing.scroll.bar_width = 6.0;
        style.spacing.scroll.floating = true;
        style.interaction.selectable_labels = false;
        // Every clickable widget (buttons, menu buttons, chips, links) shows the hand cursor while it can be clicked.
        style.visuals.interact_cursor = Some(egui::CursorIcon::PointingHand);

        let v = &mut style.visuals;
        v.dark_mode = true;
        v.override_text_color = None;
        v.panel_fill = BG;
        v.window_fill = SURFACE;
        v.window_stroke = Stroke::new(1.0, DIVIDER);
        v.window_corner_radius = CornerRadius::same(14);
        v.window_shadow = Shadow { offset: [0, 12], blur: 32, spread: 0, color: Color32::from_black_alpha(140) };
        v.popup_shadow = Shadow { offset: [0, 6], blur: 16, spread: 0, color: Color32::from_black_alpha(120) };
        v.menu_corner_radius = CornerRadius::same(10);
        v.extreme_bg_color = FIELD;
        v.faint_bg_color = SURFACE;
        v.code_bg_color = FIELD;
        v.hyperlink_color = ACCENT;
        v.warn_fg_color = AMBER;
        v.error_fg_color = RED;
        v.selection.bg_fill = ACCENT_FILL.gamma_multiply(0.45);
        v.selection.stroke = Stroke::new(1.0, ACCENT);
        v.text_edit_bg_color = Some(FIELD);

        let radius = CornerRadius::same(8);
        let w = &mut v.widgets;
        w.noninteractive.bg_fill = SURFACE;
        w.noninteractive.weak_bg_fill = SURFACE;
        w.noninteractive.bg_stroke = Stroke::new(1.0, DIVIDER);
        w.noninteractive.fg_stroke = Stroke::new(1.0, SOFT);
        w.noninteractive.corner_radius = radius;
        w.inactive.bg_fill = SURFACE_HOVER;
        w.inactive.weak_bg_fill = SURFACE_HOVER;
        w.inactive.bg_stroke = Stroke::NONE;
        w.inactive.fg_stroke = Stroke::new(1.0, TEXT);
        w.inactive.corner_radius = radius;
        w.hovered.bg_fill = Color32::from_rgb(0x2c, 0x31, 0x3c);
        w.hovered.weak_bg_fill = Color32::from_rgb(0x2c, 0x31, 0x3c);
        w.hovered.bg_stroke = Stroke::NONE;
        w.hovered.fg_stroke = Stroke::new(1.0, TEXT);
        w.hovered.corner_radius = radius;
        w.hovered.expansion = 0.0;
        w.active.bg_fill = Color32::from_rgb(0x33, 0x39, 0x46);
        w.active.weak_bg_fill = Color32::from_rgb(0x33, 0x39, 0x46);
        w.active.bg_stroke = Stroke::NONE;
        w.active.fg_stroke = Stroke::new(1.0, TEXT);
        w.active.corner_radius = radius;
        w.active.expansion = 0.0;
        w.open = w.active;
    });
}

pub fn lerp_color(a: Color32, b: Color32, t: f32) -> Color32 {
    let mix = |x: u8, y: u8| (x as f32 + (y as f32 - x as f32) * t).round() as u8;
    Color32::from_rgba_unmultiplied(mix(a.r(), b.r()), mix(a.g(), b.g()), mix(a.b(), b.b()), mix(a.a(), b.a()))
}

/// A slider drawn in `rect`: a thin rounded rail, filled in white up to a small white knob ringed by the background,
/// the rest of the rail dark. Drag or click to set `value` in `range`, snapped to `step`. Arrow keys step it while
/// it has focus. Changed on every move; `drag_stopped` (or a click) marks the end of an edit.
pub fn slider(ui: &mut Ui, rect: Rect, id: Id, value: &mut f64, range: std::ops::RangeInclusive<f64>, step: Option<f64>, enabled: bool) -> Response {
    let mut response = ui.interact(rect, id, if enabled { Sense::click_and_drag() } else { Sense::hover() });
    let (low, high) = (*range.start(), *range.end());
    let knob = 6.0;
    let rail = rect.x_range().shrink(knob);
    let snap = |v: f64| {
        let v = step.filter(|s| *s > 0.0).map_or(v, |s| low + ((v - low) / s).round() * s);
        v.clamp(low, high)
    };
    let before = *value;
    if enabled {
        if let Some(pointer) = response.interact_pointer_pos().filter(|_| response.dragged() || response.clicked()) {
            let t = ((pointer.x - rail.min) / rail.span().max(1.0)).clamp(0.0, 1.0) as f64;
            *value = snap(low + t * (high - low));
        }
        if response.has_focus() {
            let by = step.unwrap_or((high - low) / 100.0);
            let (left, right) = ui.input(|i| (i.key_pressed(egui::Key::ArrowLeft), i.key_pressed(egui::Key::ArrowRight)));
            if left || right {
                *value = snap(*value + if right { by } else { -by });
            }
        }
    }
    if *value != before {
        response.mark_changed();
    }
    response.widget_info(|| WidgetInfo::slider(enabled, *value, ""));
    if ui.is_rect_visible(rect) {
        let t = if high > low { ((*value - low) / (high - low)) as f32 } else { 0.0 };
        let x = egui::lerp(rail.min..=rail.max, t);
        let y = rect.center().y;
        let painter = ui.painter();
        let thickness = 4.0;
        let full = Rect::from_x_y_ranges(rail.min..=rail.max, (y - thickness / 2.0)..=(y + thickness / 2.0));
        painter.rect_filled(full, CornerRadius::same(255), Color32::from_rgb(0x36, 0x3b, 0x47));
        let filled = if enabled { Color32::WHITE } else { Color32::from_gray(150) };
        painter.rect_filled(Rect::from_x_y_ranges(rail.min..=x, full.y_range()), CornerRadius::same(255), filled);
        let grow = if enabled && (response.hovered() || response.dragged()) { 1.0 } else { 0.0 };
        painter.circle(egui::pos2(x, y), knob + grow, filled, Stroke::new(2.0, SURFACE));
    }
    if enabled { response.on_hover_cursor(CursorIcon::PointingHand) } else { response }
}

/// A switch drawn in `rect`; `on` flips when clicked. Register it after the row it sits on, so it gets the click.
pub fn toggle(ui: &mut Ui, rect: Rect, id: Id, on: &mut bool, enabled: bool) -> Response {
    let mut response = ui.interact(rect, id, if enabled { Sense::click() } else { Sense::hover() });
    if enabled && response.clicked() {
        *on = !*on;
        response.mark_changed();
    }
    response.widget_info(|| WidgetInfo::selected(WidgetType::Checkbox, enabled, *on, ""));
    if ui.is_rect_visible(rect) {
        let t = ui.ctx().animate_bool_with_time(id.with("on"), *on, 0.12);
        let mut fill = lerp_color(Color32::from_rgb(0x36, 0x3b, 0x47), ACCENT_FILL, t);
        if !enabled {
            fill = lerp_color(fill, CHROME, 0.55);
        } else if response.hovered() {
            fill = lerp_color(fill, Color32::WHITE, 0.10);
        }
        let painter = ui.painter();
        painter.rect_filled(rect, CornerRadius::same(255), fill);
        let half = rect.height() / 2.0;
        let x = egui::lerp((rect.left() + half)..=(rect.right() - half), t);
        let knob = if enabled { Color32::WHITE } else { Color32::from_gray(150) };
        painter.circle_filled(egui::pos2(x, rect.center().y), half - 2.5, knob);
    }
    if enabled { response.on_hover_cursor(CursorIcon::PointingHand) } else { response }
}

/// A borderless text button: `color` at rest, `hover` (and a soft fill of it) under the pointer.
pub fn quiet_button(ui: &mut Ui, text: &str, color: Color32, hover: Color32) -> Response {
    let font = FontId::proportional(13.0);
    let galley = ui.painter().layout_no_wrap(text.to_owned(), font.clone(), color);
    let (rect, response) = ui.allocate_exact_size(galley.size() + Vec2::new(20.0, 12.0), Sense::click());
    if ui.is_rect_visible(rect) {
        let t = ui.ctx().animate_bool_with_time(response.id.with("hover"), response.hovered(), 0.12);
        ui.painter().rect_filled(rect, CornerRadius::same(8), hover.gamma_multiply(0.10 * t));
        ui.painter().text(rect.center(), Align2::CENTER_CENTER, text, font, lerp_color(color, hover, t));
    }
    response.on_hover_cursor(CursorIcon::PointingHand)
}

/// A tinted status chip.
pub fn chip(ui: &mut Ui, text: &str, color: Color32) -> Response {
    let font = FontId::proportional(12.5);
    let ink = lerp_color(color, TEXT, 0.35);
    let galley = ui.painter().layout_no_wrap(text.to_owned(), font, ink);
    let (rect, response) = ui.allocate_exact_size(galley.size() + Vec2::new(22.0, 10.0), Sense::hover());
    if ui.is_rect_visible(rect) {
        ui.painter().rect_filled(rect, CornerRadius::same(255), color.gamma_multiply(0.13));
        ui.painter().galley(rect.center() - galley.size() / 2.0, galley, ink);
    }
    response
}

/// A status chip that is also a button: the chip's tint and shape with a stroke and a trailing caret, so it reads as
/// clickable next to the plain chips.
pub fn chip_button(text: &str, color: Color32) -> egui::Button<'static> {
    let ink = lerp_color(color, TEXT, 0.35);
    egui::Button::new(egui::RichText::new(format!("{text}  {}", egui_phosphor::regular::CARET_RIGHT)).size(12.5).color(ink))
        .fill(color.gamma_multiply(0.13))
        .stroke(Stroke::new(1.0, color.gamma_multiply(0.6)))
        .corner_radius(CornerRadius::same(255))
}

/// A quiet section label.
pub fn section(ui: &mut Ui, text: &str) {
    ui.label(egui::RichText::new(text).font(FontId::new(12.5, semibold())).color(MUTED));
    ui.add_space(2.0);
}

/// The filled accent button: one per view.
pub fn primary(text: impl Into<String>) -> egui::Button<'static> {
    egui::Button::new(egui::RichText::new(text.into()).color(Color32::WHITE).font(FontId::new(13.5, semibold())))
        .fill(ACCENT_FILL)
        .stroke(Stroke::NONE)
        .corner_radius(CornerRadius::same(8))
}

/// The plain, unfilled button of a secondary action beside the one primary button.
pub fn secondary(text: impl Into<String>) -> egui::Button<'static> {
    egui::Button::new(egui::RichText::new(text.into()).color(TEXT)).fill(SURFACE).corner_radius(CornerRadius::same(8))
}

/// The filled red button of a destructive confirmation.
pub fn destructive(text: impl Into<String>) -> egui::Button<'static> {
    egui::Button::new(egui::RichText::new(text.into()).color(Color32::WHITE).font(FontId::new(13.5, semibold())))
        .fill(Color32::from_rgb(0xd6, 0x45, 0x52))
        .stroke(Stroke::NONE)
        .corner_radius(CornerRadius::same(8))
}
