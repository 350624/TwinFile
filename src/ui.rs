use crate::motion::{ListMotion, PaintMotion};
use eframe::egui::{self, Key, Modifiers};
use std::{
    collections::BTreeSet,
    fs, io,
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering::Relaxed},
        mpsc::{self, Receiver},
    },
    time::{Duration, Instant},
};
use twinfile_rust::{
    archive,
    config::{self, Location, Settings, Sort, Store, Theme},
    download, files, text,
};
mod archive_ui;
mod compare_ui;
mod dialogs;
mod dock_progress;
mod hfx_tool;
mod image_view;
mod payload_tool;
mod tool_sheet;
use image_view::Preview;
mod local_tools;
mod shell_clipboard;
mod thumbs;
mod visual_smoke;
mod workbench;
type ResultText = Result<String, String>;
struct Tab {
    location: Location,
    undo: Vec<Location>,
    redo: Vec<Location>,
}
struct Pane {
    tabs: Vec<Tab>,
    index: usize,
    input: String,
    items: Vec<files::Entry>,
    selection: BTreeSet<PathBuf>,
    sort: Sort,
    desc: bool,
    pending: Option<Receiver<io::Result<Vec<files::Entry>>>>,
    anchor: Option<usize>,
    list_motion: Option<(ListMotion, Option<f64>)>,
}
impl Pane {
    fn new(data: &config::PaneSettings, base: &Path, h: bool) -> Self {
        let make = |location| Tab {
            location,
            undo: vec![],
            redo: vec![],
        };
        let mut tabs = data
            .tabs
            .iter()
            .filter(|p| {
                p.local()
                    .map_or(*p == &Location::Computer, Path::is_absolute)
            })
            .take(32)
            .cloned()
            .map(make)
            .collect::<Vec<_>>();
        if tabs.is_empty() {
            tabs.push(make(Location::Local(base.into())));
        }
        let mut o = Self {
            index: data.selected.min(tabs.len() - 1),
            tabs,
            input: String::new(),
            items: vec![],
            selection: BTreeSet::new(),
            sort: data.sort,
            desc: data.descending,
            pending: None,
            anchor: None,
            list_motion: None,
        };
        o.switch(h);
        o
    }
    fn location(&self) -> &Location {
        &self.tabs[self.index].location
    }
    fn dir(&self) -> Option<&Path> {
        self.location().local()
    }
    fn parent(&self) -> Option<Location> {
        self.dir().map(|p| {
            p.parent()
                .map_or(Location::Computer, |par| Location::Local(par.into()))
        })
    }
    fn persist(&self) -> config::PaneSettings {
        config::PaneSettings {
            tabs: self.tabs.iter().map(|t| t.location.clone()).collect(),
            selected: self.index,
            sort: self.sort,
            descending: self.desc,
        }
    }
    fn read(&mut self, hidden: bool) {
        let Some(path) = self.dir().map(Path::to_owned) else {
            return;
        };
        let (tx, rx) = mpsc::channel();
        std::thread::spawn(move || {
            let _ = tx.send(files::list_for_display(&path, hidden));
        });
        self.pending = Some(rx);
    }
    fn switch(&mut self, h: bool) {
        self.input = self.location().display();
        self.selection.clear();
        self.items.clear();
        self.pending = None;
        self.anchor = None;
        self.read(h);
    }
    fn switch_motion(&mut self, hidden: bool, motion: ListMotion) {
        self.switch(hidden);
        self.list_motion = Some((motion, None));
    }
    fn refresh_list(&mut self, hidden: bool) {
        self.read(hidden);
        self.list_motion = Some((ListMotion::Refresh, None));
    }
    fn list_paint(&mut self, ctx: &egui::Context, drives_loading: bool) -> PaintMotion {
        if crate::motion::reduced(ctx) {
            self.list_motion = None;
            return PaintMotion::REST;
        }
        if self.pending.is_some() || drives_loading {
            return PaintMotion::REST;
        }
        let Some((motion, start)) = &mut self.list_motion else {
            return PaintMotion::REST;
        };
        let now = ctx.input(|i| i.time);
        let elapsed = now - *start.get_or_insert(now);
        let paint = if crate::theme::keynote::active(ctx) {
            crate::theme::keynote::list_motion(*motion, elapsed)
        } else if crate::theme::soft::active(ctx) {
            crate::theme::soft::list_motion(elapsed)
        } else {
            motion.sample(elapsed)
        };
        if paint.done {
            self.list_motion = None;
        } else {
            ctx.request_repaint();
        }
        paint
    }
    fn goto(&mut self, location: Location, h: bool) {
        if &location == self.location() {
            self.refresh_list(h);
            return;
        }
        let motion = match (self.dir(), location.local()) {
            (_, None) => ListMotion::Leave,
            (Some(old), Some(new)) if old.starts_with(new) => ListMotion::Leave,
            _ => ListMotion::Enter,
        };
        let t = &mut self.tabs[self.index];
        t.undo.push(std::mem::replace(&mut t.location, location));
        t.redo.clear();
        self.switch_motion(h, motion);
    }
    fn history(&mut self, back: bool, h: bool) {
        let t = &mut self.tabs[self.index];
        let (from, to) = if back {
            (&mut t.undo, &mut t.redo)
        } else {
            (&mut t.redo, &mut t.undo)
        };
        if let Some(p) = from.pop() {
            let previous = std::mem::replace(&mut t.location, p);
            let motion = match (previous.local(), t.location.local()) {
                (_, None) => ListMotion::Leave,
                (None, Some(_)) => ListMotion::Enter,
                (Some(old), Some(new)) if old.starts_with(new) => ListMotion::Leave,
                (Some(old), Some(new)) if new.starts_with(old) => ListMotion::Enter,
                _ if back => ListMotion::Leave,
                _ => ListMotion::Enter,
            };
            to.push(previous);
            self.switch_motion(h, motion);
        }
    }
    fn tab(&mut self, h: bool) {
        if self.tabs.len() < 32 {
            self.tabs.push(Tab {
                location: self.location().clone(),
                undo: vec![],
                redo: vec![],
            });
            self.index = self.tabs.len() - 1;
            self.switch_motion(h, ListMotion::Tab);
        }
    }
    fn close_tab(&mut self, h: bool) {
        self.close_at(self.index, h);
    }
    fn close_at(&mut self, index: usize, h: bool) {
        if self.tabs.len() > 1 {
            self.tabs.remove(index);
            if index == self.index {
                self.index = self.index.min(self.tabs.len() - 1);
                self.switch_motion(h, ListMotion::Tab);
            } else if index < self.index {
                self.index -= 1;
            }
        }
    }
}
enum Dialog {
    New(bool, String),
    Rename(PathBuf, String),
    Delete(Vec<PathBuf>),
    Properties(local_tools::Properties),
    Hash(local_tools::HashDialog),
    Batch(local_tools::Batch),

    Search(String),
    Results(Vec<PathBuf>),
    Appearance,
}
struct Editor {
    staged: Option<archive_ui::Staged>,
    archive_unsaved: bool,
    publish: Option<Receiver<Result<String, String>>>,
    path: PathBuf,
    doc: text::Document,
    find: String,
    replace: String,
    regex: bool,
    closing: bool,
}
impl Editor {
    fn unsaved(&self) -> bool {
        self.doc.dirty() || self.archive_unsaved || self.publish.is_some()
    }
}
fn completed(rx: &Receiver<ResultText>) -> Option<ResultText> {
    match rx.try_recv() {
        Ok(result) => Some(result),
        Err(mpsc::TryRecvError::Disconnected) => Some(Err("后台任务意外结束".into())),
        Err(mpsc::TryRecvError::Empty) => None,
    }
}
struct Job {
    meter: Option<Arc<dock_progress::Local>>,
    receiver: Receiver<ResultText>,
    cancel: Arc<AtomicBool>,
    refresh: bool,
}
struct Downloader {
    prefs_root: Option<PathBuf>,
    was_open: bool,
    bound_url: String,
    manual_name: bool,
    usable_dir: Option<PathBuf>,
    suggested_name: Option<String>,
    url: String,
    dest: Option<PathBuf>,
    open: bool,
    work: Option<(Arc<download::Progress>, Receiver<ResultText>)>,
    status: String,
    rate: (Instant, u64, f64),
}
pub struct App {
    archives: Vec<archive_ui::ArchiveView>,
    entry_work: Option<Receiver<Result<archive_ui::Staged, String>>>,
    thumbs: thumbs::Cache,
    comparison: compare_ui::CompareWork,
    payload: payload_tool::Tool,
    hfx: hfx_tool::Tool,
    s: Settings,
    saved: Settings,
    store: Option<Store>,
    config_error: Option<String>,
    panes: [Pane; 2],
    drives: Vec<files::Volume>,
    drive_scan: Option<Receiver<Vec<files::Volume>>>,
    since: Instant,
    status: String,
    dialog: Option<Dialog>,
    job: Option<Job>,
    dl: Downloader,
    dock: dock_progress::Tracker,
    editor: Option<Editor>,
    preview: Option<Preview>,
    standalone: bool,
    browser_ready_frames: u8,
    overlay_exits: crate::motion::OverlayExits,
    workspace_motion: crate::motion::WorkspaceMotion,
    smoke: Option<PathBuf>,
    visual: Option<visual_smoke::VisualSmoke>,
    frame: u8,
    search: Option<Receiver<Result<Vec<PathBuf>, String>>>,
}
impl App {
    pub fn new(
        cc: &eframe::CreationContext<'_>,
        standalone: bool,
        smoke: Option<PathBuf>,
        visual: bool,
        initial: Option<twinfile_rust::bridge::Request>,
    ) -> Self {
        crate::theme::fonts(&cc.egui_ctx);
        let (store, mut s, config_error) = match smoke
            .as_ref()
            .map_or_else(
                || config::config_dir().map(|p| if standalone { p.join("downloader") } else { p }),
                |p| Ok(p.join("config")),
            )
            .and_then(|d| Store::open(&d))
        {
            Ok((st, s, e)) => (Some(st), s, e),
            Err(e) => (None, Settings::default(), Some(e.to_string())),
        };
        let visual_smoke = if visual {
            smoke
                .as_ref()
                .map(|p| visual_smoke::VisualSmoke::new(p.clone()))
        } else {
            None
        };
        if let Some(check) = &visual_smoke {
            if let Some(p) = &smoke {
                for (i, d) in ["left", "right"].into_iter().enumerate() {
                    s.panes[i].tabs = vec![Location::Local(p.join("fixture").join(d))];
                    s.panes[i].selected = 0;
                }
            }
            s.panes[1].tabs = vec![Location::Computer];
            check.begin(&mut s, &cc.egui_ctx);
        } else {
            crate::theme::apply(&cc.egui_ctx, &s);
        }
        if visual_smoke.is_none()
            && let Some([x, y]) = s
                .window_size
                .filter(|[x, y]| (680.0..=10000.0).contains(x) && (460.0..=10000.0).contains(y))
        {
            cc.egui_ctx
                .send_viewport_cmd(egui::ViewportCommand::InnerSize(egui::vec2(x, y)));
        }
        let base = std::env::var_os("USERPROFILE")
            .map(PathBuf::from)
            .filter(|p| p.is_dir())
            .unwrap_or_else(|| "C:/".into());
        let panes = [
            Pane::new(&s.panes[0], &base, s.hidden),
            Pane::new(&s.panes[1], &base, s.hidden),
        ];
        s.active = s.active.min(1);
        let browser_ready_frames = if initial.is_some() { 2 } else { 0 };
        let dl = Downloader {
            prefs_root: smoke
                .as_ref()
                .map(|p| p.join("config"))
                .or_else(|| config::config_dir().ok()),
            was_open: false,
            manual_name: false,
            bound_url: initial.as_ref().map(|r| r.url.clone()).unwrap_or_default(),
            usable_dir: None,
            open: standalone,
            suggested_name: initial
                .as_ref()
                .map(|r| r.filename.clone())
                .filter(|s| !s.is_empty()),
            url: initial.map(|r| r.url).unwrap_or_default(),
            dest: None,
            work: None,
            status: String::new(),
            rate: (Instant::now(), 0, 0.0),
        };
        Self {
            payload: payload_tool::Tool::new(&s),
            hfx: Default::default(),
            s: s.clone(),
            saved: s,
            archives: vec![],
            entry_work: None,
            thumbs: Default::default(),
            comparison: Default::default(),
            store,
            config_error,
            panes,
            drives: vec![],
            drive_scan: (!standalone).then(files::scan_volumes),
            since: Instant::now(),
            status: String::new(),
            dialog: None,
            job: None,
            dl,
            dock: Default::default(),
            editor: None,
            preview: None,
            standalone,
            browser_ready_frames,
            overlay_exits: Default::default(),
            workspace_motion: Default::default(),
            smoke: smoke.filter(|_| !visual),
            visual: visual_smoke,
            frame: 0,
            search: None,
        }
    }
    fn persist(&mut self) {
        self.s.panes = self.panes.each_ref().map(Pane::persist);
        if let Some(st) = &self.store
            && self.s != self.saved
        {
            match st.save(&self.s) {
                Ok(()) => {
                    self.saved = self.s.clone();
                    self.config_error = None;
                }
                Err(e) => {
                    self.config_error = Some(format!("保存设置失败：{}：{e}", st.path.display()));
                }
            }
        }
    }
    fn goto(&mut self, p: PathBuf) {
        let i = self.s.active;
        let p = if p.is_absolute() {
            p
        } else if let Some(dir) = self.panes[i].dir() {
            dir.join(p)
        } else {
            self.status = "请从「此电脑」选择磁盘，或输入绝对路径".into();
            return;
        };
        self.panes[i].goto(Location::Local(p.clone()), self.s.hidden);
        self.s.recent.retain(|x| x != &p);
        self.s.recent.insert(0, p);
        self.s.recent.truncate(32);
    }
    fn navigate(&mut self, location: Location) {
        match location {
            Location::Local(p) => self.goto(p),
            Location::Computer => {
                self.panes[self.s.active].goto(Location::Computer, self.s.hidden);
                self.refresh_volumes();
            }
        }
    }
    fn refresh_volumes(&mut self) {
        if self.drive_scan.is_none() {
            self.drive_scan = Some(files::scan_volumes());
            self.since = Instant::now();
        }
    }
    fn selected(&self) -> Vec<PathBuf> {
        if self.panes[self.s.active].dir().is_none() {
            return vec![];
        }
        self.panes[self.s.active]
            .selection
            .iter()
            .cloned()
            .collect()
    }
    fn one(&mut self) -> Option<PathBuf> {
        let mut v = self.selected();
        if v.len() == 1 {
            v.pop()
        } else {
            self.status = "请选择一个项目".into();
            None
        }
    }
    fn start_job(
        &mut self,
        refresh: bool,
        f: impl FnOnce(&AtomicBool) -> ResultText + Send + 'static,
    ) {
        if self.job.is_some() || self.dl.work.is_some() {
            self.status = "另一项文件操作正在运行".into();
            return;
        }
        let (tx, rx) = mpsc::channel();
        let cancel = Arc::new(AtomicBool::new(false));
        let p = cancel.clone();
        std::thread::spawn(move || {
            let _ = tx.send(f(&p));
        });
        self.job = Some(Job {
            meter: None,
            receiver: rx,
            cancel,
            refresh,
        });
        self.status = "正在处理…".into();
    }
    fn transfer(&mut self, from: Vec<PathBuf>, dir: PathBuf, moving: bool) {
        if from.is_empty() {
            self.status = "未选择文件".into();
            return;
        }
        if self.job.is_some() || self.dl.work.is_some() {
            self.status = "另一项文件操作正在运行".into();
            return;
        }
        let meter = Arc::new(dock_progress::Local::new(moving, from.len()));
        let task = meter.clone();
        self.start_job(true, move |stop| {
            if !moving {
                task.measure(&from, stop)?;
            }

            let mut ok = 0;
            let mut failed = vec![];
            for src in from {
                if files::check_cancel(stop).is_err() {
                    failed.push("已取消".into());
                    break;
                }
                task.begin_item(ok + failed.len());
                match files::transfer_metered(
                    &src,
                    &dir,
                    moving,
                    stop,
                    &|| {
                        if moving {
                            task.measure(std::slice::from_ref(&src), stop)
                                .map_err(io::Error::other)?;
                        }
                        Ok(())
                    },
                    &|n| task.wrote(n),
                ) {
                    Ok(_) => ok += 1,
                    Err(e) => failed.push(format!("{}：{e}", src.display())),
                }
            }
            if failed.is_empty() {
                Ok(format!("已完成 {ok} 项"))
            } else {
                Err(format!("已完成 {ok} 项；{}", failed.join("；")))
            }
        });
        if let Some(j) = &mut self.job {
            j.meter = Some(meter);
        }
    }
    fn other(&mut self, moving: bool) {
        let Some(dest) = self.panes[1 - self.s.active].dir() else {
            self.status = "请先在目标栏打开一个文件夹".into();
            return;
        };
        self.transfer(self.selected(), dest.to_owned(), moving);
    }
    fn refresh(&mut self) {
        let p = &mut self.panes[self.s.active];
        p.refresh_list(self.s.hidden);
        if p.dir().is_none() {
            self.refresh_volumes();
        }
    }
    fn batch(&mut self) {
        let r = self
            .selected()
            .into_iter()
            .map(|p| {
                let n = name(&p);
                (p, n)
            })
            .collect::<Vec<_>>();
        if !r.is_empty() {
            self.dialog = Some(Dialog::Batch(local_tools::Batch::new(r)));
        }
    }
    fn poll(&mut self, ctx: &egui::Context) {
        if let Some(scan) = &self.drive_scan {
            match scan.try_recv() {
                Ok(drives) => {
                    self.drives = drives;
                    self.drive_scan = None;
                }
                Err(mpsc::TryRecvError::Disconnected) => self.drive_scan = None,
                Err(mpsc::TryRecvError::Empty) => {}
            }
        }
        if self.since.elapsed() >= Duration::from_secs(5) && !self.standalone {
            self.refresh_volumes();
        }
        for p in &mut self.panes {
            if let Some(r) = &p.pending {
                match r.try_recv() {
                    Ok(result) => {
                        p.pending = None;
                        match result {
                            Ok(mut v) => {
                                files::sort(&mut v, p.sort, p.desc);
                                p.items = v;
                                let visible =
                                    p.items.iter().map(|e| &e.path).collect::<BTreeSet<_>>();
                                p.selection.retain(|x| visible.contains(x));
                            }
                            Err(e) => {
                                self.status = format!("无法读取 {}：{e}", p.location().display())
                            }
                        }
                    }
                    Err(mpsc::TryRecvError::Disconnected) => p.pending = None,
                    _ => {}
                }
            }
        }
        let now = Instant::now();
        let mut samples = Vec::new();
        if let Some((p, _)) = &self.dl.work {
            if let Ok(mut path) = p.resolved_path.lock()
                && let Some(path) = path.take()
            {
                self.dl.dest = Some(path);
            }
            samples.push(dock_progress::Sample::bytes(
                dock_progress::Kind::Download,
                p.received.load(Relaxed),
                p.total.load(Relaxed),
            ));
        }
        if let Some(m) = self.job.as_ref().and_then(|j| j.meter.as_ref()) {
            samples.push(m.sample());
        }
        if let Some(s) = self.hfx.dock_sample() {
            samples.push(s);
        }
        if let Some(s) = self.payload.dock_sample() {
            samples.push(s);
        }
        self.dock.observe(&samples, now);
        if let Some((kind, ok)) = self.payload.poll(ctx) {
            self.dock.finish(kind, ok, now);
        }
        if let Some((kind, ok)) = self.hfx.poll(ctx) {
            self.dock.finish(kind, ok, now);
        }
        if let Some(j) = &self.job
            && let Some(result) = completed(&j.receiver)
        {
            if let Some(m) = &j.meter {
                self.dock.finish(m.kind(), result.is_ok(), now);
            }
            let refresh = j.refresh;
            self.job = None;
            self.status = result.unwrap_or_else(|e| e);
            if refresh {
                for p in &mut self.panes {
                    p.read(self.s.hidden);
                }
            }
        }
        if let Some(s) = &self.search
            && let Ok(result) = s.try_recv()
        {
            self.search = None;
            match result {
                Ok(r) => {
                    if self.status == "正在搜索…" {
                        self.status.clear();
                    }
                    self.dialog = Some(Dialog::Results(r));
                }
                Err(e) => self.status = e,
            }
        }
        if let Some((_, rx)) = &self.dl.work
            && let Some(result) = completed(rx)
        {
            self.dock
                .finish(dock_progress::Kind::Download, result.is_ok(), now);
            // A short download can finish between the progress sample above and receipt of its result.
            if let Some((p, _)) = &self.dl.work
                && let Ok(mut path) = p.resolved_path.lock()
                && let Some(path) = path.take()
            {
                self.dl.dest = Some(path);
            }
            self.dl.work = None;
            self.dl.status = result.unwrap_or_else(|x| x);
            self.refresh();
        }
        let pending = self.job.is_some()
            || self.dl.work.is_some()
            || self.search.is_some()
            || self.drive_scan.is_some()
            || self.panes.iter().any(|p| p.pending.is_some());
        ctx.request_repaint_after(if pending {
            Duration::from_millis(50)
        } else {
            Duration::from_secs(1)
        });
    }
}
impl eframe::App for App {
    fn raw_input_hook(&mut self, _ctx: &egui::Context, input: &mut egui::RawInput) {
        if self.workspace_motion.closing() {
            input
                .events
                .retain(|e| matches!(e, egui::Event::WindowFocused(_)));
        }
        if let Some(visual) = &mut self.visual {
            visual.inject(input);
        }
    }
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.poll(ctx);
        self.thumbs.poll(ctx);
        let closing = ctx.input(|i| i.viewport().close_requested());
        if closing {
            self.hfx.cancel();
            self.payload.cancel();
        }
        let busy = self.job.is_some()
            || self.payload.busy()
            || self.hfx.busy()
            || self.dl.work.is_some()
            || self.archives.iter().any(|v| v.busy())
            || self.editor.as_ref().is_some_and(|e| e.unsaved());
        if busy {
            self.workspace_motion.cancel_close();
        }
        if closing && !busy {
            self.workspace_motion.begin_close(ctx);
        }
        if closing && busy {
            ctx.send_viewport_cmd(egui::ViewportCommand::CancelClose);
            if let Some(j) = &self.job {
                j.cancel.store(true, Relaxed);
            }
            if let Some((p, _)) = &self.dl.work {
                p.cancel.store(true, Relaxed);
            }
            if let Some(e) = &mut self.editor {
                e.closing = e.unsaved();
            }
        }
        if !matches!(self.s.theme, Theme::Soft | Theme::Miuix | Theme::Keynote)
            || !self.status.is_empty()
            || self.config_error.is_some()
            || self.job.is_some()
        {
            let soft = self.s.theme == Theme::Soft;
            let panel = egui::TopBottomPanel::bottom("status");
            let panel = if soft {
                panel.frame(
                    egui::Frame::new()
                        .fill(ctx.style().visuals.panel_fill)
                        .inner_margin(egui::Margin::symmetric(12, 5)),
                )
            } else {
                panel
            };
            panel.show(ctx, |ui| {
                let contents = |ui: &mut egui::Ui| {
                    if let Some(e) = &self.config_error {
                        ui.colored_label(
                            if self.s.theme == Theme::Keynote {
                                crate::theme::keynote::TEXT
                            } else {
                                egui::Color32::from_rgb(235, 100, 70)
                            },
                            e,
                        );
                    }
                    if let Some(j) = &self.job {
                        ui.horizontal(|ui| {
                            ui.spinner();
                            if ui.button("取消文件任务").clicked() {
                                j.cancel.store(true, Relaxed);
                            }
                        });
                    }
                    if !self.status.is_empty() {
                        ui.label(&self.status);
                    }
                };
                if soft {
                    crate::theme::soft::card_frame(ui, 8)
                        .show(ui, |ui| ui.horizontal_wrapped(|ui| contents(ui)));
                } else {
                    contents(ui);
                }
            });
        }
        if !self.standalone {
            if let Some(visual) = &mut self.visual {
                visual.prepare_close(&mut self.dialog, &mut self.s, ctx);
                visual.motion_input(&mut self.panes, &mut self.s, ctx);
            }
            self.workbench(ctx);
            self.dialogs(ctx);
            self.archives(ctx);
            self.editor(ctx);
            self.preview(ctx);
            self.compare_ui(ctx);
            self.payload.show(ctx, &mut self.s);
            self.hfx.show(ctx, self.s.theme == Theme::Miuix);
        } else {
            egui::CentralPanel::default().show(ctx, |ui| {
                ui.heading("TwinFile Rust 下载器");
            });
        }
        self.show_download(ctx);
        self.overlay_exits.paint(ctx);
        self.workspace_motion.paint(ctx, &self.s);
        if self.browser_ready_frames > 0 {
            self.browser_ready_frames -= 1;
            if self.browser_ready_frames > 0 {
                ctx.request_repaint();
            } else if twinfile_rust::bridge::send(
                std::io::stdout().lock(),
                &twinfile_rust::bridge::Response {
                    accepted: self.dl.open && !self.dl.url.is_empty(),
                },
            )
            .is_err()
            {
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            }
        }
        if !self.standalone
            && let Some(size) = ctx.input(|i| i.viewport().inner_rect.map(|r| r.size()))
            && size.x >= 680.0
            && size.y >= 460.0
        {
            self.s.window_size = Some([size.x.round(), size.y.round()]);
        }
        if let Some(visual) = &mut self.visual {
            visual.frame(
                self.drive_scan.is_none() && self.panes.iter().all(|p| p.pending.is_none()),
                &mut self.s,
                ctx,
            );
        } else {
            self.persist();
        }
        if let Some(dir) = &self.smoke {
            self.frame = self.frame.saturating_add(1);
            let reloading = dir.join("ready.txt").is_file();
            if self.frame == 1 && !reloading {
                self.s.theme = Theme::Miuix;
                self.s.dark = true;
                self.panes[1].goto(Location::Computer, self.s.hidden);
                crate::theme::apply(ctx, &self.s);
            }
            if self.frame >= 4 {
                let persisted = fs::read(dir.join("config/settings.json"))
                    .ok()
                    .and_then(|b| serde_json::from_slice::<Settings>(&b).ok())
                    .is_some_and(|s| {
                        s.theme == Theme::Miuix && s.dark && s.panes[1].tabs == [Location::Computer]
                    });
                if persisted
                    && self.s.theme == Theme::Miuix
                    && self.s.dark
                    && self.panes[1].location() == &Location::Computer
                {
                    let _ = fs::write(
                        dir.join(if reloading {
                            "reopened.txt"
                        } else {
                            "ready.txt"
                        }),
                        "TwinFile Rust GUI rendered; Miuix dark and This PC tab persisted.",
                    );
                }
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            } else {
                ctx.request_repaint();
            }
        }
    }
}
fn name(p: &Path) -> String {
    p.file_name()
        .unwrap_or(p.as_os_str())
        .to_string_lossy()
        .into_owned()
}
fn human(n: u64) -> String {
    let mut f = n as f64;
    for u in ["B", "KiB", "MiB", "GiB", "TiB"] {
        if f < 1024.0 || u == "TiB" {
            return if u == "B" {
                format!("{n} B")
            } else {
                format!("{f:.1} {u}")
            };
        }
        f /= 1024.0;
    }
    unreachable!()
}

#[cfg(test)]
mod pane_tests {
    use super::*;

    #[test]
    fn computer_tab_keeps_navigation_history_and_has_no_filesystem_target() {
        let base = tempfile::tempdir().unwrap();
        let mut p = Pane::new(&config::PaneSettings::default(), base.path(), false);
        p.goto(Location::Computer, false);
        assert_eq!(p.input, "此电脑");
        assert!(p.dir().is_none());
        assert!(p.pending.is_none());
        assert!(p.items.is_empty());
        assert!(p.parent().is_none());
        p.goto(Location::Local(base.path().join("sample")), false);
        p.history(true, false);
        assert_eq!(p.location(), &Location::Computer);
        assert!(p.pending.is_none());
        p.history(false, false);
        assert_eq!(p.dir(), Some(base.path().join("sample").as_path()));
        p.history(true, false);
        p.tab(false);
        assert_eq!(p.index, 1);
        assert_eq!(p.location(), &Location::Computer);
        p.close_at(0, false);
        assert_eq!(p.index, 0);
        assert_eq!(p.persist().tabs, [Location::Computer]);
    }
}
