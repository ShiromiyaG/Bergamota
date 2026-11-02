//! how the launcher looks: the port's colors (each game's, from port.toml),
//! a background painted from them (no picture of anyone's: shapes of our
//! own), Nunito, and the pieces the pages share.

use std::sync::Arc;

use eframe::egui::{self, Color32, FontFamily, FontId, Pos2, Rect, RichText, Stroke, Vec2};

use crate::port;

/// the ground everything sits on, and the pages' panel over it
pub const INK: Color32 = Color32::from_rgb(0x10, 0x11, 0x16);
pub const PANEL: Color32 = Color32::from_rgb(0x16, 0x17, 0x1E);
pub const LINE: Color32 = Color32::from_rgb(0x2A, 0x2C, 0x36);
pub const TEXT: Color32 = Color32::from_rgb(0xEC, 0xEC, 0xF1);
pub const WEAK: Color32 = Color32::from_rgb(0x9A, 0x9C, 0xA8);
pub const GOOD: Color32 = Color32::from_rgb(0x6F, 0xCF, 0x8E);
pub const WARN: Color32 = Color32::from_rgb(0xE8, 0xB0, 0x4A);
pub const BAD: Color32 = Color32::from_rgb(0xE8, 0x5A, 0x5A);
/// a game's color when its port names none
const ACCENT: Color32 = Color32::from_rgb(0xE0, 0x5A, 0x3A);

/// "#RRGGBB"
pub fn hex(text: &str) -> Option<Color32> {
    let digits = text.trim().trim_start_matches('#');
    let value = u32::from_str_radix(digits, 16).ok().filter(|_| digits.len() == 6)?;
    Some(Color32::from_rgb((value >> 16) as u8, (value >> 8) as u8, value as u8))
}

/// a game's color, by its title id
pub fn accent(title: &str) -> Color32 {
    port::get()
        .ok()
        .and_then(|port| port.games.iter().find(|game| game.title.eq_ignore_ascii_case(title)))
        .and_then(|game| game.color.as_deref())
        .and_then(hex)
        .unwrap_or(ACCENT)
}

/// the other game's color, for the second glow: the port's next game
pub fn partner(title: &str) -> Color32 {
    let Ok(port) = port::get() else { return ACCENT };
    let at = port.games.iter().position(|game| game.title.eq_ignore_ascii_case(title)).unwrap_or(0);
    port.games.get((at + 1) % port.games.len().max(1)).map_or(ACCENT, |game| accent(&game.title))
}

pub fn mix(a: Color32, b: Color32, t: f32) -> Color32 {
    let lerp = |x: u8, y: u8| (f32::from(x) + (f32::from(y) - f32::from(x)) * t).round() as u8;
    Color32::from_rgb(lerp(a.r(), b.r()), lerp(a.g(), b.g()), lerp(a.b(), b.b()))
}

/// the titles' font
pub fn title_font(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("title".into()))
}

/// Nunito and a dark look with the port's first color
pub fn install(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    fonts.font_data.insert("nunito".into(), Arc::new(egui::FontData::from_static(include_bytes!("../assets/Nunito-SemiBold.ttf"))));
    fonts.font_data.insert("nunito-black".into(), Arc::new(egui::FontData::from_static(include_bytes!("../assets/Nunito-Black.ttf"))));
    if let Some(family) = fonts.families.get_mut(&FontFamily::Proportional) {
        family.insert(0, "nunito".into());
    }
    let mut title = vec!["nunito-black".to_owned()];
    title.extend(fonts.families.get(&FontFamily::Proportional).cloned().unwrap_or_default());
    fonts.families.insert(FontFamily::Name("title".into()), title);
    ctx.set_fonts(fonts);

    let accent = port::get().ok().and_then(|port| port.games.first()).map_or(ACCENT, |game| self::accent(&game.title));
    ctx.set_theme(egui::Theme::Dark);
    ctx.all_styles_mut(|style| {
        style.text_styles.insert(egui::TextStyle::Body, FontId::proportional(15.0));
        style.text_styles.insert(egui::TextStyle::Button, FontId::proportional(15.0));
        style.text_styles.insert(egui::TextStyle::Small, FontId::proportional(12.5));
        style.text_styles.insert(egui::TextStyle::Heading, title_font(30.0));
        style.spacing.item_spacing = Vec2::new(10.0, 8.0);
        style.spacing.button_padding = Vec2::new(12.0, 6.0);
        style.spacing.interact_size.y = 28.0;
        let visuals = &mut style.visuals;
        visuals.override_text_color = Some(TEXT);
        visuals.panel_fill = PANEL;
        visuals.window_fill = PANEL;
        visuals.extreme_bg_color = INK;
        visuals.selection.bg_fill = accent;
        visuals.selection.stroke = Stroke::new(1.0, TEXT);
        visuals.hyperlink_color = accent;
        for widget in [&mut visuals.widgets.inactive, &mut visuals.widgets.hovered, &mut visuals.widgets.active, &mut visuals.widgets.open] {
            widget.corner_radius = 6.0.into();
        }
        visuals.widgets.inactive.weak_bg_fill = Color32::from_rgb(0x22, 0x24, 0x2D);
        visuals.widgets.inactive.bg_fill = Color32::from_rgb(0x22, 0x24, 0x2D);
        visuals.widgets.inactive.bg_stroke = Stroke::new(1.0, LINE);
        visuals.widgets.hovered.weak_bg_fill = Color32::from_rgb(0x2C, 0x2E, 0x39);
        visuals.widgets.hovered.bg_stroke = Stroke::new(1.0, mix(LINE, accent, 0.6));
        visuals.widgets.active.weak_bg_fill = mix(Color32::from_rgb(0x2C, 0x2E, 0x39), accent, 0.4);
        visuals.widgets.noninteractive.bg_stroke = Stroke::new(1.0, LINE);
    });
}

/// a soft disc of color fading out to nothing
fn glow(painter: &egui::Painter, center: Pos2, radius: f32, color: Color32) {
    let mut mesh = egui::Mesh::default();
    mesh.colored_vertex(center, color);
    const STEPS: u32 = 72;
    for step in 0..=STEPS {
        let angle = step as f32 / STEPS as f32 * std::f32::consts::TAU;
        mesh.colored_vertex(center + Vec2::angled(angle) * radius, Color32::TRANSPARENT);
        if step > 0 {
            mesh.add_triangle(0, step, step + 1);
        }
    }
    painter.add(mesh);
}

/// the window's ground: dark above, the game's color rising from below
/// with the port's other game across from it, and rings around the glow
pub fn background(painter: &egui::Painter, rect: Rect, accent: Color32, partner: Color32) {
    painter.rect_filled(rect, 0.0, INK);
    let mut mesh = egui::Mesh::default();
    mesh.colored_vertex(rect.left_top(), INK);
    mesh.colored_vertex(rect.right_top(), INK);
    mesh.colored_vertex(rect.right_bottom(), mix(INK, accent, 0.42));
    mesh.colored_vertex(rect.left_bottom(), mix(INK, partner, 0.22));
    mesh.add_triangle(0, 1, 2);
    mesh.add_triangle(0, 2, 3);
    painter.add(mesh);

    let size = rect.width().max(rect.height());
    let sun = Pos2::new(rect.right() - rect.width() * 0.16, rect.bottom() - rect.height() * 0.08);
    glow(painter, sun, size * 0.62, accent.gamma_multiply(0.38));
    glow(painter, Pos2::new(rect.left() + rect.width() * 0.08, rect.top() + rect.height() * 0.18), size * 0.42, partner.gamma_multiply(0.16));
    for ring in 1..=5 {
        let radius = size * (0.12 + ring as f32 * 0.085);
        painter.circle_stroke(sun, radius, Stroke::new(1.0, accent.gamma_multiply(0.11 - ring as f32 * 0.015)));
    }
}

/// a small note in a rounded outline: good, a warning, or plain
pub fn chip(ui: &mut egui::Ui, text: &str, color: Color32) {
    egui::Frame::NONE
        .stroke(Stroke::new(1.0, color.gamma_multiply(0.8)))
        .fill(INK.gamma_multiply(0.55))
        .corner_radius(10.0)
        .inner_margin(egui::Margin::symmetric(9, 3))
        .show(ui, |ui| ui.label(RichText::new(text).size(13.0).color(color)));
}

/// the big button: Play, Set up
pub fn big_button(ui: &mut egui::Ui, text: &str, accent: Color32) -> egui::Response {
    ui.add_sized(
        [210.0, 60.0],
        egui::Button::new(RichText::new(text).font(title_font(26.0)).color(Color32::WHITE))
            .fill(accent)
            .stroke(Stroke::new(1.0, mix(accent, Color32::WHITE, 0.25)))
            .corner_radius(8.0),
    )
}

/// a page's section title, in the game's color, with its line
pub fn section(ui: &mut egui::Ui, text: &str, accent: Color32) {
    ui.add_space(14.0);
    ui.label(RichText::new(text.to_uppercase()).font(title_font(15.0)).color(accent));
    let (rect, _) = ui.allocate_exact_size(Vec2::new(ui.available_width(), 1.0), egui::Sense::hover());
    ui.painter().rect_filled(rect, 0.0, LINE);
    ui.add_space(6.0);
}

/// a setting: its name and what it does on the left, its control on the right
pub fn row(ui: &mut egui::Ui, name: &str, about: &str, control: impl FnOnce(&mut egui::Ui)) {
    ui.horizontal(|ui| {
        let width = ui.available_width();
        ui.allocate_ui_with_layout(Vec2::new(width * 0.58, 0.0), egui::Layout::top_down(egui::Align::Min), |ui| {
            ui.label(RichText::new(name).size(15.5));
            if !about.is_empty() {
                ui.label(RichText::new(about).size(12.5).color(WEAK));
            }
        });
        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), control);
    });
    ui.add_space(4.0);
}

/// a page's title and what it is for
pub fn page_title(ui: &mut egui::Ui, title: &str, about: &str) {
    ui.add_space(18.0);
    ui.label(RichText::new(title).font(title_font(32.0)));
    if !about.is_empty() {
        ui.label(RichText::new(about).color(WEAK));
    }
    ui.add_space(4.0);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn colors_read() {
        assert_eq!(hex("#F08A24"), Some(Color32::from_rgb(0xF0, 0x8A, 0x24)));
        assert_eq!(hex("2f6fe4"), Some(Color32::from_rgb(0x2F, 0x6F, 0xE4)));
        assert_eq!(hex("#FFF"), None);
        assert_eq!(hex("nothing"), None);
        assert_eq!(mix(Color32::BLACK, Color32::WHITE, 0.5), Color32::from_rgb(128, 128, 128));
    }
}
