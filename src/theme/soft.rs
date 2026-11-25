//! Desktop interpretation of design.md. All geometry is in logical points.
use super::rgb;
use eframe::egui::*;
use twinfile_rust::config::Settings;

pub fn family() -> FontFamily {
    FontFamily::Name("soft-interface".into())
}
pub fn font(size: f32) -> FontId {
    FontId::new(size, family())
}
pub fn active(ctx: &Context) -> bool {
    ctx.style()
        .text_styles
        .get(&TextStyle::Body)
        .is_some_and(|f| f.family == family())
}
pub fn glow(color: Color32, alpha: u8, blur: u8, dy: i8) -> epaint::Shadow {
    epaint::Shadow {
        offset: [0, dy],
        blur,
        spread: 0,
        color: Color32::from_rgba_unmultiplied(color.r(), color.g(), color.b(), alpha),
    }
}
pub fn card_frame(ui: &Ui, margin: i8) -> Frame {
    Frame::new()
        .fill(ui.visuals().window_fill)
        .corner_radius(24)
        .inner_margin(Margin::same(margin))
        .shadow(glow(
            rgb(0x94a3b8),
            if ui.visuals().dark_mode { 19 } else { 48 },
            28,
            6,
        ))
}
pub fn apply(style: &mut Style, s: &Settings) {
    let (bg, card, ink, muted, primary) = if s.dark {
        (0x1b1e2c, 0x282c3e, 0xe7e9f5, 0xb1b9ce, 0xa5a7ff)
    } else {
        (0xf1f5f9, 0xffffff, 0x1f2937, 0x64748b, 0x6366f1)
    };
    let a = rgb([
        primary, 0x198268, 0x8156b3, 0xa65332, 0xaf4c79, 0x267c87, 0x8d681d,
    ][s.accent.min(6)]);
    let v = &mut style.visuals;
    v.panel_fill = rgb(bg);
    v.window_fill = rgb(card);
    v.extreme_bg_color = rgb(bg);
    v.faint_bg_color = rgb(bg);
    v.text_edit_bg_color = Some(rgb(bg));
    v.override_text_color = Some(rgb(ink));
    v.weak_text_color = Some(rgb(muted));
    v.window_stroke = Stroke::NONE;
    v.window_highlight_topmost = false;
    v.window_corner_radius = 24.into();
    v.menu_corner_radius = 20.into();
    v.window_shadow = glow(a, 42, 32, 10);
    v.popup_shadow = glow(a, 52, 26, 8);
    v.selection.bg_fill = rgb(card).lerp_to_gamma(a, if s.dark { 0.22 } else { 0.13 });
    v.selection.stroke = Stroke::new(1.5_f32, a);
    v.hyperlink_color = a;
    v.warn_fg_color = rgb(if s.dark { 0xfcd34d } else { 0x97600c });
    v.error_fg_color = rgb(if s.dark { 0xfda4af } else { 0xad365a });
    for (w, t) in [
        (&mut v.widgets.noninteractive, 0.),
        (&mut v.widgets.inactive, 0.02),
        (&mut v.widgets.hovered, 0.12),
        (&mut v.widgets.active, 0.22),
        (&mut v.widgets.open, 0.16),
    ] {
        let fill = rgb(bg).lerp_to_gamma(a, t);
        w.corner_radius = 16.into();
        w.bg_fill = fill;
        w.weak_bg_fill = fill;
        w.bg_stroke = Stroke::NONE;
        w.fg_stroke = Stroke::new(1.5_f32, rgb(ink));
    }
    // A translucent focus ring remains visible for keyboard use without reflow.
    v.widgets.active.bg_stroke = Stroke::new(2.0_f32, a.gamma_multiply(0.55));
    style.animation_time = if s.reduce_motion { 0. } else { 0.30 };
    style.spacing.item_spacing = vec2(8., 8.);
    style.spacing.interact_size.y = 30.;
    style.spacing.button_padding = vec2(12., 5.);
    style.spacing.menu_margin = Margin::same(10);
    style.spacing.window_margin = Margin::same(16);
    for (role, n) in [
        (TextStyle::Body, 14.),
        (TextStyle::Button, 14.),
        (TextStyle::Small, 12.),
        (TextStyle::Heading, 20.),
    ] {
        style.text_styles.insert(role, font(n));
    }
}
fn colors(ui: &Ui) -> (Color32, Color32, Color32) {
    (
        ui.visuals().selection.stroke.color,
        ui.visuals().text_color(),
        ui.visuals().window_fill,
    )
}
fn level(ui: &Ui, r: &Response, key: &str, on: bool, seconds: f32) -> f32 {
    if crate::motion::reduced(ui.ctx()) {
        return if on { 1. } else { 0. };
    }
    ui.ctx()
        .animate_bool_with_time_and_easing(r.id.with(key), on, seconds, |v| v * v * (3. - 2. * v))
}
pub fn chip(ui: &mut Ui, text: &str, tip: &str, on: bool, main: bool) -> Response {
    let galley = ui
        .painter()
        .layout_no_wrap(text.into(), font(14.), Color32::WHITE);
    let (slot, r) =
        ui.allocate_exact_size(vec2((galley.size().x + 24.).max(32.), 32.), Sense::click());
    r.widget_info(|| WidgetInfo::selected(WidgetType::Button, ui.is_enabled(), on, tip));
    let (a, ink, _) = colors(ui);
    let h = level(ui, &r, "float", r.hovered(), 0.30);
    let down = level(ui, &r, "sink", r.is_pointer_button_down_on(), 0.15);
    let animate = !crate::motion::reduced(ui.ctx());
    let rect = if animate {
        slot.translate(vec2(0., -4. * h * (1. - down)))
            .shrink2(slot.size() * 0.020 * down)
    } else {
        slot
    };
    let bg = if main {
        a
    } else if on {
        ui.visuals().selection.bg_fill
    } else {
        ui.visuals().extreme_bg_color
    };
    let p = ui.painter();
    if main || h > 0.01 {
        p.add(
            glow(
                a,
                ((if main { 38. } else { 0. } + 36. * h) * (1. - down)) as u8,
                24,
                5,
            )
            .as_shape(rect, 16),
        );
    }
    p.rect_filled(rect, 16, bg.lerp_to_gamma(ink, 0.05 * h + 0.09 * down));
    if down > 0.01 {
        p.rect_stroke(
            rect.shrink(1.),
            16,
            Stroke::new(2.0_f32, a.gamma_multiply(0.34 * down)),
            StrokeKind::Inside,
        );
    }
    if r.has_focus() {
        p.rect_stroke(
            slot.expand(1.),
            16,
            Stroke::new(2.0_f32, a),
            StrokeKind::Outside,
        );
    }
    let fg = if main {
        if ui.visuals().dark_mode {
            rgb(0x1b1e2c)
        } else {
            Color32::WHITE
        }
    } else {
        ink
    };
    p.galley_with_override_text_color(rect.center() - galley.size() * 0.5, galley, fg);
    r.on_hover_text(tip)
        .on_hover_cursor(CursorIcon::PointingHand)
}
pub fn nav(
    ui: &mut Ui,
    label: &str,
    tip: &str,
    selected: bool,
    tint: Color32,
    icon: impl FnOnce(&Painter, Pos2, Color32),
) -> Response {
    let (rect, r) = ui.allocate_exact_size(vec2(ui.available_width(), 34.), Sense::click());
    r.widget_info(|| {
        WidgetInfo::selected(WidgetType::SelectableLabel, ui.is_enabled(), selected, tip)
    });
    let h = level(ui, &r, "nav", r.hovered(), 0.50);
    let (accent, ink, base) = colors(ui);
    let shift = if crate::motion::reduced(ui.ctx()) {
        0.
    } else {
        2. * h
    };
    let b = rect.translate(vec2(shift, 0.)).shrink2(vec2(2., 0.));
    let p = ui.painter();
    if selected || h > 0.01 {
        p.rect_filled(
            b,
            16,
            base.lerp_to_gamma(accent, if selected { 0.14 } else { 0.06 * h }),
        );
    }
    let c = pos2(b.left() + 16., b.center().y);
    p.circle_filled(c, 12., base.lerp_to_gamma(tint, 0.14 + 0.06 * h));
    icon(p, c, tint);
    if b.width() > 54. {
        let textrect = Rect::from_min_max(pos2(c.x + 18., b.top()), b.right_bottom());
        p.with_clip_rect(textrect.intersect(ui.clip_rect())).text(
            textrect.left_center(),
            Align2::LEFT_CENTER,
            label,
            font(13.),
            ink,
        );
    }
    if r.has_focus() {
        p.rect_stroke(b, 16, Stroke::new(1.5_f32, accent), StrokeKind::Inside);
    }
    r.on_hover_text(tip)
        .on_hover_cursor(CursorIcon::PointingHand)
}
// The hit area doesn't move with the button. Only the bubble and its contents ease in.
fn bubble_values(progress: f32) -> (f32, f32) {
    let t = progress.clamp(0., 1.);
    (4. + 6. * (1. - t), 0.55 + 0.45 * t)
}
pub fn bubble<R>(parent: &Ui, trigger: &Response, width: f32, content: impl FnOnce(&mut Ui) -> R) {
    let pop = Popup::menu(trigger);
    let open = pop.is_open();
    let t = if crate::motion::reduced(parent.ctx()) {
        1.
    } else {
        parent.ctx().animate_bool_with_time_and_easing(
            trigger.id.with("soft-bubble-in"),
            open,
            0.30,
            |v| 1. - (1. - v).powi(3),
        )
    };
    let (gap, opacity) = bubble_values(t);
    let frame = Frame::popup(parent.style())
        .fill(parent.visuals().window_fill)
        .corner_radius(20)
        .stroke(Stroke::NONE)
        .inner_margin(Margin::same(10))
        .shadow(glow(parent.visuals().selection.stroke.color, 52, 30, 8));
    pop.width(width).gap(gap).frame(frame).show(|ui| {
        ui.multiply_opacity(opacity);
        content(ui)
    });
}
pub fn list_motion(elapsed: f64) -> crate::motion::PaintMotion {
    let t = (elapsed / 0.40).clamp(0., 1.) as f32;
    let rest = (1. - t).powi(3);
    crate::motion::PaintMotion {
        offset: vec2(0., 8. * rest),
        opacity: 1. - 0.65 * rest,
        done: t >= 1.,
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn bubbles_move_and_fade_to_rest() {
        let start = bubble_values(0.);
        let middle = bubble_values(0.5);
        let end = bubble_values(1.);
        assert!(start.0 > middle.0 && middle.0 > end.0 && start.1 < middle.1 && middle.1 < end.1);
        assert_eq!(end, (4., 1.));
    }
    #[test]
    fn switching_theme_restores_spacing_and_reduced_motion() {
        let ctx = Context::default();
        let mut s = Settings::default();
        s.theme = twinfile_rust::config::Theme::Miuix;
        super::super::apply(&ctx, &s);
        let before = ctx.style();
        s.theme = twinfile_rust::config::Theme::Soft;
        s.reduce_motion = true;
        super::super::apply(&ctx, &s);
        assert!(active(&ctx));
        assert!(crate::motion::reduced(&ctx));
        s.theme = twinfile_rust::config::Theme::Miuix;
        s.reduce_motion = false;
        super::super::apply(&ctx, &s);
        assert!(!active(&ctx));
        let after = ctx.style();
        assert_eq!(before.spacing, after.spacing);
        assert_eq!(before.text_styles, after.text_styles);
        assert_eq!(before.visuals, after.visuals);
        assert_eq!(before.animation_time, after.animation_time);
    }
    #[test]
    fn soft_motion_settles() {
        let start = list_motion(0.);
        let mid = list_motion(0.18);
        let end = list_motion(0.4);
        assert!(start.offset.y > mid.offset.y && mid.offset.y > end.offset.y);
        assert!(end.done && end.opacity == 1.);
    }
}
