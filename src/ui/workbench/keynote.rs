use super::*;
use crate::theme::keynote as k;
use eframe::egui::*;

impl App {
    pub(super) fn keynote_workbench(&mut self, ctx: &Context) {
        let compact = ctx.content_rect().width() < 960.;
        let pad = if compact { 12 } else { 24 };
        TopBottomPanel::top("keynote-masthead")
            .exact_height(48.)
            .frame(
                Frame::new()
                    .fill(Color32::from_black_alpha(179))
                    .inner_margin(Margin::symmetric(pad, 0)),
            )
            .show(ctx, |ui| {
                ui.horizontal_centered(|ui| {
                    if let Some(tex) = app_icon(ctx) {
                        ui.add(Image::new((tex.id(), vec2(22., 22.))));
                    }
                    ui.label(RichText::new("TwinFile").font(k::font(17.)).strong());
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        if k::action(ui, "外观", false, false).clicked() {
                            self.dialog = Some(Dialog::Appearance);
                        }
                        if k::action(ui, "搜索", false, false).clicked() {
                            self.dialog = Some(Dialog::Search(String::new()));
                        }
                        for (i, title) in [(1, "右栏"), (0, "左栏")] {
                            if k::action(ui, title, false, self.s.active == i).clicked() {
                                self.s.active = i;
                            }
                        }
                    });
                });
                ui.painter().hline(
                    ui.max_rect().x_range(),
                    ui.max_rect().bottom() - 0.5,
                    Stroke::new(1_f32, Color32::from_white_alpha(25)),
                );
            });
        SidePanel::left("keynote-index")
            .resizable(false)
            .exact_width(if compact { 62. } else { 168. })
            .frame(
                Frame::new()
                    .fill(k::BLACK)
                    .inner_margin(Margin::symmetric(if compact { 8 } else { 14 }, 16)),
            )
            .show(ctx, |ui| self.keynote_index(ui, compact));
        CentralPanel::default()
            .frame(
                Frame::new()
                    .fill(k::BLACK)
                    .inner_margin(Margin::symmetric(pad, 8)),
            )
            .show(ctx, |ui| {
                // Cap the content width, not the viewport; keep both panes usable on narrow windows.
                let w = ui.available_width().min(1120.);
                let shift = (ui.available_width() - w) * 0.5;
                let area = Rect::from_min_size(
                    ui.min_rect().min + vec2(shift, 0.),
                    vec2(w, ui.available_height()),
                );
                let mut inner = ui.new_child(UiBuilder::new().max_rect(area));
                let ui = &mut inner;
                let h = if compact || ui.available_height() < 520. {
                    70.
                } else {
                    98.
                };
                let (hero, _) = ui.allocate_exact_size(vec2(w, h), Sense::hover());
                let p = ui.painter();
                let x = hero.left() + 2.;
                p.text(
                    pos2(x, hero.top() + 4.),
                    Align2::LEFT_TOP,
                    "T W I N F I L E    /    W O R K S P A C E",
                    k::font(11.),
                    k::BLUE,
                );
                p.text(
                    pos2(x, hero.bottom() - 6.),
                    Align2::LEFT_BOTTOM,
                    "文件工作台",
                    crate::theme::miuix::heading(if h > 80. { 52. } else { 36. }),
                    k::TEXT,
                );
                let scroll = ctx
                    .data(|d| d.get_temp::<f32>(Id::new(("keynote-scrub", self.s.active))))
                    .unwrap_or(0.);
                let stage = Rect::from_min_max(
                    pos2(hero.right() - if compact { 140. } else { 215. }, hero.top()),
                    hero.right_bottom(),
                );
                k::plates(
                    p,
                    stage,
                    if crate::motion::reduced(ctx) {
                        0.
                    } else {
                        scroll
                    },
                    self.s.active,
                );
                ui.horizontal_wrapped(|ui| {
                    ui.spacing_mut().item_spacing.x = if compact { 2. } else { 6. };
                    if k::action(ui, "＋ 新建", true, false).clicked() {
                        self.new_item();
                    }
                    if k::action(ui, "复制", false, false)
                        .on_hover_text("复制到另一栏 · F5")
                        .clicked()
                    {
                        self.other(false);
                    }
                    if k::action(ui, "移动", false, false)
                        .on_hover_text("移动到另一栏 · F6")
                        .clicked()
                    {
                        self.other(true);
                    }
                    if k::action(ui, "删除", false, false).clicked() {
                        let s = self.selected();
                        if !s.is_empty() {
                            self.dialog = Some(Dialog::Delete(s));
                        }
                    }
                    let sel = k::action(ui, "选择", false, false);
                    k::bubble(&sel, 168., "SELECTION", |ui| {
                        if k::menu_item(ui, "全选").clicked() {
                            let p = &mut self.panes[self.s.active];
                            p.selection = p.items.iter().map(|e| e.path.clone()).collect();
                            ui.close();
                        }
                        if k::menu_item(ui, "清空选择").clicked() {
                            self.panes[self.s.active].selection.clear();
                            ui.close();
                        }
                    });
                    let sort = k::action(ui, "排序", false, false);
                    k::bubble(&sort, 176., "SORT BY", |ui| self.sort_menu(ui));
                    let more = k::action(ui, "更多 ···", false, false);
                    if self.visual.is_some() {
                        super::super::visual_smoke::menu_probe(ctx, "more", more.rect);
                    }
                    k::bubble(&more, 250., "COMMANDS", |ui| self.keynote_commands(ui));
                });
                self.download_toolbar(ui);
                ui.add_space(if compact { 8. } else { 16. });
                self.split_panes(ui);
            });
    }
    fn keynote_commands(&mut self, ui: &mut Ui) {
        for (label, probe, id) in [
            ("Payload 分区提取", "payload-entry", 0),
            ("HFX · USB / Wi-Fi 传输", "hfx-entry", 1),
            ("直链下载", "", 2),
        ] {
            let r = k::menu_item(ui, label);
            if !probe.is_empty() && self.visual.is_some() {
                super::super::visual_smoke::menu_probe(ui.ctx(), probe, r.rect);
            }
            if r.clicked() {
                ui.close();
                match id {
                    0 => self.payload.open = true,
                    1 => self.hfx.open = true,
                    _ => self.dl.open = true,
                }
            }
        }
        ui.separator();
        for (label, id) in [
            ("交换左右栏", 0),
            ("新建文件", 1),
            ("刷新当前栏", 2),
            ("复制到系统剪贴板", 3),
            ("剪切到系统剪贴板", 4),
            ("从系统剪贴板粘贴文件", 5),
            ("创建 ZIP…", 6),
            ("批量重命名…", 7),
            ("比较目录…", 8),
            ("比较文本…", 9),
            ("收藏当前目录", 10),
        ] {
            if k::menu_item(ui, label).clicked() {
                ui.close();
                match id {
                    0 => self.swap_panes(),
                    1 => self.new_item_kind(false),
                    2 => self.refresh(),
                    3 => self.clip_files(false),
                    4 => self.clip_files(true),
                    5 => self.paste_files(),
                    6 => self.make_zip(),
                    7 => self.batch(),
                    8 => self.open_compare_mode(false),
                    9 => self.open_compare_mode(true),
                    _ => self.favorite_current(),
                }
            }
        }
        ui.menu_button("文件校验…", |ui| {
            for (label, mode) in [
                ("导入校验清单", local_tools::HashMode::Import),
                ("粘贴校验值", local_tools::HashMode::Paste),
                ("导出校验清单", local_tools::HashMode::Export),
            ] {
                if k::menu_item(ui, label).clicked() {
                    ui.close();
                    if let Some(path) = self.one() {
                        if path.is_file() {
                            if let Some(d) = local_tools::HashDialog::open(path, mode) {
                                self.dialog = Some(Dialog::Hash(d));
                            }
                        } else {
                            self.status = "请选择普通文件".into();
                        }
                    }
                }
            }
        });
        if ui
            .motion_check(&mut self.s.hidden, "显示隐藏文件")
            .changed()
        {
            for p in &mut self.panes {
                p.read(self.s.hidden);
            }
        }
    }
    fn keynote_index(&mut self, ui: &mut Ui, compact: bool) {
        let current = self.panes[self.s.active].location().clone();
        let mut next = None;
        let mut remove = None;
        ScrollArea::vertical()
            .id_salt("keynote-locations")
            .auto_shrink([false, false])
            .show(ui, |ui| {
                let heading = |ui: &mut Ui, n: &str, word: &str| {
                    ui.add_space(9.);
                    k::kicker(ui, if compact { n } else { word });
                    ui.add_space(3.);
                };
                heading(ui, "01", "01   PLACES");
                if nav(
                    ui,
                    "此电脑",
                    Mark::Screen,
                    current == Location::Computer,
                    compact,
                )
                .clicked()
                {
                    next = Some(Location::Computer);
                }
                if let Some(home) = std::env::var_os("USERPROFILE").map(PathBuf::from) {
                    for (label, part, mark) in [
                        ("主目录", "", Mark::Folder),
                        ("桌面", "Desktop", Mark::Screen),
                        ("下载", "Downloads", Mark::Download),
                        ("文档", "Documents", Mark::Document),
                        ("图片", "Pictures", Mark::Image),
                        ("音乐", "Music", Mark::Music),
                        ("视频", "Videos", Mark::Video),
                    ] {
                        let path = home.join(part);
                        if path.is_dir()
                            && nav(ui, label, mark, current.local() == Some(&path), compact)
                                .clicked()
                        {
                            next = Some(Location::Local(path));
                        }
                    }
                }
                heading(ui, "02", "02   DEVICES");
                for drive in &self.drives {
                    if nav(
                        ui,
                        &volume_name(drive),
                        if drive.kind == files::VolumeKind::Removable {
                            Mark::Usb
                        } else {
                            Mark::Disk
                        },
                        current.local().is_some_and(|p| p.starts_with(&drive.root)),
                        compact,
                    )
                    .clicked()
                    {
                        next = Some(Location::Local(drive.root.clone()));
                    }
                }
                heading(ui, "03", "03   LIBRARY");
                if nav(ui, "收藏此目录", Mark::Checked, false, compact).clicked() {
                    self.favorite_current();
                }
                for p in &self.s.favorites {
                    let r = nav(
                        ui,
                        &name(p),
                        Mark::Folder,
                        current.local() == Some(p),
                        compact,
                    )
                    .on_hover_text(p.display().to_string());
                    if r.clicked() {
                        next = Some(Location::Local(p.clone()));
                    }
                    r.context_menu(|ui| {
                        if k::menu_item(ui, "移除收藏").clicked() {
                            remove = Some(p.clone());
                            ui.close();
                        }
                    });
                }
                let recent = nav(ui, "最近位置", Mark::Back, false, compact);
                k::bubble(&recent, 236., "RECENT", |ui| {
                    if self.s.recent.is_empty() {
                        ui.weak("暂无最近位置");
                    }
                    for p in &self.s.recent {
                        if k::menu_item(ui, &name(p))
                            .on_hover_text(p.display().to_string())
                            .clicked()
                        {
                            next = Some(Location::Local(p.clone()));
                            ui.close();
                        }
                    }
                });
            });
        if let Some(p) = remove {
            self.s.favorites.retain(|f| f != &p);
        }
        if let Some(p) = next {
            self.navigate(p);
        }
    }
}
fn nav(ui: &mut Ui, label: &str, mark: Mark, selected: bool, compact: bool) -> Response {
    let (rect, r) = ui.allocate_exact_size(vec2(ui.available_width(), 27.), Sense::click());
    r.widget_info(|| WidgetInfo::labeled(WidgetType::Button, ui.is_enabled(), label));
    let (h, _) = crate::motion::feedback(ui, &r);
    let p = ui.painter().with_clip_rect(rect.intersect(ui.clip_rect()));
    if h > 0. || selected {
        p.rect_filled(
            rect,
            9,
            if selected {
                k::CARD
            } else {
                k::CARD.gamma_multiply(h)
            },
        );
    }
    let ink = if selected { k::BLUE } else { k::TEXT };
    let at = if compact {
        rect.center()
    } else {
        rect.left_center() + vec2(16., 0.)
    };
    draw_mark(&p, at, mark, ink);
    if !compact {
        p.text(
            at + vec2(16., 0.),
            Align2::LEFT_CENTER,
            label,
            k::font(14.),
            ink,
        );
    }
    if r.has_focus() {
        p.rect_stroke(rect, 9, Stroke::new(1_f32, k::BLUE), StrokeKind::Inside);
    }
    r.on_hover_text(label)
}
pub(super) fn pane_head(
    ui: &mut Ui,
    p: &mut Pane,
    i: usize,
    focus: f32,
    h: bool,
    act: &mut bool,
    go: &mut Option<Location>,
    refresh: &mut bool,
) {
    ui.spacing_mut().item_spacing = vec2(4., 4.);
    ui.spacing_mut().interact_size.y = 23.;
    ui.spacing_mut().button_padding = vec2(6., 2.);
    ui.horizontal(|ui| {
        let ink = k::MUTED.lerp_to_gamma(k::BLUE, focus);
        ui.label(
            RichText::new(if i == 0 {
                "01   /   LEFT"
            } else {
                "02   /   RIGHT"
            })
            .font(k::font(11.))
            .color(ink)
            .extra_letter_spacing(1.4),
        );
        if focus > 0.0 {
            ui.painter().circle_filled(
                ui.cursor().left_center() + vec2(5., 0.),
                2.5 * focus,
                k::BLUE.gamma_multiply(focus),
            );
        }
    });
    ui.add(
        Label::new(
            RichText::new(location_title(p.location())).font(crate::theme::miuix::heading(
                if ui.available_width() < 240. {
                    19.
                } else {
                    24.
                },
            )),
        )
        .truncate(),
    );
    ui.horizontal(|ui| {
        for (l, tip, n) in [("‹", "后退", 0), ("›", "前进", 1), ("↑", "上一级", 2)] {
            if ui.small_button(l).on_hover_text(tip).clicked() {
                *act = true;
                match n {
                    0 => p.history(true, h),
                    1 => p.history(false, h),
                    2 => *go = p.parent(),
                    _ => unreachable!(),
                }
            }
        }
        if refresh_button(ui, "刷新", true).clicked() {
            *act = true;
            p.refresh_list(h);
            *refresh = p.dir().is_none();
        }
        let r = ui.small_button(format!("标签 {}", p.tabs.len()));
        k::bubble(&r, 206., "TABS", |ui| {
            let mut next = None;
            for (j, t) in p.tabs.iter().enumerate() {
                if ui
                    .motion_label(p.index == j, location_title(&t.location))
                    .clicked()
                {
                    next = Some(j);
                    ui.close();
                }
            }
            if let Some(j) = next {
                p.index = j;
                p.switch_motion(h, ListMotion::Tab);
                *act = true;
            }
            ui.separator();
            if k::menu_item(ui, "＋ 新标签").clicked() {
                p.tab(h);
                *act = true;
                ui.close();
            }
            if k::menu_item(ui, "关闭当前标签").clicked() {
                p.close_tab(h);
                *act = true;
                ui.close();
            }
        });
    });
    let id = Id::new(("path", i));
    let frame = Frame::new()
        .corner_radius(12)
        .fill(k::BLACK)
        .inner_margin(Margin::symmetric(8, 4))
        .stroke(Stroke::new(
            1_f32,
            if ui.memory(|m| m.has_focus(id)) {
                k::BLUE
            } else {
                Color32::from_white_alpha(25)
            },
        ));
    let edit = frame
        .show(ui, |ui| {
            ui.add(
                TextEdit::singleline(&mut p.input)
                    .id(id)
                    .font(k::font(12.))
                    .frame(false)
                    .desired_width(ui.available_width()),
            )
        })
        .inner;
    if edit.has_focus() {
        *act = true;
        if ui.input(|inp| inp.key_pressed(Key::Escape)) {
            p.input = p.location().display();
            edit.surrender_focus();
        }
    }
    if edit.lost_focus() && ui.input(|inp| inp.key_pressed(Key::Enter)) {
        *go = parse_location(&p.input);
        *act = true;
    }
    ui.add_space(4.);
}
