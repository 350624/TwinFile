use super::*;
use crate::theme::soft as sf;
use eframe::egui::*;
impl App {
    pub(super) fn soft_workbench(&mut self, ctx: &Context) {
        let compact = ctx.content_rect().width() < 880.;
        let canvas = ctx.style().visuals.panel_fill;
        let pad = if compact { 10 } else { 18 };
        TopBottomPanel::top("soft-header")
            .frame(
                Frame::new()
                    .fill(canvas)
                    .inner_margin(Margin::symmetric(pad, 12)),
            )
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    if let Some(icon) = super::app_icon(ctx) {
                        let chip = ui.allocate_exact_size(vec2(40., 40.), Sense::hover()).0;
                        ui.painter().circle_filled(
                            chip.center(),
                            20.,
                            ui.visuals().selection.bg_fill,
                        );
                        ui.painter().image(
                            icon.id(),
                            Rect::from_center_size(chip.center(), vec2(28., 28.)),
                            Rect::from_min_max(Pos2::ZERO, pos2(1., 1.)),
                            Color32::WHITE,
                        );
                    }
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 1.;
                        ui.label(
                            RichText::new("TWINFILE   /   WORKSPACE")
                                .font(sf::font(11.))
                                .color(ui.visuals().weak_text_color()),
                        );
                        ui.label(
                            RichText::new("文件工作台")
                                .font(sf::font(if compact { 22. } else { 28. }))
                                .strong(),
                        );
                    });
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        if sf::chip(ui, "外观", "选择主题、明暗和动效", false, false).clicked()
                        {
                            self.dialog = Some(Dialog::Appearance);
                        }
                        if sf::chip(ui, "搜索", "在当前栏搜索文件", false, false).clicked()
                        {
                            self.dialog = Some(Dialog::Search(String::new()));
                        }
                        for (i, label) in [(1, "右栏"), (0, "左栏")] {
                            if sf::chip(ui, label, "切换活动栏", i == self.s.active, false)
                                .clicked()
                            {
                                self.s.active = i;
                            }
                        }
                    });
                });
            });
        SidePanel::left("soft-nav")
            .exact_width(if compact { 70. } else { 191. })
            .resizable(false)
            .frame(Frame::new().fill(canvas).inner_margin(Margin {
                left: pad,
                right: 3,
                top: 5,
                bottom: 12,
            }))
            .show(ctx, |ui| {
                sf::card_frame(ui, if compact { 6 } else { 10 }).show(ui, |ui| {
                    ui.set_min_height(ui.available_height());
                    ui.set_min_width(ui.available_width());
                    self.soft_side(ui, compact);
                });
            });
        CentralPanel::default()
            .frame(Frame::new().fill(canvas).inner_margin(Margin {
                left: 10,
                right: pad,
                top: 5,
                bottom: 12,
            }))
            .show(ctx, |ui| {
                sf::card_frame(ui, 8).show(ui, |ui| {
                    ui.set_min_width(ui.available_width());
                    ui.horizontal_wrapped(|ui| {
                        if sf::chip(ui, "＋ 新建", "新建文件夹", false, true).clicked() {
                            self.new_item();
                        }
                        for (label, tip, op) in [
                            ("复制", "复制到另一栏 · F5", 0),
                            ("移动", "移动到另一栏 · F6", 1),
                            ("删除", "移至回收站", 2),
                        ] {
                            if sf::chip(ui, label, tip, false, false).clicked() {
                                match op {
                                    0 => self.other(false),
                                    1 => self.other(true),
                                    _ => {
                                        let files = self.selected();
                                        if !files.is_empty() {
                                            self.dialog = Some(Dialog::Delete(files));
                                        }
                                    }
                                }
                            }
                        }
                        ui.menu_button("选择", |ui| {
                            if ui.button("全选").clicked() {
                                let p = &mut self.panes[self.s.active];
                                p.selection = p.items.iter().map(|e| e.path.clone()).collect();
                                ui.close();
                            }
                            if ui.button("清空选择").clicked() {
                                self.panes[self.s.active].selection.clear();
                                ui.close();
                            }
                        });
                        ui.menu_button("排序", |ui| self.sort_menu(ui));
                        let r = sf::chip(ui, "更多 ···", "工具与文件操作", false, false);
                        if self.visual.is_some() {
                            super::super::visual_smoke::menu_probe(ctx, "more", r.rect);
                        }
                        sf::bubble(ui, &r, 238., |ui| {
                            ui.set_width(220.);
                            ScrollArea::vertical()
                                .max_height((ctx.content_rect().height() - 170.).max(120.))
                                .show(ui, |ui| self.miuix_more_rows(ui));
                        });
                    });
                    self.download_toolbar(ui);
                });
                ui.add_space(if compact { 9. } else { 16. });
                self.split_panes(ui);
            });
    }
    fn soft_side(&mut self, ui: &mut Ui, compact: bool) {
        let active = self.panes[self.s.active].location().clone();
        let mut next = None;
        let mut remove = None;
        let accent = ui.visuals().selection.stroke.color;
        let mint = if self.s.dark {
            Color32::from_rgb(110, 216, 184)
        } else {
            Color32::from_rgb(27, 132, 112)
        };
        let blush = if self.s.dark {
            Color32::from_rgb(240, 158, 199)
        } else {
            Color32::from_rgb(182, 77, 129)
        };
        let section = |ui: &mut Ui, label: &str| {
            ui.add_space(if compact { 4. } else { 8. });
            if !compact {
                ui.label(
                    RichText::new(label)
                        .small()
                        .color(ui.visuals().weak_text_color()),
                );
            }
        };
        ScrollArea::vertical()
            .auto_shrink([false, false])
            .id_salt("soft-places")
            .show(ui, |ui| {
                let row = |ui: &mut Ui, label: &str, tip: &str, mark: Mark, sel: bool, tint| {
                    sf::nav(ui, label, tip, sel, tint, |p, at, c| {
                        draw_mark(p, at, mark, c)
                    })
                };
                section(ui, "常用位置");
                if row(
                    ui,
                    "此电脑",
                    "此电脑",
                    Mark::Screen,
                    active == Location::Computer,
                    accent,
                )
                .clicked()
                {
                    next = Some(Location::Computer);
                }
                if let Some(home) = std::env::var_os("USERPROFILE").map(PathBuf::from) {
                    for (label, dir, m) in [
                        ("主目录", "", Mark::Folder),
                        ("桌面", "Desktop", Mark::Screen),
                        ("下载", "Downloads", Mark::Download),
                        ("文档", "Documents", Mark::Document),
                        ("图片", "Pictures", Mark::Image),
                        ("音乐", "Music", Mark::Music),
                        ("视频", "Videos", Mark::Video),
                    ] {
                        let path = home.join(dir);
                        if path.is_dir()
                            && row(ui, label, label, m, active.local() == Some(&path), accent)
                                .clicked()
                        {
                            next = Some(Location::Local(path));
                        }
                    }
                }
                section(ui, "设备与磁盘");
                for d in &self.drives {
                    let label = volume_name(d);
                    if row(
                        ui,
                        &label,
                        &label,
                        Mark::Disk,
                        active.local().is_some_and(|p| p.starts_with(&d.root)),
                        mint,
                    )
                    .clicked()
                    {
                        next = Some(Location::Local(d.root.clone()));
                    }
                }
                section(ui, "收藏与历史");
                if row(
                    ui,
                    "收藏此目录",
                    "收藏当前文件夹",
                    Mark::Checked,
                    false,
                    blush,
                )
                .clicked()
                {
                    self.favorite_current();
                }
                for p in &self.s.favorites {
                    let r = row(
                        ui,
                        &name(p),
                        &p.display().to_string(),
                        Mark::Folder,
                        active.local() == Some(p),
                        blush,
                    );
                    if r.clicked() {
                        next = Some(Location::Local(p.clone()));
                    }
                    r.context_menu(|ui| {
                        if ui.button("移除收藏").clicked() {
                            remove = Some(p.clone());
                            ui.close();
                        }
                    });
                }
                let r = row(ui, "最近访问", "最近访问的位置", Mark::Back, false, blush);
                sf::bubble(ui, &r, 218., |ui| {
                    if self.s.recent.is_empty() {
                        ui.weak("还没有最近位置");
                    }
                    ScrollArea::vertical().max_height(200.).show(ui, |ui| {
                        for p in &self.s.recent {
                            if ui
                                .button(name(p))
                                .on_hover_text(p.display().to_string())
                                .clicked()
                            {
                                next = Some(Location::Local(p.clone()));
                                ui.close();
                            }
                        }
                    });
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
    let a = ui.visuals().selection.stroke.color;
    ui.spacing_mut().item_spacing = vec2(5., 5.);
    ui.spacing_mut().button_padding = vec2(6., 3.);
    ui.spacing_mut().interact_size.y = 24.;
    ui.horizontal(|ui| {
        let color = ui.visuals().weak_text_color().lerp_to_gamma(a, focus);
        ui.label(
            RichText::new(if i == 0 { "左栏  /" } else { "右栏  /" })
                .small()
                .color(color),
        );
        ui.add(
            Label::new(
                RichText::new(location_title(p.location()))
                    .font(sf::font(18.))
                    .strong(),
            )
            .truncate(),
        );
    });
    ui.horizontal(|ui| {
        if ui.small_button("＋").on_hover_text("新标签页").clicked() {
            p.tab(h);
            *act = true;
        }
        if ui
            .small_button("×")
            .on_hover_text("关闭当前标签页")
            .clicked()
        {
            p.close_tab(h);
            *act = true;
        }
        let mut next = None;
        ScrollArea::horizontal()
            .id_salt(("soft-tabs", i))
            .show(ui, |ui| {
                ui.horizontal(|ui| {
                    for (k, t) in p.tabs.iter().enumerate() {
                        if ui
                            .motion_label(k == p.index, location_title(&t.location))
                            .on_hover_text(t.location.display())
                            .clicked()
                        {
                            next = Some(k);
                        }
                    }
                });
            });
        if let Some(k) = next {
            p.index = k;
            p.switch_motion(h, ListMotion::Tab);
            *act = true;
        }
    });
    ui.horizontal(|ui| {
        for (label, tip, n) in [("‹", "后退", 0), ("›", "前进", 1), ("↑", "上一级", 2)]
        {
            if ui.small_button(label).on_hover_text(tip).clicked() {
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
        let w = ui.available_width();
        let resp = ui.add(
            TextEdit::singleline(&mut p.input)
                .id(Id::new(("path", i)))
                .desired_width((w - 12.).max(8.))
                .font(sf::font(12.)),
        );
        if resp.has_focus() {
            *act = true;
        }
        if resp.lost_focus() && ui.input(|input| input.key_pressed(Key::Enter)) {
            *go = parse_location(&p.input);
            *act = true;
        }
        if resp.has_focus() {
            ui.painter().rect_stroke(
                resp.rect.expand(1.),
                16,
                Stroke::new(2.0_f32, a.gamma_multiply(0.4)),
                StrokeKind::Outside,
            );
        }
    });
}
