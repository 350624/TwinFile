// Color roles and geometry adapted from compose-miuix-ui/miuix.
// See MIUIX-NOTICE.md for the source revision and Apache-2.0 notices.
use eframe::egui::{self, Color32, FontId, Pos2, Rect, Stroke, Vec2, pos2, vec2};

use super::rgb;

#[derive(Clone, Copy)]
pub(crate) struct Colors {
    pub surface: Color32,
    pub container: Color32,
    pub popup: Color32,
    pub on_surface: Color32,
    pub summary: Color32,
    pub field: Color32,
    pub field_ink: Color32,
    pub primary: Color32,
    pub outline: Color32,
    pub divider: Color32,
    pub selected: Color32,
    pub selected_ink: Color32,
    pub disabled: Color32,
    pub error: Color32,
}

impl Colors {
    pub(crate) fn new(dark: bool) -> Self {
        let c = |light, night| rgb(if dark { night } else { light });
        Self {
            surface: c(0xf7f7f7, 0x000000),
            container: c(0xffffff, 0x242424),
            popup: c(0xe8e8e8, 0x2d2d2d),
            on_surface: c(0x000000, 0xf2f2f2),
            summary: if dark {
                Color32::from_white_alpha(128)
            } else {
                Color32::from_black_alpha(153)
            },
            field: c(0xf0f0f0, 0x434343),
            field_ink: c(0x303030, 0xd9d9d9),
            primary: c(0x3482ff, 0x277af7),
            outline: c(0xd9d9d9, 0x404040),
            divider: c(0xe0e0e0, 0x393939),
            selected: c(0xeaf2ff, 0x2b3b54),
            selected_ink: c(0x3482ff, 0x4788ff),
            disabled: c(0xb2b2b2, 0x666666),
            error: c(0xe94634, 0xf12522),
        }
    }

    pub(crate) fn workbench(self) -> Color32 {
        // A desktop canvas stays one tone across all workspace panels.
        if self.surface == Color32::BLACK {
            self.surface.lerp_to_gamma(self.container, 0.62)
        } else {
            self.surface
        }
    }

    pub(crate) fn of(ui: &egui::Ui) -> Self {
        let mut c = Self::new(ui.visuals().dark_mode);
        let accent = ui.visuals().selection.stroke.color;
        if accent != c.primary {
            c.selected = c
                .container
                .lerp_to_gamma(accent, if ui.visuals().dark_mode { 0.28 } else { 0.16 });
            c.selected_ink = if ui.visuals().dark_mode {
                accent.lerp_to_gamma(Color32::WHITE, 0.35)
            } else {
                accent.lerp_to_gamma(Color32::BLACK, 0.20)
            };
        }
        c.primary = accent;
        c
    }
}

pub(crate) const TITLE: f32 = 20.0; // title3
pub(crate) const BODY: f32 = 14.0; // body2, compact desktop workbench
pub(crate) const NOTE: f32 = 13.0; // footnote1
pub(crate) const META: f32 = 11.0; // footnote2
pub(crate) const CARD_RADIUS: f32 = 16.0;
pub(crate) const TAB_WIDTH: f32 = 155.0;
const TAB_HEIGHT: f32 = 45.0;
const TAB_PADDING: f32 = 5.0;
const TAB_GAP: f32 = 5.0;

pub(crate) fn display_family() -> egui::FontFamily {
    egui::FontFamily::Name("miuix-display".into())
}

pub(crate) fn heading(size: f32) -> FontId {
    FontId::new(size, display_family())
}

pub(crate) fn font(size: f32) -> FontId {
    FontId::proportional(size)
}

/// CPU counterpart of SquirclePath.kt for the egui paint backend.
/// Sample its four clockwise corner cubics; the polygon is convex.
fn corners(r: Rect, radius: f32) -> Vec<Pos2> {
    let half = r.width().min(r.height()) * 0.5;
    let t = (radius * 1.1).clamp(0.0, half);
    if t <= 0.0 {
        return vec![
            r.left_top(),
            r.right_top(),
            r.right_bottom(),
            r.left_bottom(),
        ];
    }
    let threshold = half * std::f32::consts::FRAC_PI_4;
    let blend = ((t - threshold) / (half - threshold)).clamp(0.0, 1.0);
    let h = t * (1.0 - 0.643 + blend * (0.643 - 0.552_284_8));
    let (l, u, x, y) = (r.left(), r.top(), r.right(), r.bottom());
    let curves = [
        [
            pos2(x - t, u),
            pos2(x - h, u),
            pos2(x, u + h),
            pos2(x, u + t),
        ],
        [
            pos2(x, y - t),
            pos2(x, y - h),
            pos2(x - h, y),
            pos2(x - t, y),
        ],
        [
            pos2(l + t, y),
            pos2(l + h, y),
            pos2(l, y - h),
            pos2(l, y - t),
        ],
        [
            pos2(l, u + t),
            pos2(l, u + h),
            pos2(l + h, u),
            pos2(l + t, u),
        ],
    ];
    let mut polygon = Vec::with_capacity(32);
    for [p0, p1, p2, p3] in curves {
        for j in 0..8 {
            let u = j as f32 / 7.0;
            let a = p0.lerp(p1, u).lerp(p1.lerp(p2, u), u);
            let b = p1.lerp(p2, u).lerp(p2.lerp(p3, u), u);
            let p = a.lerp(b, u);
            if polygon.last() != Some(&p) {
                polygon.push(p);
            }
        }
    }
    if polygon.first() == polygon.last() {
        polygon.pop();
    }
    polygon
}

pub(crate) fn squircle(rect: Rect, radius: f32, fill: Color32, stroke: Stroke) -> egui::Shape {
    if !rect.is_positive() {
        return egui::Shape::Noop;
    }
    egui::Shape::convex_polygon(corners(rect, radius), fill, stroke)
}

/// The rounded track, indicator and search action all use semicircular ends.
pub(crate) fn pill(rect: Rect, fill: Color32, stroke: Stroke) -> egui::Shape {
    squircle(rect, rect.size().min_elem() * 0.5, fill, stroke)
}

/// Paint a Miuix Card surface behind the widgets without changing their hit boxes.
pub(crate) fn group<R>(
    ui: &mut egui::Ui,
    radius: f32,
    margin: egui::Margin,
    content: impl FnOnce(&mut egui::Ui) -> R,
) -> egui::InnerResponse<R> {
    let c = Colors::of(ui);
    let bg = ui.painter().add(egui::Shape::Noop);
    let response = egui::Frame::new().inner_margin(margin).show(ui, content);
    ui.painter().set(
        bg,
        squircle(response.response.rect, radius, c.container, Stroke::NONE),
    );
    response
}

pub(crate) fn dock_item(
    ui: &mut egui::Ui,
    size: f32,
    label: &str,
    selected: bool,
    icon: impl FnOnce(&egui::Painter, Pos2, Color32),
) -> egui::Response {
    dock_item_with_response(ui, size, label, selected, |p, at, ink, _| icon(p, at, ink))
}

pub(crate) fn dock_item_with_response(
    ui: &mut egui::Ui,
    size: f32,
    label: &str,
    selected: bool,
    icon: impl FnOnce(&egui::Painter, Pos2, Color32, &egui::Response),
) -> egui::Response {
    dock_item_emphasis(ui, size, label, f32::from(selected), icon)
}

pub(crate) fn dock_item_emphasis(
    ui: &mut egui::Ui,
    size: f32,
    label: &str,
    emphasis: f32,
    icon: impl FnOnce(&egui::Painter, Pos2, Color32, &egui::Response),
) -> egui::Response {
    let (r, response) = ui.allocate_exact_size(vec2(size, size), egui::Sense::click());
    response.widget_info(|| {
        egui::WidgetInfo::labeled(egui::WidgetType::Button, ui.is_enabled(), label)
    });
    let c = Colors::of(ui);
    let (hover, press) = crate::motion::feedback(ui, &response);
    let fill = c.field.gamma_multiply(emphasis.max(hover).max(press));
    let p = ui.painter();
    p.circle_filled(r.center(), size * 0.46 - press, fill);
    if response.has_focus() {
        p.circle_stroke(r.center(), size * 0.46, Stroke::new(1.0_f32, c.primary));
    }
    icon(
        p,
        r.center(),
        c.on_surface.lerp_to_gamma(c.selected_ink, emphasis),
        &response,
    );
    response
        .on_hover_cursor(egui::CursorIcon::PointingHand)
        .on_hover_text(label)
}

// Desktop file dialogs share one type scale. Keep the workspace title independent.
pub(crate) const DIALOG_TITLE: f32 = 16.;
pub(crate) const DIALOG_BODY: f32 = 12.5;
pub(crate) const DIALOG_NOTE: f32 = 11.5;

#[derive(Clone, Copy)]
pub(crate) enum SheetBody {
    ShortForm,
    Scrollable,
}

pub(crate) fn file_sheet(
    ctx: &egui::Context,
    id: egui::Id,
    title: &str,
    width: f32,
    body: SheetBody,
    contents: impl FnOnce(&mut egui::Ui),
) -> bool {
    let c = Colors::new(ctx.style().visuals.dark_mode);
    let area = egui::Modal::default_area(id).fade_in(!crate::motion::reduced(ctx));
    let modal = egui::Modal::new(id)
        .area(area)
        .backdrop_color(Color32::from_black_alpha(
            if ctx.style().visuals.dark_mode {
                104
            } else {
                64
            },
        ))
        .frame(
            egui::Frame::new()
                .fill(c.container)
                .corner_radius(18)
                .inner_margin(16)
                .stroke(Stroke::new(1_f32, c.outline.gamma_multiply(0.8)))
                .shadow(egui::epaint::Shadow {
                    offset: [0, 6],
                    blur: 20,
                    spread: 0,
                    color: Color32::from_black_alpha(55),
                }),
        )
        .show(ctx, |ui| {
            ui.set_width(width.min((ctx.content_rect().width() - 56.).max(100.)));
            let st = ui.style_mut();
            for (key, size) in [
                (egui::TextStyle::Heading, DIALOG_TITLE),
                (egui::TextStyle::Body, DIALOG_BODY),
                (egui::TextStyle::Button, DIALOG_BODY),
                (egui::TextStyle::Small, DIALOG_NOTE),
            ] {
                st.text_styles.insert(key, font(size));
            }
            st.text_styles
                .insert(egui::TextStyle::Monospace, FontId::monospace(12.));
            st.spacing.item_spacing = vec2(6., 5.);
            st.spacing.button_padding = vec2(9., 4.);
            st.spacing.interact_size.y = 25.;
            st.visuals.extreme_bg_color = c.field;
            st.visuals.widgets.inactive.weak_bg_fill = c.field;
            for w in [
                &mut st.visuals.widgets.inactive,
                &mut st.visuals.widgets.hovered,
                &mut st.visuals.widgets.active,
            ] {
                w.corner_radius = 8.into();
            }
            let close = ui
                .horizontal(|ui| {
                    ui.label(
                        egui::RichText::new(title)
                            .font(heading(DIALOG_TITLE))
                            .color(c.on_surface),
                    );
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        let r = ui
                            .add_sized(
                                [25., 25.],
                                egui::Button::new(
                                    egui::RichText::new("×").size(15.).color(c.field_ink),
                                )
                                .fill(c.field)
                                .corner_radius(13),
                            )
                            .on_hover_text("关闭");
                        r.widget_info(|| {
                            egui::WidgetInfo::labeled(egui::WidgetType::Button, true, "关闭")
                        });
                        ctx.data_mut(|d| d.insert_temp(id.with("close-target"), r.rect));
                        r.clicked()
                    })
                    .inner
                })
                .inner;
            ui.add_space(4.);
            match body {
                // A name form is only a selector, one field and two buttons. A nested
                // scrolling viewport here can clip the last row while the modal resizes.
                SheetBody::ShortForm => contents(ui),
                SheetBody::Scrollable => {
                    egui::ScrollArea::vertical()
                        .id_salt("sheet-body")
                        .max_height((ctx.content_rect().height() - 108.).max(80.))
                        .auto_shrink([false, true])
                        .show(ui, contents);
                }
            }
            close
        });
    modal.inner || modal.should_close()
}

pub(crate) fn overlay_button(ui: &mut egui::Ui, text: &str, primary: bool) -> egui::Response {
    let c = Colors::of(ui);
    let ink = if primary { Color32::WHITE } else { c.field_ink };
    let fill = if primary { c.primary } else { c.field };
    let text = egui::RichText::new(text).size(DIALOG_BODY).color(ink);
    ui.scope(|ui| {
        ui.visuals_mut().widgets.hovered.weak_bg_fill = fill.lerp_to_gamma(ink, 0.10);
        ui.visuals_mut().widgets.active.weak_bg_fill = fill.lerp_to_gamma(ink, 0.18);
        ui.add_sized(
            [78.0, 29.0],
            egui::Button::new(text).fill(fill).corner_radius(12),
        )
    })
    .inner
}

/// The contour tab row keeps interactive rectangles fixed while its indicator moves.
pub(crate) fn contour_tabs_named(
    ui: &mut egui::Ui,
    active: &mut usize,
    names: [&str; 2],
) -> [egui::Response; 2] {
    segment(ui, active, names, TAB_WIDTH, TAB_HEIGHT, BODY)
}

pub(crate) fn file_kind(ui: &mut egui::Ui, folder: &mut bool) {
    let mut slot = usize::from(!*folder);
    segment(ui, &mut slot, ["文件夹", "文件"], 122., 29., DIALOG_BODY);
    *folder = slot == 0;
}
fn segment(
    ui: &mut egui::Ui,
    active: &mut usize,
    names: [&str; 2],
    width: f32,
    height: f32,
    size: f32,
) -> [egui::Response; 2] {
    let width = ui.available_width().min(width);
    let (r, parent) = ui.allocate_exact_size(vec2(width, height), egui::Sense::hover());
    let inner = r.shrink(if height < 32. { 2.5 } else { TAB_PADDING });
    let slot = ((inner.width() - TAB_GAP) * 0.5).max(1.0);
    let parts = [
        Rect::from_min_size(inner.min, vec2(slot, inner.height())),
        Rect::from_min_size(
            pos2(inner.right() - slot, inner.top()),
            vec2(slot, inner.height()),
        ),
    ];

    let hits: [egui::Response; 2] = std::array::from_fn(|i| {
        ui.interact(parts[i], parent.id.with(i), egui::Sense::click())
            .on_hover_cursor(egui::CursorIcon::PointingHand)
    });

    *active = (*active).min(1);
    for (i, hit) in hits.iter().enumerate() {
        if hit.clicked() {
            *active = i;
        } else if hit.has_focus() {
            let next = ui.input_mut(|input| {
                if input.consume_key(egui::Modifiers::NONE, egui::Key::ArrowLeft) {
                    Some(0)
                } else if input.consume_key(egui::Modifiers::NONE, egui::Key::ArrowRight) {
                    Some(1)
                } else {
                    None
                }
            });
            if let Some(next) = next {
                *active = next;
                hits[next].request_focus();
            }
        }
    }
    let c = Colors::of(ui);
    // Animate in local coordinates so moving/resizing the dialog doesn't drag
    // its indicator in from a previous screen-space position.
    let target = parts[*active].translate(-r.min.to_vec2());
    let indicator = crate::motion::highlight(
        ui.ctx(),
        parent.id.with("segment-slide"),
        Some(target),
        0.24,
        false,
    )
    .unwrap()
    .translate(r.min.to_vec2());
    #[cfg(test)]
    ui.data_mut(|d| d.insert_temp(ui.id().with("segment-test-geometry"), (parts, indicator)));
    let p = ui.painter();
    p.add(pill(r, c.field, Stroke::NONE));
    p.add(pill(indicator, c.primary, Stroke::NONE));
    for (i, hit) in hits.iter().enumerate() {
        hit.widget_info(|| {
            egui::WidgetInfo::selected(
                egui::WidgetType::SelectableLabel,
                ui.is_enabled(),
                *active == i,
                names[i],
            )
        });
        let (hover, press) = crate::motion::feedback(ui, hit);
        if press > 0.0 || (hover > 0.0 && *active != i) {
            let fill = c.on_surface.gamma_multiply(0.10 * hover.max(press));
            p.add(pill(parts[i].shrink(1.3 * press), fill, Stroke::NONE));
        }
        if hit.has_focus() {
            p.add(pill(
                parts[i].shrink(1.0),
                Color32::TRANSPARENT,
                Stroke::new(1.0_f32, c.primary),
            ));
        }
        p.text(
            parts[i].center(),
            egui::Align2::CENTER_CENTER,
            names[i],
            font(size),
            c.field_ink.lerp_to_gamma(
                Color32::WHITE,
                (indicator.intersect(parts[i]).width().max(0.) / slot).clamp(0., 1.),
            ),
        );
    }
    hits
}

// 20×20 viewport, adapted from MiuixIcons.Basic.Search. Tint is a theme role.
pub(crate) fn search_icon(painter: &egui::Painter, center: Pos2, color: Color32) {
    let pos = |x: f32, y: f32| center + Vec2::new(x - 10., y - 10.);
    let stroke = Stroke::new(1.5_f32, color);
    painter.circle_stroke(pos(8.835, 8.581), 5.331, stroke);
    painter.line_segment([pos(12.95, 12.70), pos(17.0, 16.7)], stroke);
    painter.circle_filled(pos(17.0, 16.7), 0.75, color);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn upstream_squircle_has_clockwise_convex_silhouette() {
        for (w, h, rad) in [(155., 45., 13.), (20., 20., 40.), (2., 8., 0.)] {
            let r = Rect::from_min_size(Pos2::ZERO, vec2(w, h));
            let points = corners(r, rad);
            assert!(points.len() >= 4 && points.iter().all(|p| r.expand(0.001).contains(*p)));
            for i in 0..points.len() {
                let a = points[(i + 1) % points.len()] - points[i];
                let b = points[(i + 2) % points.len()] - points[(i + 1) % points.len()];
                assert!(a.x * b.y - a.y * b.x >= -0.001);
            }
        }
    }
}

#[cfg(test)]
mod switch_motion_tests {
    use super::*;
    #[test]
    fn file_folder_rapid_clicks_show_intermediate_indicator_positions() {
        let ctx = egui::Context::default();
        let mut folder = false;
        let mut draw = |time: f64, events: Vec<egui::Event>| {
            let mut geometry = None;
            let _ = ctx.run(
                egui::RawInput {
                    time: Some(time),
                    events,
                    screen_rect: Some(Rect::from_min_size(Pos2::ZERO, vec2(640., 410.))),
                    ..Default::default()
                },
                |ctx| {
                    egui::CentralPanel::default().show(ctx, |ui| {
                        file_kind(ui, &mut folder);
                        geometry = ui.data(|d| {
                            d.get_temp::<([Rect; 2], Rect)>(ui.id().with("segment-test-geometry"))
                        });
                    });
                },
            );
            (folder, geometry.unwrap())
        };
        let (_, (slots, steady)) = draw(0., vec![]);
        assert_eq!(steady, slots[1]);
        let click = |i: usize| {
            vec![
                egui::Event::PointerMoved(slots[i].center()),
                egui::Event::PointerButton {
                    pos: slots[i].center(),
                    button: egui::PointerButton::Primary,
                    pressed: true,
                    modifiers: Default::default(),
                },
                egui::Event::PointerButton {
                    pos: slots[i].center(),
                    button: egui::PointerButton::Primary,
                    pressed: false,
                    modifiers: Default::default(),
                },
            ]
        };
        assert!(draw(0.10, click(0)).0);
        let mid = draw(0.16, vec![]).1.1.center().x;
        assert!(slots[0].center().x < mid && mid < slots[1].center().x);
        let (folder, (_, rev)) = draw(0.165, click(1));
        assert!(!folder);
        assert!((rev.center().x - mid).abs() < 4.);
        let back = draw(0.23, vec![]).1.1.center().x;
        assert!(back > rev.center().x && back < slots[1].center().x);
        assert_eq!(draw(0.55, vec![]).1.1, slots[1]);
        ctx.style_mut(|s| s.animation_time = 0.);
        assert_eq!(draw(0.60, click(0)).1.1, slots[0]);
    }
}
