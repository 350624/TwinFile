use super::*;
use crate::motion::ChoiceUi as _;
use twinfile_rust::compare::{self, Line, Row, Rule, State};
pub(super) enum Compare {
    Pick {
        text: bool,
        rule: Rule,
        space: bool,
    },
    Directory {
        left: PathBuf,
        right: PathBuf,
        rows: Vec<Row>,
        changed: bool,
    },
    Text {
        left: Vec<String>,
        right: Vec<String>,
        rows: Vec<Line>,
        changed: bool,
    },
}
type ComparePayload = Result<Compare, String>;
pub(super) struct CompareWork {
    pub(super) view: Option<Compare>,
    pub(super) result: Option<Receiver<ComparePayload>>,
    pub(super) stop: Arc<AtomicBool>,
}
impl Default for CompareWork {
    fn default() -> Self {
        Self {
            view: None,
            result: None,
            stop: Arc::new(AtomicBool::new(false)),
        }
    }
}
impl App {
    pub(super) fn open_compare(&mut self) {
        self.open_compare_mode(false);
    }
    pub(super) fn open_compare_mode(&mut self, text: bool) {
        if self.comparison.result.is_none() {
            self.comparison.view = Some(Compare::Pick {
                text,
                rule: Rule::Sha256,
                space: false,
            });
        }
    }
    pub(super) fn compare_ui(&mut self, ctx: &egui::Context) {
        if let Some(rx) = &self.comparison.result {
            match rx.try_recv() {
                Ok(r) => {
                    self.comparison.result = None;
                    match r {
                        Ok(v) => self.comparison.view = Some(v),
                        Err(e) => self.status = e,
                    }
                }
                Err(mpsc::TryRecvError::Disconnected) => {
                    self.comparison.result = None;
                    self.status = "比较任务异常结束".into();
                }
                _ => {
                    ctx.request_repaint_after(Duration::from_millis(60));
                }
            }
        }
        let mut open = true;
        if self.comparison.result.is_some() {
            egui::Window::new("比较").open(&mut open).show(ctx, |ui| {
                ui.spinner();
                if ui.button("取消").clicked() {
                    self.comparison.stop.store(true, Relaxed);
                }
            });
            if !open {
                self.comparison.stop.store(true, Relaxed);
            }
            return;
        }
        let Some(mut page) = self.comparison.view.take() else {
            return;
        };
        let mut start = None;
        egui::Window::new("比较左右栏")
            .id(egui::Id::new("local-compare"))
            .open(&mut open)
            .default_width(640.0)
            .show(ctx, |ui| {
                match &mut page {
                    Compare::Pick { text, rule, space } => {
                        ui.horizontal(|ui| {
                            ui.motion_value(text, false, "当前目录");
                            ui.motion_value(text, true, "各选一个文本文件");
                        });
                        if *text {
                            ui.motion_check(space, "忽略空白");
                        } else {
                            ui.horizontal(|ui| {
                                for (r, n) in [
                                    (Rule::Size, "大小"),
                                    (Rule::Modified, "大小 + 修改时间"),
                                    (Rule::Sha256, "SHA256"),
                                ] {
                                    ui.motion_value(rule, r, n);
                                }
                            });
                        }
                        if ui.button("开始比较").clicked() {
                            if *text {
                                let pair = self.panes.each_ref().map(|p| {
                                    let mut s = p.selection.iter();
                                    let first = s.next()?.clone();
                                    if s.next().is_none() && first.is_file() {
                                        Some(first)
                                    } else {
                                        None
                                    }
                                });
                                if let [Some(a), Some(b)] = pair {
                                    start = Some((a, b, true, *rule, *space));
                                } else {
                                    self.status = "请在左右栏各选一个文本文件".into();
                                }
                            } else if let (Some(left), Some(right)) =
                                (self.panes[0].dir(), self.panes[1].dir())
                            {
                                start = Some((left.into(), right.into(), false, *rule, *space));
                            } else {
                                self.status = "请在左右栏各打开一个文件夹".into();
                            }
                        }
                    }
                    Compare::Directory {
                        left,
                        right,
                        rows,
                        changed,
                    } => {
                        ui.label(format!("{}  ↔  {}", left.display(), right.display()));
                        ui.horizontal(|ui| {
                            ui.motion_check(changed, "仅显示差异");
                            if ui.button("导出 TSV…").clicked()
                                && let Some(dst) = rfd::FileDialog::new()
                                    .set_file_name("comparison.tsv")
                                    .save_file()
                            {
                                let mut data = String::from("状态\t类型\t相对路径\n");
                                for r in rows.iter() {
                                    data.push_str(&format!(
                                        "{}\t{}\t{}\n",
                                        r.state.name(),
                                        if r.directory { "目录" } else { "文件" },
                                        r.relative.display()
                                    ));
                                }
                                self.status = publish(&dst, data.as_bytes())
                                    .map(|_| "已导出".into())
                                    .unwrap_or_else(|e| e.to_string());
                            }
                        });
                        let shown = rows
                            .iter()
                            .filter(|r| !*changed || r.state != State::Same)
                            .collect::<Vec<_>>();
                        egui::ScrollArea::vertical().max_height(410.0).show_rows(
                            ui,
                            23.0,
                            shown.len(),
                            |ui, range| {
                                for i in range {
                                    let r = shown[i];
                                    let label =
                                        format!("{}  {}", r.state.name(), r.relative.display());
                                    let a = left.join(&r.relative);
                                    let b = right.join(&r.relative);
                                    if ui.motion_label(false, label).double_clicked() {
                                        if a.is_dir() && b.is_dir() {
                                            start = Some((a, b, false, Rule::Sha256, false));
                                        } else if a.is_file() && b.is_file() {
                                            start = Some((a, b, true, Rule::Sha256, false));
                                        }
                                    }
                                }
                            },
                        );
                    }
                    Compare::Text {
                        left,
                        right,
                        rows,
                        changed,
                    } => {
                        ui.motion_check(changed, "仅显示差异");
                        let shown = rows
                            .iter()
                            .filter(|r| !*changed || !matches!(r, Line::Equal(..)))
                            .collect::<Vec<_>>();
                        egui::ScrollArea::both().max_height(420.0).show_rows(
                            ui,
                            23.0,
                            shown.len(),
                            |ui, range| {
                                for i in range {
                                    let (a, b) = match *shown[i] {
                                        Line::Equal(a, b) => (Some((a, " ")), Some((b, " "))),
                                        Line::Replace(a, b) => (Some((a, "−")), Some((b, "+"))),
                                        Line::Remove(a) => (Some((a, "−")), None),
                                        Line::Add(b) => (None, Some((b, "+"))),
                                    };
                                    ui.columns(2, |cols| {
                                        for (j, item) in [a, b].into_iter().enumerate() {
                                            if let Some((k, sign)) = item {
                                                let line =
                                                    if j == 0 { &left[k] } else { &right[k] };
                                                let label = egui::RichText::new(format!(
                                                    "{sign} {:>4}  {line}",
                                                    k + 1
                                                ))
                                                .monospace();
                                                let label = match sign {
                                                    "−" => label.color(egui::Color32::from_rgb(
                                                        218, 70, 78,
                                                    )),
                                                    "+" => label.color(egui::Color32::from_rgb(
                                                        41, 155, 92,
                                                    )),
                                                    _ => label,
                                                };
                                                cols[j].add(egui::Label::new(label).truncate());
                                            }
                                        }
                                    });
                                }
                            },
                        );
                    }
                }
                ui.label(&self.status);
            });
        if let Some((left, right, text, rule, space)) = start {
            let cancel = Arc::new(AtomicBool::new(false));
            self.comparison.stop = cancel.clone();
            let (tx, rx) = mpsc::channel();
            let hidden = self.s.hidden;
            std::thread::spawn(move || {
                let task = || -> io::Result<Compare> {
                    if text {
                        let a = text::Document::open(&left, None)?.body;
                        let b = text::Document::open(&right, None)?.body;
                        let rows = compare::lines(&a, &b, space, &cancel)?;
                        Ok(Compare::Text {
                            left: a.split('\n').map(str::to_owned).collect(),
                            right: b.split('\n').map(str::to_owned).collect(),
                            rows,
                            changed: false,
                        })
                    } else {
                        let rows = compare::directory(&left, &right, hidden, rule, &cancel)?;
                        Ok(Compare::Directory {
                            left,
                            right,
                            rows,
                            changed: false,
                        })
                    }
                };
                let _ = tx.send(task().map_err(|e| e.to_string()));
            });
            self.comparison.result = Some(rx);
            self.status.clear();
        } else if open {
            self.comparison.view = Some(page);
        }
    }
}
fn publish(p: &Path, data: &[u8]) -> io::Result<()> {
    use std::io::Write;
    let mut tmp = tempfile::NamedTempFile::new_in(
        p.parent().ok_or_else(|| io::Error::other("缺少保存目录"))?,
    )?;
    tmp.write_all(data)?;
    tmp.as_file().sync_all()?;
    tmp.persist_noclobber(p).map_err(|e| e.error)?;
    Ok(())
}
