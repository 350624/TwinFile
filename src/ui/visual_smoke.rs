use super::*;

pub(super) struct VisualSmoke {
    dir: PathBuf,
    case: usize,
    frames: u8,
    requested: bool,
    motion: Option<MotionSmoke>,
    menu: Option<MenuSmoke>,
    switch: Option<SwitchSmoke>,
    overlay: Option<OverlaySmoke>,
    close: Option<CloseSmoke>,
    sheets: bool,
}
impl VisualSmoke {
    pub(super) fn new(dir: PathBuf) -> Self {
        Self {
            dir,
            case: 0,
            frames: 0,
            requested: false,
            sheets: std::env::var_os("TWINFILE_SMOKE_SHEETS").is_some(),
            motion: None,
            menu: std::env::var("TWINFILE_SMOKE_TOOL")
                .ok()
                .filter(|t| matches!(t.as_str(), "payload" | "hfx"))
                .map(|t| MenuSmoke {
                    tool: Some(t),
                    ..Default::default()
                }),
            switch: None,
            overlay: None,
            close: std::env::var_os("TWINFILE_SMOKE_CLOSE").map(|_| CloseSmoke::default()),
        }
    }
    fn size(&self) -> [f32; 2] {
        if self.case < Theme::ALL.len() * 2 {
            [1180.0, 740.0]
        } else {
            [760.0, 560.0]
        }
    }
    pub(super) fn begin(&self, settings: &mut Settings, ctx: &egui::Context) {
        // Exercise fractional raster scales in the isolated screenshot harness only.
        if let Some(ppp) = std::env::var("TWINFILE_SMOKE_PIXELS_PER_POINT")
            .ok()
            .and_then(|s| s.parse::<f32>().ok())
            .filter(|v| (0.75..=3.0).contains(v))
        {
            ctx.set_pixels_per_point(ppp);
        }
        settings.theme = Theme::ALL[(self.case % (Theme::ALL.len() * 2)) / 2];
        settings.dark = self.case % 2 == 1;
        if self.menu.as_ref().is_some_and(|m| m.tool.is_some()) {
            settings.theme = if std::env::var_os("TWINFILE_SMOKE_KEYNOTE").is_some() {
                Theme::Keynote
            } else if std::env::var_os("TWINFILE_SMOKE_SOFT").is_some() {
                Theme::Soft
            } else {
                Theme::Miuix
            };
            settings.dark = std::env::var_os("TWINFILE_SMOKE_TOOL_LIGHT").is_none();
        }
        settings.reduce_motion = true;
        crate::theme::apply(ctx, settings);
        let [x, y] = if self.menu.as_ref().is_some_and(|m| m.tool.is_some())
            && std::env::var_os("TWINFILE_SMOKE_TOOL_NARROW").is_some()
        {
            [720., 540.]
        } else {
            self.size()
        };
        ctx.send_viewport_cmd(egui::ViewportCommand::InnerSize(egui::vec2(x, y)));
    }
    pub(super) fn motion_input(
        &mut self,
        panes: &mut [Pane; 2],
        settings: &mut Settings,
        ctx: &egui::Context,
    ) {
        // Cover both the selected and unselected trailing area in Miuix screenshots.
        if self.case < Theme::ALL.len() * 4 && self.close.is_none() && self.menu.is_none() {
            if settings.theme == Theme::Miuix {
                if let Some(first) = panes[0].items.first() {
                    panes[0].selection.insert(first.path.clone());
                }
            } else {
                panes[0].selection.clear();
            }
        }
        let Some(m) = &mut self.motion else {
            return;
        };
        if m.applied {
            return;
        }
        m.applied = true;
        match m.step {
            0 => {
                settings.theme = Theme::Miuix;
                settings.dark = true;
                settings.active = 0;
                settings.reduce_motion = false;
                crate::theme::apply(ctx, settings);
                ctx.send_viewport_cmd(egui::ViewportCommand::InnerSize(egui::vec2(1180., 740.)));
            }
            1 => {
                panes[0].goto(Location::Computer, settings.hidden);
                m.start = ctx.input(|i| i.time);
            }
            3 => {
                settings.reduce_motion = true;
                crate::theme::apply(ctx, settings);
                panes[0].history(true, settings.hidden);
            }
            _ => {}
        }
    }
    pub(super) fn prepare_close(
        &mut self,
        dialog: &mut Option<Dialog>,
        settings: &mut Settings,
        ctx: &egui::Context,
    ) {
        if self.sheets {
            if self.case < 8 && self.frames == 0 && !self.requested {
                settings.theme = Theme::Miuix;
                settings.dark = self.case % 2 == 1;
                settings.reduce_motion = true;
                crate::theme::apply(ctx, settings);
                *dialog = Some(match self.case / 2 {
                    0 => Dialog::Properties(local_tools::Properties::new(vec![
                        self.dir.join("fixture/left/下载资料"),
                    ])),
                    1 => Dialog::New(true, String::new()),
                    2 => Dialog::Search(String::new()),
                    _ => Dialog::New(false, "说明.txt".into()),
                });
            }
            return;
        }
        let Some(c) = &mut self.close else {
            return;
        };
        if !c.initialized && c.case < Theme::ALL.len() * 2 {
            settings.theme = Theme::ALL[c.case / 2];
            settings.dark = true;
            settings.reduce_motion = c.case % 2 == 1;
            crate::theme::apply(ctx, settings);
            *dialog = Some(Dialog::Appearance);
            c.initialized = true;
        }
    }
    pub(super) fn inject(&mut self, input: &mut egui::RawInput) {
        if let Some(c) = &mut self.close {
            // Advance test time independently of screenshot readback and slow CI GPUs.
            c.clock += 1. / 60.;
            input.time = Some(c.clock);
            input.events.append(&mut c.events);
        }
        if let Some(menu) = &mut self.menu {
            input.events.append(&mut menu.events);
        }
        if let Some(switch) = &mut self.switch {
            input.events.append(&mut switch.events);
        }
        if let Some(overlay) = &mut self.overlay {
            // This test injects keyboard input into its own window. A hidden smoke
            // process need not own the real user's OS keyboard focus to exercise it.
            input.focused = true;
            input.events.append(&mut overlay.events);
        }
    }
    pub(super) fn frame(&mut self, ready: bool, settings: &mut Settings, ctx: &egui::Context) {
        if self.sheets {
            if self.case >= 8 {
                return;
            }
            self.frames = self.frames.saturating_add(1);
            let key = ["properties", "create", "search", "create-file"][self.case / 2];
            if self.requested
                && let Some(im) = ctx.input(|i| {
                    i.events.iter().find_map(|event| {
                        if let egui::Event::Screenshot { image, .. } = event {
                            Some(image.clone())
                        } else {
                            None
                        }
                    })
                })
            {
                let bytes: Vec<_> = im.pixels.iter().flat_map(|p| p.to_array()).collect();
                let name = format!("{key}-{}.png", if settings.dark { "dark" } else { "light" });
                if let Err(e) = image::save_buffer(
                    self.dir.join(name),
                    &bytes,
                    im.width() as u32,
                    im.height() as u32,
                    image::ColorType::Rgba8,
                ) {
                    let _ = fs::write(self.dir.join("sheet-error.txt"), e.to_string());
                    ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                    return;
                }
                self.case += 1;
                self.frames = 0;
                self.requested = false;
                if self.case == 8 {
                    let _ = fs::write(
                        self.dir.join("sheet-ready.txt"),
                        "Captured properties, new folder (empty), new file (enabled) and search in both Miuix palettes",
                    );
                    ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                    return;
                }
            } else if self.frames >= 12 && !self.requested {
                self.requested = true;
                ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(egui::UserData::default()));
            }
            ctx.request_repaint_after(Duration::from_millis(20));
            return;
        }
        if let Some(c) = &mut self.close {
            c.frame(&self.dir, settings, ctx);
            return;
        }
        if let Some(overlay) = &mut self.overlay {
            overlay.frame(&self.dir, ctx, settings);
            return;
        }
        if let Some(switch) = &mut self.switch {
            switch.frame(&self.dir, ctx, settings.active);
            if switch.stage == 8 {
                self.switch = None;
                self.overlay = Some(OverlaySmoke::default());
                ctx.request_repaint();
            }
            return;
        }
        if let Some(menu) = &mut self.menu {
            menu.frame(&self.dir, ctx);
            if menu.stage == 3 {
                ctx.data_mut(|d| d.insert_temp(egui::Id::new("pane-switch-smoke"), true));
                self.menu = None;
                self.switch = Some(SwitchSmoke::default());
                ctx.request_repaint();
            }
            return;
        }
        if let Some(m) = &mut self.motion {
            m.frame(&self.dir, ctx, ready);
            if m.step == 4 {
                self.motion = None;
                self.menu = Some(MenuSmoke::default());
            }
            return;
        }
        if self.requested {
            let im = ctx.input(|i| {
                i.events.iter().find_map(|e| match e {
                    egui::Event::Screenshot { image, .. } => Some(image.clone()),
                    _ => None,
                })
            });
            if let Some(im) = im {
                let [w, h] = im.size;
                let is_flat = im
                    .pixels
                    .first()
                    .is_none_or(|c| im.pixels.iter().all(|x| x == c));
                let fnm = format!(
                    "{:02}-{:?}-{}-{}.png",
                    self.case,
                    settings.theme,
                    if settings.dark { "dark" } else { "light" },
                    if self.case < Theme::ALL.len() * 2 {
                        "wide"
                    } else {
                        "narrow"
                    }
                );
                let raw: Vec<_> = im.pixels.iter().flat_map(|p| p.to_array()).collect();
                let res = if is_flat {
                    Err("unexpected blank screenshot".to_owned())
                } else {
                    image::save_buffer(
                        self.dir.join(fnm),
                        &raw,
                        w as u32,
                        h as u32,
                        image::ColorType::Rgba8,
                    )
                    .map_err(|e| e.to_string())
                };
                if let Err(e) = res {
                    let _ = fs::write(self.dir.join("themes-error.txt"), e);
                    ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                    return;
                }
                self.case += 1;
                if self.case == Theme::ALL.len() * 4 {
                    let _ = fs::write(
                        self.dir.join("themes-ready.txt"),
                        format!("Captured {} actual GUI frames.", self.case),
                    );
                    self.motion = Some(MotionSmoke::default());
                    ctx.request_repaint();
                    return;
                }
                self.frames = 0;
                self.requested = false;
                self.begin(settings, ctx);
            }
        } else {
            self.frames = self.frames.saturating_add(1);
            if ready && self.frames >= 5 {
                self.requested = true;
                ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(egui::UserData::default()));
            }
        }
        ctx.request_repaint_after(Duration::from_millis(40));
    }
}

#[derive(Default)]
struct MotionSmoke {
    step: usize,
    applied: bool,
    requested: bool,
    frames: u8,
    start: f64,
}

impl MotionSmoke {
    fn frame(&mut self, root: &Path, ctx: &egui::Context, ready: bool) {
        const NAMES: [&str; 4] = ["01-before", "02-transition", "03-settled", "04-reduced"];
        let out = root.join("motion");
        if self.requested {
            let found = ctx.input(|input| {
                input.events.iter().find_map(|e| {
                    if let egui::Event::Screenshot { image, .. } = e {
                        Some(image.clone())
                    } else {
                        None
                    }
                })
            });
            if let Some(im) = found {
                let result = fs::create_dir_all(&out).and_then(|_| {
                    let raw: Vec<_> = im.pixels.iter().flat_map(|px| px.to_array()).collect();
                    image::save_buffer(
                        out.join(format!("{}.png", NAMES[self.step])),
                        &raw,
                        im.size[0] as u32,
                        im.size[1] as u32,
                        image::ColorType::Rgba8,
                    )
                    .map_err(io::Error::other)
                });
                if let Err(e) = result {
                    let _ = fs::write(root.join("motion-error.txt"), e.to_string());
                    ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                    return;
                }
                self.step += 1;
                self.requested = false;
                self.applied = false;
                self.frames = 0;
                if self.step == NAMES.len() {
                    let _ = fs::write(
                        root.join("motion-ready.txt"),
                        "Miuix actual GUI: before, mid-motion, settled, reduced.",
                    );
                    ctx.request_repaint();
                    return;
                }
            }
        } else if ready {
            self.frames = self.frames.saturating_add(1);
            let elapsed = ctx.input(|i| i.time) - self.start;
            let capture = match self.step {
                1 => elapsed >= 0.045 && elapsed < 0.20,
                2 => elapsed >= 0.34,
                _ => self.frames >= 7,
            };
            if capture {
                self.requested = true;
                ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(egui::UserData::default()));
            } else if self.step == 1 && elapsed >= 0.20 {
                let _ = fs::write(
                    root.join("motion-error.txt"),
                    "No intermediate frame arrived within 200 ms.",
                );
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                return;
            }
        }
        ctx.request_repaint_after(Duration::from_millis(15));
    }
}

pub(super) fn menu_probe(ctx: &egui::Context, key: &str, rect: egui::Rect) {
    let pass = ctx.cumulative_pass_nr();
    ctx.data_mut(|data| data.insert_temp(egui::Id::new(("menu-smoke", key)), (pass, rect)));
}

#[derive(Default)]
struct MenuSmoke {
    tool: Option<String>,
    stage: usize,
    frames: u8,
    clicked: bool,
    requested: bool,
    events: Vec<egui::Event>,
}

impl MenuSmoke {
    fn frame(&mut self, root: &Path, ctx: &egui::Context) {
        const STEPS: [(&str, &str, &str); 3] = [
            ("more", "sort", "01-main"),
            ("settings", "settings-open", "02-settings"),
            ("sort", "sort-open", "03-sort"),
        ];

        let specific = match self.tool.as_deref() {
            Some("payload") => Some([
                ("more", "payload-entry", "01-entries"),
                ("payload-entry", "payload-open", "02-payload"),
            ]),
            Some("hfx") => Some([
                ("more", "hfx-entry", "01-entries"),
                ("hfx-entry", "hfx-open", "02-hfx"),
            ]),
            _ => None,
        };
        let steps = specific.as_ref().map(|a| a.as_slice()).unwrap_or(&STEPS);
        let Some(&(click, target, name)) = steps.get(self.stage) else {
            return;
        };
        let pass = ctx.cumulative_pass_nr();
        let probe = |key| {
            ctx.data(|data| data.get_temp::<(u64, egui::Rect)>(egui::Id::new(("menu-smoke", key))))
                .filter(|(seen, _)| *seen == pass)
                .map(|(_, rect)| rect)
        };
        let close = || ctx.send_viewport_cmd(egui::ViewportCommand::Close);
        self.frames = self.frames.saturating_add(1);
        if self.frames > 45 {
            let _ = fs::write(
                root.join("menu-error.txt"),
                format!("Menu stage {name} did not render."),
            );
            close();
            return;
        }
        if !self.clicked {
            if let Some(rect) = probe(click) {
                let pos = rect.center();
                self.events.push(egui::Event::PointerMoved(pos));
                for pressed in [true, false] {
                    self.events.push(egui::Event::PointerButton {
                        pos,
                        button: egui::PointerButton::Primary,
                        pressed,
                        modifiers: egui::Modifiers::NONE,
                    });
                }
                self.clicked = true;
            }
        } else if self.requested {
            if let Some(im) = ctx.input(|input| {
                input.events.iter().find_map(|e| match e {
                    egui::Event::Screenshot { image, .. } => Some(image.clone()),
                    _ => None,
                })
            }) {
                let out = root.join("menu");
                let result = fs::create_dir_all(&out).and_then(|_| {
                    let raw: Vec<_> = im.pixels.iter().flat_map(|px| px.to_array()).collect();
                    image::save_buffer(
                        out.join(format!("{name}.png")),
                        &raw,
                        im.size[0] as u32,
                        im.size[1] as u32,
                        image::ColorType::Rgba8,
                    )
                    .map_err(io::Error::other)
                });
                if let Err(e) = result {
                    let _ = fs::write(root.join("menu-error.txt"), e.to_string());
                    close();
                    return;
                }
                self.stage += 1;
                self.clicked = false;
                self.requested = false;
                self.frames = 0;
                if self.stage == steps.len() && self.tool.is_some() {
                    let _ = fs::write(
                        root.join("tool-ready.txt"),
                        format!("Clicked {name} from More; tool is open."),
                    );
                    close();
                    return;
                }
                if self.stage == STEPS.len() {
                    let _ = fs::write(
                        root.join("menu-ready.txt"),
                        "Opened Miuix More, Settings and Sort via pointer events; captured real GUI frames.",
                    );
                    egui::Popup::close_all(ctx);
                    return;
                }
            }
        } else if self.frames
            >= (if target.ends_with("-open") && self.tool.is_some() {
                26
            } else {
                6
            })
            && let Some(rect) = probe(target)
        {
            if self.tool.is_some() && target.ends_with("-open") {
                let screen = ctx.content_rect();
                let size = format!(
                    "{}: {:.1} x {:.1} pt; viewport {:.1} x {:.1} pt",
                    target,
                    rect.width(),
                    rect.height(),
                    screen.width(),
                    screen.height()
                );
                let _ = fs::write(root.join("tool-size.txt"), &size);
                if rect.width() > 485.
                    || rect.height() > 305.
                    || !screen.expand(1.).contains_rect(rect)
                {
                    let _ = fs::write(root.join("menu-error.txt"), size);
                    close();
                    return;
                }
            }
            self.requested = true;
            ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(egui::UserData::default()));
        }
        ctx.request_repaint_after(Duration::from_millis(24));
    }
}

pub(super) fn switch_probe(ctx: &egui::Context, source: &str, index: usize, rect: egui::Rect) {
    let pass = ctx.cumulative_pass_nr();
    ctx.data_mut(|d| d.insert_temp(egui::Id::new((source, index)), (pass, rect)));
}

#[derive(Default)]
struct SwitchSmoke {
    stage: usize,
    phase: u8,
    frames: u8,
    held: Option<egui::Pos2>,
    requested: bool,
    events: Vec<egui::Event>,
}

impl SwitchSmoke {
    fn frame(&mut self, root: &Path, ctx: &egui::Context, selected: usize) {
        let cases = [
            ("pane-switch-slot", 1usize),
            ("pane-switch-slot", 0),
            ("pane-blank", 1),
            ("pane-blank", 0),
            ("pane-switch-slot", 1),
            ("pane-switch-slot", 0),
            ("pane-blank", 1),
            ("pane-blank", 0),
        ];
        let Some(&(source, target)) = cases.get(self.stage) else {
            return;
        };
        let close = || ctx.send_viewport_cmd(egui::ViewportCommand::Close);
        let pass = ctx.cumulative_pass_nr();
        let phase = self.phase;
        self.frames = self.frames.saturating_add(1);
        if self.frames > 50 {
            let _ = fs::write(
                root.join("switch-error.txt"),
                format!(
                    "Case {} ({source}): expected active pane {target}; actual {selected}, phase {phase}.",
                    self.stage
                ),
            );
            close();
            return;
        }
        match phase {
            0 => {
                if self.stage == 4 && self.frames == 1 {
                    ctx.send_viewport_cmd(egui::ViewportCommand::InnerSize(egui::vec2(760., 560.)));
                }
                if self.frames >= 6
                    && let Some((seen, rect)) = ctx
                        .data(|d| d.get_temp::<(u64, egui::Rect)>(egui::Id::new((source, target))))
                    && seen == pass
                {
                    self.held = Some(rect.center());
                    self.events.push(egui::Event::PointerMoved(rect.center()));
                    self.phase = 1;
                }
            }
            1 | 2 => {
                if let Some(pos) = self.held {
                    self.events.push(egui::Event::PointerButton {
                        pos,
                        button: egui::PointerButton::Primary,
                        pressed: phase == 1,
                        modifiers: egui::Modifiers::NONE,
                    });
                    self.phase += 1;
                }
            }

            _ if selected == target => {
                if source == "pane-blank" && self.stage < 4 {
                    if self.frames < 17 {
                        ctx.request_repaint_after(Duration::from_millis(24));
                        return;
                    }
                    if !self.requested {
                        self.requested = true;
                        ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(
                            egui::UserData::default(),
                        ));
                        ctx.request_repaint_after(Duration::from_millis(24));
                        return;
                    }
                    let Some(image) = ctx.input(|i| {
                        i.events.iter().find_map(|e| {
                            if let egui::Event::Screenshot { image, .. } = e {
                                Some(image.clone())
                            } else {
                                None
                            }
                        })
                    }) else {
                        ctx.request_repaint_after(Duration::from_millis(24));
                        return;
                    };
                    let png = if target == 1 {
                        "switch/pane-right.png"
                    } else {
                        "switch/pane-left.png"
                    };
                    let _ = fs::create_dir_all(root.join("switch"));
                    let bytes: Vec<_> = image.pixels.iter().flat_map(|p| p.to_array()).collect();
                    if let Err(error) = image::save_buffer(
                        root.join(png),
                        &bytes,
                        image.size[0] as u32,
                        image.size[1] as u32,
                        image::ColorType::Rgba8,
                    ) {
                        let _ = fs::write(root.join("switch-error.txt"), error.to_string());
                        close();
                        return;
                    }
                }
                self.requested = false;
                self.stage += 1;
                self.phase = 0;
                self.frames = 0;
                if self.stage == cases.len() {
                    let _ = fs::write(
                        root.join("switch-ready.txt"),
                        "Miuix real GUI: clicked both header tabs and both pane backgrounds at 1180 and 760 px; active state followed every click.",
                    );
                    return;
                }
            }
            _ => {}
        }
        ctx.request_repaint_after(Duration::from_millis(24));
    }
}

#[derive(Default)]
struct OverlaySmoke {
    step: usize,
    age: u16,
    seen_at: Option<f64>,
    captured: bool,
    events: Vec<egui::Event>,
}

impl OverlaySmoke {
    fn frame(&mut self, root: &Path, ctx: &egui::Context, settings: &mut Settings) {
        let pass = ctx.cumulative_pass_nr();
        let probe = |key| {
            ctx.data(|data| data.get_temp::<(u64, egui::Rect)>(egui::Id::new(("menu-smoke", key))))
                .filter(|(seen, _)| *seen == pass)
                .map(|(_, rect)| rect)
        };
        let close = || ctx.send_viewport_cmd(egui::ViewportCommand::Close);
        let fail = |msg: String| {
            let _ = fs::write(root.join("overlay-error.txt"), msg);
            close();
        };
        self.age += 1;
        if self.age > 85 {
            fail(format!(
                "Miuix overlay step {} timed out; search field={}, focus={}, result={}, arrived={}",
                self.step,
                probe("search-field").is_some(),
                probe("search-focused").is_some(),
                probe("search-result-first").is_some(),
                probe("search-arrived").is_some(),
            ));
            return;
        }
        let click = |events: &mut Vec<egui::Event>, rect: egui::Rect| {
            let pos = rect.center();
            events.push(egui::Event::PointerMoved(pos));
            for pressed in [true, false] {
                events.push(egui::Event::PointerButton {
                    pos,
                    button: egui::PointerButton::Primary,
                    pressed,
                    modifiers: egui::Modifiers::NONE,
                });
            }
        };
        let key = |events: &mut Vec<egui::Event>, key| {
            for pressed in [true, false] {
                events.push(egui::Event::Key {
                    key,
                    physical_key: None,
                    pressed,
                    repeat: false,
                    modifiers: egui::Modifiers::NONE,
                });
            }
        };
        let shot =
            || ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(egui::UserData::default()));
        let save = |name: &str| -> io::Result<bool> {
            let Some(image) = ctx.input(|i| {
                i.events.iter().find_map(|e| match e {
                    egui::Event::Screenshot { image, .. } => Some(image.clone()),
                    _ => None,
                })
            }) else {
                return Ok(false);
            };
            let out = root.join("overlay");
            fs::create_dir_all(&out)?;
            let bytes: Vec<_> = image.pixels.iter().flat_map(|px| px.to_array()).collect();
            image::save_buffer(
                out.join(name),
                &bytes,
                image.size[0] as u32,
                image.size[1] as u32,
                image::ColorType::Rgba8,
            )
            .map_err(io::Error::other)?;
            Ok(true)
        };
        let elapsed = |since: &mut Option<f64>| {
            let now = ctx.input(|i| i.time);
            now - *since.get_or_insert(now)
        };
        let advance = match self.step {
            0 => {
                if self.age == 1 {
                    settings.dark = false;
                    settings.reduce_motion = false;
                    crate::theme::apply(ctx, settings);
                }
                if self.age >= 4
                    && let Some(r) = probe("search")
                {
                    click(&mut self.events, r);
                    true
                } else {
                    false
                }
            }
            1 => {
                if probe("search-focused").is_none() || elapsed(&mut self.seen_at) < 0.32 {
                    false
                } else if !self.captured {
                    shot();
                    self.captured = true;
                    false
                } else {
                    match save("search-light.png") {
                        Ok(true) => {
                            key(&mut self.events, Key::Escape);
                            true
                        }
                        Ok(false) => false,
                        Err(e) => {
                            fail(e.to_string());
                            return;
                        }
                    }
                }
            }
            2 => {
                // Wait a frame for egui to clear the modal layer after Esc.
                if self.age >= 3
                    && probe("search-field").is_none()
                    && let Some(r) = probe("search")
                {
                    click(&mut self.events, r);
                    true
                } else {
                    false
                }
            }
            3 => {
                if probe("search-focused").is_some() {
                    self.events.push(egui::Event::Text("下载资料".into()));
                    key(&mut self.events, Key::Enter);
                    true
                } else {
                    false
                }
            }
            4 => {
                if let Some(r) =
                    probe("search-result-first").filter(|_| elapsed(&mut self.seen_at) >= 0.32)
                {
                    if !self.captured {
                        shot();
                        self.captured = true;
                        false
                    } else {
                        match save("results-light.png") {
                            Ok(true) => {
                                click(&mut self.events, r);
                                true
                            }
                            Ok(false) => false,
                            Err(e) => {
                                fail(e.to_string());
                                return;
                            }
                        }
                    }
                } else {
                    false
                }
            }
            5 => {
                if probe("search-arrived").is_some()
                    && self.age >= 3
                    && let Some(r) = probe("dock-appearance")
                {
                    click(&mut self.events, r);
                    true
                } else {
                    false
                }
            }
            _ => {
                if probe("appearance-open").is_some() {
                    let _ = fs::write(
                        root.join("overlay-ready.txt"),
                        "Miuix real GUI: search button focuses input; Escape closes; reopened and Enter returned a fixture result; clicked result and navigated to the matching folder; dock appearance opened.",
                    );
                    close();
                    return;
                }
                false
            }
        };
        if advance {
            self.step += 1;
            self.age = 0;
            self.seen_at = None;
            self.captured = false;
        }
        ctx.request_repaint_after(Duration::from_millis(25));
    }
}

#[derive(Default)]
struct CloseSmoke {
    case: usize,
    initialized: bool,
    phase: usize,
    age: u16,
    clock: f64,
    gone_at: Option<f64>,
    requested: bool,
    events: Vec<egui::Event>,
}
impl CloseSmoke {
    fn frame(&mut self, dir: &Path, settings: &Settings, ctx: &egui::Context) {
        let close = || ctx.send_viewport_cmd(egui::ViewportCommand::Close);
        let fail = |s: &str| {
            let _ = fs::write(dir.join("close-error.txt"), s);
            close();
        };
        self.age += 1;
        if self.age > 150 {
            fail(&format!(
                "case {} phase {} timed out",
                self.case, self.phase
            ));
            return;
        }
        let pass = ctx.cumulative_pass_nr();
        let opened = ctx
            .data(|d| {
                d.get_temp::<(u64, egui::Rect)>(egui::Id::new(("menu-smoke", "appearance-open")))
            })
            .is_some_and(|(nr, _)| nr == pass);
        let reduced = settings.reduce_motion;
        if self.requested {
            if let Some(im) = ctx.input(|i| {
                i.events.iter().find_map(|e| match e {
                    egui::Event::Screenshot { image, .. } => Some(image.clone()),
                    _ => None,
                })
            }) {
                let label = ["before", "mid", "gone"][self.phase];
                let file = dir.join(format!(
                    "{:?}-{}-{label}.png",
                    settings.theme,
                    if reduced { "reduced" } else { "animated" }
                ));
                let raw: Vec<_> = im.pixels.iter().flat_map(|p| p.to_array()).collect();
                if let Err(e) = image::save_buffer(
                    file,
                    &raw,
                    im.width() as u32,
                    im.height() as u32,
                    image::ColorType::Rgba8,
                ) {
                    fail(&e.to_string());
                    return;
                }
                if self.phase == 0 {
                    let pos = if settings.theme == Theme::Miuix {
                        let Some(r) = ctx.data(|d| {
                            d.get_temp::<egui::Rect>(
                                egui::Id::new(("miuix-sheet", "外观设置")).with("close-target"),
                            )
                        }) else {
                            fail("missing Miuix close control");
                            return;
                        };
                        r.center()
                    } else {
                        let Some(rect) = ctx.memory(|m| m.area_rect(egui::Id::new("外观设置")))
                        else {
                            fail("missing appearance window");
                            return;
                        };
                        let style = ctx.style();
                        let height = ctx
                            .fonts_mut(|f| f.row_height(&egui::TextStyle::Heading.resolve(&style)))
                            .max(style.spacing.interact_size.y);
                        let inset = egui::Frame::window(&style).inner_margin;
                        rect.right_top()
                            + egui::vec2(
                                -f32::from(inset.right) - height / 2.,
                                f32::from(inset.top) + height / 2.,
                            )
                    };
                    self.events.push(egui::Event::PointerMoved(pos));
                    for pressed in [true, false] {
                        self.events.push(egui::Event::PointerButton {
                            pos,
                            button: egui::PointerButton::Primary,
                            pressed,
                            modifiers: egui::Modifiers::NONE,
                        });
                    }
                }
                self.phase += 1;
                if self.phase == 3 {
                    self.phase = 0;
                    self.case += 1;
                    self.initialized = false;
                    self.gone_at = None;
                }
                self.requested = false;
                self.age = 0;
                if self.case == Theme::ALL.len() * 2 {
                    let _ = fs::write(
                        dir.join("close-ready.txt"),
                        "Clicked real appearance title-bar close for all themes, animated and reduced.",
                    );
                    close();
                    return;
                }
            }
        } else {
            let elapsed = if !opened {
                self.clock - *self.gone_at.get_or_insert(self.clock)
            } else {
                0.
            };
            let ready = match self.phase {
                0 => opened && self.age >= 28,
                1 => {
                    !opened
                        && (reduced
                            || elapsed >= f64::from(ctx.style().animation_time.min(0.25)) / 2.)
                }
                _ => !opened && elapsed >= 0.36,
            };
            if ready {
                ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(egui::UserData::default()));
                self.requested = true;
            }
        }
        ctx.request_repaint();
    }
}
