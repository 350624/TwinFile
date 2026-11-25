use super::*;
use crate::motion::ChoiceUi as _;
use twinfile_rust::checksum::{self, Format};

pub(super) struct Properties {
    paths: Vec<PathBuf>,
    totals: Option<Result<files::Totals, String>>,
    work: Receiver<Result<files::Totals, String>>,
    cancel: Arc<AtomicBool>,
}
impl Properties {
    pub(super) fn new(paths: Vec<PathBuf>) -> Self {
        let (tx, work) = mpsc::channel();
        let cancel = Arc::new(AtomicBool::new(false));
        let worker_stop = cancel.clone();
        let selected = paths.clone();
        std::thread::spawn(move || {
            let _ = tx.send(files::aggregate(&selected, &worker_stop).map_err(|e| e.to_string()));
        });
        Self {
            paths,
            totals: None,
            work,
            cancel,
        }
    }
    fn compact(&self, ui: &mut egui::Ui) {
        use crate::theme::miuix as mui;
        let c = mui::Colors::of(ui);
        let mut fields = Vec::<(&str, String)>::new();
        if let [p] = self.paths.as_slice() {
            ui.label(egui::RichText::new(name(p)).size(13.).strong());
            ui.add(
                egui::Label::new(
                    egui::RichText::new(p.display().to_string())
                        .size(mui::DIALOG_NOTE)
                        .color(c.summary),
                )
                .wrap()
                .selectable(true),
            );
            if let Ok(meta) = p.symlink_metadata() {
                fields.push(("类型", if meta.is_dir() { "文件夹" } else { "文件" }.into()));
                fields.push((
                    "只读",
                    if meta.permissions().readonly() {
                        "是"
                    } else {
                        "否"
                    }
                    .into(),
                ));
                if let Ok(t) = meta.modified() {
                    fields.push((
                        "修改时间",
                        super::workbench::local_date(t)
                            .unwrap_or_else(|| httpdate::fmt_http_date(t)),
                    ));
                }
            }
        } else {
            ui.label(egui::RichText::new(format!("已选择 {} 项", self.paths.len())).strong());
        }
        if let Some(Ok(v)) = &self.totals {
            fields.push((
                "逻辑大小",
                format!("{}（{} 字节）", human(v.bytes), v.bytes),
            ));
            fields.push((
                "包含",
                format!("{} 个文件 · {} 个子文件夹", v.files, v.directories),
            ));
            if v.links_skipped > 0 {
                fields.push(("已跳过", format!("{} 个链接或重解析点", v.links_skipped)));
            }
            if v.errors > 0 {
                fields.push(("未完成", format!("{} 处读取失败", v.errors)));
            }
        }
        ui.add_space(3.);
        egui::Frame::new()
            .fill(c.field)
            .corner_radius(11)
            .inner_margin(egui::Margin::symmetric(11, 8))
            .show(ui, |ui| {
                ui.set_width(ui.available_width());
                for (i, (k, v)) in fields.iter().enumerate() {
                    if i > 0 {
                        ui.add_space(1.);
                    }
                    ui.horizontal_top(|ui| {
                        ui.allocate_ui(egui::vec2(66., 16.), |ui| {
                            ui.set_min_width(66.);
                            ui.label(
                                egui::RichText::new(*k)
                                    .size(mui::DIALOG_NOTE)
                                    .color(c.summary),
                            );
                        });
                        ui.add(
                            egui::Label::new(egui::RichText::new(v).size(mui::DIALOG_BODY))
                                .wrap()
                                .selectable(true),
                        );
                    });
                }
                match &self.totals {
                    None => {
                        ui.horizontal(|ui| {
                            ui.spinner();
                            ui.weak("正在统计…");
                            if ui.small_button("停止统计").clicked() {
                                self.cancel.store(true, Relaxed);
                            }
                        });
                    }
                    Some(Err(e)) => {
                        ui.colored_label(c.error, e);
                    }
                    _ => {}
                }
            });
    }
    pub(super) fn show(&mut self, ui: &mut egui::Ui, mi: bool) {
        if self.totals.is_none() {
            match self.work.try_recv() {
                Ok(result) => self.totals = Some(result),
                Err(mpsc::TryRecvError::Disconnected) => {
                    self.totals = Some(Err("统计任务异常退出".into()))
                }
                Err(mpsc::TryRecvError::Empty) => {
                    ui.ctx().request_repaint_after(Duration::from_millis(90))
                }
            }
        }
        if mi {
            self.compact(ui);
            return;
        }

        if let [p] = self.paths.as_slice() {
            ui.strong(name(p));
            ui.label(p.display().to_string());
            if let Ok(m) = p.symlink_metadata() {
                ui.label(if m.is_dir() {
                    "类型：文件夹"
                } else {
                    "类型：文件"
                });
                ui.label(format!(
                    "只读：{}",
                    if m.permissions().readonly() {
                        "是"
                    } else {
                        "否"
                    }
                ));
                if let Ok(t) = m.modified() {
                    ui.label(format!("修改时间：{}", httpdate::fmt_http_date(t)));
                }
            }
        } else {
            ui.strong(format!("已选 {} 项", self.paths.len()));
        }
        match &self.totals {
            Some(Ok(v)) => {
                ui.label(format!("逻辑大小：{}（{} 字节）", human(v.bytes), v.bytes));
                ui.label(format!("{} 个文件 · {} 个子文件夹", v.files, v.directories));
                if v.links_skipped > 0 {
                    ui.label(format!("已跳过 {} 个链接或重解析点", v.links_skipped));
                }
                if v.errors > 0 {
                    ui.label(format!("统计未完成：{} 处读取失败", v.errors));
                }
            }
            Some(Err(e)) => {
                ui.label(e);
            }
            None => {
                ui.horizontal(|ui| {
                    ui.spinner();
                    if ui.button("停止统计").clicked() {
                        self.cancel.store(true, Relaxed);
                    }
                });
            }
        }
    }
}
impl Drop for Properties {
    fn drop(&mut self) {
        self.cancel.store(true, Relaxed);
    }
}

#[derive(Clone, Copy)]
pub(super) enum HashMode {
    Import,
    Paste,
    Export,
}

pub(super) struct HashDialog {
    mode: Option<HashMode>,
    focus_expected: bool,
    path: PathBuf,
    expected: String,
    result: Option<[String; 3]>,
    work: Option<Receiver<Result<[String; 3], String>>>,
    cancel: Arc<AtomicBool>,
    message: String,
    format: Format,
}
impl HashDialog {
    pub(super) fn new(path: PathBuf) -> Self {
        Self {
            path,
            mode: None,
            focus_expected: false,
            expected: String::new(),
            result: None,
            work: None,
            cancel: Arc::default(),
            message: String::new(),
            format: Format::Gnu,
        }
    }

    pub(super) fn open(path: PathBuf, mode: HashMode) -> Option<Self> {
        let mut state = Self::new(path);
        state.mode = Some(mode);
        if matches!(mode, HashMode::Import) {
            let mut picker = rfd::FileDialog::new().set_title("导入校验清单（验证所选文件）");
            if let Some(parent) = state.path.parent() {
                picker = picker.set_directory(parent);
            }
            let manifest = picker.pick_file()?;
            match checksum::import(&manifest) {
                Ok(expected) => state.expected = expected,
                Err(error) => {
                    state.message = error.to_string();
                    return Some(state);
                }
            }
        }
        state.focus_expected = matches!(mode, HashMode::Paste);
        state.calculate();
        Some(state)
    }
    pub(super) fn title(&self) -> &'static str {
        match self.mode {
            Some(HashMode::Import) => "导入并验证校验清单",
            Some(HashMode::Paste) => "粘贴校验值并验证",
            Some(HashMode::Export) => "导出所选文件校验清单",
            None => "文件校验",
        }
    }
    fn calculate(&mut self) {
        let (tx, rx) = mpsc::channel();
        let path = self.path.clone();
        self.cancel = Arc::default();
        let cancel = self.cancel.clone();
        std::thread::spawn(move || {
            let _ = tx.send(files::checksum(&path, &cancel).map_err(|err| err.to_string()));
        });
        self.result = None;
        self.work = Some(rx);
        self.message.clear();
    }
    pub(super) fn show(&mut self, ui: &mut egui::Ui) {
        if let Some(rx) = &self.work {
            match rx.try_recv() {
                Ok(outcome) => {
                    self.work = None;
                    match outcome {
                        Ok(values) => self.result = Some(values),
                        Err(e) => self.message = e,
                    }
                }
                Err(mpsc::TryRecvError::Disconnected) => {
                    self.work = None;
                    self.message = "校验任务意外关闭".into();
                }
                Err(mpsc::TryRecvError::Empty) => {
                    ui.ctx().request_repaint_after(Duration::from_millis(70))
                }
            }
        }
        ui.label(self.path.display().to_string());
        if self.mode.is_some() {
            ui.weak("当前版本仅处理所选的单个文件");
        }
        ui.horizontal_wrapped(|ui| {
            if ui
                .add_enabled(self.work.is_none(), egui::Button::new("计算"))
                .clicked()
            {
                self.calculate();
            }
            if self.work.is_some() {
                ui.spinner();
                if ui.button("停止").clicked() {
                    self.cancel.store(true, Relaxed);
                }
            }
            if ui.button("导入清单…").clicked()
                && let Some(p) = rfd::FileDialog::new().pick_file()
            {
                match checksum::import(&p) {
                    Ok(s) => {
                        self.expected = s;
                        self.message.clear();
                    }
                    Err(e) => self.message = e.to_string(),
                }
            }
        });
        if !matches!(self.mode, Some(HashMode::Export)) {
            let field = ui.add(
                egui::TextEdit::multiline(&mut self.expected)
                    .hint_text("粘贴摘要、GNU 或 BSD 校验清单")
                    .desired_rows(3),
            );
            if std::mem::take(&mut self.focus_expected) {
                field.request_focus();
            }
        }
        if let Some(values) = &self.result {
            let name = name(&self.path);
            let targets = if self.expected.trim().is_empty() {
                [None, None, None]
            } else {
                match checksum::expected(&self.expected, &name) {
                    Ok(targets) => targets,
                    Err(e) => {
                        ui.label(e.to_string());
                        [None, None, None]
                    }
                }
            };
            for (index, (tag, value)) in checksum::TAGS.iter().zip(values).enumerate() {
                ui.horizontal_wrapped(|ui| {
                    ui.label(*tag);
                    ui.monospace(value);
                    if ui.small_button("复制").clicked() {
                        ui.ctx().copy_text(value.clone());
                    }
                    if let Some(expected) = &targets[index] {
                        if expected == value {
                            ui.label("一致");
                        } else {
                            ui.colored_label(ui.visuals().error_fg_color, "不一致");
                        }
                    }
                });
            }
            ui.horizontal(|ui| {
                ui.motion_value(&mut self.format, Format::Gnu, "GNU");
                ui.motion_value(&mut self.format, Format::Bsd, "BSD");
                if ui.button("导出清单…").clicked()
                    && let Some(p) = rfd::FileDialog::new()
                        .set_file_name(format!("{name}.checksums.txt"))
                        .save_file()
                {
                    self.message = checksum::export(&p, &name, values, self.format)
                        .map(|_| format!("已导出到 {}", p.display()))
                        .unwrap_or_else(|e| e.to_string());
                }
            });
        }
        if !self.message.is_empty() {
            ui.label(&self.message);
        }
    }
}
impl Drop for HashDialog {
    fn drop(&mut self) {
        self.cancel.store(true, Relaxed);
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq)]
enum Rule {
    #[default]
    Replace,
    Decorate,
    Number,
}
#[derive(Default)]
pub(super) struct Batch {
    pub(super) rows: Vec<(PathBuf, String)>,
    rule: Rule,
    first: String,
    second: String,
    start: u64,
    digits: usize,
}
impl Batch {
    pub(super) fn new(rows: Vec<(PathBuf, String)>) -> Self {
        Self {
            rows,
            start: 1,
            digits: 2,
            ..Default::default()
        }
    }
    pub(super) fn rules(&mut self, ui: &mut egui::Ui) {
        ui.horizontal_wrapped(|ui| {
            if ui
                .motion_value(&mut self.rule, Rule::Replace, "替换文字")
                .clicked()
                | ui.motion_value(&mut self.rule, Rule::Decorate, "前 / 后缀")
                    .clicked()
                | ui.motion_value(&mut self.rule, Rule::Number, "数字编号")
                    .clicked()
            {
                self.first.clear();
                self.second.clear();
            }
        });
        ui.horizontal_wrapped(|ui| {
            let (one, two) = match self.rule {
                Rule::Replace => ("查找", "替换为"),
                Rule::Decorate | Rule::Number => ("前缀", "后缀"),
            };
            ui.add(
                egui::TextEdit::singleline(&mut self.first)
                    .hint_text(one)
                    .desired_width(105.0),
            );
            ui.add(
                egui::TextEdit::singleline(&mut self.second)
                    .hint_text(two)
                    .desired_width(105.0),
            );
            if self.rule == Rule::Number {
                ui.add(
                    egui::DragValue::new(&mut self.start)
                        .range(0..=1_000_000_000)
                        .prefix("起始 "),
                );
                ui.add(
                    egui::DragValue::new(&mut self.digits)
                        .range(1..=12)
                        .prefix("位数 "),
                );
            }
            let available = self.rule != Rule::Replace || !self.first.is_empty();
            if ui
                .add_enabled(available, egui::Button::new("生成预览"))
                .clicked()
            {
                for (offset, (p, dest)) in self.rows.iter_mut().enumerate() {
                    let original = name(p);
                    let is_file = !p.is_dir();
                    let stem = if is_file {
                        p.file_stem().unwrap_or_default().to_string_lossy()
                    } else {
                        p.file_name().unwrap_or_default().to_string_lossy()
                    };
                    let extension = if is_file {
                        p.extension()
                            .map(|x| format!(".{}", x.to_string_lossy()))
                            .unwrap_or_default()
                    } else {
                        String::new()
                    };
                    *dest = match self.rule {
                        Rule::Replace => original.replace(&self.first, &self.second),
                        Rule::Decorate => format!("{}{stem}{}{extension}", self.first, self.second),
                        Rule::Number => format!(
                            "{}{:0width$}{}{extension}",
                            self.first,
                            self.start + offset as u64,
                            self.second,
                            width = self.digits
                        ),
                    };
                }
            }
        });
    }
}
