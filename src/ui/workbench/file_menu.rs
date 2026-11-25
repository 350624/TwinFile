use super::*;

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub(super) enum Command {
    Open,
    System,
    Text,
    Props,
    Copy,
    Cut,
    Paste,
    Rename,
    Batch,
    Bin,
    Zip,
    Extract,
    Hash,
    Refresh,
    NewFile,
    NewDir,
}
use Command::*;
impl Command {
    fn label(self) -> &'static str {
        match self {
            Open => "打开",
            System => "系统打开",
            Text => "文本编辑",
            Props => "属性",
            Copy => "复制",
            Cut => "剪切",
            Paste => "粘贴",
            Rename => "重命名",
            Batch => "批量重命名",
            Bin => "移到回收站…",
            Zip => "压缩为 ZIP…",
            Extract => "解压 ZIP / APK…",
            Hash => "校验值",
            Refresh => "刷新当前栏",
            NewFile => "新建文件",
            NewDir => "新建文件夹",
        }
    }
    fn key(self) -> &'static str {
        match self {
            Copy => "Ctrl+C",
            Cut => "Ctrl+X",
            Paste => "Ctrl+V",
            Props => "Space",
            Batch => "Ctrl+M",
            Bin => "Del",
            Refresh => "F2",
            _ => "",
        }
    }
    fn icon(self) -> Mark {
        match self {
            Open | NewDir => Mark::Folder,
            System => Mark::Screen,
            Text | Rename | NewFile => Mark::Document,
            Props => Mark::Info,
            Copy | Batch => Mark::Copy,
            Cut => Mark::Cut,
            Paste => Mark::Paste,
            Bin => Mark::Trash,
            Zip => Mark::Archive,
            Extract => Mark::Download,
            Hash => Mark::Checked,
            Refresh => Mark::Refresh,
        }
    }
}

fn groups(item: Option<&Path>, directory: bool, count: usize) -> Vec<Vec<Command>> {
    if item.is_none() {
        return vec![vec![Paste], vec![NewDir, NewFile], vec![Props, Refresh]];
    }
    let single = count == 1;
    let mut out = vec![vec![Copy, Cut, Paste]];
    if single {
        let mut open = vec![Open, System];
        if !directory {
            open.push(Text);
        }
        out.push(open);
    }
    let mut manage = vec![];
    if single {
        manage.push(Rename);
    }
    manage.push(Batch);
    out.push(manage);
    let mut tools = vec![Zip];
    if single && !directory {
        let n = item
            .and_then(Path::file_name)
            .unwrap_or_default()
            .to_string_lossy()
            .to_ascii_lowercase();
        if n.ends_with(".zip")
            || n.ends_with(".apk")
            || n.rsplit_once(".apk.")
                .is_some_and(|(_, s)| !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit()))
        {
            tools.push(Extract);
        }
        tools.push(Hash);
    }
    out.extend([tools, vec![Props, Refresh], vec![Bin]]);
    out
}

pub(super) fn show(
    r: &egui::Response,
    parent: &egui::Ui,
    pane: usize,
    item: Option<&Path>,
    dir: bool,
    count: usize,
    mi: bool,
) -> Option<Command> {
    let ctx = parent.ctx();
    #[cfg(test)]
    ctx.data_mut(|d| d.insert_temp(egui::Id::new(("file-menu-anchor", pane, item)), r.rect));
    let width = (ctx.content_rect().width() - 28.).clamp(96., 224.);
    let height = (ctx.content_rect().height() - 34.).max(64.);
    let mut popup = egui::Popup::context_menu(r)
        .id(r.id.with(("file-actions", pane, item)))
        .width(width + 16.);
    if mi {
        let c = mui::Colors::of(parent);
        popup = popup.style(miuix_menu_style).frame(
            egui::Frame::popup(parent.style())
                .fill(c.container)
                .stroke(egui::Stroke::new(1_f32, c.outline))
                .inner_margin(8)
                .corner_radius(16),
        );
    }
    popup
        .show(|ui| {
            ui.set_width(width);
            ui.spacing_mut().item_spacing.y = 0.;
            egui::ScrollArea::vertical()
                .max_height(height)
                .auto_shrink([false, true])
                .show(ui, |ui| {
                    let mut picked = None;
                    for (g, commands) in groups(item, dir, count).into_iter().enumerate() {
                        if g > 0 {
                            if mi {
                                miuix_menu_divider(ui);
                            } else {
                                ui.separator();
                            }
                        }
                        if mi && g == 0 && commands.len() == 3 {
                            ui.horizontal(|ui| {
                                ui.spacing_mut().item_spacing.x = 4.;
                                let w = (ui.available_width() - 8.) / 3.;
                                for cmd in commands {
                                    if row(ui, cmd, true, w).clicked() {
                                        picked = Some(cmd);
                                    }
                                }
                            });
                        } else {
                            for cmd in commands {
                                let r = if mi {
                                    row(ui, cmd, false, ui.available_width())
                                } else {
                                    ui.button(cmd.label())
                                };
                                if r.clicked() {
                                    picked = Some(cmd);
                                }
                            }
                        }
                    }
                    if picked.is_some() {
                        ui.close();
                    }
                    picked
                })
                .inner
        })
        .and_then(|out| out.inner)
}
fn row(ui: &mut egui::Ui, cmd: Command, tile: bool, width: f32) -> egui::Response {
    let (rect, r) = ui.allocate_exact_size(
        egui::vec2(width, if tile { 48. } else { 29. }),
        egui::Sense::click(),
    );
    #[cfg(test)]
    ui.ctx()
        .data_mut(|d| d.insert_temp(egui::Id::new(("file-menu-action", cmd.label())), rect));
    r.widget_info(|| {
        egui::WidgetInfo::labeled(egui::WidgetType::Button, ui.is_enabled(), cmd.label())
    });
    let c = mui::Colors::of(ui);
    let (hover, press) = crate::motion::feedback(ui, &r);
    if ui.is_rect_visible(rect) {
        let p = ui.painter();
        let base = if tile { c.field } else { c.container };
        let amount = if r.has_focus() { 1. } else { hover.max(press) };
        if tile || amount > 0. {
            p.add(mui::squircle(
                rect.shrink(0.6 + press),
                if tile { 11. } else { 9. },
                base.lerp_to_gamma(c.selected, amount),
                egui::Stroke::NONE,
            ));
        }
        let icon = if tile {
            rect.center() - egui::vec2(0., 8.)
        } else {
            rect.left_center() + egui::vec2(17., 0.)
        };
        let ink = if cmd == Bin { c.error } else { c.on_surface };
        draw_mark(p, icon, cmd.icon(), if tile { c.field_ink } else { ink });
        if tile {
            p.text(
                rect.center() + egui::vec2(0., 10.),
                egui::Align2::CENTER_CENTER,
                cmd.label(),
                mui::font(11.5),
                ink,
            );
        } else {
            p.text(
                rect.left_center() + egui::vec2(35., 0.),
                egui::Align2::LEFT_CENTER,
                cmd.label(),
                mui::font(13.),
                ink,
            );
            p.text(
                rect.right_center() - egui::vec2(9., 0.),
                egui::Align2::RIGHT_CENTER,
                cmd.key(),
                mui::font(11.),
                c.summary,
            );
        }
    }
    if tile {
        r.on_hover_text(format!("{}  {}", cmd.label(), cmd.key()))
    } else {
        r
    }
}
impl App {
    pub(super) fn run_file_command(
        &mut self,
        cmd: Command,
        item: Option<PathBuf>,
        ctx: &egui::Context,
    ) {
        match cmd {
            Open | System | Text | Rename | Hash | Extract => {
                let Some(p) = item.filter(|p| {
                    self.panes[self.s.active].selection.len() == 1
                        && self.panes[self.s.active].selection.contains(p)
                }) else {
                    self.status = "所选项目已改变，请重新右击".into();
                    return;
                };
                match cmd {
                    Open => self.open_item(p, ctx),
                    System => {
                        if let Err(e) = system_open(&p) {
                            self.status = e.to_string()
                        }
                    }
                    Text => self.open_text(p),
                    Rename => self.dialog = Some(Dialog::Rename(p.clone(), name(&p))),
                    Hash => self.dialog = Some(Dialog::Hash(local_tools::HashDialog::new(p))),
                    Extract => {
                        if let Some(folder) = rfd::FileDialog::new()
                            .set_title("选择解压位置（将新建同名目录）")
                            .pick_folder()
                        {
                            let Some(stem) = p.file_stem() else { return };
                            let dest = folder.join(stem);
                            self.start_job(true, move |stop| {
                                archive::extract(&p, &dest, stop)
                                    .map(|_| format!("已解压到 {}", dest.display()))
                                    .map_err(|e| e.to_string())
                            });
                        }
                    }
                    _ => unreachable!(),
                }
            }
            Props => {
                let paths = if item.is_some() {
                    self.selected()
                } else {
                    self.panes[self.s.active]
                        .dir()
                        .map(Path::to_owned)
                        .into_iter()
                        .collect()
                };
                if !paths.is_empty() {
                    self.dialog = Some(Dialog::Properties(local_tools::Properties::new(paths)));
                }
            }
            Copy | Cut => self.clip_files(cmd == Cut),
            Paste => self.paste_files(),
            Batch => self.batch(),
            Zip => self.make_zip(),
            Refresh => self.refresh(),
            Bin => {
                let v = self.selected();
                if !v.is_empty() {
                    self.dialog = Some(Dialog::Delete(v));
                }
            }
            NewDir | NewFile => self.new_item_kind(cmd == NewDir),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn context_matrix_for_folder_file_archive_and_multi() {
        let cmds = |n: &str, dir, count| {
            groups(Some(Path::new(n)), dir, count)
                .into_iter()
                .flatten()
                .collect::<Vec<_>>()
        };
        for n in ["a.ZiP", "x.apk", "x.APK.01"] {
            assert!(cmds(n, false, 1).contains(&Extract));
        }
        assert!(!cmds("a.apk.bak", false, 1).contains(&Extract));
        for op in [Text, Hash, Extract] {
            assert!(!cmds("fake.zip", true, 1).contains(&op));
        }
        let many = cmds("a.zip", false, 2);
        for cmd in [Open, System, Text, Rename, Hash, Extract] {
            assert!(!many.contains(&cmd));
        }
        for cmd in [Copy, Cut, Paste, Batch, Bin, Zip, Props, Refresh] {
            assert!(many.contains(&cmd));
        }
        assert_eq!(
            groups(None, true, 0).concat(),
            [Paste, NewDir, NewFile, Props, Refresh]
        );
    }
    struct Harness {
        app: App,
        ctx: egui::Context,
        t: f64,
    }
    impl Harness {
        fn frame(&mut self, events: Vec<egui::Event>) {
            self.t += 0.03;
            let _ = self.ctx.run(
                egui::RawInput {
                    screen_rect: Some(egui::Rect::from_min_size(
                        egui::Pos2::ZERO,
                        egui::vec2(960., 640.),
                    )),
                    time: Some(self.t),
                    events,
                    ..Default::default()
                },
                |ctx| {
                    egui::CentralPanel::default().show(ctx, |ui| {
                        ui.columns(2, |col| {
                            self.app
                                .pane(0, &mut col[0], f32::from(self.app.s.active == 0));
                            self.app
                                .pane(1, &mut col[1], f32::from(self.app.s.active == 1));
                        });
                    });
                },
            );
        }
        fn click(&mut self, id: egui::Id, b: egui::PointerButton) {
            let pos = self
                .ctx
                .data(|d| d.get_temp::<egui::Rect>(id))
                .expect("visible menu target")
                .center();
            self.frame(vec![egui::Event::PointerMoved(pos)]);
            for pressed in [true, false] {
                self.frame(vec![egui::Event::PointerButton {
                    pos,
                    button: b,
                    pressed,
                    modifiers: Default::default(),
                }]);
            }
            for _ in 0..3 {
                self.frame(vec![]);
            }
        }
        fn open(&mut self, p: Option<&Path>) {
            self.click(
                egui::Id::new(("file-menu-anchor", 1usize, p)),
                egui::PointerButton::Secondary,
            );
        }
        fn pick(&mut self, cmd: Command) {
            self.click(
                egui::Id::new(("file-menu-action", cmd.label())),
                egui::PointerButton::Primary,
            );
        }
    }
    #[test]
    #[ignore = "Requires TWINFILE_OPEN_FIXTURE: harmless GUI exe appending x to opened.marker"]
    fn shell_double_click_in_either_pane() {
        let fixture = std::env::var_os("TWINFILE_OPEN_FIXTURE").unwrap();
        let tmp = tempfile::tempdir().unwrap();
        let ctx = egui::Context::default();
        let cc = eframe::CreationContext::_new_kittest(ctx.clone());
        let app = App::new(&cc, true, Some(tmp.path().to_owned()), false, None);
        let mut h = Harness { app, ctx, t: 0. };
        h.app.s.reduce_motion = true;
        for pane in 0..2 {
            h.t += 2.; // Each pane gets a separate double-click, not a triple-click.
            let dir = tmp.path().join(format!("面板 {pane}"));
            fs::create_dir(&dir).unwrap();
            let path = dir.join("双击 & 测试.exe");
            fs::copy(&fixture, &path).unwrap();
            h.app.panes[pane].goto(Location::Local(dir.clone()), false);
            h.app.panes[pane].pending = None;
            h.app.panes[pane].items = files::list(&dir, false).unwrap();
            h.app.s.active = 1 - pane;
            for _ in 0..4 {
                h.frame(vec![]);
            }
            let id = egui::Id::new(("file-menu-anchor", pane, Some(path.as_path())));
            let marker = dir.join("opened.marker");
            h.click(id, egui::PointerButton::Primary);
            assert!(!marker.exists());
            h.click(id, egui::PointerButton::Primary);
            assert_eq!(h.app.s.active, pane);
            for _ in 0..150 {
                if fs::read(&marker).is_ok_and(|s| !s.is_empty()) {
                    break;
                }
                std::thread::sleep(Duration::from_millis(30));
            }
            assert_eq!(
                fs::read(&marker).unwrap_or_default(),
                b"x",
                "pane={pane}: {}",
                h.app.status
            );
        }
    }
    #[test]
    fn right_pane_popup_routes_real_clicks_in_light_and_dark() {
        for dark in [false, true] {
            let temp = tempfile::tempdir().unwrap();
            let ctx = egui::Context::default();
            let cc = eframe::CreationContext::_new_kittest(ctx.clone());
            let mut app = App::new(&cc, true, Some(temp.path().to_owned()), false, None);
            app.s.theme = Theme::Miuix;
            app.s.reduce_motion = true;
            app.s.dark = dark;
            app.s.active = 0;
            crate::theme::apply(&ctx, &app.s);
            for (i, sub) in ["left", "right"].into_iter().enumerate() {
                let dir = temp.path().join(sub);
                fs::create_dir(&dir).unwrap();
                fs::write(dir.join("sample.txt"), b"hello").unwrap();
                fs::create_dir(dir.join("folder")).unwrap();
                app.panes[i].goto(Location::Local(dir.clone()), false);
                app.panes[i].pending = None;
                app.panes[i].items = files::list(&dir, false).unwrap();
            }
            let l = temp.path().join("left/sample.txt");
            let f = temp.path().join("right/sample.txt");
            let d = temp.path().join("right/folder");
            app.panes[0].selection.insert(l.clone());
            let mut h = Harness { app, ctx, t: 0. };
            for _ in 0..4 {
                h.frame(vec![]);
            }
            h.open(Some(&f));
            h.pick(Rename);
            assert!(matches!(h.app.dialog.take(),Some(Dialog::Rename(p,_)) if p==f));
            assert_eq!(h.app.s.active, 1);
            h.app.panes[1].selection.insert(d.clone());
            h.open(Some(&d));
            h.pick(Bin);
            assert!(
                matches!(h.app.dialog.take(),Some(Dialog::Delete(v)) if v.len()==2 && v.contains(&d) && v.contains(&f))
            );
            assert!(d.exists() && f.exists());
            assert!(h.app.panes[0].selection.contains(&l));
            h.open(None);
            h.pick(NewFile);
            assert!(matches!(h.app.dialog.take(), Some(Dialog::New(false, _))));
            assert!(h.app.panes[1].selection.is_empty());
        }
    }
}
