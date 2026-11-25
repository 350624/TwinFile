use eframe::egui::{self, Id, Rect, Vec2};

pub(crate) fn reduced(ctx: &egui::Context) -> bool {
    ctx.style().animation_time <= 0.0 || ctx.input(|i| i.viewport().minimized == Some(true))
}

fn ease(t: f32) -> f32 {
    1.0 - (1.0 - t.clamp(0.0, 1.0)).powi(3)
}

#[derive(Clone, Copy)]
struct RectMotion {
    from: Rect,
    target: Rect,
    since: f64,
}

impl RectMotion {
    fn sample(self, now: f64, seconds: f64) -> (Rect, bool) {
        let elapsed = ((now - self.since) / seconds) as f32;
        let t = ease(elapsed);
        (
            Rect::from_min_max(
                self.from.min.lerp(self.target.min, t),
                self.from.max.lerp(self.target.max, t),
            ),
            elapsed < 1.0 && self.from != self.target,
        )
    }
}

pub(crate) fn highlight(
    ctx: &egui::Context,
    id: Id,
    dest: Option<Rect>,
    seconds: f64,
    snap: bool,
) -> Option<Rect> {
    let now = ctx.input(|input| input.time);
    let reduced = snap || reduced(ctx);
    let (rect, moving) = ctx.data_mut(|data| {
        let Some(target) = dest else {
            data.remove::<RectMotion>(id);
            return (None, false);
        };
        let mut state = data.get_temp::<RectMotion>(id).unwrap_or(RectMotion {
            from: target,
            target,
            since: now,
        });
        if reduced {
            state = RectMotion {
                from: target,
                target,
                since: now,
            };
        } else if target != state.target {
            // Re-target from what is already on screen, never jump back to a stale endpoint.
            state = RectMotion {
                from: state.sample(now, seconds).0,
                target,
                since: now,
            };
        }
        data.insert_temp(id, state);
        let (rect, moving) = state.sample(now, seconds);
        (Some(rect), moving)
    });
    if moving {
        ctx.request_repaint();
    }
    rect
}

pub(crate) fn feedback(ui: &egui::Ui, response: &egui::Response) -> (f32, f32) {
    let hover = response.enabled() && response.hovered();
    let press = response.enabled() && response.is_pointer_button_down_on();
    let ctx = ui.ctx();
    let instant = reduced(ctx);
    let d = if instant { 0.0 } else { 0.14 };
    let a = ctx.animate_bool_with_time_and_easing(response.id.with("hover"), hover, d, ease);
    let b = ctx.animate_bool_with_time_and_easing(
        response.id.with("press"),
        press,
        if instant { 0.0 } else { 0.10 },
        ease,
    );
    (a, b)
}

#[derive(Default, Clone, Copy)]
struct HoverShot {
    inside: bool,
    started: Option<f64>,
    frame: u64,
}

/// A pointer entry triggers a single clip. Leaving rearms it without cutting its tail short.
/// Key by the actual control, not the glyph: identical icons can be crossed independently.
pub(crate) fn hover_clip(response: &egui::Response, seconds: f64) -> Option<f32> {
    let ctx = &response.ctx;
    let id = response.id.with("hover-clip");
    let now = ctx.input(|i| i.time);
    let frame = ctx.cumulative_frame_nr();
    let inside = response.enabled() && response.hovered() && ctx.pointer_hover_pos().is_some();
    let mute = reduced(ctx) || !response.enabled();
    let phase = ctx.data_mut(|d| {
        let mut s = d.get_temp::<HoverShot>(id).unwrap_or_default();
        if frame.saturating_sub(s.frame) > 1 {
            s = HoverShot::default();
        }
        if mute {
            s.started = None;
        } else if inside && !s.inside {
            s.started = Some(now);
        }
        s.inside = inside;
        s.frame = frame;
        let t = s
            .started
            .map(|start| ((now - start) / seconds).max(0.) as f32)
            .filter(|&t| t < 1.);
        if t.is_none() {
            s.started = None;
        }
        d.insert_temp(id, s);
        t
    });
    if phase.is_some() {
        ctx.request_repaint();
    }
    phase
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ListMotion {
    Enter,
    Leave,
    Tab,
    Refresh,
}

impl ListMotion {
    pub(crate) fn sample(self, elapsed: f64) -> PaintMotion {
        let duration = if matches!(self, Self::Refresh) {
            0.20
        } else {
            0.26
        };
        let done = elapsed >= duration;
        let rest = 1.0 - ease((elapsed / duration) as f32);
        let offset = match self {
            Self::Enter => egui::vec2(18.0 * rest, 0.0),
            Self::Leave => egui::vec2(-18.0 * rest, 0.0),
            Self::Tab => egui::vec2(9.0 * rest, 0.0),
            Self::Refresh => egui::vec2(0.0, 8.0 * rest),
        };
        PaintMotion {
            offset,
            opacity: 1.0 - 0.75 * rest,
            done,
        }
    }
}

#[derive(Clone, Copy)]
pub(crate) struct PaintMotion {
    pub(crate) offset: Vec2,
    pub(crate) opacity: f32,
    pub(crate) done: bool,
}
impl PaintMotion {
    pub(crate) const REST: Self = Self {
        offset: Vec2::ZERO,
        opacity: 1.0,
        done: true,
    };
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn list_motion_moves_in_the_correct_direction_then_stops() {
        let inspect = |kind: ListMotion, d: Vec2| {
            let first = kind.sample(0.0);
            let middle = kind.sample(0.08);
            assert!(first.offset.dot(d) > middle.offset.dot(d) && middle.offset.dot(d) > 0.0);
            assert!(first.opacity < middle.opacity && middle.opacity < 1.0);
            assert!(!first.done && !middle.done);
            let last = kind.sample(1.0);
            assert_eq!(last.offset, Vec2::ZERO);
            assert_eq!(last.opacity, 1.0);
            assert!(last.done);
        };
        inspect(ListMotion::Enter, Vec2::X);
        inspect(ListMotion::Leave, -Vec2::X);
        inspect(ListMotion::Refresh, Vec2::Y);
        inspect(ListMotion::Tab, Vec2::X);
    }

    #[test]
    fn indicator_animates_between_rectangles_and_snaps_when_reduced() {
        let ctx = egui::Context::default();
        let key = Id::new("test");
        let from = Rect::from_min_size(egui::pos2(0., 0.), egui::vec2(30., 10.));
        let to = Rect::from_min_size(egui::pos2(50., 40.), egui::vec2(60., 12.));
        let run = |time, target, snap| {
            let mut result = None;
            let _ = ctx.run(
                egui::RawInput {
                    time: Some(time),
                    ..Default::default()
                },
                |ctx| {
                    result = highlight(ctx, key, Some(target), 0.22, snap);
                },
            );
            result.unwrap()
        };
        assert_eq!(run(0., from, false), from);
        assert_eq!(run(0.01, to, false), from);
        let mid = run(0.10, to, false);
        assert!(mid.left() > from.left() && mid.left() < to.left());
        assert!(mid.width() > from.width() && mid.width() < to.width());
        assert_eq!(run(0.40, from, true), from);
    }
}

/// The model closes immediately. Only the last paint list survives; no widgets,
/// callbacks, focus or file operations are replayed during an exit.
#[derive(Default)]
pub(crate) struct OverlayExits {
    layers: std::collections::HashMap<egui::LayerId, OverlayPaint>,
}
struct OverlayPaint {
    shapes: egui::layers::PaintList,
    _textures: Vec<egui::TextureHandle>,
    closed_at: Option<f64>,
    opened_at: f64,
    from: f32,
    shown: f32,
}
impl OverlayPaint {
    fn capture(
        ctx: &egui::Context,
        shapes: egui::layers::PaintList,
        opened_at: f64,
        from: f32,
        shown: f32,
    ) -> Self {
        fn textures(shape: &egui::Shape, ids: &mut std::collections::HashSet<egui::TextureId>) {
            if let egui::Shape::Vec(children) = shape {
                for child in children {
                    textures(child, ids);
                }
            } else if shape.texture_id() != egui::TextureId::default() {
                ids.insert(shape.texture_id());
            }
        }
        let mut ids = std::collections::HashSet::new();
        for item in shapes.all_entries() {
            textures(&item.shape, &mut ids);
        }
        let manager = ctx.tex_manager();
        let mut pinned = Vec::new();
        let mut m = manager.write();
        for id in ids {
            if m.meta(id).is_some() {
                m.retain(id);
                pinned.push(egui::TextureHandle::new(manager.clone(), id));
            }
        }
        Self {
            shapes,
            _textures: pinned,
            closed_at: None,
            opened_at,
            from,
            shown,
        }
    }
}
impl OverlayExits {
    pub(crate) fn paint(&mut self, ctx: &egui::Context) {
        if reduced(ctx) {
            self.layers.clear();
            return;
        }
        if ctx.will_discard() {
            return;
        }
        let now = ctx.input(|i| i.time);
        let duration = f64::from(ctx.style().animation_time.clamp(0.14, 0.25));
        let ids: Vec<_> = ctx.memory(|m| {
            m.layer_ids()
                .filter(|l| matches!(l.order, egui::Order::Middle | egui::Order::Foreground))
                .collect()
        });
        self.layers.retain(|id, snapshot| {
            if ctx.graphics(|g| g.get(*id).is_some_and(|list| !list.is_empty())) {
                if let Some(closed) = snapshot.closed_at.take() {
                    let t = ((now - closed) / duration).clamp(0., 1.) as f32;
                    snapshot.from = snapshot.shown * (1. - t * t * (3. - 2. * t));
                    snapshot.opened_at = now;
                }

                return true;
            }
            let t =
                ((now - *snapshot.closed_at.get_or_insert(now)) / duration).clamp(0., 1.) as f32;
            if t >= 1. {
                return false;
            }
            let shown = snapshot.shown * (1. - t * t * (3. - 2. * t));
            let painter = ctx.layer_painter(*id);
            for item in snapshot.shapes.all_entries() {
                if !matches!(item.shape, egui::Shape::Callback(_)) {
                    let mut item = item.clone();
                    overlay_pose(&mut item, shown, ctx.content_rect());
                    painter.with_clip_rect(item.clip_rect).add(item.shape);
                }
            }
            ctx.request_repaint();
            true
        });
        for id in ids {
            // A live layer supersedes a closing layer immediately on quick reopen.
            // Never sample the copy just painted, or it would restart its own fade.
            if self.layers.get(&id).is_some_and(|p| p.closed_at.is_some()) {
                continue;
            }
            if let Some(list) = ctx.graphics(|g| g.get(id).filter(|list| !list.is_empty()).cloned())
            {
                let (start, from) = self
                    .layers
                    .get(&id)
                    .map_or((now, 0.), |s| (s.opened_at, s.from));
                let shown = from + (1. - from) * ease(((now - start) / 0.26) as f32);
                self.layers
                    .insert(id, OverlayPaint::capture(ctx, list, start, from, shown));
                if shown < 1. {
                    let viewport = ctx.content_rect();
                    ctx.graphics_mut(|g| {
                        if let Some(list) = g.get_mut(id) {
                            let count = list.all_entries().len();
                            for idx in 0..count {
                                list.mutate_shape(egui::layers::ShapeIdx(idx), |item| {
                                    overlay_pose(item, shown, viewport)
                                });
                            }
                        }
                    });
                    ctx.request_repaint();
                }
            }
        }
    }
}

// Move clipped content together but never shift the full-screen modal scrim.
fn overlay_pose(item: &mut egui::epaint::ClippedShape, shown: f32, screen: Rect) {
    let shape = &mut item.shape;
    if matches!(shape, egui::Shape::Callback(_)) {
        return;
    }
    if !matches!(shape,egui::Shape::Rect(r) if r.rect.contains_rect(screen.shrink(1.))) {
        let delta = egui::vec2(0., 5. * (1. - shown));
        shape.translate(delta);
        item.clip_rect = item.clip_rect.translate(delta).intersect(screen);
    }
    egui::epaint::shape_transform::adjust_colors(shape, move |c| {
        if *c != egui::Color32::PLACEHOLDER {
            *c = c.gamma_multiply(shown)
        }
    });
}

#[derive(Default)]
pub(crate) struct WorkspaceMotion {
    palette: Option<(twinfile_rust::config::Theme, bool, usize)>,
    last_fill: egui::Color32,
    reveal: Option<(f64, egui::Color32)>,
    exit: Option<f64>,
    pub(crate) finished: bool,
}
impl WorkspaceMotion {
    pub(crate) fn begin_close(&mut self, ctx: &egui::Context) {
        if !reduced(ctx) && !self.finished {
            self.exit.get_or_insert_with(|| ctx.input(|i| i.time));
            ctx.send_viewport_cmd(egui::ViewportCommand::CancelClose);
        }
    }
    pub(crate) fn cancel_close(&mut self) {
        self.exit = None;
        self.finished = false;
    }
    pub(crate) fn paint(&mut self, ctx: &egui::Context, s: &twinfile_rust::config::Settings) {
        let palette = (s.theme, s.dark, s.accent);
        let now = ctx.input(|i| i.time);
        let fill = ctx.style().visuals.panel_fill;
        if self.palette != Some(palette) {
            self.reveal = Some((
                now,
                if self.palette.is_none() {
                    fill
                } else {
                    self.last_fill
                },
            ));
            self.palette = Some(palette);
        }
        self.last_fill = fill;
        if reduced(ctx) {
            self.reveal = None;
        }
        let veil = if let Some(start) = self.exit {
            let t = if reduced(ctx) {
                1.
            } else {
                ((now - start) / 0.20).clamp(0., 1.) as f32
            };
            if t >= 1. {
                self.finished = true;
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            }
            Some((t * t * (3. - 2. * t), fill))
        } else if let Some((start, c)) = self.reveal {
            let t = ((now - start) / 0.25) as f32;
            if t >= 1. {
                self.reveal = None;
                None
            } else {
                Some(((1. - ease(t)) * 0.94, c))
            }
        } else {
            None
        };
        if let Some((alpha, col)) = veil {
            ctx.layer_painter(egui::LayerId::new(
                egui::Order::Tooltip,
                Id::new("workspace-palette-transition"),
            ))
            .rect_filled(ctx.content_rect(), 0, col.gamma_multiply(alpha));
            ctx.request_repaint();
        }
    }
    pub(crate) fn closing(&self) -> bool {
        self.exit.is_some()
    }
}

/// Animate paint only; click, keyboard activation and value changes stay synchronous.
pub(crate) trait ChoiceUi {
    fn motion_label(&mut self, on: bool, label: impl Into<egui::WidgetText>) -> egui::Response;
    fn motion_value<T: PartialEq>(
        &mut self,
        value: &mut T,
        next: T,
        label: impl Into<egui::WidgetText>,
    ) -> egui::Response;
    fn motion_check(&mut self, on: &mut bool, label: impl Into<egui::WidgetText>)
    -> egui::Response;
}
impl ChoiceUi for egui::Ui {
    fn motion_label(&mut self, on: bool, label: impl Into<egui::WidgetText>) -> egui::Response {
        let label = label.into();
        let info = label.text().to_owned();
        let paint = self.painter().add(egui::Shape::Noop);
        let r = self.add(egui::Button::selectable(on, label).frame(false));
        r.widget_info(|| {
            egui::WidgetInfo::selected(
                egui::WidgetType::SelectableLabel,
                r.enabled(),
                on,
                info.clone(),
            )
        });
        if self.is_rect_visible(r.rect) {
            let amount = level(self.ctx(), r.id.with("selected"), on);
            let (hover, press) = feedback(self, &r);
            let fill = self
                .visuals()
                .widgets
                .hovered
                .weak_bg_fill
                .gamma_multiply(hover.max(press) * 0.7)
                .lerp_to_gamma(self.visuals().selection.bg_fill, amount);
            self.painter().set(
                paint,
                egui::Shape::rect_filled(
                    r.rect,
                    self.visuals().widgets.inactive.corner_radius,
                    fill,
                ),
            );
            if r.has_focus() {
                self.painter().rect_stroke(
                    r.rect,
                    5,
                    self.visuals().widgets.hovered.bg_stroke,
                    egui::StrokeKind::Inside,
                );
            }
        }
        r
    }
    fn motion_value<T: PartialEq>(
        &mut self,
        value: &mut T,
        next: T,
        label: impl Into<egui::WidgetText>,
    ) -> egui::Response {
        let mut r = self.motion_label(*value == next, label);
        if r.clicked() && *value != next {
            *value = next;
            r.mark_changed();
            self.ctx().request_repaint();
        }
        r
    }
    fn motion_check(
        &mut self,
        on: &mut bool,
        label: impl Into<egui::WidgetText>,
    ) -> egui::Response {
        let label = label.into();
        let info = label.text().to_owned();
        let size = self.spacing().icon_width;
        let gap = if info.is_empty() {
            0.
        } else {
            self.spacing().icon_spacing
        };
        let galley = label.into_galley(
            self,
            None,
            (self.available_width() - size - gap).max(20.),
            egui::TextStyle::Button,
        );
        let (rect, mut r) = self.allocate_exact_size(
            egui::vec2(
                size + gap + galley.size().x,
                self.spacing().interact_size.y.max(galley.size().y),
            ),
            egui::Sense::click(),
        );
        if r.clicked() {
            *on = !*on;
            r.mark_changed();
        }
        r.widget_info(|| {
            egui::WidgetInfo::selected(egui::WidgetType::Checkbox, r.enabled(), *on, info.clone())
        });
        if self.is_rect_visible(rect) {
            let t = level(self.ctx(), r.id.with("checked"), *on);
            let v = self.style().interact(&r);
            let ink = self.visuals().text_color();
            let boxrect = Rect::from_center_size(
                rect.left_center() + egui::vec2(size / 2., 0.),
                Vec2::splat(size),
            );
            let p = self.painter();
            p.rect(
                boxrect,
                4,
                v.weak_bg_fill
                    .lerp_to_gamma(self.visuals().selection.bg_fill, t),
                v.fg_stroke,
                egui::StrokeKind::Inside,
            );
            if t > 0. {
                let a = boxrect.center();
                let full = [
                    a + egui::vec2(-4., 0.),
                    a + egui::vec2(-1., 3.),
                    a + egui::vec2(5., -3.),
                ];
                let stroke = egui::Stroke::new(1.65_f32, ink);
                p.line_segment([full[0], full[0].lerp(full[1], (t * 3.).min(1.))], stroke);
                if t > 0.333 {
                    p.line_segment(
                        [
                            full[1],
                            full[1].lerp(full[2], ((t - 0.333) * 1.5).clamp(0., 1.)),
                        ],
                        stroke,
                    );
                }
            }
            p.galley(
                egui::pos2(
                    boxrect.right() + gap,
                    rect.center().y - galley.size().y / 2.,
                ),
                galley,
                ink,
            );
        }
        r
    }
}
fn level(ctx: &egui::Context, id: Id, on: bool) -> f32 {
    if reduced(ctx) {
        return f32::from(on);
    }
    ctx.animate_value_with_time(id, f32::from(on), 0.20)
}

#[cfg(test)]
mod overlay_motion_tests {
    use super::*;
    #[test]
    fn overlay_paint_has_distinct_enter_settle_exit_frames_without_moving_backdrop() {
        let screen = Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(600., 400.));
        let card = Rect::from_min_size(egui::pos2(100., 50.), egui::vec2(200., 100.));
        let sample = |bounds, amount| {
            let mut s = egui::epaint::ClippedShape {
                clip_rect: screen,
                shape: egui::Shape::rect_filled(bounds, 7, egui::Color32::WHITE),
            };
            overlay_pose(&mut s, amount, screen);
            let egui::Shape::Rect(r) = s.shape else {
                panic!()
            };
            (r.rect, r.fill.a())
        };
        let (rest, opaque) = sample(card, 1.);
        let (mid, a) = sample(card, 0.5);
        let (first, b) = sample(card, 0.05);
        assert_eq!(rest, card);
        assert!(rest.top() < mid.top() && mid.top() < first.top() && b < a && a < opaque);
        assert_eq!(sample(screen, 0.25).0, screen);
    }
}
