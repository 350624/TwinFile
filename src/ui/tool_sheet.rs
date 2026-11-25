//! Content-sized desktop tool sheet. The list scrolls, not a preallocated empty window.
use crate::theme::miuix;
use eframe::egui::*;

pub(super) fn show(
    ctx: &Context,
    key: &'static str,
    title: &str,
    mi: bool,
    content: impl FnOnce(&mut Ui),
) -> bool {
    let screen = ctx.content_rect();
    let width = (screen.width() - 56.).clamp(120., 440.);
    let palette = miuix::Colors::new(ctx.style().visuals.dark_mode);
    let mut frame = Frame::window(&ctx.style()).inner_margin(Margin::same(14));
    if mi {
        frame = frame
            .fill(palette.workbench())
            .corner_radius(20)
            .stroke(Stroke::new(1.0_f32, palette.outline));
    }
    let stage = crate::theme::keynote::active(ctx);
    if stage {
        frame = frame.inner_margin(Margin::same(20));
    }
    let soft = crate::theme::soft::active(ctx);
    if soft {
        frame = frame.inner_margin(Margin::same(16)).corner_radius(24);
    }
    let mut open = true;
    let response = Window::new(title)
        .id(Id::new(("tool-sheet-compact", key)))
        .title_bar(false)
        .collapsible(false)
        .pivot(Align2::CENTER_CENTER)
        .default_pos(screen.center())
        .auto_sized()
        .resizable(false)
        .frame(frame)
        .show(ctx, |ui| {
            ui.set_width(width);
            for (role, size) in [
                (TextStyle::Body, 13.),
                (TextStyle::Button, 13.),
                (TextStyle::Small, 11.5),
                (TextStyle::Monospace, 13.),
            ] {
                if let Some(font) = ui.style_mut().text_styles.get_mut(&role) {
                    font.size = size;
                }
            }
            ui.spacing_mut().item_spacing = vec2(7., 6.);
            ui.spacing_mut().interact_size.y = 24.;
            ui.spacing_mut().button_padding = vec2(9., 3.);
            ui.horizontal(|ui| {
                ui.label(RichText::new(title).font(if stage {
                    crate::theme::keynote::font(21.)
                } else if mi {
                    miuix::heading(16.)
                } else if soft {
                    crate::theme::soft::font(18.)
                } else {
                    FontId::proportional(16.)
                }));
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if mi {
                        if miuix::dock_item(ui, 24., "关闭", false, |p, c, ink| {
                            let d = 4.;
                            for dy in [d, -d] {
                                p.line_segment(
                                    [c + vec2(-d, -dy), c + vec2(d, dy)],
                                    Stroke::new(1.5_f32, ink),
                                );
                            }
                        })
                        .on_hover_text("关闭")
                        .clicked()
                        {
                            open = false;
                        }
                    } else if ui.small_button("关闭").clicked() {
                        open = false;
                    }
                });
            });
            let cap = (screen.height() - 104.).min(490.).max(80.);
            ScrollArea::vertical()
                .id_salt((key, "body"))
                .max_height(cap)
                .auto_shrink([false, true])
                .show(ui, |ui| {
                    let w = ui.available_width();
                    if mi {
                        miuix::group(ui, 14., Margin::same(12), |ui| {
                            ui.set_width((w - 24.).max(1.));
                            content(ui);
                        });
                    } else {
                        content(ui);
                    }
                });
        });
    if let Some(r) = response {
        super::visual_smoke::menu_probe(ctx, key, r.response.rect);
    }
    open
}

pub(super) fn primary(
    ui: &mut Ui,
    label: impl Into<WidgetText>,
    enabled: bool,
    mi: bool,
) -> Response {
    let mut b = Button::new(label).min_size(vec2(82., 27.));
    if crate::theme::soft::active(ui.ctx()) && enabled {
        b = b
            .fill(ui.visuals().selection.stroke.color)
            .corner_radius(16);
        return ui
            .scope(|ui| {
                ui.visuals_mut().override_text_color = Some(if ui.visuals().dark_mode {
                    Color32::from_rgb(27, 30, 44)
                } else {
                    Color32::WHITE
                });
                ui.add(b)
            })
            .inner;
    }
    if mi && enabled {
        let c = miuix::Colors::of(ui);
        b = b.fill(c.primary).corner_radius(14);
        return ui
            .scope(|ui| {
                ui.visuals_mut().override_text_color = Some(Color32::WHITE);
                ui.add(b)
            })
            .inner;
    }
    ui.add_enabled(enabled, b)
}
