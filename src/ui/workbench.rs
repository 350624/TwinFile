use crate::motion::ChoiceUi as _;
mod file_menu;
mod keynote;
mod soft;
use super::*;
use crate::theme::miuix as mui;
impl App {
    pub(super) fn workbench(&mut self, ctx: &egui::Context) {
        self.shortcut(ctx);
        if self.s.theme == Theme::Keynote {
            self.keynote_workbench(ctx);
            return;
        }
        if self.s.theme == Theme::Soft {
            self.soft_workbench(ctx);
            return;
        }
        if self.s.theme == Theme::Miuix {
            self.miuix_workbench(ctx);
            return;
        }
        egui::TopBottomPanel::top("top").show(ctx, |ui| self.top(ui));
        egui::SidePanel::left("nav")
            .default_width(150.0)
            .width_range(100.0..=320.0)
            .show(ctx, |ui| self.side(ui));
        egui::CentralPanel::default().show(ctx, |ui| self.split_panes(ui));
    }

    fn miuix_workbench(&mut self, ctx: &egui::Context) {
        let canvas = mui::Colors::new(self.s.dark).workbench();
        egui::TopBottomPanel::bottom("miuix-dock")
            .frame(egui::Frame::new().fill(canvas).inner_margin(egui::Margin {
                top: 7,
                bottom: 10,
                ..Default::default()
            }))
            .show(ctx, |ui| self.miuix_dock(ui));
        let w = ctx.content_rect().width();
        egui::SidePanel::left("miuix-nav")
            .resizable(false)
            .exact_width(if w < 900.0 { 148.0 } else { 182.0 })
            .frame(egui::Frame::new().fill(canvas).inner_margin(egui::Margin {
                left: 12,
                right: 0,
                top: 12,
                bottom: 12,
            }))
            .show(ctx, |ui| {
                mui::group(ui, 16.0, egui::Margin::same(8), |ui| self.miuix_side(ui));
            });
        egui::CentralPanel::default()
            .frame(egui::Frame::new().fill(canvas).inner_margin(egui::Margin {
                left: 10,
                right: 12,
                top: 12,
                bottom: 12,
            }))
            .show(ctx, |ui| {
                self.miuix_actions(ui);
                ui.add_space(4.0);
                self.split_panes(ui);
            });
    }

    fn miuix_dock(&mut self, ui: &mut egui::Ui) {
        const ITEM: f32 = 44.0;
        const WIDTH: f32 = ITEM * 5.0 + 6.0 * 4.0 + 18.0;
        let (on, view) = self.dock.view(Instant::now());
        let expand = if crate::motion::reduced(ui.ctx()) {
            f32::from(on)
        } else {
            ui.ctx()
                .animate_bool_with_time(ui.id().with("transfer-expand"), on, 0.26)
        };
        if on {
            ui.ctx().request_repaint_after(Duration::from_millis(40));
        }
        let extra = (ui.available_width() - WIDTH).clamp(0., 132.) * expand;
        let left = (ui.available_width() - WIDTH - extra).max(0.) * 0.5;
        let c = mui::Colors::of(ui);
        let i = self.s.active;
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 0.0;
            ui.add_space(left);
            let capsule =
                egui::Frame::new()
                    .fill(c.container)
                    .corner_radius(28)
                    .stroke(egui::Stroke::new(1.0_f32, c.outline.gamma_multiply(0.45)))
                    .shadow(egui::epaint::Shadow {
                        offset: [0, 2],
                        blur: 10,
                        spread: 0,
                        color: egui::Color32::from_black_alpha(if self.s.dark { 125 } else { 36 }),
                    })
                    .inner_margin(egui::Margin::symmetric(8, 4))
                    .show(ui, |ui| {
                        ui.horizontal(|ui| {
                            ui.spacing_mut().item_spacing.x = 6.0;
                            for (icon, label, selected, action) in [
                                (Mark::Back, "后退", false, 0),
                                (Mark::Forward, "前进", false, 1),
                                (Mark::Refresh, "刷新当前栏", false, 2),
                                (Mark::Download, "直链下载", self.dl.open, 3),
                                (
                                    Mark::Appearance,
                                    "外观设置",
                                    matches!(self.dialog, Some(Dialog::Appearance)),
                                    4,
                                ),
                            ] {
                                let response = mui::dock_item_with_response(
                                    ui,
                                    ITEM,
                                    label,
                                    selected,
                                    |p, at, color, hit| {
                                        let t = crate::motion::hover_clip(
                                            hit,
                                            match icon {
                                                Mark::Refresh => 0.54,
                                                Mark::Appearance => 0.50,
                                                _ => 0.40,
                                            },
                                        );
                                        draw_mark_phase(p, at, icon, color, t);
                                        #[cfg(test)]
                                        hit.ctx.data_mut(|d| {
                                            d.insert_temp(
                                                egui::Id::new(("dock-hover-test", action)),
                                                (hit.rect, t),
                                            )
                                        });
                                    },
                                );
                                if self.visual.is_some() && action == 4 {
                                    super::visual_smoke::menu_probe(
                                        ui.ctx(),
                                        "dock-appearance",
                                        response.rect,
                                    );
                                }
                                if response.clicked() {
                                    match action {
                                        0 => self.panes[i].history(true, self.s.hidden),
                                        1 => self.panes[i].history(false, self.s.hidden),
                                        2 => self.refresh(),
                                        3 => self.dl.open = true,
                                        _ => self.dialog = Some(Dialog::Appearance),
                                    }
                                }
                            }
                            if let Some(v) = view.filter(|_| extra > 0.01) {
                                // No inter-widget gap at zero width: preserve the idle dock silhouette.
                                ui.spacing_mut().item_spacing.x = 0.;
                                let details = on
                                    && !v.success
                                    && !matches!(
                                        v.kind,
                                        dock_progress::Kind::Copy | dock_progress::Kind::Move
                                    );
                                let (r, hit) = ui.allocate_exact_size(
                                    egui::vec2((extra - 6.).max(0.), ITEM),
                                    if details {
                                        egui::Sense::click()
                                    } else {
                                        egui::Sense::hover()
                                    },
                                );
                                if details && hit.on_hover_text("打开任务详情").clicked() {
                                    match v.kind {
                                        dock_progress::Kind::Download => self.dl.open = true,
                                        dock_progress::Kind::Send
                                        | dock_progress::Kind::Receive => self.hfx.open = true,
                                        dock_progress::Kind::Read
                                        | dock_progress::Kind::Extract => self.payload.open = true,
                                        _ => {}
                                    }
                                }
                                v.info(ui, r, expand);
                            }
                        });
                    });
            #[cfg(test)]
            ui.ctx().data_mut(|d| {
                d.insert_temp(
                    egui::Id::new("test-transfer-dock-rect"),
                    capsule.response.rect,
                )
            });
            if let Some(v) = view.filter(|_| expand > 0.001) {
                v.rim(ui, capsule.response.rect, expand);
            }
        });
    }
    fn new_item(&mut self) {
        self.new_item_kind(true);
    }

    fn new_item_kind(&mut self, folder: bool) {
        if self.panes[self.s.active].dir().is_some() {
            self.dialog = Some(Dialog::New(folder, String::new()));
        } else {
            self.status = "请先打开一个文件夹".into();
        }
    }

    fn favorite_current(&mut self) {
        if let Some(path) = self.panes[self.s.active].dir().map(Path::to_owned)
            && !self.s.favorites.contains(&path)
        {
            self.s.favorites.push(path);
        }
    }

    fn miuix_actions(&mut self, ui: &mut egui::Ui) {
        mui::group(ui, 16.0, egui::Margin::symmetric(10, 6), |ui| {
            ui.spacing_mut().button_padding = egui::vec2(9.0, 5.0);
            ui.spacing_mut().item_spacing = egui::vec2(3.0, 3.0);
            ui.spacing_mut().interact_size.y = 29.0;
            ui.visuals_mut().widgets.inactive.weak_bg_fill = egui::Color32::TRANSPARENT;
            let wide = ui.available_width() >= 625.0;
            ui.horizontal(|ui| {
                ui.set_min_height(40.0);
                let accent = ui.visuals().selection.stroke.color;
                if ui
                    .add_sized(
                        [76.0, 37.0],
                        egui::Button::new(
                            egui::RichText::new("＋ 新建")
                                .size(14.0)
                                .strong()
                                .color(egui::Color32::WHITE),
                        )
                        .fill(accent)
                        .corner_radius(12),
                    )
                    .clicked()
                {
                    self.new_item();
                }
                let i = self.s.active;
                for (icon, label, ix) in [
                    (Mark::Copy, "复制", 0),
                    (Mark::Move, "移动", 1),
                    (Mark::Trash, "删除", 2),
                    (Mark::Checked, "全选", 3),
                    (Mark::Check, "取消全选", 4),
                ] {
                    let title = if wide { label } else { "" };
                    if mark_button(ui, icon, title, label, false, false).clicked() {
                        match ix {
                            0 => self.other(false),
                            1 => self.other(true),
                            2 => {
                                let files = self.selected();
                                if !files.is_empty() {
                                    self.dialog = Some(Dialog::Delete(files));
                                }
                            }
                            3 => {
                                let p = &mut self.panes[i];
                                p.selection = p.items.iter().map(|row| row.path.clone()).collect();
                            }
                            _ => self.panes[i].selection.clear(),
                        }
                    }
                }
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    self.miuix_more(ui);
                    let search = mui::dock_item(
                        ui,
                        32.,
                        "搜索当前栏文件（Ctrl+F）",
                        false,
                        mui::search_icon,
                    );
                    if self.visual.is_some() {
                        super::visual_smoke::menu_probe(ui.ctx(), "search", search.rect);
                    }
                    if search.clicked() {
                        self.dialog = Some(Dialog::Search(String::new()));
                    }
                    ui.menu_button(egui::RichText::new("排序").size(13.0), |ui| {
                        self.sort_menu(ui)
                    });
                    if mark_button(ui, Mark::Contrast, "", "切换亮色 / 暗色", false, false)
                        .clicked()
                    {
                        self.s.dark = !self.s.dark;
                        crate::theme::apply(ui.ctx(), &self.s);
                    }
                });
            });
            if self.dl.work.is_none() {
                self.download_toolbar(ui);
            }
        });
    }

    fn sort_menu(&mut self, ui: &mut egui::Ui) {
        let p = &mut self.panes[self.s.active];
        let mut changed = false;
        for (s, n) in [
            (Sort::Name, "名称"),
            (Sort::Size, "大小"),
            (Sort::Modified, "修改时间"),
            (Sort::Extension, "类型"),
        ] {
            if p.dir().is_some() || s != Sort::Modified {
                changed |= ui.motion_value(&mut p.sort, s, n).changed();
            }
        }
        changed |= ui.motion_check(&mut p.desc, "降序").changed();
        if changed {
            files::sort(&mut p.items, p.sort, p.desc);
        }
    }

    fn miuix_side(&mut self, ui: &mut egui::Ui) {
        let active = self.panes[self.s.active].location().clone();
        let mut go = None;
        let mut remove = None;
        ui.horizontal(|ui| {
            ui.set_min_height(34.0);
            ui.add_space(7.0);
            let icon = app_icon(ui.ctx());
            if let Some(icon) = icon {
                ui.image((icon.id(), egui::vec2(22.0, 22.0)));
            }
            ui.label(egui::RichText::new("TwinFile").font(mui::heading(mui::TITLE)));
        });
        ui.add_space(4.0);
        egui::ScrollArea::vertical()
            .id_salt("miuix-nav-scroller")
            .auto_shrink([false, false])
            .max_height(ui.available_height().max(0.))
            .show(ui, |ui| {
                let pill = ui.painter().add(egui::Shape::Noop);
                let mut target = None;
                let home = std::env::var_os("USERPROFILE").map(PathBuf::from);
                let places = [
                    ("桌面", "Desktop", Mark::Screen),
                    ("下载", "Downloads", Mark::Download),
                    ("文档", "Documents", Mark::Document),
                    ("图片", "Pictures", Mark::Image),
                    ("音乐", "Music", Mark::Music),
                    ("视频", "Videos", Mark::Video),
                ];
                let matches_place = home.as_ref().is_some_and(|home| {
                    places
                        .iter()
                        .any(|(_, dir, _)| active.local() == Some(&home.join(dir)))
                });
                let matches_bookmark = active
                    .local()
                    .is_some_and(|path| self.s.favorites.iter().any(|p| p == path));
                side_heading(ui, "位置");
                let select = active == Location::Computer;
                let r = side_row(ui, Mark::Screen, "此电脑", select, true);
                if select {
                    target = Some(r.rect);
                }
                if r.clicked() {
                    go = Some(Location::Computer);
                }
                if let Some(home) = home {
                    for (label, dir, icon) in places {
                        let path = home.join(dir);
                        let selected = active.local() == Some(&path);
                        let r = ui
                            .add_enabled_ui(path.is_dir(), |ui| {
                                side_row(ui, icon, label, selected, true)
                            })
                            .inner;
                        if selected {
                            target = Some(r.rect);
                        }
                        if r.clicked() {
                            go = Some(Location::Local(path));
                        }
                    }
                }
                ui.add_space(22.0);
                side_heading(ui, "设备与磁盘");
                for d in &self.drives {
                    let selected = !matches_place
                        && !matches_bookmark
                        && active.local().is_some_and(|p| {
                            p.ancestors().last().is_some_and(|root| {
                                root.as_os_str()
                                    .to_string_lossy()
                                    .replace('/', "\\")
                                    .eq_ignore_ascii_case(&d.root.to_string_lossy())
                            })
                        });
                    let icon = if d.kind == files::VolumeKind::Removable {
                        Mark::Usb
                    } else {
                        Mark::Disk
                    };
                    let r = side_row(ui, icon, &volume_name(d), selected, false);
                    if selected {
                        target = Some(r.rect);
                    }
                    if r.clicked() {
                        go = Some(Location::Local(d.root.clone()));
                    }
                }
                ui.add_space(11.0);
                side_heading(ui, "收藏夹").context_menu(|ui| {
                    if ui.button("收藏当前目录").clicked() {
                        self.favorite_current();
                        ui.close();
                    }
                });
                for path in &self.s.favorites {
                    let selected = !matches_place && active.local() == Some(path);
                    let r = side_row(ui, Mark::Folder, &name(path), selected, false)
                        .on_hover_text(path.display().to_string());
                    if selected {
                        target = Some(r.rect);
                    }
                    if r.clicked() {
                        go = Some(Location::Local(path.clone()));
                    }
                    r.context_menu(|ui| {
                        if ui.button("移除收藏").clicked() {
                            remove = Some(path.clone());
                            ui.close();
                        }
                    });
                }
                let scrolled = ui.input(|i| i.smooth_scroll_delta.y != 0.0);
                if let Some(rect) = crate::motion::highlight(
                    ui.ctx(),
                    ui.id().with("drive-pill"),
                    target,
                    0.22,
                    scrolled,
                ) {
                    let marker = egui::Rect::from_center_size(
                        egui::pos2(rect.left() + 1.5, rect.center().y),
                        egui::vec2(3.0, 18.0),
                    );
                    ui.painter().set(
                        pill,
                        egui::Shape::Vec(vec![
                            mui::squircle(rect, 10.0, miuix_selected(ui), egui::Stroke::NONE),
                            egui::Shape::rect_filled(marker, 1.5, miuix_selected_text(ui)),
                        ]),
                    );
                }
            });
        if let Some(path) = remove {
            self.s.favorites.retain(|p| p != &path);
        }
        if let Some(location) = go {
            self.navigate(location);
        }
    }

    fn split_panes(&mut self, ui: &mut egui::Ui) {
        let (bounds, _) = ui.allocate_exact_size(ui.available_size(), egui::Sense::hover());
        let gap = if self.s.theme == Theme::Soft {
            18.
        } else if self.s.theme == Theme::Miuix {
            12.0
        } else {
            8.0
        };
        let fraction = self.s.split.clamp(0.22, 0.78);
        let middle = bounds.left() + bounds.width() * fraction;
        let bar = egui::Rect::from_min_max(
            egui::pos2(middle - gap / 2.0, bounds.top()),
            egui::pos2(middle + gap / 2.0, bounds.bottom()),
        );
        let r = ui
            .interact(
                bar,
                ui.make_persistent_id("dual-pane-divider"),
                egui::Sense::drag(),
            )
            .on_hover_cursor(egui::CursorIcon::ResizeHorizontal);
        let id = r.id.with("offset");
        if r.drag_started()
            && let Some(p) = r.interact_pointer_pos()
        {
            ui.ctx().data_mut(|d| d.insert_temp(id, p.x - middle));
        }
        if r.dragged()
            && let Some(p) = r.interact_pointer_pos()
        {
            let offset = ui.ctx().data(|d| d.get_temp::<f32>(id)).unwrap_or(0.0);
            self.s.split =
                ((p.x - offset - bounds.left()) / bounds.width().max(1.0)).clamp(0.22, 0.78);
        }
        if r.hovered() || r.dragged() {
            ui.painter()
                .vline(middle, bar.y_range(), ui.visuals().selection.stroke);
        }
        // One clock for both panes; keep all hit testing on the settled geometry.
        let right = self.s.active == 1;
        let duration = if crate::motion::reduced(ui.ctx()) {
            0.0
        } else {
            0.22
        };
        let t = ui
            .ctx()
            .animate_bool_with_time(ui.id().with("pane-focus"), right, duration);
        let t = if duration == 0.0 {
            f32::from(right)
        } else {
            t * t * (3.0 - 2.0 * t)
        };
        for (i, (x0, x1)) in [(bounds.left(), bar.left()), (bar.right(), bounds.right())]
            .into_iter()
            .enumerate()
        {
            let rect = egui::Rect::from_min_max(
                egui::pos2(x0, bounds.top()),
                egui::pos2(x1, bounds.bottom()),
            );
            let mut child =
                ui.new_child(egui::UiBuilder::new().id_salt(("pane", i)).max_rect(rect));
            child.set_clip_rect(rect.intersect(ui.clip_rect()));
            if self.visual.is_some() {
                let point = egui::pos2(rect.center().x, rect.bottom() - 80.0);
                super::visual_smoke::switch_probe(
                    ui.ctx(),
                    "pane-blank",
                    i,
                    egui::Rect::from_center_size(point, egui::vec2(1.0, 1.0)),
                );
            }
            self.pane(i, &mut child, if i == 0 { 1.0 - t } else { t });
        }
    }
    pub(super) fn appearance(&mut self, ui: &mut egui::Ui) {
        let mut change = false;
        for theme in Theme::ALL {
            change |= ui
                .motion_value(&mut self.s.theme, theme, theme.label())
                .changed();
        }
        ui.separator();
        let fixed = self.s.theme == Theme::Keynote;
        if fixed {
            ui.weak("发布会采用固定的纯黑背景和电蓝强调色。");
        } else {
            ui.horizontal(|ui| {
                change |= ui.motion_value(&mut self.s.dark, false, "亮色").changed();
                change |= ui.motion_value(&mut self.s.dark, true, "暗色").changed();
            });
        }
        change |= ui
            .motion_check(&mut self.s.reduce_motion, "减少动画")
            .changed();
        if !fixed {
            ui.horizontal(|ui| {
                ui.label("强调色");
                for n in 0..7 {
                    change |= ui
                        .motion_value(
                            &mut self.s.accent,
                            n,
                            if n == 0 {
                                "默认".into()
                            } else {
                                n.to_string()
                            },
                        )
                        .changed();
                }
            });
        }
        if change {
            crate::theme::apply(ui.ctx(), &self.s);
            self.persist();
        }
    }
    fn file_actions(&mut self, ui: &mut egui::Ui) {
        ui.horizontal_wrapped(|ui| {
            let i = self.s.active;
            let arrow = if i == 0 { "→" } else { "←" };
            if ui.button("新建").clicked() {
                self.new_item();
            }
            if ui
                .button(format!("复制 {arrow}"))
                .on_hover_text("复制到另一栏（F5）")
                .clicked()
            {
                self.other(false);
            }
            if ui
                .button(format!("移动 {arrow}"))
                .on_hover_text("移动到另一栏（F6）")
                .clicked()
            {
                self.other(true);
            }
            if ui.button("删除").clicked() {
                let v = self.selected();
                if !v.is_empty() {
                    self.dialog = Some(Dialog::Delete(v));
                }
            }
            if ui.button("全选").clicked() {
                let p = &mut self.panes[i];
                p.selection = p.items.iter().map(|row| row.path.clone()).collect();
            }
            if ui.button("取消全选").clicked() {
                self.panes[i].selection.clear();
            }
            ui.menu_button("排序", |ui| self.sort_menu(ui));
        });
        self.download_toolbar(ui);
    }
    fn top(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            ui.heading("TwinFile");
            ui.weak(self.s.theme.label());
        });
        self.file_actions(ui);
        ui.horizontal_wrapped(|ui| {
            if ui.button("搜索").clicked() {
                self.dialog = Some(Dialog::Search(String::new()));
            }
            ui.menu_button(format!("主题 · {}", self.s.theme.label()), |ui| {
                self.appearance(ui)
            });
            self.more(ui);
        });
    }

    fn miuix_more(&mut self, ui: &mut egui::Ui) {
        let r = ui
            .add_sized([33.0, 29.0], egui::Button::new(""))
            .on_hover_text("更多");
        r.widget_info(|| {
            egui::WidgetInfo::labeled(egui::WidgetType::Button, ui.is_enabled(), "更多")
        });
        for x in [-4.0, 0.0, 4.0] {
            ui.painter().circle_filled(
                r.rect.center() + egui::vec2(x, 0.0),
                1.15,
                ui.visuals().text_color(),
            );
        }
        if self.visual.is_some() {
            super::visual_smoke::menu_probe(ui.ctx(), "more", r.rect);
        }
        let height = (ui.ctx().content_rect().bottom() - r.rect.bottom() - 14.0).max(90.0);
        egui::Popup::menu(&r)
            .style(miuix_menu_style)
            .align(egui::RectAlign::BOTTOM_END)
            .gap(3.0)
            .width(MIUIX_MENU_WIDTH + 14.0)
            .close_behavior(egui::PopupCloseBehavior::CloseOnClickOutside)
            .show(|ui| {
                ui.set_width(MIUIX_MENU_WIDTH);
                ui.set_max_height(height);
                egui::ScrollArea::vertical()
                    .max_height(height)
                    .auto_shrink([false, true])
                    .show(ui, |ui| self.miuix_more_rows(ui));
            });
    }

    fn miuix_more_rows(&mut self, ui: &mut egui::Ui) {
        use MenuMark::{Arrow, Check};
        use egui::containers::menu::SubMenu;
        if miuix_menu_action(ui, "直链下载…") {
            self.dl.open = true;
        }
        miuix_menu_divider(ui);
        let settings = miuix_menu_row(ui, "设置", Arrow);
        if self.visual.is_some() {
            super::visual_smoke::menu_probe(ui.ctx(), "settings", settings.rect);
        }
        SubMenu::new().show(ui, &settings, |ui| {
            ui.set_width(192.0);
            if self.visual.is_some() {
                super::visual_smoke::menu_probe(ui.ctx(), "settings-open", ui.max_rect());
            }
            miuix_unavailable(ui, "软件更新…");
            if miuix_menu_action(ui, "外观设置…") {
                self.dialog = Some(Dialog::Appearance);
            }
            let tools = miuix_menu_row(ui, "扩展工具", Arrow);
            SubMenu::new().show(ui, &tools, |ui| self.miuix_extra_tools(ui));
        });
        miuix_menu_divider(ui);
        miuix_unavailable(ui, "管理插件");
        miuix_menu_divider(ui);
        if miuix_menu_action(ui, "交换左右栏") {
            self.swap_panes();
        }
        if miuix_menu_row(ui, "刷新当前栏", MenuMark::RefreshIcon).clicked() {
            ui.close();
            self.refresh();
        }
        if miuix_menu_action(ui, "新建文件") {
            self.new_item_kind(false);
        }
        if miuix_menu_action(ui, "新建文件夹") {
            self.new_item_kind(true);
        }
        if miuix_menu_action(ui, "比较文本（左右各选一项）") {
            self.open_compare_mode(true);
        }
        if miuix_menu_action(ui, "比较目录") {
            self.open_compare_mode(false);
        }
        for (label, mode) in [
            ("导入并验证校验清单", local_tools::HashMode::Import),
            ("粘贴校验值并验证所选文件", local_tools::HashMode::Paste),
            ("导出所选文件校验清单", local_tools::HashMode::Export),
        ] {
            if miuix_menu_action(ui, label) {
                if let Some(path) = self.one() {
                    if !path.is_file() {
                        self.status = "当前预览仅支持校验单个普通文件".into();
                    } else if let Some(dialog) = local_tools::HashDialog::open(path, mode) {
                        self.dialog = Some(Dialog::Hash(dialog));
                    }
                }
            }
        }
        let sorting = miuix_menu_row(ui, "排序", Arrow);
        if self.visual.is_some() && ui.clip_rect().contains(sorting.rect.center()) {
            super::visual_smoke::menu_probe(ui.ctx(), "sort", sorting.rect);
        }
        SubMenu::new().show(ui, &sorting, |ui| {
            ui.set_width(178.0);
            if self.visual.is_some() {
                super::visual_smoke::menu_probe(ui.ctx(), "sort-open", ui.max_rect());
            }
            let p = &mut self.panes[self.s.active];
            for (sort, label) in [
                (Sort::Name, "名称"),
                (Sort::Size, "大小"),
                (Sort::Modified, "修改时间"),
                (Sort::Extension, "类型"),
            ] {
                if (p.dir().is_some() || sort != Sort::Modified)
                    && miuix_menu_row(ui, label, Check(p.sort == sort)).clicked()
                {
                    p.sort = sort;
                    files::sort(&mut p.items, p.sort, p.desc);
                    ui.close();
                }
            }
            miuix_menu_divider(ui);
            if miuix_menu_row(ui, "降序", Check(p.desc)).clicked() {
                p.desc = !p.desc;
                files::sort(&mut p.items, p.sort, p.desc);
                ui.close();
            }
        });
        if miuix_menu_row(ui, "显示隐藏文件", Check(self.s.hidden)).clicked() {
            self.s.hidden = !self.s.hidden;
            for p in &mut self.panes {
                p.read(self.s.hidden);
            }
            ui.close();
        }
        if miuix_menu_action(ui, "收藏当前文件夹") {
            self.favorite_current();
        }
        let recent = miuix_menu_row(ui, "最近的位置", Arrow);
        SubMenu::new().show(ui, &recent, |ui| {
            ui.set_width(230.0);
            let mut go = None;
            if self.s.recent.is_empty() {
                ui.label("暂无最近访问的文件夹");
            }
            egui::ScrollArea::vertical()
                .max_height(265.0)
                .show(ui, |ui| {
                    for path in &self.s.recent {
                        let label = path.display().to_string();
                        let row = miuix_menu_row(ui, &label, MenuMark::Plain).on_hover_text(label);
                        if row.clicked() {
                            go = Some(path.clone());
                            ui.close();
                        }
                    }
                });
            if let Some(path) = go {
                self.navigate(Location::Local(path));
            }
        });
        miuix_menu_divider(ui);
        for (is_payload, label, key) in [
            (true, "本地 / 远程 Payload 分区提取", "payload-entry"),
            (false, "HFX USB + WiFi 多轨传输", "hfx-entry"),
        ] {
            let r = miuix_menu_row(ui, label, MenuMark::Plain);
            if self.visual.is_some() {
                if ui.clip_rect().contains_rect(r.rect) {
                    super::visual_smoke::menu_probe(ui.ctx(), key, r.rect);
                } else if std::env::var("TWINFILE_SMOKE_TOOL")
                    .ok()
                    .is_some_and(|t| key == format!("{t}-entry"))
                {
                    r.scroll_to_me(Some(egui::Align::Center));
                }
            }
            if r.clicked() {
                ui.close();
                if is_payload {
                    self.payload.open = true;
                } else {
                    self.hfx.open = true;
                }
            }
        }
    }

    fn miuix_extra_tools(&mut self, ui: &mut egui::Ui) {
        ui.set_width(225.0);
        if miuix_menu_action(ui, "复制到系统剪贴板") {
            self.clip_files(false);
        }
        if miuix_menu_action(ui, "剪切到系统剪贴板") {
            self.clip_files(true);
        }
        if miuix_menu_action(ui, "从系统剪贴板粘贴文件") {
            self.paste_files();
        }
        miuix_menu_divider(ui);
        if miuix_menu_action(ui, "创建 ZIP…") {
            self.make_zip();
        }
        if miuix_menu_action(ui, "批量重命名…") {
            self.batch();
        }
    }

    fn swap_panes(&mut self) {
        self.panes.swap(0, 1);
        self.s.active = 1 - self.s.active;
    }

    fn more(&mut self, ui: &mut egui::Ui) {
        ui.menu_button("更多", |ui| {
            if ui.button("交换左右栏").clicked() {
                self.swap_panes();
                ui.close();
            }
            if ui.button("本地 / 远程 Payload 分区提取").clicked() {
                self.payload.open = true;
                ui.close();
            }
            if ui.button("HFX USB + WiFi 多轨传输").clicked() {
                self.hfx.open = true;
                ui.close();
            }
            ui.separator();
            if ui.button("复制到系统剪贴板").clicked() {
                self.clip_files(false);
                ui.close();
            }
            if ui.button("剪切到系统剪贴板").clicked() {
                self.clip_files(true);
                ui.close();
            }
            if ui.button("从系统剪贴板粘贴文件").clicked() {
                self.paste_files();
                ui.close();
            }
            ui.separator();
            if ui.button("直链下载").clicked() {
                self.dl.open = true;
                ui.close();
            }
            if ui.button("外观设置").clicked() {
                self.dialog = Some(Dialog::Appearance);
                ui.close();
            }
            if ui
                .motion_check(&mut self.s.hidden, "显示隐藏文件")
                .changed()
            {
                for p in &mut self.panes {
                    p.read(self.s.hidden);
                }
            }
            if ui.button("校验所选…").clicked() {
                if let Some(p) = self.one() {
                    self.dialog = Some(Dialog::Hash(local_tools::HashDialog::new(p)));
                }
                ui.close();
            }
            if ui.button("比较左右栏…").clicked() {
                self.open_compare();
                ui.close();
            }
            if ui.button("创建 ZIP…").clicked() {
                self.make_zip();
                ui.close();
            }
            if ui.button("批量重命名…").clicked() {
                self.batch();
                ui.close();
            }
        });
    }
    fn side(&mut self, ui: &mut egui::Ui) {
        ui.heading("位置");
        let mut go = None;
        let mut remove = None;
        egui::ScrollArea::vertical().show(ui, |ui| {
            ui.weak("位置");
            if ui.button("此电脑").clicked() {
                go = Some(Location::Computer);
            }
            if let Some(profile) = std::env::var_os("USERPROFILE").map(PathBuf::from) {
                for (label, dir) in [
                    ("主目录", ""),
                    ("桌面", "Desktop"),
                    ("下载", "Downloads"),
                    ("文档", "Documents"),
                    ("图片", "Pictures"),
                    ("音乐", "Music"),
                    ("视频", "Videos"),
                ] {
                    let dir = profile.join(dir);
                    if dir.is_dir() && ui.button(label).clicked() {
                        go = Some(Location::Local(dir));
                    }
                }
            }
            ui.weak("设备与磁盘");
            for p in &self.drives {
                if ui.button(volume_name(p)).clicked() {
                    go = Some(Location::Local(p.root.clone()));
                }
            }
            ui.separator();
            ui.strong("收藏夹");
            if ui.button("收藏当前目录").clicked() {
                self.favorite_current();
            }
            for p in &self.s.favorites {
                let r = ui.button(name(p)).on_hover_text(p.display().to_string());
                if r.clicked() {
                    go = Some(Location::Local(p.clone()));
                }
                r.context_menu(|ui| {
                    if ui.button("移除收藏").clicked() {
                        remove = Some(p.clone());
                        ui.close();
                    }
                });
            }
            ui.separator();
            ui.collapsing("最近", |ui| {
                for p in &self.s.recent {
                    if ui.button(p.display().to_string()).clicked() {
                        go = Some(Location::Local(p.clone()));
                    }
                }
            });
        });
        if let Some(p) = remove {
            self.s.favorites.retain(|x| x != &p);
        }
        if let Some(p) = go {
            self.navigate(p);
        }
    }
    fn pane(&mut self, i: usize, ui: &mut egui::Ui, focus: f32) {
        let h = self.s.hidden;
        let mi = self.s.theme == Theme::Miuix;
        let soft = self.s.theme == Theme::Soft;
        let stage = self.s.theme == Theme::Keynote;
        let active = self.s.active == i;
        if self.visual.is_some()
            && active
            && self.panes[i]
                .dir()
                .is_some_and(|path| path.ends_with("下载资料"))
        {
            super::visual_smoke::menu_probe(ui.ctx(), "search-arrived", ui.max_rect());
        }
        let mut refresh_volumes = false;

        let p = &mut self.panes[i];

        // Register the panel hit region below its children. Rows, tabs, scrollbars and
        // text inputs keep their existing priority; uncovered space now activates its pane.
        let mut act = (mi || soft || stage)
            && ui
                .interact(
                    ui.max_rect(),
                    ui.make_persistent_id("miuix-pane-background"),
                    egui::Sense::click(),
                )
                .clicked();
        let mut go = None;
        let mut open = None;
        let mut context = None;
        let mut card = crate::theme::card(ui, self.s.theme);
        if mi {
            let c = mui::Colors::of(ui);
            let border = c
                .outline
                .gamma_multiply(0.62)
                .lerp_to_gamma(c.outline.lerp_to_gamma(c.primary, 0.36), focus);
            ui.painter().add(mui::squircle(
                ui.max_rect().shrink(0.5),
                17.0,
                c.container,
                egui::Stroke::new(1.0_f32, border),
            ));
            card.fill = egui::Color32::TRANSPARENT;
            card.stroke = egui::Stroke::NONE;
        }
        if soft {
            let hovered = ui.rect_contains_pointer(ui.max_rect());
            let v = if crate::motion::reduced(ui.ctx()) {
                if hovered { 1. } else { 0. }
            } else {
                ui.ctx()
                    .animate_bool_with_time(ui.id().with("soft-card-hover"), hovered, 0.5)
            };
            let pad = if crate::motion::reduced(ui.ctx()) {
                5
            } else {
                (7. - 4. * v).round() as i8
            };
            card = card
                .outer_margin(egui::Margin {
                    top: pad,
                    bottom: 12 - pad,
                    left: 2,
                    right: 2,
                })
                .shadow(crate::theme::soft::glow(
                    ui.visuals().selection.stroke.color,
                    (24. + 26. * v) as u8,
                    28,
                    6,
                ));
        }
        card.show(ui, |ui| {
            ui.set_min_width(ui.available_width());
            if stage {
                keynote::pane_head(ui, p, i, focus, h, &mut act, &mut go, &mut refresh_volumes);
            } else if soft {
                soft::pane_head(ui, p, i, focus, h, &mut act, &mut go, &mut refresh_volumes);
            } else if mi {
                ui.spacing_mut().interact_size.y = 24.0;
                ui.spacing_mut().button_padding = egui::vec2(4.0, 3.0);
                ui.spacing_mut().item_spacing.y = 4.0;
            } else {
                ui.horizontal(|ui| {
                    let dot = if active { "●" } else { "○" };
                    let ink = ui
                        .visuals()
                        .weak_text_color()
                        .lerp_to_gamma(ui.visuals().strong_text_color(), focus);
                    ui.label(
                        egui::RichText::new(format!(
                            "{dot} {}",
                            if i == 0 { "左栏" } else { "右栏" }
                        ))
                        .strong()
                        .color(ink),
                    );
                    ui.weak(format!("{} 项", p.items.len()));
                });
            }
            if mi {
                act |= miuix_tabs(ui, p, i, focus, h, self.visual.is_some());
                ui.horizontal(|ui| {
                    ui.spacing_mut().item_spacing.x = 4.0;
                    if mui::dock_item(ui, 25., "后退", false, |p, at, ink| {
                        draw_mark(p, at, Mark::Back, ink)
                    })
                    .clicked()
                    {
                        p.history(true, h);
                        act = true;
                    }
                    if mui::dock_item(ui, 25., "前进", false, |p, at, ink| {
                        draw_mark(p, at, Mark::Forward, ink)
                    })
                    .clicked()
                    {
                        p.history(false, h);
                        act = true;
                    }
                    if mui::dock_item(ui, 25., "刷新", false, |p, at, ink| {
                        draw_mark(p, at, Mark::Refresh, ink)
                    })
                    .clicked()
                    {
                        p.refresh_list(h);
                        refresh_volumes = p.dir().is_none();
                        act = true;
                    }
                    let width = ui.available_width();
                    egui::Frame::new()
                        .fill(miuix_control(ui))
                        .corner_radius(11)
                        .inner_margin(egui::Margin::symmetric(9, 0))
                        .show(ui, |ui| {
                            let edit = ui.add(
                                egui::TextEdit::singleline(&mut p.input)
                                    .id(egui::Id::new(("path", i)))
                                    .font(egui::FontId::proportional(12.0))
                                    .frame(false)
                                    .vertical_align(egui::Align::Center)
                                    .desired_width((width - 48.0).max(24.0))
                                    .min_size(egui::vec2(0., 27.)),
                            );
                            if edit.has_focus() {
                                act = true;
                                if ui.input(|inp| inp.key_pressed(Key::Escape)) {
                                    p.input = p.location().display();
                                    edit.surrender_focus();
                                }
                            }
                            if edit.lost_focus() && ui.input(|inp| inp.key_pressed(Key::Enter)) {
                                go = parse_location(&p.input);
                                act = true;
                            }
                            if ui
                                .add_sized([22.0, 26.0], egui::Button::new("×").frame(false))
                                .on_hover_text("清空地址")
                                .clicked()
                            {
                                p.input.clear();
                                edit.request_focus();
                                act = true;
                            }
                        });
                });
                ui.add_space(3.0);
            } else if !soft && !stage {
                let mut next = None;
                ui.horizontal(|ui| {
                    if ui.small_button("＋").clicked() {
                        p.tab(h);
                        act = true;
                    }
                    if ui.small_button("×").on_hover_text("关闭当前标签").clicked() {
                        p.close_tab(h);
                        act = true;
                    }
                    egui::ScrollArea::horizontal()
                        .id_salt(("tabs", i))
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
                });
                if let Some(k) = next {
                    p.index = k;
                    p.switch_motion(h, ListMotion::Tab);
                    act = true;
                }
                ui.horizontal(|ui| {
                    if ui.small_button("←").clicked() {
                        p.history(true, h);
                        act = true;
                    }
                    if ui.small_button("→").clicked() {
                        p.history(false, h);
                        act = true;
                    }
                    if ui.small_button("↑").clicked() {
                        go = p.parent();
                        act = true;
                    }
                    if refresh_button(ui, "刷新", true).clicked() {
                        p.refresh_list(h);
                        refresh_volumes = p.dir().is_none();
                        act = true;
                    }
                    let edit = ui.add(
                        egui::TextEdit::singleline(&mut p.input)
                            .id(egui::Id::new(("path", i)))
                            .desired_width(ui.available_width()),
                    );
                    if edit.has_focus() {
                        act = true;
                    }
                    if edit.lost_focus()
                        && ui.input(|in_| in_.key_pressed(Key::Enter))
                        && !p.input.trim().is_empty()
                    {
                        go = parse_location(&p.input);
                        act = true;
                    }
                });
            }
            if p.pending.is_some()
                || (p.dir().is_none() && self.drives.is_empty() && self.drive_scan.is_some())
            {
                ui.spinner();
            }
            let drives_loading = p.dir().is_none()
                && self.drive_scan.is_some()
                && (self.drives.is_empty()
                    || matches!(p.list_motion, Some((ListMotion::Refresh, _))));
            let paint_motion = p.list_paint(ui.ctx(), drives_loading);
            let row = crate::theme::row_height(self.s.theme);
            let height = (ui.available_height() - if mi { 24.0 } else { 19.0 }).max(24.0);
            let scroll = egui::ScrollArea::vertical()
                .id_salt(("items", i, p.location().clone()))
                .max_height(height)
                .auto_shrink([false, false]);
            let rendered = if p.dir().is_none() {
                let volumes = sorted_volumes(&self.drives, p.sort, p.desc);
                scroll.show_rows(ui, row, volumes.len(), |ui, rows| {
                    for &v in &volumes[rows] {
                        let (rect, response) = ui.allocate_exact_size(
                            egui::vec2(ui.available_width(), row),
                            egui::Sense::click(),
                        );
                        let mut paint = ui.painter().with_clip_rect(rect.intersect(ui.clip_rect()));
                        paint.multiply_opacity(paint_motion.opacity);
                        let rect = rect.translate(paint_motion.offset);
                        let (hover, press) = crate::motion::feedback(ui, &response);
                        if hover > 0.0 || press > 0.0 {
                            paint.rect_filled(
                                rect,
                                9,
                                ui.visuals()
                                    .widgets
                                    .hovered
                                    .weak_bg_fill
                                    .gamma_multiply(hover.max(press)),
                            );
                        }
                        let at = rect.left_center() + egui::vec2(22.0, 0.0);
                        if stage {
                            draw_mark(&paint, at, Mark::Disk, crate::theme::keynote::TEXT);
                        } else {
                            volume_icon(&paint, at, ui.visuals().dark_mode);
                        }
                        let label = if v.label.is_empty() {
                            format!("{} ({})", drive_letter(v), drive_letter(v))
                        } else {
                            format!("{} ({})", v.label, drive_letter(v))
                        };
                        let size = v.capacity.map_or_else(
                            || "容量不可用".into(),
                            |(free, total)| {
                                format!("可用 {} / 共 {}", volume_size(free), volume_size(total))
                            },
                        );
                        row_text(ui, &paint, rect, &label, &size);
                        if response.clicked() {
                            act = true;
                        }
                        if response.double_clicked() {
                            go = Some(Location::Local(v.root.clone()));
                        }
                        response.context_menu(|ui| {
                            if ui.button("打开磁盘").clicked() {
                                act = true;
                                go = Some(Location::Local(v.root.clone()));
                                ui.close();
                            }
                        });
                    }
                })
            } else {
                scroll.show_rows(ui, row, p.items.len(), |ui, range| {
                    for k in range {
                        let entry = &p.items[k];
                        let selected = p.selection.contains(&entry.path);
                        let (rect, r) = ui.allocate_exact_size(
                            egui::vec2(ui.available_width(), row),
                            egui::Sense::click(),
                        );
                        let mut painter =
                            ui.painter().with_clip_rect(rect.intersect(ui.clip_rect()));
                        painter.multiply_opacity(paint_motion.opacity);
                        let rect = rect.translate(paint_motion.offset);
                        let (hover, press) = crate::motion::feedback(ui, &r);
                        if selected || hover > 0.0 || press > 0.0 {
                            painter.rect_filled(
                                rect,
                                if stage {
                                    12
                                } else if soft {
                                    16
                                } else if mi {
                                    12
                                } else {
                                    5
                                },
                                if selected {
                                    ui.visuals().selection.bg_fill
                                } else {
                                    ui.visuals()
                                        .widgets
                                        .hovered
                                        .weak_bg_fill
                                        .gamma_multiply(hover.max(press))
                                },
                            );
                        }
                        let icon = egui::Rect::from_center_size(
                            rect.left_center() + egui::vec2(23.0, 0.0),
                            egui::vec2(30.0, 24.0),
                        );
                        let ppp = if stage {
                            ui.ctx().pixels_per_point().min(2.)
                        } else {
                            ui.ctx().pixels_per_point()
                        };
                        if let Some((id, pixels)) = self.thumbs.image(entry, ppp) {
                            let target = thumbs::icon_rect(icon.center(), pixels, ppp);
                            painter.image(
                                id,
                                target,
                                egui::Rect::from_min_max(egui::Pos2::ZERO, egui::pos2(1.0, 1.0)),
                                egui::Color32::WHITE,
                            );
                        } else if stage {
                            draw_mark(
                                &painter,
                                icon.center(),
                                if entry.dir {
                                    Mark::Folder
                                } else {
                                    Mark::Document
                                },
                                crate::theme::keynote::TEXT,
                            );
                        } else if soft {
                            let tint = ui.visuals().selection.stroke.color;
                            painter.circle_filled(
                                icon.center(),
                                14.,
                                ui.visuals().selection.bg_fill,
                            );
                            draw_mark(
                                &painter,
                                icon.center(),
                                if entry.dir {
                                    Mark::Folder
                                } else {
                                    Mark::Document
                                },
                                tint,
                            );
                        } else if mi {
                            miuix_file_icon(
                                &painter,
                                icon.center(),
                                entry.dir,
                                ui.visuals().dark_mode,
                            );
                        } else {
                            let gold = false;
                            painter.rect_filled(
                                icon,
                                5,
                                if gold {
                                    egui::Color32::from_rgb(239, 183, 47)
                                } else {
                                    ui.visuals().widgets.active.weak_bg_fill
                                },
                            );
                            painter.text(
                                icon.center(),
                                egui::Align2::CENTER_CENTER,
                                if entry.dir { "目录" } else { "文件" },
                                egui::FontId::proportional(10.0),
                                if gold {
                                    egui::Color32::from_rgb(57, 37, 9)
                                } else {
                                    ui.visuals().text_color()
                                },
                            );
                        }
                        if mi || soft || stage {
                            let mut meta = entry.modified.and_then(local_date).unwrap_or_default();
                            if !entry.dir {
                                if !meta.is_empty() {
                                    meta.push_str("  ·  ");
                                }
                                meta.push_str(&human(entry.size));
                            }
                            let mut text_rect = rect;
                            if selected {
                                text_rect.max.x -= 24.0;
                            }
                            row_text(ui, &painter, text_rect, &entry.name, &meta);
                        } else {
                            painter.text(
                                rect.left_center() + egui::vec2(46.0, 0.0),
                                egui::Align2::LEFT_CENTER,
                                &entry.name,
                                egui::FontId::proportional(13.0),
                                ui.visuals().text_color(),
                            );
                        }
                        if (mi || soft || stage) && selected {
                            paint_tick(
                                &painter,
                                rect.right_center() - egui::vec2(17.0, 0.0),
                                1.4,
                                ui.visuals().selection.stroke.color,
                            );
                        }
                        if r.clicked() {
                            act = true;
                            let m = ui.input(|i| i.modifiers);
                            if m.shift {
                                let a = p.anchor.unwrap_or(k).min(p.items.len() - 1);
                                if !m.ctrl {
                                    p.selection.clear();
                                }
                                for e in &p.items[a.min(k)..=a.max(k)] {
                                    p.selection.insert(e.path.clone());
                                }
                            } else if m.ctrl {
                                if !p.selection.remove(&entry.path) {
                                    p.selection.insert(entry.path.clone());
                                }
                                p.anchor = Some(k);
                            } else {
                                p.selection.clear();
                                p.selection.insert(entry.path.clone());
                                p.anchor = Some(k);
                            }
                        }
                        if r.double_clicked() {
                            act = true;
                            open = Some(entry.path.clone());
                        }
                        if r.secondary_clicked() {
                            act = true;
                            if !selected {
                                p.selection.clear();
                                p.selection.insert(entry.path.clone());
                            }
                        }
                        if r.secondary_clicked() {
                            p.anchor = Some(k);
                        }
                        if let Some(cmd) = file_menu::show(
                            &r,
                            ui,
                            i,
                            Some(&entry.path),
                            entry.dir,
                            p.selection.len(),
                            mi,
                        ) {
                            if p.selection.contains(&entry.path) {
                                context = Some((cmd, Some(entry.path.clone())));
                            }
                        }
                    }
                })
            };
            // Only the unoccupied list viewport is a background target. Never cover a row or scrollbar.
            if let Some(dir) = p.dir().map(Path::to_owned) {
                let mut blank = rendered.inner_rect;
                blank.min.y += (rendered.content_size.y - rendered.state.offset.y).max(0.);
                if blank.height() > 1. {
                    let r = ui.interact(
                        blank,
                        ui.id().with(("empty-files", i, &dir)),
                        egui::Sense::click(),
                    );
                    if r.secondary_clicked() || r.clicked() {
                        act = true;
                        p.selection.clear();
                        p.anchor = None;
                    }
                    if let Some(cmd) = file_menu::show(&r, ui, i, None, true, 0, mi) {
                        context = Some((cmd, None));
                    }
                }
            }
            if stage {
                let travel = (rendered.content_size.y - rendered.inner_rect.height()).max(0.);
                let progress = if travel > 0. {
                    (rendered.state.offset.y / travel).clamp(0., 1.)
                } else {
                    0.
                };
                let key = egui::Id::new(("keynote-scrub", i));
                let changed = ui.ctx().data_mut(|d| {
                    let prev = d.get_temp::<f32>(key);
                    d.insert_temp(key, progress);
                    prev != Some(progress)
                });
                if changed && active && !crate::motion::reduced(ui.ctx()) {
                    ui.ctx().request_repaint();
                }
                if p.pending.is_none() && p.dir().is_some() && p.items.is_empty() {
                    ui.painter().text(
                        rendered.inner_rect.center(),
                        egui::Align2::CENTER_CENTER,
                        "文件夹为空",
                        crate::theme::keynote::font(14.),
                        crate::theme::keynote::MUTED,
                    );
                }
            }
            if mi || stage {
                if mi {
                    miuix_rule(ui);
                }
                let n = if p.dir().is_none() {
                    self.drives.len()
                } else {
                    p.items.len()
                };
                let label = if p.selection.is_empty() {
                    format!("共 {n} 项")
                } else {
                    format!("已选择 {} / {n} 项", p.selection.len())
                };
                if ui
                    .add(
                        egui::Label::new(egui::RichText::new(label).weak().size(mui::META))
                            .sense(egui::Sense::click()),
                    )
                    .clicked()
                {
                    act = true;
                    p.selection.clear();
                }
            } else if ui
                .allocate_response(egui::vec2(ui.available_width(), 16.0), egui::Sense::click())
                .clicked()
            {
                act = true;
                p.selection.clear();
            }
        });
        if act && self.s.active != i {
            ui.ctx().request_repaint();
            self.s.active = i;
        }
        if refresh_volumes {
            self.refresh_volumes();
        }
        if let Some(p) = go {
            self.navigate(p);
        }
        if let Some(p) = open {
            self.open_local_item(p);
        }
        if let Some((cmd, target)) = context {
            self.s.active = i;
            self.run_file_command(cmd, target, ui.ctx());
        }
    }
    fn clip_files(&mut self, moving: bool) {
        let selection = self.selected();
        if selection.is_empty() {
            self.status = "请先选择文件".into();
            return;
        }
        self.status = match shell_clipboard::copy(&selection, moving) {
            Ok(()) if moving => "已剪切到系统剪贴板".into(),
            Ok(()) => "已复制到系统剪贴板".into(),
            Err(e) => format!("系统剪贴板：{e}"),
        };
    }
    fn paste_files(&mut self) {
        let Some(dest) = self.panes[self.s.active].dir().map(Path::to_owned) else {
            self.status = "请先打开粘贴目标文件夹".into();
            return;
        };
        match shell_clipboard::files() {
            Ok(Some((paths, moving))) => self.transfer(paths, dest, moving),
            Ok(None) => self.status = "系统剪贴板没有文件".into(),
            Err(e) => self.status = format!("系统剪贴板：{e}"),
        }
    }
    fn shortcut(&mut self, ctx: &egui::Context) {
        let native_paste = shell_clipboard::paste_key();
        if ctx.wants_keyboard_input() || self.dialog.is_some() || self.editor.is_some() {
            return;
        }
        let hit = |key, mods| ctx.input_mut(|i| i.consume_key(mods, key));
        let c = Modifiers::CTRL;
        let a = Modifiers::ALT;
        let n = Modifiers::NONE;
        let i = self.s.active;
        if hit(Key::F2, n) || hit(Key::R, c) {
            self.refresh();
        }
        if hit(Key::F5, n) {
            self.other(false);
        }
        if hit(Key::F6, n) {
            self.other(true);
        }
        if hit(Key::F7, n) {
            self.new_item();
        }
        if hit(Key::Escape, n) {
            self.panes[i].selection.clear();
        }
        if hit(Key::Delete, n) {
            let v = self.selected();
            if !v.is_empty() {
                self.dialog = Some(Dialog::Delete(v));
            }
        }
        if hit(Key::C, c) || ctx.input(|i| i.events.contains(&egui::Event::Copy)) {
            self.clip_files(false);
        }
        if hit(Key::X, c) || ctx.input(|i| i.events.contains(&egui::Event::Cut)) {
            self.clip_files(true);
        }
        if native_paste || hit(Key::V, c) {
            self.paste_files();
        }
        if hit(Key::A, c) {
            let p = &mut self.panes[i];
            p.selection = p.items.iter().map(|row| row.path.clone()).collect();
        }
        if hit(Key::M, c) {
            self.batch();
        }
        if hit(Key::F, c) {
            self.dialog = Some(Dialog::Search(String::new()));
        }
        if hit(Key::L, c) {
            ctx.memory_mut(|m| m.request_focus(egui::Id::new(("path", i))));
        }
        if hit(Key::ArrowLeft, a) {
            self.panes[i].history(true, self.s.hidden);
        }
        if hit(Key::ArrowRight, a) {
            self.panes[i].history(false, self.s.hidden);
        }
        if hit(Key::Backspace, n)
            && let Some(p) = self.panes[i].parent()
        {
            self.navigate(p);
        }
        if hit(Key::T, c) {
            self.panes[i].tab(self.s.hidden);
        }
        if hit(Key::W, c) {
            self.panes[i].close_tab(self.s.hidden);
        }
        for (modifier, delta) in [(c | Modifiers::SHIFT, -1i32), (c, 1i32)] {
            if hit(Key::Tab, modifier) {
                let p = &mut self.panes[i];
                p.index = (p.index as i32 + delta).rem_euclid(p.tabs.len() as i32) as usize;
                p.switch_motion(self.s.hidden, ListMotion::Tab);
            }
        }
        if hit(Key::Space, n) {
            let mut paths = self.selected();
            if paths.is_empty()
                && let Some(dir) = self.panes[i].dir()
            {
                paths.push(dir.to_owned());
            }
            if !paths.is_empty() {
                self.dialog = Some(Dialog::Properties(local_tools::Properties::new(paths)));
            }
        }
    }
}
fn app_icon(ctx: &egui::Context) -> Option<egui::TextureHandle> {
    ctx.data_mut(|data| data.get_temp::<egui::TextureHandle>(egui::Id::new("miuix-logo")))
        .or_else(|| {
            let img = image::load_from_memory(include_bytes!("../../assets/app.png"))
                .ok()?
                .to_rgba8();
            let tex = ctx.load_texture(
                "miuix-logo",
                egui::ColorImage::from_rgba_unmultiplied(
                    [img.width() as usize, img.height() as usize],
                    &img,
                ),
                egui::TextureOptions::LINEAR,
            );
            ctx.data_mut(|data| data.insert_temp(egui::Id::new("miuix-logo"), tex.clone()));
            Some(tex)
        })
}

fn parse_location(input: &str) -> Option<Location> {
    let raw = input.trim();
    if raw.is_empty() {
        None
    } else if raw == "此电脑" || raw.eq_ignore_ascii_case("This PC") {
        Some(Location::Computer)
    } else {
        Some(Location::Local(raw.into()))
    }
}

fn location_title(location: &Location) -> String {
    location.local().map_or_else(|| location.display(), name)
}

fn miuix_rule(ui: &mut egui::Ui) {
    let (rect, _) =
        ui.allocate_exact_size(egui::vec2(ui.available_width(), 1.0), egui::Sense::hover());
    ui.painter().hline(
        rect.x_range(),
        rect.center().y,
        egui::Stroke::new(1.0_f32, mui::Colors::of(ui).divider),
    );
}

fn miuix_control(ui: &egui::Ui) -> egui::Color32 {
    mui::Colors::of(ui).field
}

fn nav_button(ui: &mut egui::Ui, text: &str, tip: &str) -> bool {
    ui.add_sized(
        [23.0, 27.0],
        egui::Button::new(egui::RichText::new(text).size(17.0)).frame(false),
    )
    .on_hover_text(tip)
    .clicked()
}

fn miuix_tabs(
    ui: &mut egui::Ui,
    pane: &mut Pane,
    side: usize,
    emphasis: f32,
    hidden: bool,
    probe: bool,
) -> bool {
    let mut focus = false;
    let mut next = None;
    let mut close = None;
    let mut new = false;
    ui.horizontal(|ui| {
        ui.spacing_mut().item_spacing.x = 5.0;
        let slot = mui::dock_item_emphasis(
            ui,
            27.,
            if side == 0 {
                "切换到左栏"
            } else {
                "切换到右栏"
            },
            emphasis,
            |p, at, ink, _| {
                let rect = egui::Rect::from_center_size(at, egui::vec2(16., 12.));
                p.rect_stroke(
                    rect,
                    3,
                    egui::Stroke::new(1.25_f32, ink),
                    egui::StrokeKind::Inside,
                );
                let mut half = rect.shrink(2.5);
                if side == 0 {
                    half.max.x = at.x - 0.5;
                } else {
                    half.min.x = at.x + 0.5;
                }
                p.rect_filled(half, 1, ink);
            },
        );
        focus = slot.clicked();
        if probe {
            super::visual_smoke::switch_probe(
                ui.ctx(),
                "pane-switch-slot",
                side,
                slot.interact_rect,
            );
        }
        let width = (ui.available_width() - 29.0).max(24.0);
        ui.allocate_ui_with_layout(
            egui::vec2(width, 28.0),
            egui::Layout::left_to_right(egui::Align::Center),
            |ui| {
                egui::ScrollArea::horizontal()
                    .id_salt(("miuix-tabs", side))
                    .max_width(width)
                    .auto_shrink([false, true])
                    .scroll_bar_visibility(egui::scroll_area::ScrollBarVisibility::AlwaysHidden)
                    .show(ui, |ui| {
                        ui.spacing_mut().button_padding = egui::vec2(4.0, 3.0);
                        ui.spacing_mut().interact_size.y = 24.0;
                        let shape = ui.painter().add(egui::Shape::Noop);
                        let mut selected_rect = None;
                        ui.horizontal(|ui| {
                            for (j, tab) in pane.tabs.iter().enumerate() {
                                let selected = j == pane.index;
                                let frame = egui::Frame::new()
                                    .corner_radius(10)
                                    .inner_margin(egui::Margin::symmetric(4, 1))
                                    .show(ui, |ui| {
                                        ui.spacing_mut().item_spacing.x = 1.0;
                                        let title = location_title(&tab.location);
                                        let title = egui::RichText::new(title).size(13.0);
                                        let title = if selected {
                                            title.color(miuix_selected_text(ui)).strong()
                                        } else {
                                            title
                                        };
                                        let r = ui
                                            .add(egui::Button::new(title).frame(false).truncate())
                                            .on_hover_text(tab.location.display());
                                        if r.clicked() {
                                            next = Some(j);
                                        }
                                        if r.middle_clicked() {
                                            close = Some(j);
                                        }
                                        if ui
                                            .add_enabled(
                                                pane.tabs.len() > 1,
                                                egui::Button::new(
                                                    egui::RichText::new("×").size(16.0).weak(),
                                                )
                                                .frame(false),
                                            )
                                            .on_hover_text("关闭标签")
                                            .clicked()
                                        {
                                            close = Some(j);
                                        }
                                    });
                                if selected {
                                    selected_rect = Some(frame.response.rect);
                                }
                            }
                        });
                        let scrolled = ui.input(|i| i.smooth_scroll_delta.x != 0.0);
                        if let Some(rect) = crate::motion::highlight(
                            ui.ctx(),
                            ui.id().with("tab-pill"),
                            selected_rect,
                            0.20,
                            scrolled,
                        ) {
                            ui.painter().set(
                                shape,
                                egui::Shape::rect_filled(rect, 10, miuix_selected(ui)),
                            );
                        }
                    });
            },
        );
        new = nav_button(ui, "+", "新建标签");
    });
    if new {
        pane.tab(hidden);
    } else if let Some(j) = close {
        pane.close_at(j, hidden);
    } else if let Some(j) = next {
        pane.index = j;
        pane.switch_motion(hidden, ListMotion::Tab);
    }
    focus || new || close.is_some() || next.is_some()
}

fn miuix_selected(ui: &egui::Ui) -> egui::Color32 {
    mui::Colors::of(ui).selected
}

fn miuix_selected_text(ui: &egui::Ui) -> egui::Color32 {
    mui::Colors::of(ui).selected_ink
}

#[derive(Clone, Copy)]
enum Mark {
    Info,
    Cut,
    Paste,
    Archive,
    Screen,
    Download,
    Document,
    Image,
    Music,
    Video,
    Disk,
    Usb,
    Folder,
    Copy,
    Move,
    Trash,
    Checked,
    Check,
    Contrast,
    Back,
    Forward,
    Refresh,
    Appearance,
}
fn side_heading(ui: &mut egui::Ui, title: &str) -> egui::Response {
    ui.horizontal(|ui| {
        ui.add_space(10.0);
        ui.label(egui::RichText::new(title).size(11.5).weak())
    })
    .inner
}

fn side_row(
    ui: &mut egui::Ui,
    icon: Mark,
    title: &str,
    selected: bool,
    blue: bool,
) -> egui::Response {
    mark_button(ui, icon, title, title, selected, blue)
}

fn mark_button(
    ui: &mut egui::Ui,
    icon: Mark,
    title: &str,
    tip: &str,
    selected: bool,
    blue: bool,
) -> egui::Response {
    let side = matches!(
        icon,
        Mark::Screen
            | Mark::Download
            | Mark::Document
            | Mark::Image
            | Mark::Music
            | Mark::Video
            | Mark::Disk
            | Mark::Usb
            | Mark::Folder
    );
    let ink = if selected {
        miuix_selected_text(ui)
    } else {
        ui.visuals().text_color()
    };
    let icon_color = if blue && !selected {
        egui::Color32::from_rgb(18, 150, 239)
    } else {
        ink
    };
    let text = ui
        .painter()
        .layout_no_wrap(title.to_owned(), egui::FontId::proportional(13.0), ink);
    let width = if side {
        ui.available_width()
    } else if title.is_empty() {
        31.0
    } else {
        text.size().x + 29.0
    };
    let (rect, resp) = ui.allocate_exact_size(
        egui::vec2(width, if side { 28.0 } else { 33.0 }),
        egui::Sense::click(),
    );
    let paint = ui.painter().with_clip_rect(rect.intersect(ui.clip_rect()));
    let (hover, press) = crate::motion::feedback(ui, &resp);
    if !selected && (hover > 0.0 || press > 0.0) {
        let fill = ui
            .visuals()
            .widgets
            .hovered
            .weak_bg_fill
            .gamma_multiply((hover * 0.75 + press * 0.25).min(1.0));
        paint.rect_filled(rect.shrink(1.5 * press), 10, fill);
    }
    let x = if side {
        18.0
    } else if title.is_empty() {
        rect.width() / 2.0
    } else {
        13.0
    };
    draw_mark(
        &paint,
        rect.left_center() + egui::vec2(x, 0.0),
        icon,
        icon_color,
    );
    if !title.is_empty() {
        paint.galley(
            egui::pos2(
                rect.left() + x + 14.0,
                rect.center().y - text.size().y / 2.0,
            ),
            text,
            ink,
        );
    }
    resp.on_hover_text(tip)
}

// Two balanced clockwise sweeps. Both arrowheads meet their arcs and the endpoints
// are capped in the same stroke, rather than depending on a font's refresh glyph.
fn refresh_icon(p: &egui::Painter, at: egui::Pos2, ink: egui::Color32) {
    refresh_icon_rotated(p, at, ink, egui::emath::Rot2::IDENTITY);
}
fn refresh_icon_rotated(
    p: &egui::Painter,
    at: egui::Pos2,
    ink: egui::Color32,
    rotation: egui::emath::Rot2,
) {
    let stroke = egui::Stroke::new(1.65_f32, ink);
    for sign in [1.0, -1.0] {
        let point = |x: f32, y: f32| at + rotation * egui::vec2(x * sign, y * sign);
        let arc: Vec<_> = (0..=18)
            .map(|i| {
                let t = (197.0 + 133.0 * i as f32 / 18.).to_radians();
                point(6.1 * t.cos(), 6.1 * t.sin())
            })
            .collect();
        let tip = point(6.2, -1.65);
        let start = arc[0];
        let mut sweep = arc;
        sweep.push(tip);
        p.add(egui::Shape::line(sweep, stroke));
        let inner = point(2.65, -1.65);
        let outer = point(6.2, -5.2);
        p.add(egui::Shape::line(vec![inner, tip, outer], stroke));
        for cap in [start, inner, outer] {
            p.circle_filled(cap, stroke.width / 2., ink);
        }
    }
}

pub(super) fn refresh_button(ui: &mut egui::Ui, label: &str, compact: bool) -> egui::Response {
    let id = ui.next_auto_id().with("refresh-icon");
    let icon = egui::Atom::custom(id, egui::vec2(17., 17.));
    let button = if compact {
        egui::Button::new(icon).small()
    } else {
        egui::Button::new((icon, label))
    };
    let out = button.atom_ui(ui);
    if let Some(rect) = out.rect(id) {
        refresh_icon(
            ui.painter(),
            rect.center(),
            ui.style().interact(&out.response).fg_stroke.color,
        );
    }
    out.response.widget_info(|| {
        egui::WidgetInfo::labeled(egui::WidgetType::Button, ui.is_enabled(), label)
    });
    out.response.on_hover_text(label)
}

fn draw_mark(p: &egui::Painter, at: egui::Pos2, icon: Mark, color: egui::Color32) {
    draw_mark_phase(p, at, icon, color, None);
}
fn draw_mark_phase(
    p: &egui::Painter,
    at: egui::Pos2,
    icon: Mark,
    color: egui::Color32,
    phase: Option<f32>,
) {
    use egui::{Stroke, StrokeKind};
    use std::f32::consts::{PI, TAU};
    let pulse = |t: f32| (PI * t).sin().powi(2);
    let swing = |t: f32| 4.2 * (TAU * t).sin() * pulse(t).sqrt() * (1. - 0.7 * t);
    let at = match (icon, phase) {
        (Mark::Back, Some(t)) => at - egui::vec2(swing(t), 0.),
        (Mark::Forward, Some(t)) => at + egui::vec2(swing(t), 0.),
        _ => at,
    };
    let xy = |x, y| at + egui::vec2(x, y);
    let stroke = Stroke::new(1.25_f32, color);
    let line = |a, b, c, d| p.line_segment([xy(a, b), xy(c, d)], stroke);
    let rect = |a, b, c, d| {
        p.rect_stroke(
            egui::Rect::from_min_max(xy(a, b), xy(c, d)),
            1.5,
            stroke,
            StrokeKind::Inside,
        )
    };
    match icon {
        Mark::Info => {
            p.circle_stroke(at, 7., stroke);
            line(0., -1., 0., 4.);
            p.circle_filled(xy(0., -4.), 1., color);
        }
        Mark::Cut => {
            p.circle_stroke(xy(-4., 4.), 2.5, stroke);
            p.circle_stroke(xy(4., 4.), 2.5, stroke);
            line(-2., 2., 5., -7.);
            line(2., 2., -5., -7.);
        }
        Mark::Paste => {
            rect(-6., -5., 6., 7.);
            rect(-3., -7., 3., -3.);
            line(-3., 1., 3., 1.);
            line(-3., 4., 1., 4.);
        }
        Mark::Archive => {
            rect(-7., -5., 7., 7.);
            rect(-7., -7., 7., -3.);
            line(-2., 0., 2., 0.);
        }
        Mark::Screen => {
            rect(-7., -6., 7., 4.);
            line(0., 4., 0., 7.);
            line(-3., 7., 3., 7.);
        }
        Mark::Download => {
            // The tray stays still; only the arrow drops in and springs back.
            let dy = phase.map_or(0., |t| {
                if t < 0.58 {
                    3.2 * pulse(t / 0.58)
                } else {
                    -1.5 * pulse((t - 0.58) / 0.42)
                }
            });
            line(0., -7. + dy, 0., 3. + dy);
            line(-4., -1. + dy, 0., 3. + dy);
            line(0., 3. + dy, 4., -1. + dy);
            line(-7., 3., -7., 7.);
            line(-7., 7., 7., 7.);
            line(7., 7., 7., 3.);
        }
        Mark::Document => {
            rect(-5.5, -7., 5.5, 7.);
            line(-3., -2., 3., -2.);
            line(-3., 1., 3., 1.);
            line(-3., 4., 1., 4.);
        }
        Mark::Image => {
            rect(-7., -6., 7., 6.);
            line(-5., 4., -1., 0.);
            line(-1., 0., 2., 4.);
            p.circle_filled(xy(3., -2.), 1.2, color);
        }
        Mark::Music => {
            p.circle_filled(xy(-4., 4.), 2.4, color);
            p.circle_filled(xy(4., 2.), 2.4, color);
            line(-2., 4., -2., -5.);
            line(6., 2., 6., -7.);
            line(-2., -5., 6., -7.);
        }
        Mark::Video => {
            rect(-7., -6., 7., 6.);
            p.add(egui::Shape::convex_polygon(
                vec![xy(-2., -3.5), xy(-2., 3.5), xy(4., 0.)],
                color,
                Stroke::NONE,
            ));
        }
        Mark::Disk => {
            rect(-7., -5.5, 7., 5.5);
            line(-6., 1., 6., 1.);
            p.circle_filled(xy(4., 3.), 0.8, color);
        }
        Mark::Usb => {
            rect(-4., -3., 4., 7.);
            rect(-2.5, -7., 2.5, -3.);
        }
        Mark::Folder => {
            rect(-7., -4., 7., 6.);
            line(-6., -6., -1., -6.);
        }
        Mark::Copy => {
            rect(-7., -6., 2., 3.);
            rect(-2., -2., 7., 7.);
        }
        Mark::Move => {
            rect(-7., -5., 0., 5.);
            line(-2., 0., 8., 0.);
            line(5., -3., 8., 0.);
            line(5., 3., 8., 0.);
        }
        Mark::Trash => {
            rect(-4.5, -4., 4.5, 7.);
            line(-7., -4., 7., -4.);
            line(-2.5, -7., 2.5, -7.);
        }
        Mark::Checked | Mark::Check => {
            rect(-6.5, -6.5, 6.5, 6.5);
            if matches!(icon, Mark::Checked) {
                line(-4., 0., -1., 3.);
                line(-1., 3., 4., -3.);
            }
        }
        Mark::Back | Mark::Forward => {
            let d = if matches!(icon, Mark::Back) {
                -1.0
            } else {
                1.0
            };
            line(-7.0 * d, 0.0, 7.0 * d, 0.0);
            line(7.0 * d, 0.0, 2.0 * d, -5.0);
            line(7.0 * d, 0.0, 2.0 * d, 5.0);
        }
        Mark::Refresh => {
            if let Some(t) = phase {
                refresh_icon_rotated(
                    p,
                    at,
                    color,
                    egui::emath::Rot2::from_angle(TAU * t * t * (3. - 2. * t)),
                );
            } else {
                refresh_icon(p, at, color);
            }
        }
        Mark::Appearance => {
            let glow = phase.map_or(0., pulse);
            p.circle_stroke(at, 3.1 + 0.5 * glow, stroke);
            for i in 0..8 {
                let a = i as f32 * std::f32::consts::FRAC_PI_4
                    + phase.map_or(0., |t| std::f32::consts::FRAC_PI_4 * t * t * (3. - 2. * t));
                let inner = 5.4 + 0.5 * glow;
                let outer = 8. + 2. * glow;
                line(
                    a.cos() * inner,
                    a.sin() * inner,
                    a.cos() * outer,
                    a.sin() * outer,
                );
            }
        }
        Mark::Contrast => {
            p.circle_stroke(at, 6.2, stroke);
            let points = (0..=12)
                .map(|n| {
                    let a = -std::f32::consts::FRAC_PI_2 + (n as f32) * std::f32::consts::PI / 12.0;
                    xy(a.cos() * 6.0, a.sin() * 6.0)
                })
                .collect();
            p.add(egui::Shape::convex_polygon(points, color, Stroke::NONE));
        }
    }
}

fn sorted_volumes(drives: &[files::Volume], sort: Sort, descending: bool) -> Vec<&files::Volume> {
    let mut out: Vec<_> = drives.iter().collect();
    out.sort_by_cached_key(|v| {
        let label = if v.label.is_empty() {
            drive_letter(v)
        } else {
            v.label.to_lowercase()
        };
        let group = match sort {
            Sort::Size => v.capacity.map_or(0, |(_, total)| total),
            Sort::Extension => v.kind as u64,
            Sort::Modified | Sort::Name => 0,
        };
        (group, label, &v.root)
    });
    if descending {
        out.reverse();
    }
    out
}

fn drive_letter(v: &files::Volume) -> String {
    v.root.display().to_string().replace('\\', "/")
}

fn volume_name(v: &files::Volume) -> String {
    let ty = match v.kind {
        files::VolumeKind::Fixed => "本地磁盘",
        files::VolumeKind::Removable => "U盘",
        files::VolumeKind::Optical => "光驱",
        files::VolumeKind::RamDisk => "内存盘",
    };
    let label = if v.label.is_empty() { ty } else { &v.label };
    format!("{label} ({})", drive_letter(v).trim_end_matches('/'))
}

fn volume_size(n: u64) -> String {
    let gigs = n as f64 / 1_073_741_824.0;
    if gigs >= 1024.0 {
        format!("{:.1} TB", gigs / 1024.0)
    } else {
        format!("{gigs:.1} GB")
    }
}

// Selection is an icon, not a font character (some text fonts lack U+2713).
fn paint_tick(p: &egui::Painter, center: egui::Pos2, scale: f32, color: egui::Color32) {
    p.add(egui::Shape::line(
        [(-4.0, 0.0), (-1.0, 3.0), (5.0, -3.0)]
            .map(|(x, y)| center + egui::vec2(x, y) * scale)
            .to_vec(),
        egui::Stroke::new(1.5 * scale, color),
    ));
}

fn row_text(ui: &egui::Ui, paint: &egui::Painter, rect: egui::Rect, title: &str, subtitle: &str) {
    let left = rect.left() + 50.0;
    let paint = paint.with_clip_rect(
        paint
            .clip_rect()
            .intersect(rect.shrink2(egui::vec2(6.0, 0.0))),
    );
    for (dy, t, font, ink) in [
        (-10.0, title, 14.0, ui.visuals().text_color()),
        (9.0, subtitle, 10.5, ui.visuals().weak_text_color()),
    ] {
        paint.text(
            egui::pos2(left, rect.center().y + dy),
            egui::Align2::LEFT_CENTER,
            t,
            egui::FontId::proportional(font),
            ink,
        );
    }
}

pub(super) fn local_date(value: std::time::SystemTime) -> Option<String> {
    use windows_sys::Win32::{
        Foundation::{FILETIME, SYSTEMTIME},
        System::Time::{FileTimeToSystemTime, SystemTimeToTzSpecificLocalTime},
    };
    let duration = value.duration_since(std::time::UNIX_EPOCH).ok()?;
    let ticks = u64::try_from(duration.as_nanos() / 100)
        .ok()?
        .checked_add(116444736000000000)?;
    let ft = FILETIME {
        dwLowDateTime: ticks as u32,
        dwHighDateTime: (ticks >> 32) as u32,
    };
    let mut utc = SYSTEMTIME::default();
    let mut local = SYSTEMTIME::default();
    if unsafe { FileTimeToSystemTime(&ft, &mut utc) } == 0 {
        return None;
    }
    if unsafe { SystemTimeToTzSpecificLocalTime(std::ptr::null(), &utc, &mut local) } == 0 {
        local = utc;
    }
    Some(format!(
        "{:04}-{:02}-{:02} {:02}:{:02}",
        local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute
    ))
}

fn volume_icon(p: &egui::Painter, at: egui::Pos2, dark: bool) {
    use egui::{Color32 as C, Rect, Stroke, StrokeKind};
    let rect = Rect::from_center_size(at, egui::vec2(36.0, 36.0));
    p.rect(
        rect,
        9,
        C::from_gray(if dark { 48 } else { 239 }),
        Stroke::new(1.0_f32, C::from_gray(if dark { 63 } else { 222 })),
        StrokeKind::Inside,
    );
    p.rect_filled(
        Rect::from_center_size(at, egui::vec2(25.0, 19.0)),
        3,
        C::from_rgb(148, 164, 213),
    );
    p.rect_filled(
        Rect::from_center_size(at + egui::vec2(0., 5.5), egui::vec2(25., 8.)),
        2,
        C::from_rgb(100, 114, 173),
    );
    p.circle_filled(at + egui::vec2(8., 5.), 1., C::from_rgb(220, 238, 255));
}

fn miuix_file_icon(painter: &egui::Painter, at: egui::Pos2, dir: bool, dark: bool) {
    use egui::{Color32 as C, Rect};
    let bg = if dark {
        C::from_gray(48)
    } else {
        C::from_gray(243)
    };
    painter.rect(
        Rect::from_center_size(at, egui::vec2(36.0, 36.0)),
        9,
        bg,
        egui::Stroke::new(1.0_f32, C::from_gray(if dark { 63 } else { 223 })),
        egui::StrokeKind::Inside,
    );
    if dir {
        painter.rect_filled(
            Rect::from_min_size(at - egui::vec2(13.0, 10.0), egui::vec2(14.0, 10.0)),
            3,
            C::from_rgb(215, 146, 34),
        );
        painter.rect_filled(
            Rect::from_center_size(at + egui::vec2(0.0, 3.0), egui::vec2(27.0, 16.0)),
            3,
            C::from_rgb(255, 201, 75),
        );
    } else {
        painter.rect_filled(
            Rect::from_center_size(at, egui::vec2(19.0, 25.0)),
            3,
            C::from_rgb(146, 164, 231),
        );
        let stroke = egui::Stroke::new(1.0_f32, C::from_rgb(59, 73, 129));
        for (dy, len) in [(-3.0, 12.0), (1.0, 12.0), (5.0, 7.0)] {
            let x = at + egui::vec2(-6.0, dy);
            painter.line_segment([x, x + egui::vec2(len, 0.0)], stroke);
        }
    }
}
pub(super) fn system_open(p: &Path) -> io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::UI::{
        Shell::{SHELLEXECUTEINFOW, ShellExecuteExW},
        WindowsAndMessaging::SW_SHOWNORMAL,
    };
    let path = std::path::absolute(p)?;
    let wide = |p: &Path| {
        p.as_os_str()
            .encode_wide()
            .chain(Some(0))
            .collect::<Vec<_>>()
    };
    let arg = wide(&path);
    let folder = path.parent().map(wide);
    let mut request = SHELLEXECUTEINFOW {
        cbSize: std::mem::size_of::<SHELLEXECUTEINFOW>() as u32,
        lpFile: arg.as_ptr(),
        lpDirectory: folder.as_ref().map_or(std::ptr::null(), |d| d.as_ptr()),
        nShow: SW_SHOWNORMAL,
        // Leave lpVerb null: an association's default verb isn't always "open".
        ..Default::default()
    };
    if unsafe { ShellExecuteExW(&mut request) } == 0 {
        let err = io::Error::last_os_error();
        Err(io::Error::new(
            err.kind(),
            format!("无法打开 {}：{err}", p.display()),
        ))
    } else {
        Ok(())
    }
}

const MIUIX_MENU_WIDTH: f32 = 252.0;

#[derive(Clone, Copy)]
enum MenuMark {
    RefreshIcon,
    Plain,
    Arrow,
    Check(bool),
}

fn miuix_menu_style(style: &mut egui::Style) {
    let c = mui::Colors::new(style.visuals.dark_mode);
    style.spacing.menu_margin = egui::Margin::same(6);
    style.spacing.item_spacing.y = 0.0;
    style.visuals.menu_corner_radius = (mui::CARD_RADIUS as u8).into();
    style.visuals.window_fill = c.popup;
    style.visuals.window_stroke = egui::Stroke::new(1.0_f32, c.outline);
}

fn miuix_menu_divider(ui: &mut egui::Ui) {
    let (rect, _) =
        ui.allocate_exact_size(egui::vec2(ui.available_width(), 7.0), egui::Sense::hover());
    if crate::theme::soft::active(ui.ctx()) {
        return;
    }
    let color = mui::Colors::of(ui).divider;
    ui.painter().hline(
        (rect.left() + 8.0)..=(rect.right() - 8.0),
        rect.center().y,
        egui::Stroke::new(1.0_f32, color),
    );
}

fn miuix_menu_row(ui: &mut egui::Ui, label: &str, mark: MenuMark) -> egui::Response {
    if crate::theme::soft::active(ui.ctx()) {
        let tint = ui.visuals().selection.stroke.color;
        return crate::theme::soft::nav(
            ui,
            label,
            label,
            matches!(mark, MenuMark::Check(true)),
            tint,
            |p, at, c| match mark {
                MenuMark::Arrow => draw_mark(p, at, Mark::Forward, c),
                MenuMark::Check(true) => draw_mark(p, at, Mark::Check, c),
                _ => {
                    p.circle_filled(at, 2.5, c);
                }
            },
        );
    }
    let (rect, r) =
        ui.allocate_exact_size(egui::vec2(ui.available_width(), 34.0), egui::Sense::click());
    r.widget_info(|| match mark {
        MenuMark::Check(checked) => {
            egui::WidgetInfo::selected(egui::WidgetType::Checkbox, ui.is_enabled(), checked, label)
        }
        _ => egui::WidgetInfo::labeled(egui::WidgetType::Button, ui.is_enabled(), label),
    });
    if ui.is_rect_visible(rect) {
        let p = ui.painter();
        let v = ui.visuals();
        if r.hovered() || r.has_focus() {
            p.rect_filled(rect.shrink(1.0), 10, v.selection.bg_fill);
        }
        let c = mui::Colors::of(ui);
        let color = if !ui.is_enabled() {
            c.disabled
        } else {
            c.on_surface
        };
        let y = rect.center().y;
        p.text(
            egui::pos2(rect.left() + 38.0, y),
            egui::Align2::LEFT_CENTER,
            label,
            egui::FontId::proportional(13.0),
            color,
        );
        match mark {
            MenuMark::RefreshIcon => refresh_icon(p, egui::pos2(rect.left() + 15., y), color),
            MenuMark::Arrow => {
                let x = rect.right() - 7.0;
                p.add(egui::Shape::convex_polygon(
                    vec![
                        egui::pos2(x, y - 4.0),
                        egui::pos2(x + 4.0, y),
                        egui::pos2(x, y + 4.0),
                    ],
                    v.weak_text_color(),
                    egui::Stroke::NONE,
                ));
            }
            MenuMark::Check(true) => {
                paint_tick(p, egui::pos2(rect.left() + 15.0, y), 1.0, color);
            }
            _ => {}
        }
    }
    r
}

fn miuix_menu_action(ui: &mut egui::Ui, label: &str) -> bool {
    let clicked = miuix_menu_row(ui, label, MenuMark::Plain).clicked();
    if clicked {
        ui.close();
    }
    clicked
}

fn miuix_unavailable(ui: &mut egui::Ui, label: &str) {
    ui.add_enabled_ui(false, |ui| miuix_menu_row(ui, label, MenuMark::Plain))
        .inner
        .on_disabled_hover_text("Rust 预览版尚未迁移");
}

#[cfg(test)]
mod dock_hover_tests {
    use super::*;
    // Compare the real per-icon vector shapes; ignore the hover disc, tooltip and labels.
    fn glyph(out: &egui::FullOutput, rect: egui::Rect) -> Vec<egui::Shape> {
        let bounds = egui::Rect::from_center_size(rect.center(), egui::vec2(30., 30.));
        out.shapes
            .iter()
            .map(|s| &s.shape)
            .filter(|s| {
                matches!(
                    s,
                    egui::Shape::LineSegment { .. } | egui::Shape::Path(_) | egui::Shape::Circle(_)
                ) && bounds.contains_rect(s.visual_bounding_rect())
            })
            .cloned()
            .collect()
    }
    #[test]
    fn each_floating_dock_icon_plays_once_rearms_without_moving_hitbox() {
        let ctx = egui::Context::default();
        let tmp = tempfile::tempdir().unwrap();
        let cc = eframe::CreationContext::_new_kittest(ctx.clone());
        let mut app = App::new(&cc, false, Some(tmp.path().into()), false, None);
        app.s.theme = Theme::Miuix;
        app.s.reduce_motion = false;
        crate::theme::apply(&ctx, &app.s);
        let frame = |app: &mut App, time: f64, p: egui::Pos2| {
            ctx.run(
                egui::RawInput {
                    time: Some(time),
                    screen_rect: Some(egui::Rect::from_min_size(
                        egui::Pos2::ZERO,
                        egui::vec2(760., 560.),
                    )),
                    events: vec![egui::Event::PointerMoved(p)],
                    ..Default::default()
                },
                |ctx| app.workbench(ctx),
            )
        };
        let sample = |k: i32| {
            ctx.data(|d| {
                d.get_temp::<(egui::Rect, Option<f32>)>(egui::Id::new(("dock-hover-test", k)))
            })
            .unwrap()
        };
        let outside = egui::pos2(1., 1.);
        let mut time = 0.;
        for _ in 0..4 {
            frame(&mut app, time, outside);
            time += 0.04;
        }
        for k in 0..5 {
            let rest = frame(&mut app, time, outside);
            let (r, idle) = sample(k);
            assert_eq!(idle, None);
            let static_glyph = glyph(&rest, r);
            assert!(!static_glyph.is_empty(), "missing icon {k}");
            frame(&mut app, time + 0.01, r.center());
            assert_eq!(sample(k), (r, Some(0.)));
            let mid = frame(&mut app, time + 0.12, r.center());
            let (rm, t) = sample(k);
            assert_eq!(rm, r);
            assert!(t.is_some_and(|v| v > 0. && v < 1.));
            assert_ne!(glyph(&mid, r), static_glyph, "static glyph {k}");
            let end = frame(&mut app, time + 0.78, r.center());
            assert_eq!(sample(k), (r, None));
            assert_eq!(glyph(&end, r), static_glyph);
            frame(&mut app, time + 0.82, r.center() + egui::vec2(2., 0.));
            assert_eq!(sample(k), (r, None));
            frame(&mut app, time + 0.86, outside);
            frame(&mut app, time + 0.90, r.center());
            assert_eq!(sample(k), (r, Some(0.)));
            time += 1.55;
        }
        // A direct sweep starts the next glyph even while the previous one finishes.
        let a = sample(0).0.center();
        let b = sample(1).0.center();
        frame(&mut app, time, a);
        frame(&mut app, time + 0.04, b);
        assert!(sample(0).1.unwrap() > 0.);
        assert_eq!(sample(1).1, Some(0.));
        app.s.reduce_motion = true;
        crate::theme::apply(&ctx, &app.s);
        frame(&mut app, time + 0.08, a);
        assert_eq!(sample(0).1, None);
        assert_eq!(sample(1).1, None);
    }
}

#[cfg(test)]
mod pane_focus_tests {
    use super::*;

    #[test]
    fn clicks_crossfade_reverse_and_settle_without_moving_either_pane() {
        for width in [1180., 760.] {
            let ctx = egui::Context::default();
            let tmp = tempfile::tempdir().unwrap();
            let cc = eframe::CreationContext::_new_kittest(ctx.clone());
            let mut app = App::new(&cc, false, Some(tmp.path().into()), true, None);
            app.s.theme = Theme::Miuix;
            app.s.reduce_motion = false;
            app.s.active = 0;
            crate::theme::apply(&ctx, &app.s);
            let mut time = 0.;
            let mut frame = |app: &mut App, events| {
                time += 0.016;
                ctx.run(
                    egui::RawInput {
                        time: Some(time),
                        events,
                        screen_rect: Some(egui::Rect::from_min_size(
                            egui::Pos2::ZERO,
                            egui::vec2(width, 560.),
                        )),
                        ..Default::default()
                    },
                    |ctx| app.workbench(ctx),
                )
            };
            let probe = |source: &str, i: usize| {
                ctx.data(|d| d.get_temp::<(u64, egui::Rect)>(egui::Id::new((source, i))))
                    .unwrap()
                    .1
            };
            for _ in 0..4 {
                frame(&mut app, vec![]);
            }
            let slots = [probe("pane-switch-slot", 0), probe("pane-switch-slot", 1)];
            let ink = |out: &egui::FullOutput| {
                slots.map(|r| {
                    out.shapes
                        .iter()
                        .find_map(|s| match &s.shape {
                            egui::Shape::Rect(rect)
                                if r.contains_rect(rect.rect)
                                    && (rect.rect.width() - 5.).abs() < 0.01
                                    && (rect.rect.height() - 7.).abs() < 0.01 =>
                            {
                                Some(rect.fill)
                            }
                            _ => None,
                        })
                        .expect("missing painted pane glyph")
                })
            };
            let mut step = |app: &mut App, events| ink(&frame(app, events));
            let click = |pos: egui::Pos2, pressed| {
                vec![
                    egui::Event::PointerMoved(pos),
                    egui::Event::PointerButton {
                        pos,
                        button: egui::PointerButton::Primary,
                        pressed,
                        modifiers: egui::Modifiers::NONE,
                    },
                ]
            };
            let start = step(&mut app, vec![]);
            assert_ne!(start[0], start[1]);
            let end = [start[1], start[0]];
            let dirs = app.panes.each_ref().map(|p| p.location().clone());
            for p in &mut app.panes {
                p.selection.insert(tmp.path().join("selected"));
            }
            let selection = app.panes.each_ref().map(|p| p.selection.clone());
            step(&mut app, click(slots[1].center(), true));
            step(&mut app, click(slots[1].center(), false));
            assert_eq!(app.s.active, 1);
            for _ in 0..4 {
                step(&mut app, vec![]);
            }
            let mid = step(&mut app, vec![]);
            for i in 0..2 {
                assert_ne!(mid[i], start[i]);
                assert_ne!(mid[i], end[i]);
            }
            step(&mut app, click(slots[0].center(), true));
            step(&mut app, click(slots[0].center(), false));
            assert_eq!(app.s.active, 0);
            let reverse = step(&mut app, vec![]);
            assert_ne!(reverse, start);
            assert_ne!(reverse, end);
            for _ in 0..20 {
                step(&mut app, vec![]);
            }
            assert_eq!(step(&mut app, vec![]), start);
            step(&mut app, click(slots[0].center(), true));
            step(&mut app, click(slots[0].center(), false));
            assert_eq!(step(&mut app, vec![]), start);
            assert_eq!(dirs, app.panes.each_ref().map(|p| p.location().clone()));
            assert_eq!(selection, app.panes.each_ref().map(|p| p.selection.clone()));
            let blank = probe("pane-blank", 1).center();
            step(&mut app, click(blank, true));
            step(&mut app, click(blank, false));
            assert_eq!(app.s.active, 1);
            for _ in 0..5 {
                step(&mut app, vec![]);
            }
            assert_ne!(step(&mut app, vec![]), end);
            app.s.reduce_motion = true;
            crate::theme::apply(&ctx, &app.s);
            assert_eq!(step(&mut app, vec![]), end);
            step(&mut app, click(slots[0].center(), true));
            step(&mut app, click(slots[0].center(), false));
            assert_eq!(step(&mut app, vec![]), start);
            assert_eq!(
                slots,
                [probe("pane-switch-slot", 0), probe("pane-switch-slot", 1)]
            );
        }
    }
}
