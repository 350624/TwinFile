//! Native desktop adaptation of the dark product-launch stage. No bitmaps or animation atlas.
use crate::motion::{ListMotion, PaintMotion};
use eframe::egui::*;
pub const BLACK: Color32 = Color32::BLACK;
pub const CARD: Color32 = Color32::from_rgb(29, 29, 31);
pub const TEXT: Color32 = Color32::from_rgb(245, 245, 247);
pub const MUTED: Color32 = Color32::from_rgb(134, 134, 139);
pub const BLUE: Color32 = Color32::from_rgb(41, 151, 255);
pub fn family() -> FontFamily {
    FontFamily::Name("keynote-interface".into())
}
pub fn font(n: f32) -> FontId {
    FontId::new(n, family())
}
pub fn active(ctx: &Context) -> bool {
    ctx.style()
        .text_styles
        .get(&TextStyle::Body)
        .is_some_and(|f| f.family == family())
}
pub fn apply(style: &mut Style, reduced: bool) {
    style.visuals = Visuals::dark();
    let v = &mut style.visuals;
    v.panel_fill = BLACK;
    v.window_fill = CARD;
    v.faint_bg_color = Color32::from_gray(37);
    v.extreme_bg_color = Color32::from_gray(21);
    v.text_edit_bg_color = Some(v.extreme_bg_color);
    v.override_text_color = Some(TEXT);
    v.weak_text_color = Some(MUTED);
    v.hyperlink_color = BLUE;
    v.selection.bg_fill = Color32::from_rgb(23, 55, 85);
    v.selection.stroke = Stroke::new(1.5_f32, BLUE);
    v.window_corner_radius = 24.into();
    v.menu_corner_radius = 16.into();
    v.window_stroke = Stroke::NONE;
    v.window_highlight_topmost = false;
    v.window_shadow = epaint::Shadow::NONE;
    v.popup_shadow = epaint::Shadow {
        offset: [0, 6],
        blur: 18,
        spread: 0,
        color: Color32::from_black_alpha(160),
    };
    v.warn_fg_color = TEXT;
    v.error_fg_color = TEXT;
    for (w, grey) in [
        (&mut v.widgets.noninteractive, 29),
        (&mut v.widgets.inactive, 40),
        (&mut v.widgets.hovered, 59),
        (&mut v.widgets.active, 75),
        (&mut v.widgets.open, 52),
    ] {
        w.bg_fill = Color32::from_gray(grey);
        w.weak_bg_fill = w.bg_fill;
        w.corner_radius = 16.into();
        w.bg_stroke = Stroke::NONE;
        w.fg_stroke = Stroke::new(1.25_f32, TEXT);
    }
    v.widgets.noninteractive.bg_stroke = Stroke::new(1_f32, Color32::from_white_alpha(25));
    v.widgets.active.bg_stroke = Stroke::new(1_f32, BLUE);
    v.text_cursor.stroke.color = BLUE;
    style.animation_time = if reduced { 0. } else { 0.3 };
    style.spacing.item_spacing = vec2(8., 6.);
    style.spacing.button_padding = vec2(10., 4.);
    style.spacing.interact_size.y = 28.;
    style.spacing.menu_margin = Margin::same(10);
    style.spacing.window_margin = Margin::same(18);
    for (r, n) in [
        (TextStyle::Heading, 24.),
        (TextStyle::Body, 14.),
        (TextStyle::Button, 14.),
        (TextStyle::Small, 12.),
    ] {
        style.text_styles.insert(r, font(n));
    }
}
pub fn kicker(ui: &mut Ui, label: &str) {
    ui.label(
        RichText::new(label)
            .font(font(11.5))
            .extra_letter_spacing(1.8)
            .color(BLUE),
    );
}
pub fn action(ui: &mut Ui, label: &str, primary: bool, selected: bool) -> Response {
    let galley = ui.painter().layout_no_wrap(label.into(), font(14.), TEXT);
    let (rect, r) = ui.allocate_exact_size(vec2(galley.size().x + 22., 30.), Sense::click());
    r.widget_info(|| WidgetInfo::labeled(WidgetType::Button, ui.is_enabled(), label));
    let (h, p) = crate::motion::feedback(ui, &r);
    let shape = rect.shrink2(rect.size() * (0.01 * p));
    let bg = if primary {
        BLUE.lerp_to_gamma(Color32::from_rgb(0, 113, 227), h)
    } else if selected {
        Color32::from_gray(48)
    } else {
        Color32::from_gray(52).gamma_multiply(h * 0.85)
    };
    ui.painter().rect_filled(shape, 100, bg);
    if r.has_focus() {
        ui.painter()
            .rect_stroke(shape, 100, Stroke::new(1_f32, BLUE), StrokeKind::Inside);
    }
    ui.painter().galley(
        rect.center() - galley.size() / 2.,
        galley,
        if primary { Color32::WHITE } else { TEXT },
    );
    r
}
pub fn bubble(trigger: &Response, width: f32, section: &str, content: impl FnOnce(&mut Ui)) {
    let pop = Popup::menu(trigger);
    let ctx = &trigger.ctx;
    let open = pop.is_open();
    let phase = ctx.animate_bool_with_time(
        trigger.id.with("keynote-menu"),
        open,
        if crate::motion::reduced(ctx) {
            0.
        } else {
            0.18
        },
    );
    let f = Frame::popup(&ctx.style())
        .fill(Color32::from_gray(38))
        .corner_radius(16)
        .stroke(Stroke::NONE)
        .inner_margin(Margin::same(12));
    pop.frame(f).width(width).gap(4. + 4. * phase).show(|ui| {
        ui.set_width(width - 24.);
        kicker(ui, section);
        ui.add_space(4.);
        ScrollArea::vertical()
            .max_height((ctx.content_rect().height() - 150.).clamp(120., 480.))
            .show(ui, content);
    });
}
pub fn menu_item(ui: &mut Ui, label: &str) -> Response {
    ui.add_sized(
        [ui.available_width(), 29.],
        Button::new(label)
            .frame_when_inactive(false)
            .right_text("›"),
    )
}
pub fn list_motion(kind: ListMotion, elapsed: f64) -> PaintMotion {
    let t = (elapsed / 0.30).clamp(0., 1.) as f32;
    let rest = (1. - t).powi(3);
    let d = if matches!(kind, ListMotion::Leave) {
        -8.
    } else {
        12.
    };
    PaintMotion {
        offset: vec2(0., rest * d),
        opacity: 1. - 0.72 * rest,
        done: t >= 1.,
    }
}
// A pair of machined storage plates: a vector keyframe sampled from actual pane scroll.
// Labels stay in live widgets, and the only graphics work is for the visible hero.
pub fn plates(p: &Painter, r: Rect, progress: f32, side: usize) {
    if !r.is_positive() || !p.clip_rect().intersects(r) {
        return;
    }
    let t = progress.clamp(0., 1.);
    let p = p.with_clip_rect(r.intersect(p.clip_rect()));
    let s = (r.height() / 84.).min(r.width() / 168.).min(1.5);
    let c = r.center();
    for k in 0..2 {
        let depth = k as f32;
        let cy = c + vec2(
            (depth - 0.5) * (25. + t * 22.),
            (0.5 - depth) * (16. + t * 8.),
        ) * s;
        let pt = |x: f32, y: f32| {
            cy + vec2(
                x * (1. - t * 0.10) + y * (0.64 - t * 0.18),
                y * (0.49 + t * 0.22) - x * (0.17 - t * 0.12),
            ) * s
        };
        let top = [
            pt(-45., -23.),
            pt(40., -23.),
            pt(45., -18.),
            pt(45., 19.),
            pt(40., 23.),
            pt(-40., 23.),
            pt(-45., 18.),
        ];
        let foot = vec2(0., 4.) * s;
        p.add(Shape::convex_polygon(
            top.iter().map(|q| *q + foot).collect(),
            Color32::from_gray(44),
            Stroke::NONE,
        ));
        p.add(Shape::convex_polygon(
            top.to_vec(),
            Color32::from_gray(if k == 0 { 31 } else { 54 }),
            Stroke::new(0.9 * s, Color32::from_gray(if k == 0 { 86 } else { 151 })),
        ));
        for y in [-8., -3., 2., 7.] {
            p.line_segment(
                [pt(9., y), pt(32., y)],
                Stroke::new(s, Color32::from_gray(87)),
            );
        }
        p.line_segment(
            [pt(-32., 10.), pt(-21., 10.)],
            Stroke::new(2. * s, if k == side { BLUE } else { MUTED }),
        );
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn fixed_palette_and_motion() {
        let mut st = Style::default();
        apply(&mut st, false);
        assert_eq!(st.visuals.panel_fill, BLACK);
        assert_eq!(st.visuals.selection.stroke.color, BLUE);
        let a = list_motion(ListMotion::Enter, 0.);
        let b = list_motion(ListMotion::Enter, 0.15);
        let c = list_motion(ListMotion::Enter, 0.30);
        assert!(
            a.offset.y > b.offset.y
                && b.offset.y > c.offset.y
                && a.opacity < b.opacity
                && b.opacity < c.opacity
                && c.done
        );
        apply(&mut st, true);
        assert_eq!(st.animation_time, 0.);
    }
}
