//! The look: a dark slate theme with an indigo accent, Inter text, Phosphor icons, and the small painted widgets
//! (state badges, pills, a toggle switch, cards).

use eframe::egui::{
    self, Color32, CornerRadius, FontData, FontDefinitions, FontFamily, FontId, Margin, Response, Sense, Shadow, Stroke,
    StrokeKind, TextStyle, Ui, Vec2, WidgetInfo, WidgetType,
};
use ro_plugins_core::resolve::State;

pub const BG: Color32 = Color32::from_rgb(0x0f, 0x11, 0x17);
pub const PANEL: Color32 = Color32::from_rgb(0x15, 0x18, 0x21);
pub const CARD: Color32 = Color32::from_rgb(0x1b, 0x1f, 0x2b);
pub const CARD_HOVER: Color32 = Color32::from_rgb(0x22, 0x27, 0x36);
pub const BORDER: Color32 = Color32::from_rgb(0x2a, 0x30, 0x42);
pub const TEXT: Color32 = Color32::from_rgb(0xe6, 0xe9, 0xf2);
pub const MUTED: Color32 = Color32::from_rgb(0x8b, 0x93, 0xa8);
pub const FAINT: Color32 = Color32::from_rgb(0x5d, 0x65, 0x7a);
pub const ACCENT: Color32 = Color32::from_rgb(0x7c, 0x8c, 0xff);
pub const ACCENT_DIM: Color32 = Color32::from_rgb(0x2b, 0x31, 0x5e);
pub const GREEN: Color32 = Color32::from_rgb(0x3d, 0xd6, 0x8c);
pub const AMBER: Color32 = Color32::from_rgb(0xf5, 0xb5, 0x42);
pub const RED: Color32 = Color32::from_rgb(0xf2, 0x5f, 0x6b);
pub const GREY: Color32 = Color32::from_rgb(0x7a, 0x82, 0x96);
pub const BLUE: Color32 = Color32::from_rgb(0x5a, 0xb4, 0xf5);

pub const RADIUS: u8 = 10;

pub fn semibold() -> FontFamily {
    FontFamily::Name("semibold".into())
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
    ctx.set_fonts(fonts);

    ctx.all_styles_mut(|style| {
        style.text_styles = [
            (TextStyle::Heading, FontId::new(20.0, semibold())),
            (TextStyle::Body, FontId::new(14.0, FontFamily::Proportional)),
            (TextStyle::Button, FontId::new(14.0, FontFamily::Proportional)),
            (TextStyle::Small, FontId::new(12.0, FontFamily::Proportional)),
            (TextStyle::Monospace, FontId::new(13.0, FontFamily::Monospace)),
        ]
        .into();
        style.spacing.item_spacing = Vec2::new(8.0, 6.0);
        style.spacing.button_padding = Vec2::new(12.0, 6.0);
        style.spacing.interact_size.y = 30.0;
        style.spacing.scroll.bar_width = 6.0;
        style.spacing.scroll.floating = true;
        style.interaction.selectable_labels = false;

        let v = &mut style.visuals;
        v.dark_mode = true;
        v.override_text_color = None;
        v.panel_fill = PANEL;
        v.window_fill = CARD;
        v.window_stroke = Stroke::new(1.0, BORDER);
        v.window_corner_radius = CornerRadius::same(14);
        v.window_shadow = Shadow { offset: [0, 12], blur: 32, spread: 0, color: Color32::from_black_alpha(140) };
        v.popup_shadow = Shadow { offset: [0, 6], blur: 16, spread: 0, color: Color32::from_black_alpha(120) };
        v.menu_corner_radius = CornerRadius::same(RADIUS);
        v.extreme_bg_color = BG;
        v.faint_bg_color = CARD;
        v.code_bg_color = BG;
        v.hyperlink_color = ACCENT;
        v.warn_fg_color = AMBER;
        v.error_fg_color = RED;
        v.selection.bg_fill = ACCENT_DIM;
        v.selection.stroke = Stroke::new(1.0, ACCENT);
        v.text_edit_bg_color = Some(BG);

        let radius = CornerRadius::same(8);
        let w = &mut v.widgets;
        w.noninteractive.bg_fill = CARD;
        w.noninteractive.weak_bg_fill = CARD;
        w.noninteractive.bg_stroke = Stroke::new(1.0, BORDER);
        w.noninteractive.fg_stroke = Stroke::new(1.0, TEXT);
        w.noninteractive.corner_radius = radius;
        w.inactive.bg_fill = CARD_HOVER;
        w.inactive.weak_bg_fill = CARD_HOVER;
        w.inactive.bg_stroke = Stroke::new(1.0, BORDER);
        w.inactive.fg_stroke = Stroke::new(1.0, TEXT);
        w.inactive.corner_radius = radius;
        w.hovered.bg_fill = Color32::from_rgb(0x2a, 0x30, 0x44);
        w.hovered.weak_bg_fill = Color32::from_rgb(0x2a, 0x30, 0x44);
        w.hovered.bg_stroke = Stroke::new(1.0, Color32::from_rgb(0x3a, 0x42, 0x5c));
        w.hovered.fg_stroke = Stroke::new(1.5, TEXT);
        w.hovered.corner_radius = radius;
        w.hovered.expansion = 0.0;
        w.active.bg_fill = ACCENT_DIM;
        w.active.weak_bg_fill = ACCENT_DIM;
        w.active.bg_stroke = Stroke::new(1.0, ACCENT);
        w.active.fg_stroke = Stroke::new(1.5, TEXT);
        w.active.corner_radius = radius;
        w.active.expansion = 0.0;
        w.open = w.active;
    });
}

/// Colour of a state badge.
pub fn state_color(state: State) -> Color32 {
    match state {
        State::Loaded => GREEN,
        State::Off => GREY,
        State::Disabled | State::FailedLoad | State::FailedInit => AMBER,
        State::Refused => RED,
        State::Pending => BLUE,
    }
}

pub fn state_text_color(text: &str) -> Color32 {
    State::parse(text).map_or(GREY, state_color)
}

/// A rounded tinted label: `text` in `color` on a translucent fill.
pub fn badge(ui: &mut Ui, text: &str, color: Color32) -> Response {
    let font = FontId::new(11.5, semibold());
    let galley = ui.painter().layout_no_wrap(text.to_owned(), font, color);
    let size = galley.size() + Vec2::new(16.0, 6.0);
    let (rect, response) = ui.allocate_exact_size(size, Sense::hover());
    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        painter.rect_filled(rect, CornerRadius::same(255), color.gamma_multiply(0.14));
        painter.rect_stroke(rect, CornerRadius::same(255), Stroke::new(1.0, color.gamma_multiply(0.35)), StrokeKind::Inside);
        painter.galley(rect.center() - galley.size() / 2.0, galley, color);
    }
    response
}

/// A larger status pill with a glowing dot.
pub fn pill(ui: &mut Ui, text: &str, color: Color32) -> Response {
    let font = FontId::new(13.0, semibold());
    let galley = ui.painter().layout_no_wrap(text.to_owned(), font, TEXT);
    let size = Vec2::new(galley.size().x + 36.0, 28.0);
    let (rect, response) = ui.allocate_exact_size(size, Sense::hover());
    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        painter.rect_filled(rect, CornerRadius::same(255), color.gamma_multiply(0.12));
        painter.rect_stroke(rect, CornerRadius::same(255), Stroke::new(1.0, color.gamma_multiply(0.4)), StrokeKind::Inside);
        let dot = egui::pos2(rect.left() + 15.0, rect.center().y);
        painter.circle_filled(dot, 7.0, color.gamma_multiply(0.25));
        painter.circle_filled(dot, 4.0, color);
        painter.galley(egui::pos2(rect.left() + 26.0, rect.center().y - galley.size().y / 2.0), galley, TEXT);
    }
    response
}

/// An iOS-style switch. Returns the response; `on` flips when clicked.
pub fn toggle(ui: &mut Ui, on: &mut bool, enabled: bool) -> Response {
    let size = Vec2::new(36.0, 20.0);
    let sense = if enabled { Sense::click() } else { Sense::hover() };
    let (rect, mut response) = ui.allocate_exact_size(size, sense);
    if enabled && response.clicked() {
        *on = !*on;
        response.mark_changed();
    }
    response.widget_info(|| WidgetInfo::selected(WidgetType::Checkbox, enabled, *on, ""));
    if ui.is_rect_visible(rect) {
        let t = ui.ctx().animate_bool_responsive(response.id, *on);
        let off_fill = Color32::from_rgb(0x33, 0x39, 0x4d);
        let mut fill = lerp_color(off_fill, ACCENT, t);
        if !enabled {
            fill = fill.gamma_multiply(0.45);
        } else if response.hovered() {
            fill = lerp_color(fill, Color32::WHITE, 0.08);
        }
        let painter = ui.painter();
        painter.rect_filled(rect, CornerRadius::same(255), fill);
        let x = egui::lerp((rect.left() + 10.0)..=(rect.right() - 10.0), t);
        let knob = if enabled { Color32::WHITE } else { Color32::from_gray(170) };
        painter.circle_filled(egui::pos2(x, rect.center().y), 7.5, knob);
    }
    response
}

pub fn lerp_color(a: Color32, b: Color32, t: f32) -> Color32 {
    let mix = |x: u8, y: u8| (x as f32 + (y as f32 - x as f32) * t).round() as u8;
    Color32::from_rgba_unmultiplied(mix(a.r(), b.r()), mix(a.g(), b.g()), mix(a.b(), b.b()), mix(a.a(), b.a()))
}

/// A rounded card.
pub fn card() -> egui::Frame {
    egui::Frame::new()
        .fill(CARD)
        .stroke(Stroke::new(1.0, BORDER))
        .corner_radius(CornerRadius::same(RADIUS))
        .inner_margin(Margin::same(14))
}

/// A section caption in small caps style.
pub fn caption(ui: &mut Ui, text: &str) {
    ui.label(egui::RichText::new(text.to_uppercase()).font(FontId::new(11.0, semibold())).color(FAINT).extra_letter_spacing(0.8));
}

/// A filled accent button.
pub fn primary(text: impl Into<String>) -> egui::Button<'static> {
    egui::Button::new(egui::RichText::new(text.into()).color(Color32::WHITE).font(FontId::new(14.0, semibold())))
        .fill(Color32::from_rgb(0x5b, 0x6b, 0xf0))
        .stroke(Stroke::NONE)
        .corner_radius(CornerRadius::same(8))
}

/// A red outlined button.
pub fn danger(text: impl Into<String>) -> egui::Button<'static> {
    egui::Button::new(egui::RichText::new(text.into()).color(RED))
        .fill(RED.gamma_multiply(0.10))
        .stroke(Stroke::new(1.0, RED.gamma_multiply(0.45)))
        .corner_radius(CornerRadius::same(8))
}

/// A quiet button.
pub fn ghost(text: impl Into<String>) -> egui::Button<'static> {
    egui::Button::new(egui::RichText::new(text.into()).color(TEXT)).corner_radius(CornerRadius::same(8))
}
