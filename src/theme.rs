pub(crate) mod keynote;
pub(crate) mod miuix;
pub(crate) mod soft;

use eframe::egui::{self, Color32, CornerRadius, FontId, Margin, Stroke, TextStyle, Vec2};
use twinfile_rust::config::{Settings, Theme};
pub fn fonts(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    let dir = std::env::var_os("WINDIR")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| "C:/Windows".into());
    if let Ok(data) = std::fs::read(dir.join("Fonts/msyh.ttc")) {
        fonts.font_data.insert(
            "windows-cjk".into(),
            egui::FontData::from_owned(data).into(),
        );
        for family in [egui::FontFamily::Proportional, egui::FontFamily::Monospace] {
            fonts
                .families
                .entry(family)
                .or_default()
                .push("windows-cjk".into());
        }
    }
    let mut display = fonts
        .families
        .get(&egui::FontFamily::Proportional)
        .cloned()
        .unwrap_or_default();
    if let Ok(data) = std::fs::read(dir.join("Fonts/msyhbd.ttc")) {
        fonts.font_data.insert(
            "windows-cjk-bold".into(),
            egui::FontData::from_owned(data).into(),
        );
        display.insert(0, "windows-cjk-bold".into());
    }
    fonts.families.insert(miuix::display_family(), display);
    let mut stack = fonts.families[&egui::FontFamily::Proportional].clone();
    for candidate in ["Inter-Regular.ttf", "Inter.ttf", "segoeui.ttf"] {
        if let Ok(data) = std::fs::read(dir.join("Fonts").join(candidate)) {
            fonts
                .font_data
                .insert("soft-latin".into(), egui::FontData::from_owned(data).into());
            stack.insert(0, "soft-latin".into());
            break;
        }
    }
    let mut keynote_stack = fonts.families[&egui::FontFamily::Proportional].clone();
    let mut face = None;
    for file in [
        "SF-Pro-Display-Regular.otf",
        "SFProDisplay-Regular.ttf",
        "segoeui.ttf",
    ] {
        if file == "segoeui.ttf"
            && fonts.font_data.contains_key("soft-latin")
            && !dir.join("Fonts/Inter.ttf").is_file()
            && !dir.join("Fonts/Inter-Regular.ttf").is_file()
        {
            face = Some("soft-latin");
            break;
        }
        if let Ok(data) = std::fs::read(dir.join("Fonts").join(file)) {
            fonts.font_data.insert(
                "keynote-face".into(),
                egui::FontData::from_owned(data).into(),
            );
            face = Some("keynote-face");
            break;
        }
    }
    if let Some(face) = face {
        keynote_stack.insert(0, face.into());
    }
    fonts.families.insert(keynote::family(), keynote_stack);
    fonts.families.insert(soft::family(), stack);
    ctx.set_fonts(fonts);
}
fn rgb(v: u32) -> Color32 {
    Color32::from_rgb((v >> 16) as u8, (v >> 8) as u8, v as u8)
}
pub fn apply(ctx: &egui::Context, s: &Settings) {
    let scheme = (s.theme == Theme::Miuix).then(|| miuix::Colors::new(s.dark));
    let mi = scheme.is_some();
    let (bg, surface, line, primary, radius) = match s.dark {
        false => (0xeef3f9, 0xffffff, 0xccd6e2, 0x285ab2, 10),
        true => (0x181e29, 0x232c3b, 0x414b5c, 0x92b9ff, 10),
    };
    let mut accent = rgb([
        primary, 0x17a064, 0x884cdb, 0xdb5c2b, 0xd34280, 0x008d9a, 0x9b7200,
    ][s.accent.min(6)]);
    if let Some(c) = scheme.filter(|_| s.accent == 0) {
        accent = c.primary;
    }
    let mut style = egui::Style::default();
    style.animation_time = if s.reduce_motion {
        0.0
    } else if mi {
        0.28
    } else {
        0.18
    };
    style.spacing.item_spacing = if mi {
        Vec2::new(8.0, 7.0)
    } else {
        Vec2::new(6.0, 5.0)
    };
    style.spacing.button_padding = if mi {
        Vec2::new(16.0, 9.0)
    } else {
        Vec2::new(9.0, 5.0)
    };
    style.spacing.interact_size.y = if mi { 34.0 } else { 27.0 };
    style.text_styles.insert(
        TextStyle::Heading,
        FontId::proportional(if mi { 24.0 } else { 21.0 }),
    );
    style.text_styles.insert(
        TextStyle::Body,
        FontId::proportional(if mi { miuix::BODY } else { 14.0 }),
    );
    style.visuals = if s.dark {
        egui::Visuals::dark()
    } else {
        egui::Visuals::light()
    };
    let v = &mut style.visuals;
    v.panel_fill = rgb(bg);
    v.window_fill = rgb(surface);
    v.extreme_bg_color = rgb(surface);
    v.faint_bg_color = rgb(bg);
    v.override_text_color = Some(rgb(if s.dark { 0xf2f2f2 } else { 0x171717 }));
    v.window_corner_radius = CornerRadius::same(radius);
    v.menu_corner_radius = CornerRadius::same(radius);
    v.window_stroke = Stroke::new(1.0_f32, rgb(line));
    v.selection.bg_fill = accent.gamma_multiply(if s.dark { 0.34 } else { 0.23 });
    v.selection.stroke = Stroke::new(1.0_f32, accent);
    v.hyperlink_color = accent;
    for (widget, amount) in [
        (&mut v.widgets.noninteractive, 0.0),
        (&mut v.widgets.inactive, 0.04),
        (&mut v.widgets.hovered, 0.12),
        (&mut v.widgets.active, 0.22),
        (&mut v.widgets.open, 0.12),
    ] {
        let base = rgb(surface).lerp_to_gamma(accent, amount);
        widget.corner_radius = CornerRadius::same(radius);
        widget.bg_fill = base;
        widget.weak_bg_fill = base;
        widget.bg_stroke = Stroke::new(if mi { 0.0_f32 } else { 1.0 }, rgb(line));
    }

    if let Some(c) = scheme {
        style
            .text_styles
            .insert(TextStyle::Button, miuix::font(miuix::BODY));
        style
            .text_styles
            .insert(TextStyle::Small, miuix::font(miuix::NOTE));
        v.panel_fill = c.surface;
        v.window_fill = c.container;
        v.extreme_bg_color = c.field;
        v.faint_bg_color = c.surface;
        v.window_stroke = Stroke::new(1.0_f32, c.outline);
        v.window_corner_radius = CornerRadius::same(miuix::CARD_RADIUS as u8);
        v.menu_corner_radius = v.window_corner_radius;
        v.override_text_color = Some(c.on_surface);
        v.weak_text_color = Some(c.summary);
        v.error_fg_color = c.error;
        v.selection.bg_fill = if s.accent == 0 {
            c.selected
        } else {
            c.container
                .lerp_to_gamma(accent, if s.dark { 0.28 } else { 0.16 })
        };
        for (w, amount) in [
            (&mut v.widgets.noninteractive, 0.0),
            (&mut v.widgets.inactive, 0.0),
            (&mut v.widgets.hovered, 0.12),
            (&mut v.widgets.active, 0.23),
            (&mut v.widgets.open, 0.12),
        ] {
            w.bg_fill = c.field.lerp_to_gamma(accent, amount);
            w.weak_bg_fill = w.bg_fill;
            w.fg_stroke.color = c.on_surface;
            w.corner_radius = CornerRadius::same(12);
        }
    }
    if !mi {
        let defaults = egui::Style::default();
        for role in [TextStyle::Button, TextStyle::Small] {
            style
                .text_styles
                .insert(role.clone(), defaults.text_styles[&role].clone());
        }
    }
    if s.theme == Theme::Soft {
        soft::apply(&mut style, s);
    }
    if s.theme == Theme::Keynote {
        keynote::apply(&mut style, s.reduce_motion);
    }
    ctx.set_style(style);
}
pub fn card(ui: &egui::Ui, theme: Theme) -> egui::Frame {
    let (radius, margin, border) = match theme {
        Theme::Miuix => (miuix::CARD_RADIUS as u8, 14, true),
        Theme::Keynote => {
            return egui::Frame::new()
                .fill(keynote::CARD)
                .corner_radius(24)
                .inner_margin(Margin::same(if ui.available_width() < 290. {
                    10
                } else {
                    16
                }));
        }
        Theme::Soft => return soft::card_frame(ui, 12),
        Theme::TwinFile => (12, 8, true),
    };
    egui::Frame::new()
        .fill(ui.visuals().window_fill())
        .corner_radius(radius)
        .inner_margin(Margin::same(margin))
        .stroke(if border {
            ui.visuals().window_stroke
        } else {
            Stroke::NONE
        })
}
pub fn row_height(theme: Theme) -> f32 {
    match theme {
        Theme::Miuix => 47.,
        Theme::TwinFile => 44.,
        Theme::Soft => 42.,
        Theme::Keynote => 48.,
    }
}
