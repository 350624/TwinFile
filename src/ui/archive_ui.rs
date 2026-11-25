use super::*;
use crate::motion::ChoiceUi as _;
use twinfile_rust::archive_edit::{self, Change, Source};
type Listing = Result<(Vec<archive::Item>, String), String>;
pub(super) struct ArchiveView {
    source: Source,
    closing: bool,
    items: Vec<archive::Item>,
    hash: String,
    inner: PathBuf,
    selection: Option<(PathBuf, bool)>,
    edit: Option<(u8, String)>,
    pending: Option<Receiver<Listing>>,
    stop: Arc<AtomicBool>,
    message: String,
}
impl ArchiveView {
    pub(super) fn busy(&self) -> bool {
        self.pending.is_some()
    }
    fn start(&mut self, change: Option<Change>) {
        if self.pending.is_some() {
            return;
        }
        let source = self.source.clone();
        let expected = self.hash.clone();
        let (tx, rx) = mpsc::channel();
        let stop = Arc::new(AtomicBool::new(false));
        self.stop = stop.clone();
        self.pending = Some(rx);
        std::thread::spawn(move || {
            let result = (|| -> Result<_, Box<dyn std::error::Error + Send + Sync>> {
                let rev = change
                    .map(|change| source.rewrite(&expected, change, &stop))
                    .transpose()?;
                source
                    .read(rev.as_deref(), &stop, |p| archive::list(p))
                    .map_err(|err| {
                        if rev.is_some() {
                            format!("修改已写入，但刷新失败：{err}").into()
                        } else {
                            err
                        }
                    })
            })();
            let _ = tx.send(result.map_err(|e| e.to_string()));
        });
    }
}
impl App {
    pub(super) fn browse_archive(&mut self, path: PathBuf) {
        self.open_archive(Source::local(path.canonicalize().unwrap_or(path)));
    }
    fn open_archive(&mut self, source: Source) {
        if self.archives.iter().any(|v| v.source == source) {
            self.status = "压缩包已打开".into();
            return;
        }
        if self.archives.len() >= 8 {
            self.status = "已达到 8 个归档窗口上限".into();
            return;
        }
        let mut v = ArchiveView {
            source,
            closing: false,
            items: vec![],
            hash: String::new(),
            inner: PathBuf::new(),
            selection: None,
            edit: None,
            pending: None,
            stop: Default::default(),
            message: String::new(),
        };
        v.start(None);
        self.archives.push(v);
    }
    pub(super) fn refresh_archive_windows(&mut self, root: &Path) {
        for view in &mut self.archives {
            if view.source.root() == root && !view.closing {
                view.start(None);
            }
        }
    }
    pub(super) fn archives(&mut self, ctx: &egui::Context) {
        let mut views = std::mem::take(&mut self.archives);
        let mut nested = None;
        let mut observed_revisions = Vec::new();
        for mut v in views.drain(..) {
            if let Some(rx) = &v.pending {
                match rx.try_recv() {
                    Ok(r) => {
                        v.pending = None;
                        match r {
                            Ok((items, hash)) => {
                                if !v.hash.is_empty() && v.hash != hash {
                                    observed_revisions
                                        .push((v.source.root().to_owned(), hash.clone()));
                                }
                                v.items = items;
                                v.hash = hash;
                                if !v.inner.as_os_str().is_empty()
                                    && !v.items.iter().any(|x| x.path.starts_with(&v.inner))
                                {
                                    v.inner.clear();
                                }
                                v.selection = None;
                                v.message.clear();
                            }
                            Err(e) => {
                                v.message = e;
                                v.hash.clear();
                            }
                        }
                    }
                    Err(mpsc::TryRecvError::Disconnected) => {
                        v.pending = None;
                        v.hash.clear();
                        v.message = "任务意外退出".into();
                    }
                    _ => ctx.request_repaint_after(Duration::from_millis(100)),
                }
            }
            if v.closing && v.pending.is_none() {
                continue;
            }
            let mut open = !v.closing;
            let mut change = None;
            let mut refresh = false;
            let mut unzip = false;
            let mut stage = None;
            let n = name(v.source.leaf());
            egui::Window::new(format!("压缩包 · {n}"))
                .id(egui::Id::new(("archive", &v.source)))
                .open(&mut open)
                .default_width(625.0)
                .show(ctx, |ui| {
                    ui.label(v.source.label());
                    if n.to_ascii_lowercase().contains(".apk")
                        || name(v.source.root()).to_ascii_lowercase().contains(".apk")
                    {
                        ui.weak("修改 APK 会使原签名失效，修改后需重新签名。");
                    }
                    ui.horizontal_wrapped(|ui| {
                        if ui.button("上一级").clicked() {
                            v.inner = v.inner.parent().unwrap_or(Path::new("")).into();
                            v.selection = None;
                        }
                        if super::workbench::refresh_button(ui, "刷新", false).clicked() {
                            refresh = true;
                        }
                        ui.label(format!("/{}", v.inner.display()));
                    });
                    if v.pending.is_some() {
                        ui.horizontal(|ui| {
                            ui.spinner();
                            if ui.button("取消").clicked() {
                                v.stop.store(true, Relaxed);
                            }
                        });
                    }
                    let enabled = v.pending.is_none() && !v.hash.is_empty();
                    ui.add_enabled_ui(enabled, |ui| {
                        ui.horizontal_wrapped(|ui| {
                            if ui.button("新建目录").clicked() {
                                v.edit = Some((0, String::new()));
                            }
                            if ui.button("添加本地文件…").clicked()
                                && let Some(p) = rfd::FileDialog::new().pick_file()
                            {
                                change = Some(Change::AddFile {
                                    at: v.inner.join(name(&p)),
                                    source: p,
                                });
                            }
                            if ui.button("解压全部…").clicked() {
                                unzip = true;
                            }
                            if let Some((p, dir)) = &v.selection {
                                if ui.button("重命名…").clicked() {
                                    v.edit = Some((1, name(p)));
                                }
                                if ui.button("删除…").clicked() {
                                    v.edit = Some((2, String::new()));
                                }
                                if !dir
                                    && ui.button("用本地文件替换…").clicked()
                                    && let Some(source) = rfd::FileDialog::new().pick_file()
                                {
                                    change = Some(Change::Replace {
                                        at: p.clone(),
                                        source,
                                    });
                                }
                                if !dir && ui.button("打开/编辑…").clicked() {
                                    stage = Some(p.clone());
                                }
                            }
                        });
                        if let Some((op, text)) = &mut v.edit {
                            if *op == 2 {
                                ui.label("确认永久删除包内选中条目？");
                            } else {
                                ui.text_edit_singleline(text);
                            }
                            let mut done = false;
                            ui.horizontal(|ui| {
                                if ui.button("确认").clicked() {
                                    if *op == 2 {
                                        change = v
                                            .selection
                                            .as_ref()
                                            .map(|(p, _)| Change::Delete(p.clone()));
                                        done = true;
                                    } else {
                                        match files::safe_name(text) {
                                            Ok(n) => {
                                                change = if *op == 0 {
                                                    Some(Change::AddDirectory(v.inner.join(n)))
                                                } else {
                                                    v.selection.as_ref().map(|(p, _)| {
                                                        Change::Rename {
                                                            from: p.clone(),
                                                            to: p.with_file_name(n),
                                                        }
                                                    })
                                                };
                                                done = true;
                                            }
                                            Err(e) => v.message = e.to_string(),
                                        }
                                    }
                                }
                                if ui.button("放弃").clicked() {
                                    done = true;
                                }
                            });
                            if done {
                                v.edit = None;
                            }
                        }
                        let mut children = std::collections::BTreeMap::new();
                        for e in &v.items {
                            let Ok(rel) = e.path.strip_prefix(&v.inner) else {
                                continue;
                            };
                            let mut parts = rel.components();
                            let Some(first) = parts.next() else {
                                continue;
                            };
                            let dir = e.dir || parts.next().is_some();
                            let key = v.inner.join(first.as_os_str());
                            children.entry(key).and_modify(|d| *d |= dir).or_insert(dir);
                        }
                        let children = children.into_iter().collect::<Vec<_>>();
                        egui::ScrollArea::vertical().max_height(380.0).show_rows(
                            ui,
                            23.0,
                            children.len(),
                            |ui, range| {
                                for i in range {
                                    let (p, dir) = &children[i];
                                    let resp = ui.motion_label(
                                        v.selection.as_ref().is_some_and(|(q, _)| q == p),
                                        format!(
                                            "{}  {}",
                                            if *dir { "目录" } else { "文件" },
                                            name(p)
                                        ),
                                    );
                                    if resp.clicked() {
                                        v.selection = Some((p.clone(), *dir));
                                    }
                                    if resp.double_clicked() {
                                        if *dir {
                                            v.inner = p.clone();
                                            v.selection = None;
                                            v.edit = None;
                                        } else {
                                            stage = Some(p.clone());
                                        }
                                    }
                                }
                            },
                        );
                    });
                    if !v.message.is_empty() {
                        ui.label(&v.message);
                    }
                });
            if !open && v.pending.is_none() {
                continue;
            }
            if !open {
                v.closing = true;
                v.stop.store(true, Relaxed);
                self.archives.push(v);
                continue;
            }
            if let Some(c) = change {
                v.start(Some(c));
            } else if refresh {
                v.start(None);
            }
            if unzip && let Some(folder) = rfd::FileDialog::new().pick_folder() {
                let src = v.source.clone();
                let hash = v.hash.clone();
                let dest = folder.join(src.leaf().file_stem().unwrap_or_default());
                self.start_job(true, move |stop| {
                    src.read(Some(&hash), stop, |p| archive::extract(p, &dest, stop))
                        .map(|_| format!("解压到 {}", dest.display()))
                        .map_err(|e| e.to_string())
                });
            }
            if let Some(inner) = stage {
                let source = v.source.clone();
                let hash = v.hash.clone();
                let (tx, rx) = mpsc::channel();
                if self.entry_work.is_some() {
                    self.status = "另一个包内文件正在读取".into();
                } else if self.editor.as_ref().is_some_and(|e| e.unsaved()) {
                    self.status = "请先关闭或保存编辑器".into();
                } else {
                    std::thread::spawn(move || {
                        let c = AtomicBool::new(false);
                        let outcome =
                            (|| -> Result<_, Box<dyn std::error::Error + Send + Sync>> {
                                let (temp, _) = source.read(Some(&hash), &c, |p| {
                                    archive_edit::stage(p, &inner, &c)
                                })?;
                                Ok(Staged {
                                    temp: temp.into_temp_path(),
                                    inner,
                                    source,
                                    hash,
                                })
                            })();
                        let _ = tx.send(outcome.map_err(|e| e.to_string()));
                    });
                    self.entry_work = Some(rx);
                }
            }
            self.archives.push(v);
        }
        if let Some(r) = &self.entry_work {
            match r.try_recv() {
                Ok(res) => {
                    self.entry_work = None;
                    match res {
                        Ok(s) => {
                            let n = name(&s.inner).to_ascii_lowercase();
                            if n.ends_with(".zip") || n.ends_with(".apk") {
                                match s.source.child(s.inner) {
                                    Ok(source) => nested = Some(source),
                                    Err(e) => self.status = e.to_string(),
                                }
                            } else if matches!(
                                s.inner
                                    .extension()
                                    .and_then(|e| e.to_str())
                                    .map(|s| s.to_ascii_lowercase())
                                    .as_deref(),
                                Some("png" | "jpg" | "jpeg" | "gif" | "webp" | "bmp" | "ico")
                            ) {
                                let p = Path::new(&s.temp).to_owned();
                                self.open_item(p, ctx);
                            } else if self.editor.as_ref().is_none_or(|e| !e.unsaved()) {
                                match text::Document::open(Path::new(&s.temp), None) {
                                    Ok(doc) => {
                                        self.editor = Some(Editor {
                                            path: Path::new(&s.temp).to_owned(),
                                            doc,
                                            find: String::new(),
                                            replace: String::new(),
                                            regex: false,
                                            closing: false,
                                            archive_unsaved: false,
                                            publish: None,
                                            staged: Some(s),
                                        })
                                    }
                                    Err(e) => self.status = e.to_string(),
                                }
                            }
                        }
                        Err(e) => self.status = e,
                    }
                }
                Err(mpsc::TryRecvError::Disconnected) => {
                    self.entry_work = None;
                    self.status = "读取包内文件失败".into();
                }
                _ => ctx.request_repaint_after(Duration::from_millis(100)),
            }
        }
        if let Some(source) = nested {
            self.open_archive(source);
        }
        for (root, latest) in observed_revisions {
            for view in &mut self.archives {
                if view.source.root() == root && view.hash != latest && !view.closing {
                    view.start(None);
                }
            }
        }
    }
}
pub(super) struct Staged {
    pub(super) temp: tempfile::TempPath,
    pub(super) source: Source,
    pub(super) inner: PathBuf,
    pub(super) hash: String,
}
