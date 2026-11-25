use super::*;
use crate::motion::ChoiceUi as _;
use crate::theme::miuix as mui;
impl Downloader {
    fn filename(&self) -> Option<String> {
        self.suggested_name.clone().or_else(|| {
            download::url(self.url.trim())
                .ok()
                .and_then(|u| download::url_filename(&u))
        })
    }
    fn ready(&self) -> bool {
        self.usable_dir.is_some() || self.dest.is_some()
    }
    fn update_target(&mut self) {
        if self.work.is_some() {
            return;
        }
        if self.bound_url != self.url {
            // A manual choice before typing the first link remains valid. A new link after that gets its own name.
            if !self.bound_url.trim().is_empty() {
                self.dest = None;
                self.manual_name = false;
            }
            self.suggested_name = None;
            self.bound_url.clone_from(&self.url);
        }
        if self.dest.is_none()
            && !self.url.trim().is_empty()
            && let Some(dir) = &self.usable_dir
            && let Some(name) = self.filename()
        {
            self.dest = Some(dir.join(name));
        }
    }
}
impl App {
    /// Open a local filesystem item with the same Windows association that
    /// Explorer uses for a double-click. Directories stay inside TwinFile so
    /// the active pane remains navigable; every file is handed to ShellExecute
    /// without an extension allowlist.
    pub(super) fn open_local_item(&mut self, p: PathBuf) {
        if p.is_dir() {
            self.goto(p);
            return;
        }
        match super::workbench::system_open(&p) {
            Ok(()) => self.status.clear(),
            Err(e) => self.status = e.to_string(),
        }
    }

    pub(super) fn open_item(&mut self, p: PathBuf, ctx: &egui::Context) {
        if p.is_dir() {
            self.goto(p);
            return;
        }
        let ext = p
            .extension()
            .and_then(|s| s.to_str())
            .unwrap_or("")
            .to_ascii_lowercase();
        let n = name(&p).to_ascii_lowercase();
        match ext.as_str() {
            "zip" | "apk" => self.browse_archive(p),
            "png" | "jpg" | "jpeg" | "gif" | "webp" | "bmp" | "ico" => {
                match Preview::open(ctx, p) {
                    Some(view) => self.preview = Some(view),
                    None => self.status = "无法打开图片或超过预览限制".into(),
                }
            }
            "txt" | "md" | "ini" | "json" | "yaml" | "xml" | "csv" | "toml" | "log" | "rs"
            | "js" | "html" | "css" | "java" | "kt" | "cpp" | "h" | "py" | "bat" | "cmd" => {
                self.open_text(p)
            }
            _ if n
                .rsplit_once(".apk.")
                .is_some_and(|(_, s)| !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit())) =>
            {
                self.browse_archive(p)
            }
            _ => {
                self.status = "使用右键菜单 → 系统打开（执行文件前请确认来源）".into();
            }
        }
    }
    pub(super) fn open_text(&mut self, p: PathBuf) {
        if self.editor.as_ref().is_some_and(|e| e.unsaved()) {
            self.status = "请先保存或关闭当前编辑器".into();
            return;
        }
        match text::Document::open(&p, None) {
            Ok(doc) => {
                self.editor = Some(Editor {
                    path: p,
                    archive_unsaved: false,
                    publish: None,
                    staged: None,
                    doc,
                    find: String::new(),
                    replace: String::new(),
                    regex: false,
                    closing: false,
                })
            }
            Err(e) => self.status = e.to_string(),
        }
    }
    pub(super) fn make_zip(&mut self) {
        let files = self.selected();
        if files.is_empty() {
            self.status = "请先选择待压缩文件".into();
            return;
        }
        if let Some(dst) = rfd::FileDialog::new()
            .set_file_name("archive.zip")
            .add_filter("ZIP", &["zip"])
            .save_file()
        {
            self.start_job(true, move |c| {
                archive::create(&files, &dst, c)
                    .map(|_| format!("已创建 {}", dst.display()))
                    .map_err(|e| e.to_string())
            });
        }
    }

    fn start_name_search(&mut self, query: &str) -> bool {
        if self.search.is_some() || query.trim().is_empty() {
            return false;
        }
        let Some(path) = self.panes[self.s.active].dir().map(Path::to_owned) else {
            self.status = "请先打开要搜索的磁盘或文件夹".into();
            return false;
        };
        let query = query.trim().to_lowercase();
        let hidden = self.s.hidden;
        let (tx, rx) = mpsc::channel();
        std::thread::spawn(move || {
            let _ = tx.send(search_dir(path, &query, hidden).map_err(|e| e.to_string()));
        });
        self.search = Some(rx);
        self.status = "正在搜索…".into();
        true
    }

    fn visit_search_result(&mut self, path: &Path) {
        self.goto(if path.is_dir() {
            path.to_owned()
        } else {
            path.parent().unwrap_or(path).to_owned()
        });
    }

    fn miuix_search_overlay(&mut self, ctx: &egui::Context, mut dialog: Dialog) {
        use egui::{Color32, Id, RichText, Stroke};
        let search = matches!(dialog, Dialog::Search(_));
        let c = mui::Colors::new(self.s.dark);
        let width = (ctx.content_rect().width() - 94.0).clamp(240.0, 414.0);
        let modal_id = Id::new(if search {
            "miuix-search"
        } else {
            "miuix-search-results"
        });
        let field_id = modal_id.with("field");
        let initialized = field_id.with("initialized");
        let mut acted = false;
        let dismissed = mui::file_sheet(
            ctx,
            modal_id,
            if search {
                "搜索文件"
            } else {
                "搜索结果"
            },
            width,
            mui::SheetBody::Scrollable,
            |ui| {
                let mut close = false;
                match &mut dialog {
                    Dialog::Search(q) => {
                        let active = &self.panes[self.s.active];
                        let folder = active.dir().is_some();
                        let scope = if folder {
                            format!(
                                "{}：{}",
                                if self.s.active == 0 {
                                    "左栏"
                                } else {
                                    "右栏"
                                },
                                active.location().display()
                            )
                        } else {
                            "先打开一个磁盘或文件夹，再搜索文件名。".into()
                        };
                        ui.add(
                            egui::Label::new(RichText::new(&scope).size(mui::DIALOG_NOTE).weak())
                                .truncate(),
                        )
                        .on_hover_text(scope);
                        ui.add_space(7.0);
                        let outline = ctx.memory(|mem| mem.has_focus(field_id));
                        let response = egui::Frame::new()
                            .fill(c.field)
                            .corner_radius(13)
                            .stroke(Stroke::new(
                                1.5_f32,
                                if outline {
                                    ui.visuals().selection.stroke.color
                                } else {
                                    Color32::TRANSPARENT
                                },
                            ))
                            .inner_margin(egui::Margin::symmetric(10, 3))
                            .show(ui, |ui| {
                                ui.horizontal(|ui| {
                                    let (icon, _) = ui.allocate_exact_size(
                                        egui::vec2(20.0, 25.0),
                                        egui::Sense::hover(),
                                    );
                                    mui::search_icon(ui.painter(), icon.center(), c.field_ink);
                                    ui.add(
                                        egui::TextEdit::singleline(q)
                                            .id(field_id)
                                            .hint_text("输入文件名")
                                            .font(mui::font(mui::DIALOG_BODY))
                                            .frame(false)
                                            .vertical_align(egui::Align::Center)
                                            .desired_width(ui.available_width())
                                            .min_size(egui::vec2(0.0, 25.0)),
                                    )
                                })
                                .inner
                            })
                            .inner;
                        if !ui.is_sizing_pass()
                            && response.enabled()
                            && !ctx.data(|d| d.get_temp::<bool>(initialized).unwrap_or(false))
                        {
                            response.request_focus();
                            ctx.data_mut(|d| d.insert_temp(initialized, true));
                        }
                        if self.visual.is_some() {
                            super::visual_smoke::menu_probe(ctx, "search-field", response.rect);
                            if response.has_focus() {
                                super::visual_smoke::menu_probe(
                                    ctx,
                                    "search-focused",
                                    response.rect,
                                );
                            }
                        }
                        ui.add_space(4.0);
                        ui.label(
                            RichText::new(if self.search.is_some() {
                                "正在进行另一项搜索…"
                            } else {
                                "按文件名匹配，包含子文件夹。"
                            })
                            .size(mui::DIALOG_NOTE)
                            .weak(),
                        );
                        ui.add_space(7.0);
                        let enabled = folder && !q.trim().is_empty() && self.search.is_none();
                        let enter =
                            response.lost_focus() && ui.input(|i| i.key_pressed(Key::Enter));
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            let submit = ui
                                .add_enabled_ui(enabled, |ui| mui::overlay_button(ui, "搜索", true))
                                .inner;
                            let cancel = mui::overlay_button(ui, "取消", false);
                            if (submit.clicked() || (enter && enabled)) && self.start_name_search(q)
                            {
                                close = true;
                            }
                            close |= cancel.clicked();
                        });
                    }
                    Dialog::Results(rows) => {
                        ui.label(
                            RichText::new(if rows.is_empty() {
                                "没有找到符合条件的文件或文件夹。".to_owned()
                            } else {
                                format!(
                                    "找到 {} 项 · 点击项目打开所在位置（最多显示 10000 项）",
                                    rows.len()
                                )
                            })
                            .size(mui::DIALOG_NOTE)
                            .weak(),
                        );
                        ui.add_space(10.0);
                        let mut destination = None;
                        egui::ScrollArea::vertical()
                            .max_height((ctx.content_rect().height() - 224.0).clamp(80.0, 350.0))
                            .auto_shrink([false, true])
                            .show_rows(ui, 32.0, rows.len(), |ui, range| {
                                for i in range {
                                    let p = &rows[i];
                                    let caption = format!("{}    ·    {}", name(p), p.display());
                                    let r = ui
                                        .add_sized(
                                            [ui.available_width(), 32.0],
                                            egui::Button::new(
                                                RichText::new(&caption).size(mui::DIALOG_BODY),
                                            )
                                            .truncate()
                                            .fill(c.field)
                                            .corner_radius(10),
                                        )
                                        .on_hover_text(p.display().to_string());
                                    if self.visual.is_some() && i == 0 {
                                        super::visual_smoke::menu_probe(
                                            ctx,
                                            "search-result-first",
                                            r.rect,
                                        );
                                    }
                                    if r.clicked() {
                                        destination = Some(p.clone());
                                    }
                                }
                            });
                        if let Some(path) = destination {
                            self.visit_search_result(&path);
                            close = true;
                        }
                        ui.add_space(10.0);
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            close |= mui::overlay_button(ui, "关闭", false).clicked();
                        });
                    }
                    _ => unreachable!("search overlay only renders search dialogs"),
                }
                acted = close;
            },
        );
        if dismissed || acted {
            ctx.data_mut(|data| data.remove::<bool>(initialized));
        } else {
            self.dialog = Some(dialog);
        }
    }
    pub(super) fn dialogs(&mut self, ctx: &egui::Context) {
        let Some(mut d) = self.dialog.take() else {
            return;
        };
        if self.s.theme == Theme::Miuix && matches!(d, Dialog::Search(_) | Dialog::Results(_)) {
            self.miuix_search_overlay(ctx, d);
            return;
        }
        let title = match &d {
            Dialog::New(..) => "新建",
            Dialog::Rename(..) => "重命名",
            Dialog::Delete(..) => "移至回收站",
            Dialog::Properties(..) => "属性",
            Dialog::Hash(state) => state.title(),
            Dialog::Batch(..) => "批量重命名",

            Dialog::Search(..) => "文件名搜索",
            Dialog::Results(..) => "搜索结果",
            Dialog::Appearance => "外观设置",
        };
        let mi = self.s.theme == Theme::Miuix;
        let width = match &d {
            Dialog::Batch(..) => 520.,
            Dialog::Hash(..) => 450.,
            Dialog::Properties(..) => 346.,
            _ => 320.,
        };
        let sheet_id = egui::Id::new(("miuix-sheet", title));
        let short = matches!(d, Dialog::New(..) | Dialog::Rename(..));
        let field_id = sheet_id.with("name");
        let mut keep = true;
        let err_key = sheet_id.with("feedback");
        let mut body = |ui: &mut egui::Ui| {
            match &mut d {
                Dialog::New(dir, n) => {
                    let submitted = if mi {
                        mui::file_kind(ui, dir);
                        name_field(
                            ui,
                            n,
                            field_id,
                            if *dir {
                                "文件夹名称"
                            } else {
                                "文件名称"
                            },
                        )
                    } else {
                        ui.motion_check(dir, "文件夹");
                        ui.text_edit_singleline(n);
                        false
                    };
                    if action(ui, mi, "创建", !n.trim().is_empty(), &mut keep) || submitted {
                        let Some(p) = self.panes[self.s.active].dir() else {
                            self.status = "请先打开文件夹".into();
                            if mi {
                                ctx.data_mut(|d| d.insert_temp(err_key, self.status.clone()));
                            }
                            return;
                        };
                        let result = files::safe_name(n).and_then(|n| {
                            if *dir {
                                fs::create_dir(p.join(n))
                            } else {
                                fs::File::create_new(p.join(n)).map(drop)
                            }
                        });
                        match result {
                            Ok(()) => {
                                keep = false;
                                self.refresh();
                            }
                            Err(e) => {
                                self.status = e.to_string();
                                if mi {
                                    ctx.data_mut(|d| d.insert_temp(err_key, self.status.clone()));
                                }
                            }
                        }
                    }
                }
                Dialog::Rename(p, n) => {
                    let submitted = if mi {
                        ui.add(
                            egui::Label::new(
                                egui::RichText::new(p.display().to_string())
                                    .size(mui::DIALOG_NOTE)
                                    .weak(),
                            )
                            .truncate(),
                        )
                        .on_hover_text(p.display().to_string());
                        name_field(ui, n, field_id, "新名称")
                    } else {
                        ui.label(p.display().to_string());
                        ui.text_edit_singleline(n);
                        false
                    };
                    if action(ui, mi, "重命名", !n.trim().is_empty(), &mut keep) || submitted {
                        match files::safe_name(n)
                            .and_then(|n| files::rename_new(p, &p.with_file_name(n)))
                        {
                            Ok(()) => {
                                keep = false;
                                self.refresh();
                            }
                            Err(e) => {
                                self.status = e.to_string();
                                if mi {
                                    ctx.data_mut(|d| d.insert_temp(err_key, self.status.clone()));
                                }
                            }
                        }
                    }
                }
                Dialog::Delete(v) => {
                    ui.label(format!("将 {} 项移入 Windows 回收站？", v.len()));
                    if action(ui, mi, "确认移动到回收站", true, &mut keep) {
                        let v = v.clone();
                        self.start_job(true, move |c| {
                            files::check_cancel(c).map_err(|e| e.to_string())?;
                            trash::delete_all(&v)
                                .map(|_| format!("已移入回收站：{} 项", v.len()))
                                .map_err(|e| e.to_string())
                        });
                        keep = false;
                    }
                }
                Dialog::Properties(state) => state.show(ui, mi),
                Dialog::Hash(state) => state.show(ui),
                Dialog::Batch(batch) => {
                    batch.rules(ui);
                    let rows = &mut batch.rows;
                    ui.label("编辑预览表。目标重名时停止，不覆盖。");
                    egui::ScrollArea::vertical()
                        .max_height(330.0)
                        .show(ui, |ui| {
                            for (p, n) in rows.iter_mut() {
                                ui.horizontal(|ui| {
                                    ui.label(name(p));
                                    ui.text_edit_singleline(n);
                                });
                            }
                        });
                    if action(ui, mi, "应用", true, &mut keep) {
                        let rows = rows.clone();
                        self.start_job(true, move |c| {
                            let mut planned = vec![];
                            let mut seen = BTreeSet::new();
                            for (p, n) in rows {
                                if name(&p) == n {
                                    continue;
                                }
                                files::safe_name(&n).map_err(|e| e.to_string())?;
                                let dst = p.with_file_name(n);
                                if dst.symlink_metadata().is_ok()
                                    || !seen.insert(dst.to_string_lossy().to_lowercase())
                                {
                                    return Err(format!("目标冲突：{}", dst.display()));
                                }
                                planned.push((p, dst));
                            }
                            for (i, (a, b)) in planned.iter().enumerate() {
                                files::check_cancel(c)
                                    .and_then(|_| files::rename_new(a, b))
                                    .map_err(|e| format!("已有 {i} 项改名；{e}"))?;
                            }
                            Ok(format!("已重命名 {} 项", planned.len()))
                        });
                        keep = false;
                    }
                }
                Dialog::Search(q) => {
                    ui.text_edit_singleline(q);
                    if ui.button("递归搜索当前目录").clicked() && self.start_name_search(q)
                    {
                        keep = false;
                    }
                }
                Dialog::Results(rows) => {
                    ui.label(format!("{} 项（最多显示 10000 项）", rows.len()));
                    egui::ScrollArea::vertical().max_height(360.0).show_rows(
                        ui,
                        25.0,
                        rows.len(),
                        |ui, r| {
                            for i in r {
                                let p = &rows[i];
                                if ui.button(p.display().to_string()).clicked() {
                                    self.visit_search_result(p);
                                    keep = false;
                                }
                            }
                        },
                    );
                }
                Dialog::Appearance => {
                    if self.visual.is_some() {
                        super::visual_smoke::menu_probe(ctx, "appearance-open", ui.max_rect());
                    }
                    self.appearance(ui);
                }
            }
            if mi && let Some(error) = ctx.data(|d| d.get_temp::<String>(err_key)) {
                if short {
                    ui.add(
                        egui::Label::new(
                            egui::RichText::new(&error).color(mui::Colors::of(ui).error),
                        )
                        .truncate(),
                    )
                    .on_hover_text(error);
                } else {
                    ui.colored_label(mui::Colors::of(ui).error, error);
                }
            } else if !mi && !self.status.is_empty() {
                ui.separator();
                ui.label(egui::RichText::new(&self.status).weak());
            }
        };
        let mut x = true;
        if mi {
            x = !mui::file_sheet(
                ctx,
                sheet_id,
                title,
                width,
                if short {
                    mui::SheetBody::ShortForm
                } else {
                    mui::SheetBody::Scrollable
                },
                &mut body,
            );
        } else {
            egui::Window::new(title)
                .open(&mut x)
                .default_width(480.)
                .show(ctx, &mut body);
        }
        if !keep || !x {
            ctx.data_mut(|d| {
                d.remove::<bool>(field_id.with("focused"));
                d.remove::<String>(err_key);
            });
        }
        if keep && x && self.dialog.is_none() {
            self.dialog = Some(d);
        }
    }
    pub(super) fn editor(&mut self, ctx: &egui::Context) {
        let mut refresh_archive = None;
        let Some(e) = &mut self.editor else {
            return;
        };
        if let Some(rx) = &e.publish {
            match rx.try_recv() {
                Ok(r) => {
                    e.publish = None;
                    match r {
                        Ok(hash) => {
                            if let Some(s) = &mut e.staged {
                                s.hash = hash;
                                refresh_archive = Some(s.source.root().to_owned());
                            }
                            e.archive_unsaved = false;
                            self.status = "已回写压缩包".into();
                        }
                        Err(msg) => self.status = msg,
                    }
                }
                Err(mpsc::TryRecvError::Disconnected) => {
                    e.publish = None;
                    self.status = "包内写回任务异常退出".into();
                }
                _ => ctx.request_repaint_after(Duration::from_millis(100)),
            }
        }
        let displayed = e
            .staged
            .as_ref()
            .map_or_else(|| name(&e.path), |s| name(&s.inner));
        let mut open = true;
        let mut discard = false;
        let save_key = ctx.input_mut(|i| i.consume_key(Modifiers::CTRL, Key::S));
        egui::Window::new(format!("文本编辑：{displayed}"))
            .id(egui::Id::new("editor"))
            .open(&mut open)
            .default_size([700.0, 530.0])
            .show(ctx, |ui| {
                ui.horizontal_wrapped(|ui| {
                    let send = ui
                        .add_enabled(e.publish.is_none(), egui::Button::new("保存 Ctrl+S"))
                        .clicked()
                        || save_key;
                    if send && e.publish.is_none() {
                        let dirty = e.doc.dirty();
                        match e.doc.save(&e.path) {
                            Err(err) => self.status = err.to_string(),
                            Ok(()) => {
                                if let Some(s) = &e.staged
                                    && (dirty || e.archive_unsaved)
                                {
                                    e.archive_unsaved = true;
                                    let (archive, inner, expected, source) = (
                                        s.source.clone(),
                                        s.inner.clone(),
                                        s.hash.clone(),
                                        e.path.clone(),
                                    );
                                    let (tx, rx) = mpsc::channel();
                                    std::thread::spawn(move || {
                                        let c = AtomicBool::new(false);
                                        let result = archive
                                            .rewrite(
                                                &expected,
                                                twinfile_rust::archive_edit::Change::Replace {
                                                    at: inner,
                                                    source,
                                                },
                                                &c,
                                            )
                                            .map_err(|e| e.to_string());
                                        let _ = tx.send(result);
                                    });
                                    e.publish = Some(rx);
                                    self.status = "正在写回压缩包…".into();
                                } else if dirty {
                                    self.status = "保存完成".into();
                                }
                            }
                        }
                    }
                    egui::ComboBox::from_id_salt("encoding")
                        .selected_text(e.doc.encoding.name())
                        .show_ui(ui, |ui| {
                            for en in text::Encoding::ALL {
                                if ui.motion_label(en == e.doc.encoding, en.name()).clicked()
                                    && !e.unsaved()
                                {
                                    match text::Document::open(&e.path, Some(en)) {
                                        Ok(d) => e.doc = d,
                                        Err(x) => self.status = x.to_string(),
                                    }
                                }
                            }
                        });
                    ui.label(if e.unsaved() {
                        "未保存"
                    } else {
                        "已保存"
                    });
                });
                ui.horizontal_wrapped(|ui| {
                    ui.add(
                        egui::TextEdit::singleline(&mut e.find)
                            .hint_text("查找")
                            .desired_width(110.0),
                    );
                    ui.add(
                        egui::TextEdit::singleline(&mut e.replace)
                            .hint_text("替换")
                            .desired_width(110.0),
                    );
                    ui.motion_check(&mut e.regex, "正则");
                    if ui.button("统计匹配").clicked() && !e.find.is_empty() {
                        let pattern = if e.regex {
                            e.find.clone()
                        } else {
                            regex::escape(&e.find)
                        };
                        self.status = regex::Regex::new(&pattern)
                            .map(|r| format!("{} 处匹配", r.find_iter(&e.doc.body).count()))
                            .unwrap_or_else(|err| err.to_string());
                    }
                    if ui.button("全部替换").clicked() && !e.find.is_empty() {
                        if e.regex {
                            match regex::Regex::new(&e.find) {
                                Ok(r) => {
                                    e.doc.body =
                                        r.replace_all(&e.doc.body, e.replace.as_str()).into_owned()
                                }
                                Err(err) => self.status = err.to_string(),
                            }
                        } else {
                            e.doc.body = e.doc.body.replace(&e.find, &e.replace);
                        }
                    }
                });
                if e.closing {
                    ui.horizontal(|ui| {
                        ui.label("有未保存内容；是否放弃？");
                        if ui
                            .add_enabled(e.publish.is_none(), egui::Button::new("放弃"))
                            .clicked()
                        {
                            discard = true;
                        }
                        if ui.button("返回").clicked() {
                            e.closing = false;
                        }
                    });
                }
                ui.label(&self.status);
                egui::ScrollArea::both().max_height(465.0).show(ui, |ui| {
                    ui.add(
                        egui::TextEdit::multiline(&mut e.doc.body)
                            .code_editor()
                            .desired_width(f32::INFINITY)
                            .desired_rows(20),
                    );
                });
            });
        if discard || (!open && !e.unsaved()) {
            self.editor = None;
        } else if !open {
            e.closing = true;
        }
        if let Some(root) = refresh_archive {
            self.refresh_archive_windows(&root);
        }
    }
    pub(super) fn download_toolbar(&mut self, ui: &mut egui::Ui) {
        let d = &mut self.dl;
        if let Some((p, _)) = &d.work {
            ui.horizontal_wrapped(|ui| {
                let (a, z) = (p.received.load(Relaxed), p.total.load(Relaxed));
                let elapsed = d.rate.0.elapsed().as_secs_f64();
                if elapsed >= 0.5 {
                    d.rate = (
                        Instant::now(),
                        a,
                        a.saturating_sub(d.rate.1) as f64 / elapsed,
                    );
                }
                let details = format!(
                    "{} / {} · {}/s · {} 连接 · 重试 {}",
                    human(a),
                    if z == 0 { "未知".into() } else { human(z) },
                    human(d.rate.2 as u64),
                    p.connections.load(Relaxed),
                    p.retries.load(Relaxed)
                );
                if z > 0 {
                    ui.add(
                        egui::ProgressBar::new(a as f32 / z as f32)
                            .desired_width(320.0)
                            .text(details),
                    );
                } else {
                    ui.spinner();
                    ui.label(details);
                }
                if ui.button("取消").clicked() {
                    p.cancel.store(true, Relaxed);
                }
                if ui.button("详情").clicked() {
                    d.open = true;
                }
            });
        } else if !d.status.is_empty() {
            ui.horizontal_wrapped(|ui| {
                ui.label(&d.status);
                if ui.button("详情").clicked() {
                    d.open = true;
                }
                if d.status.starts_with("下载完成")
                    && ui.button("打开目录").clicked()
                    && let Some(parent) = d.dest.as_deref().and_then(Path::parent)
                {
                    if let Err(e) = super::workbench::system_open(parent) {
                        self.status = e.to_string();
                    }
                }
            });
        }
    }
    pub(super) fn show_download(&mut self, ctx: &egui::Context) {
        if !self.dl.open {
            self.dl.was_open = false;
            return;
        }
        let mut show = true;
        let mut start = false;
        let d = &mut self.dl;
        use config::DownloadPrefs;
        if !d.was_open {
            d.was_open = true;
            if let Some(root) = &d.prefs_root {
                match DownloadPrefs::read(root, &DownloadPrefs::from_settings(&self.s)) {
                    Ok(prefs) => prefs.apply(&mut self.s),
                    Err(e) => d.status = format!("读取下载偏好失败：{e}"),
                }
            }
            let usable = self
                .s
                .download_dir
                .clone()
                .filter(|p| p.is_absolute() && p.is_dir());
            if usable != d.usable_dir && d.work.is_none() {
                d.dest = None;
                d.manual_name = false;
            }
            d.usable_dir = usable;
            d.update_target();
        }
        let before = DownloadPrefs::from_settings(&self.s);
        let mut choose = false;
        let mut save_as = false;
        let mut picked = false;
        let miuix = self.s.theme == Theme::Miuix;
        if miuix {
            let id = egui::Id::new("miuix-direct-download");
            let busy = d.work.is_some();
            show = !mui::file_sheet(
                ctx,
                id,
                "直链下载",
                404.,
                if ctx.content_rect().height() < 350. {
                    mui::SheetBody::Scrollable
                } else {
                    mui::SheetBody::ShortForm
                },
                |ui| {
                    let c = mui::Colors::of(ui);
                    let caption = |ui: &mut egui::Ui, text: &str| {
                        ui.label(
                            egui::RichText::new(text)
                                .size(mui::DIALOG_NOTE)
                                .color(c.summary),
                        );
                    };
                    caption(ui, "下载链接");
                    ui.add_enabled_ui(!busy, |ui| {
                        name_field(ui, &mut d.url, id.with("url"), "HTTP / HTTPS 地址");
                    });
                    ui.add_space(3.);
                    d.update_target();
                    caption(ui, "保存到");
                    ui.horizontal(|ui| {
                        let path = d.dest.as_deref();
                        let mut display = path
                            .or(self.s.download_dir.as_deref())
                            .map(|p| p.display().to_string());
                        if path.is_none()
                            && d.usable_dir.is_none()
                            && let Some(text) = &mut display
                        {
                            *text = format!("目录不可用：{text}");
                        }
                        let width =
                            (ui.available_width() - 78. - ui.spacing().item_spacing.x).max(1.);
                        let r = ui.add_sized(
                            [width, 29.],
                            egui::Label::new(
                                egui::RichText::new(display.as_deref().unwrap_or("未选择位置"))
                                    .color(if display.is_some() {
                                        c.on_surface
                                    } else {
                                        c.summary
                                    }),
                            )
                            .halign(egui::Align::Min)
                            .truncate(),
                        );
                        if let Some(display) = display {
                            r.on_hover_text(display);
                        }
                        choose = ui
                            .add_enabled_ui(!busy, |ui| mui::overlay_button(ui, "选择位置", false))
                            .inner
                            .clicked();
                    });
                    ui.add_space(2.);
                    ui.horizontal(|ui| {
                        caption(ui, "连接数");
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            let connections = ui.add_enabled(
                                !busy,
                                egui::DragValue::new(&mut self.s.download_connections)
                                    .range(1..=512),
                            );
                            #[cfg(test)]
                            ui.data_mut(|data| {
                                data.insert_temp(id.with("connections"), connections.rect)
                            });
                            connections
                        });
                    });
                    if busy {
                        download_progress(ui, d, true);
                    }
                    if !d.status.is_empty() && (!busy || d.status != "正在下载…") {
                        ui.add(
                            egui::Label::new(
                                egui::RichText::new(&d.status)
                                    .size(mui::DIALOG_NOTE)
                                    .color(c.summary),
                            )
                            .truncate(),
                        )
                        .on_hover_text(&d.status);
                    }
                    ui.add_space(5.);
                    ui.allocate_ui_with_layout(
                        egui::vec2(ui.available_width(), 30.),
                        egui::Layout::right_to_left(egui::Align::Center),
                        |ui| {
                            let response = if let Some((p, _)) = &d.work {
                                let response = ui
                                    .add_enabled_ui(!p.cancel.load(Relaxed), |ui| {
                                        mui::overlay_button(ui, "取消下载", false)
                                    })
                                    .inner;
                                if response.clicked() {
                                    p.cancel.store(true, Relaxed);
                                    d.status = "正在取消…".into();
                                }
                                response
                            } else {
                                let response = ui
                                    .add_enabled_ui(d.ready() && !d.url.trim().is_empty(), |ui| {
                                        mui::overlay_button(ui, "开始下载", true)
                                    })
                                    .inner;
                                start = response.clicked();
                                if !d.url.trim().is_empty() {
                                    save_as = mui::overlay_button(ui, "另存为…", false).clicked();
                                }
                                response
                            };
                            #[cfg(test)]
                            ui.data_mut(|data| {
                                data.insert_temp(
                                    id.with("action"),
                                    (response.rect, response.enabled()),
                                )
                            });
                            response
                        },
                    );
                },
            );
        } else {
            egui::Window::new("直链下载")
                .open(&mut show)
                .default_width(580.0)
                .show(ctx, |ui| {
                    ui.add_enabled(
                        d.work.is_none(),
                        egui::TextEdit::singleline(&mut d.url)
                            .desired_width(f32::INFINITY)
                            .hint_text("HTTP / HTTPS 地址"),
                    );
                    d.update_target();
                    ui.horizontal(|ui| {
                        if ui
                            .add_enabled(d.work.is_none(), egui::Button::new("保存位置…"))
                            .clicked()
                        {
                            choose = true;
                        }
                        if d.work.is_none()
                            && !d.url.trim().is_empty()
                            && ui.button("另存为…").clicked()
                        {
                            save_as = true;
                        }
                        if let Some(p) = d.dest.as_ref().or(self.s.download_dir.as_ref()) {
                            ui.add(egui::Label::new(p.display().to_string()).truncate())
                                .on_hover_text(p.display().to_string());
                        }
                    });
                    ui.horizontal(|ui| {
                        ui.label("连接数");
                        ui.add_enabled(
                            d.work.is_none(),
                            egui::DragValue::new(&mut self.s.download_connections).range(1..=512),
                        );
                        if ui
                            .add_enabled(
                                d.work.is_none() && d.ready() && !d.url.trim().is_empty(),
                                egui::Button::new("开始"),
                            )
                            .clicked()
                        {
                            start = true;
                        }
                        if let Some((p, _)) = &d.work
                            && ui.button("取消").clicked()
                        {
                            p.cancel.store(true, Relaxed);
                        }
                    });
                    download_progress(ui, d, false);
                    ui.label(&d.status);
                });
        }
        if choose || save_as {
            let mut dialog = rfd::FileDialog::new();
            if let Some(p) = &d.usable_dir {
                dialog = dialog.set_directory(p);
            }
            if choose {
                if let Some(dir) = dialog.pick_folder() {
                    picked = true;
                    self.s.download_dir = Some(dir.clone());
                    d.usable_dir = Some(dir);
                    d.dest = None;
                    d.manual_name = false;
                    d.update_target();
                }
            } else {
                let hint = if d.manual_name {
                    d.dest
                        .as_deref()
                        .and_then(Path::file_name)
                        .map(|n| n.to_string_lossy().into_owned())
                } else {
                    d.filename()
                };
                if let Some(hint) = &hint {
                    dialog = dialog.set_file_name(hint);
                }
                if let Some(p) = dialog.save_file() {
                    picked = true;
                    self.s.download_dir = p.parent().map(Path::to_owned);
                    d.usable_dir.clone_from(&self.s.download_dir);
                    d.manual_name = true;
                    d.dest = Some(p);
                }
            }
        }
        let after = DownloadPrefs::from_settings(&self.s);
        if (picked || before != after)
            && let Some(root) = &d.prefs_root
        {
            match DownloadPrefs::save_changes(root, &before, &after) {
                Ok(prefs) => {
                    if prefs.dir != self.s.download_dir && d.work.is_none() {
                        d.usable_dir = prefs.dir.clone().filter(|p| p.is_absolute() && p.is_dir());
                        d.dest = None;
                        d.manual_name = false;
                        d.update_target();
                    }
                    prefs.apply(&mut self.s);
                }
                Err(e) => d.status = format!("保存下载偏好失败：{e}"),
            }
        }
        if start && self.job.is_some() {
            d.status = "文件操作运行中，请等待完成".into();
        } else if start && d.ready() {
            let manual = d.manual_name.then(|| d.dest.clone()).flatten();
            let dir = d.usable_dir.clone();
            let suggested = d.suggested_name.clone();
            let (tx, rx) = mpsc::channel();
            let a = Arc::new(download::Progress::default());
            let progress = a.clone();
            let u = d.url.trim().to_owned();
            let n = self.s.download_connections;
            d.rate = (Instant::now(), 0, 0.0);
            d.status = "正在下载…".into();
            d.work = Some((a, rx));
            if !self.standalone {
                d.open = false;
            }
            std::thread::spawn(move || {
                let r = match (manual, dir) {
                    (Some(dst), _) => download::run(&u, &dst, n, progress),
                    (_, Some(dir)) => {
                        download::run_auto(&u, &dir, suggested.as_deref(), n, progress)
                    }
                    _ => Err("请选择保存目录".into()),
                }
                .map(|p| format!("下载完成：{}", p.display()))
                .map_err(|e| e.to_string());
                let _ = tx.send(r);
            });
        }
        if !show {
            if miuix && d.work.is_some() {
                // A backdrop/Escape dismissal must never cancel a transfer. Standalone has no dock to reopen it.
                d.open = self.standalone;
            } else if let Some((p, _)) = &d.work {
                p.cancel.store(true, Relaxed);
                d.status = "正在取消…".into();
            } else {
                d.open = false;
                if self.standalone {
                    ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                }
            }
        }
    }
}
fn download_progress(ui: &mut egui::Ui, d: &mut Downloader, miuix: bool) {
    if let Some((p, _)) = &d.work {
        let a = p.received.load(Relaxed);
        let z = p.total.load(Relaxed);
        let elapsed = d.rate.0.elapsed().as_secs_f64();
        if elapsed >= 0.5 {
            d.rate = (
                Instant::now(),
                a,
                a.saturating_sub(d.rate.1) as f64 / elapsed,
            );
        }
        let label = format!(
            "{} / {} · {}/s · {} 连接 · 重试 {}",
            human(a),
            if z == 0 { "未知".into() } else { human(z) },
            human(d.rate.2 as u64),
            p.connections.load(Relaxed),
            p.retries.load(Relaxed)
        );
        if miuix {
            let c = mui::Colors::of(ui);
            ui.label(
                egui::RichText::new(label)
                    .size(mui::DIALOG_NOTE)
                    .color(c.summary),
            );
            if z > 0 {
                ui.add(
                    egui::ProgressBar::new((a as f32 / z as f32).clamp(0., 1.))
                        .desired_height(5.)
                        .desired_width(ui.available_width())
                        .fill(c.primary),
                );
            } else {
                ui.spinner();
            }
        } else if z > 0 {
            ui.add(egui::ProgressBar::new(a as f32 / z as f32).text(label));
        } else {
            ui.label(label);
        }
    }
}
fn search_dir(root: PathBuf, q: &str, hidden: bool) -> io::Result<Vec<PathBuf>> {
    let mut pending = vec![root];
    let mut result = vec![];
    let mut count = 0;
    while let Some(p) = pending.pop() {
        let items = match files::list_for_display(&p, hidden) {
            Ok(v) => v,
            Err(e) if count == 0 => return Err(e),
            Err(_) => continue,
        };
        count += 1;
        if count > 200_000 {
            break;
        }
        for e in items {
            if e.name.to_lowercase().contains(q) {
                result.push(e.path.clone());
                if result.len() >= 10_000 {
                    return Ok(result);
                }
            }
            if e.dir
                && e.path.symlink_metadata().is_ok_and(|m| {
                    use std::os::windows::fs::MetadataExt;
                    m.file_attributes() & 0x400 == 0
                })
            {
                pending.push(e.path);
            }
        }
    }
    Ok(result)
}
pub(super) fn load_image(p: &Path, size: u32) -> Option<egui::ColorImage> {
    let mut reader = image::ImageReader::open(p)
        .ok()?
        .with_guessed_format()
        .ok()?;
    let mut limits = image::Limits::default();
    limits.max_image_width = Some(16384);
    limits.max_image_height = Some(16384);
    limits.max_alloc = Some(128 * 1024 * 1024);
    reader.limits(limits);
    let rgba = reader.decode().ok()?.thumbnail(size, size).to_rgba8();
    Some(egui::ColorImage::from_rgba_unmultiplied(
        [rgba.width() as usize, rgba.height() as usize],
        rgba.as_raw(),
    ))
}

#[cfg(test)]
mod desktop_ini_tests {
    use super::*;

    #[test]
    fn desktop_ini_is_hidden_only_in_browser_and_search() {
        let tmp = tempfile::tempdir().unwrap();
        let root = tmp.path();
        let ini = root.join("Desktop.INI");
        fs::write(&ini, b"metadata").unwrap();
        fs::write(root.join(".other-hidden"), b"keep").unwrap();
        fs::write(root.join("desktop.ini.bak"), b"keep").unwrap();
        let same_name_dir = root.join("sub").join("desktop.ini");
        fs::create_dir_all(&same_name_dir).unwrap();
        fs::write(same_name_dir.join("inside.txt"), b"keep").unwrap();
        for hidden in [false, true] {
            let results = search_dir(root.to_owned(), "desktop.ini", hidden).unwrap();
            assert!(!results.contains(&ini), "desktop.ini leaked into search");
            assert!(results.contains(&same_name_dir));
            assert!(results.contains(&root.join("desktop.ini.bak")));
            assert_eq!(
                search_dir(root.to_owned(), "inside.txt", hidden)
                    .unwrap()
                    .len(),
                1
            );
            assert_eq!(
                !search_dir(root.to_owned(), "other-hidden", hidden)
                    .unwrap()
                    .is_empty(),
                hidden
            );
        }
        // The comparison engine still uses the unfiltered directory listing.
        assert!(
            files::list(root, true)
                .unwrap()
                .iter()
                .any(|e| e.path == ini)
        );
        assert_eq!(fs::read(ini).unwrap(), b"metadata");
    }
}

fn name_field(ui: &mut egui::Ui, n: &mut String, id: egui::Id, hint: &str) -> bool {
    let c = mui::Colors::of(ui);
    let focused = ui.memory(|m| m.has_focus(id));
    let r = egui::Frame::new()
        .fill(c.field)
        .corner_radius(9)
        .stroke(egui::Stroke::new(
            1_f32,
            if focused { c.primary } else { c.field },
        ))
        .inner_margin(egui::Margin::symmetric(9, 3))
        .show(ui, |ui| {
            ui.add(
                egui::TextEdit::singleline(n)
                    .id(id)
                    .hint_text(hint)
                    .frame(false)
                    .font(mui::font(mui::DIALOG_BODY))
                    .desired_width(ui.available_width())
                    .min_size(egui::vec2(0., 24.))
                    .vertical_align(egui::Align::Center),
            )
        })
        .inner;
    if !ui.is_sizing_pass() && !ui.data(|d| d.get_temp::<bool>(id.with("focused")).unwrap_or(false))
    {
        r.request_focus();
        ui.data_mut(|d| d.insert_temp(id.with("focused"), true));
    }
    !n.trim().is_empty() && r.lost_focus() && ui.input(|i| i.key_pressed(Key::Enter))
}
fn action(ui: &mut egui::Ui, mi: bool, label: &str, enabled: bool, keep: &mut bool) -> bool {
    if !mi {
        return ui.button(label).clicked();
    }
    ui.add_space(5.);
    ui.allocate_ui_with_layout(
        egui::vec2(ui.available_width(), 34.),
        egui::Layout::right_to_left(egui::Align::Center),
        |ui| {
            let confirm = ui
                .add_enabled_ui(enabled, |ui| mui::overlay_button(ui, label, true))
                .inner;
            let cancel = mui::overlay_button(ui, "取消", false);
            #[cfg(test)]
            ui.data_mut(|data| {
                data.insert_temp(
                    egui::Id::new(("footer-geometry", label)),
                    (cancel.rect, confirm.rect, ui.clip_rect()),
                )
            });
            if cancel.clicked() {
                *keep = false;
            }
            confirm.clicked()
        },
    )
    .inner
}

#[cfg(test)]
mod compact_sheet_tests {
    use super::*;
    fn frame(app: &mut App, ctx: &egui::Context, events: Vec<egui::Event>) {
        let _ = ctx.run(
            egui::RawInput {
                screen_rect: Some(egui::Rect::from_min_size(
                    egui::Pos2::ZERO,
                    egui::vec2(680., 460.),
                )),
                events,
                ..Default::default()
            },
            |ctx| app.dialogs(ctx),
        );
    }
    fn key(key: Key) -> Vec<egui::Event> {
        [true, false]
            .into_iter()
            .map(|pressed| egui::Event::Key {
                key,
                physical_key: None,
                pressed,
                repeat: false,
                modifiers: Default::default(),
            })
            .collect()
    }
    #[test]
    fn download_defaults_restore_across_modes_and_keep_each_links_name() {
        let tmp = tempfile::tempdir().unwrap();
        let root = tmp.path().join("config");
        let folder = tmp.path().join("保存位置");
        fs::create_dir(&folder).unwrap();
        // Migrate an existing installation, which stored its directory in settings.json.
        {
            let (st, mut s, _) = Store::open(&root).unwrap();
            s.download_dir = Some(folder.clone());
            s.download_connections = 27;
            st.save(&s).unwrap();
        }
        let draw = |app: &mut App, ctx: &egui::Context, events| {
            let _ = ctx.run(
                egui::RawInput {
                    events,
                    ..Default::default()
                },
                |ctx| app.show_download(ctx),
            );
        };
        let ctx = egui::Context::default();
        let cc = eframe::CreationContext::_new_kittest(ctx.clone());
        let mut app = App::new(&cc, false, Some(tmp.path().into()), false, None);
        app.s.theme = Theme::Miuix;
        app.dl.open = true;
        for _ in 0..4 {
            draw(&mut app, &ctx, vec![]);
        }
        assert_eq!(app.dl.usable_dir.as_ref(), Some(&folder));
        assert!(app.dl.dest.is_none());
        app.dl.url = "https://example.invalid/first.zip".into();
        draw(&mut app, &ctx, vec![]);
        assert_eq!(app.dl.dest, Some(folder.join("first.zip")));
        let pos = ctx
            .data(|d| {
                d.get_temp::<egui::Rect>(egui::Id::new("miuix-direct-download").with("connections"))
            })
            .unwrap()
            .center();
        draw(&mut app, &ctx, vec![egui::Event::PointerMoved(pos)]);
        for pressed in [true, false] {
            draw(
                &mut app,
                &ctx,
                vec![egui::Event::PointerButton {
                    pos,
                    button: egui::PointerButton::Primary,
                    pressed,
                    modifiers: Default::default(),
                }],
            );
        }
        draw(&mut app, &ctx, vec![]);
        draw(&mut app, &ctx, vec![egui::Event::Paste("28".into())]);
        draw(&mut app, &ctx, key(Key::Enter));
        assert_eq!(app.s.download_connections, 28);
        drop(app); // No persist() call: the edit must already be on disk.
        let ctx = egui::Context::default();
        let cc = eframe::CreationContext::_new_kittest(ctx.clone());
        let mut app = App::new(
            &cc,
            true,
            Some(tmp.path().into()),
            false,
            Some(twinfile_rust::bridge::Request {
                url: "https://example.invalid/id".into(),
                filename: "实际文件.zip".into(),
            }),
        );
        app.s.download_connections = 16;
        app.s.download_dir = None;
        draw(&mut app, &ctx, vec![]);
        assert_eq!(app.s.download_connections, 28);
        assert_eq!(app.dl.dest, Some(folder.join("实际文件.zip")));
        app.dl.url = "https://example.invalid/next.txt?token=secret".into();
        draw(&mut app, &ctx, vec![]);
        assert_eq!(app.dl.dest, Some(folder.join("next.txt")));
        assert!(
            !fs::read_to_string(root.join("download-prefs.json"))
                .unwrap()
                .contains("token")
        );
        app.dl.open = false;
        draw(&mut app, &ctx, vec![]);
        fs::remove_dir(&folder).unwrap();
        app.dl.open = true;
        draw(&mut app, &ctx, vec![]);
        assert!(app.dl.dest.is_none());
    }
    #[test]
    fn concurrent_preference_edits_merge_without_clobbering_other_field() {
        use config::DownloadPrefs;
        let tmp = tempfile::tempdir().unwrap();
        let old = DownloadPrefs {
            dir: None,
            connections: 16,
        };
        let folder = DownloadPrefs {
            dir: Some(tmp.path().into()),
            ..old.clone()
        };
        let count = DownloadPrefs {
            connections: 128,
            ..old.clone()
        };
        DownloadPrefs::save_changes(tmp.path(), &old, &folder).unwrap();
        let merged = DownloadPrefs::save_changes(tmp.path(), &old, &count).unwrap();
        assert_eq!(merged.dir, folder.dir);
        assert_eq!(merged.connections, 128);
    }
    #[test]
    fn download_sheet_long_inputs_fit_and_dismiss_never_cancels_work() {
        for (dark, standalone) in [(false, true), (true, false)] {
            let tmp = tempfile::tempdir().unwrap();
            let ctx = egui::Context::default();
            let cc = eframe::CreationContext::_new_kittest(ctx.clone());
            let mut app = App::new(&cc, standalone, Some(tmp.path().into()), false, None);
            app.s.theme = Theme::Miuix;
            app.s.dark = dark;
            app.s.reduce_motion = true;
            crate::theme::apply(&ctx, &app.s);
            app.dl.open = true;
            app.dl.url = format!("https://example.invalid/{}", "abc".repeat(300));
            let id = egui::Id::new("miuix-direct-download");
            let screen = egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(340., 410.));
            let draw = |app: &mut App, events| {
                let _ = ctx.run(
                    egui::RawInput {
                        screen_rect: Some(screen),
                        events,
                        ..Default::default()
                    },
                    |ctx| app.show_download(ctx),
                );
            };
            let action = || {
                ctx.data(|d| d.get_temp::<(egui::Rect, bool)>(id.with("action")))
                    .unwrap()
            };
            for _ in 0..4 {
                draw(&mut app, vec![]);
            }
            assert!(!action().1);
            app.dl.dest = Some(tmp.path().join("a".repeat(240)));
            for _ in 0..4 {
                draw(&mut app, vec![]);
            }
            let sheet = ctx.memory(|m| m.area_rect(id)).unwrap();
            assert!(
                screen.contains_rect(sheet) && sheet.contains_rect(action().0) && action().1,
                "{screen:?} {sheet:?} {:?}",
                action()
            );
            let (_tx, rx) = mpsc::channel();
            let progress = Arc::new(download::Progress::default());
            progress.total.store(100, Relaxed);
            progress.received.store(37, Relaxed);
            app.dl.work = Some((progress.clone(), rx));
            for _ in 0..4 {
                draw(&mut app, vec![]);
            }
            draw(&mut app, key(Key::Escape));
            assert_eq!(app.dl.open, standalone);
            assert!(!progress.cancel.load(Relaxed));
            app.dl.open = true;
            for _ in 0..4 {
                draw(&mut app, vec![]);
            }
            let pos = action().0.center();
            draw(&mut app, vec![egui::Event::PointerMoved(pos)]);
            for pressed in [true, false] {
                draw(
                    &mut app,
                    vec![egui::Event::PointerButton {
                        pos,
                        button: egui::PointerButton::Primary,
                        pressed,
                        modifiers: Default::default(),
                    }],
                );
            }
            assert!(
                progress.cancel.load(Relaxed),
                "explicit cancel, dark={dark}"
            );
            assert_eq!(app.dl.status, "正在取消…");
        }
    }
    #[test]
    fn new_file_and_folder_footer_fits_and_is_clickable_at_fractional_dpi() {
        for scale in [1_f32, 1.25, 1.5, 2.] {
            let tmp = tempfile::tempdir().unwrap();
            let ctx = egui::Context::default();
            let cc = eframe::CreationContext::_new_kittest(ctx.clone());
            let mut app = App::new(&cc, true, Some(tmp.path().to_owned()), false, None);
            app.s.theme = Theme::Miuix;
            app.s.reduce_motion = true;
            app.s.active = 0;
            app.panes[0].goto(Location::Local(tmp.path().to_owned()), false);
            crate::theme::apply(&ctx, &app.s);
            let viewport =
                egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(680., 460.) / scale);
            let render = |app: &mut App, events| {
                let mut input = egui::RawInput {
                    screen_rect: Some(viewport),
                    events,
                    ..Default::default()
                };
                input
                    .viewports
                    .get_mut(&egui::ViewportId::ROOT)
                    .unwrap()
                    .native_pixels_per_point = Some(scale);
                let _ = ctx.run(input, |ctx| app.dialogs(ctx));
            };
            for (folder, text, cancel_action) in [
                (true, "", true),
                (false, "file.txt", false),
                (true, "folder", false),
            ] {
                app.dialog = Some(Dialog::New(folder, text.into()));
                for _ in 0..5 {
                    render(&mut app, vec![]);
                }
                let (cancel, confirm, clip) = ctx
                    .data(|d| {
                        d.get_temp::<(egui::Rect, egui::Rect, egui::Rect)>(egui::Id::new((
                            "footer-geometry",
                            "创建",
                        )))
                    })
                    .unwrap();
                let sheet = ctx
                    .memory(|m| m.area_rect(egui::Id::new(("miuix-sheet", "新建"))))
                    .unwrap();
                for button in [cancel, confirm] {
                    assert!(
                        viewport.contains_rect(button)
                            && clip.contains_rect(button)
                            && sheet.shrink(9.).contains_rect(button),
                        "scale={scale}: {button:?} clip={clip:?} sheet={sheet:?}"
                    );
                    assert!(button.height() >= 28.);
                }
                assert!(cancel.right() + 3. <= confirm.left());
                let target = if cancel_action { cancel } else { confirm };
                let pos = target.center_bottom() - egui::vec2(0., 2.);
                render(&mut app, vec![egui::Event::PointerMoved(pos)]);
                for pressed in [true, false] {
                    render(
                        &mut app,
                        vec![egui::Event::PointerButton {
                            pos,
                            button: egui::PointerButton::Primary,
                            pressed,
                            modifiers: Default::default(),
                        }],
                    );
                }
                assert!(app.dialog.is_none(), "footer click failed at {scale}x");
                if !cancel_action {
                    let p = tmp.path().join(text);
                    assert!(if folder { p.is_dir() } else { p.is_file() });
                }
            }
        }
    }
    #[test]
    fn compact_sheets_on_small_window_create_rename_cancel_and_close() {
        let tmp = tempfile::tempdir().unwrap();
        let ctx = egui::Context::default();
        let cc = eframe::CreationContext::_new_kittest(ctx.clone());
        let mut app = App::new(&cc, true, Some(tmp.path().to_owned()), false, None);
        app.s.theme = Theme::Miuix;
        app.s.reduce_motion = true;
        app.s.active = 1;
        app.panes[1].goto(Location::Local(tmp.path().to_owned()), false);
        crate::theme::apply(&ctx, &app.s);
        let p = tmp.path().join("from.txt");
        let to = tmp.path().join("after.txt");
        for (dlg, title, input) in [
            (Dialog::New(false, String::new()), "新建", "from.txt"),
            (
                Dialog::Rename(p.clone(), String::new()),
                "重命名",
                "after.txt",
            ),
        ] {
            app.dialog = Some(dlg);
            for _ in 0..5 {
                frame(&mut app, &ctx, vec![]);
            }
            let id = egui::Id::new(("miuix-sheet", title));
            assert!(ctx.memory(|m| m.has_focus(id.with("name"))));
            frame(&mut app, &ctx, vec![egui::Event::Text(input.to_owned())]);
            frame(&mut app, &ctx, key(Key::Enter));
            assert!(app.dialog.is_none(), "{title} submit");
            assert!(tmp.path().join(input).is_file());
        }
        assert!(!p.exists());
        app.dialog = Some(Dialog::Delete(vec![to.clone()]));
        for _ in 0..3 {
            frame(&mut app, &ctx, vec![]);
        }
        frame(&mut app, &ctx, key(Key::Escape));
        assert!(app.dialog.is_none() && to.exists());
        app.dialog = Some(Dialog::Properties(local_tools::Properties::new(vec![to])));
        for _ in 0..4 {
            frame(&mut app, &ctx, vec![]);
        }
        let id = egui::Id::new(("miuix-sheet", "属性"));
        let rect = ctx.memory(|m| m.area_rect(id)).unwrap();
        assert!(ctx.content_rect().contains_rect(rect));
        let pos = ctx
            .data(|d| d.get_temp::<egui::Rect>(id.with("close-target")))
            .unwrap()
            .center();
        frame(&mut app, &ctx, vec![egui::Event::PointerMoved(pos)]);
        for pressed in [true, false] {
            frame(
                &mut app,
                &ctx,
                vec![egui::Event::PointerButton {
                    pos,
                    button: egui::PointerButton::Primary,
                    pressed,
                    modifiers: Default::default(),
                }],
            );
        }
        assert!(app.dialog.is_none());
    }
}
