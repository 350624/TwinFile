use super::*;
use crate::motion::ChoiceUi as _;
use std::sync::{Mutex, mpsc::TryRecvError};
use twinfile_rust::payload::{self, Payload};

enum Completed {
    Inspect(Arc<Payload>),
    Extract(Vec<PathBuf>),
}
type Reply = Result<Completed, String>;
type Meter = Arc<Mutex<(String, u64, u64)>>;
struct Work {
    stop: Arc<AtomicBool>,
    rx: Receiver<Reply>,
    progress: Meter,
    kind: dock_progress::Kind,
}
pub(super) struct Tool {
    pub(super) open: bool,
    remote: bool,
    local: String,
    url: String,
    dir: String,
    remember: bool,
    workers: usize,
    parsed: Option<Arc<Payload>>,
    picks: Vec<bool>,
    work: Option<Work>,
    status: String,
    last_dir: Option<PathBuf>,
}
impl Tool {
    pub(super) fn new(s: &Settings) -> Self {
        Self {
            open: false,
            remote: false,
            local: String::new(),
            url: s.payload_url.clone().unwrap_or_default(),
            remember: s.payload_url.is_some(),
            dir: s
                .payload_dir
                .clone()
                .or_else(|| {
                    std::env::var_os("USERPROFILE")
                        .map(|p| PathBuf::from(p).join("Downloads"))
                        .filter(|p| p.is_dir())
                })
                .map(|p| p.display().to_string())
                .unwrap_or_default(),
            workers: s.payload_workers.clamp(1, 8),
            parsed: None,
            picks: Vec::new(),
            work: None,
            status: String::new(),
            last_dir: None,
        }
    }
    pub(super) fn busy(&self) -> bool {
        self.work.is_some()
    }
    pub(super) fn cancel(&mut self) {
        if let Some(w) = &self.work {
            w.stop.store(true, Relaxed);
        }
    }
    fn reset(&mut self) {
        self.parsed = None;
        self.picks.clear();
        self.status.clear();
        self.last_dir = None;
    }
    fn start(
        &mut self,
        ctx: &egui::Context,
        kind: dock_progress::Kind,
        op: impl FnOnce(&AtomicBool, &(dyn Fn(&str, u64, u64) + Sync)) -> Reply + Send + 'static,
    ) {
        if self.busy() {
            return;
        }
        let (tx, rx) = mpsc::channel();
        let stop = Arc::new(AtomicBool::new(false));
        let progress = Arc::new(Mutex::new((String::from("正在准备…"), 0u64, 0u64)));
        let watch = progress.clone();
        let c = stop.clone();
        let ui = ctx.clone();
        self.status.clear();
        self.last_dir = None;
        std::thread::spawn(move || {
            let f = |phase: &str, done, total| {
                if let Ok(mut d) = watch.lock() {
                    d.0.clear();
                    d.0.push_str(phase);
                    d.1 = done;
                    d.2 = total;
                }
            };
            let result = op(&c, &f);
            let _ = tx.send(result);
            ui.request_repaint();
        });
        self.work = Some(Work {
            rx,
            stop,
            progress,
            kind,
        });
    }
    pub(super) fn dock_sample(&self) -> Option<dock_progress::Sample> {
        let w = self.work.as_ref()?;
        let m = w.progress.lock().ok()?;
        Some(dock_progress::Sample::bytes(w.kind, m.1, m.2))
    }
    pub(super) fn poll(&mut self, ctx: &egui::Context) -> Option<(dock_progress::Kind, bool)> {
        let w = self.work.as_ref()?;
        let result = w.rx.try_recv();
        let outcome = (w.kind, matches!(&result, Ok(Ok(_))));
        match result {
            Err(TryRecvError::Empty) => {
                ctx.request_repaint_after(Duration::from_millis(100));
                return None;
            }
            Err(TryRecvError::Disconnected) => self.status = "后台任务意外退出".into(),
            Ok(Err(e)) => self.status = e,
            Ok(Ok(Completed::Inspect(_))) if !self.open => {
                self.reset();
            }
            Ok(Ok(Completed::Inspect(p))) => {
                self.picks = vec![true; p.partitions.len()];
                self.status = format!("读到 {} 个分区", p.partitions.len());
                self.parsed = Some(p);
            }
            Ok(Ok(Completed::Extract(v))) => {
                self.last_dir = v.first().and_then(|p| p.parent()).map(Path::to_owned);
                self.status = format!("提取完成：{} 个分区", v.len());
            }
        };
        self.work = None;
        Some(outcome)
    }
    pub(super) fn show(&mut self, ctx: &egui::Context, s: &mut Settings) {
        if !self.open {
            return;
        }
        let mi = s.theme == Theme::Miuix;
        let open = super::tool_sheet::show(ctx, "payload-open", "Payload 分区提取", mi, |ui| {
            ui.add_enabled_ui(!self.busy(),|ui|{
                if mi {let mut mode=self.remote as usize;crate::theme::miuix::contour_tabs_named(ui,&mut mode,["本地文件","远程直链"]);if self.remote!=(mode==1){self.remote=mode==1;self.reset();}}
                else {ui.horizontal(|ui|{let local=ui.motion_value(&mut self.remote,false,"本地文件").changed(); let remote=ui.motion_value(&mut self.remote,true,"远程直链").changed();if local || remote {self.reset();}});}
                ui.weak(egui::RichText::new("选择完整 OTA ZIP 或 payload.bin，不会刷机或覆盖已有镜像。").small());
                let mut changed=false;
                ui.horizontal(|ui|{
                    let src=if self.remote{&mut self.url}else{&mut self.local};
                    changed=ui.add(egui::TextEdit::singleline(src).hint_text(if self.remote{"https://…/OTA.zip 或 payload.bin"}else{"选择或输入本地文件路径"}).desired_width((ui.available_width()-106.).max(120.))).changed();
                    if !self.remote && ui.button("选择文件…").clicked() && let Some(p)=rfd::FileDialog::new().add_filter("Payload / OTA", &["bin","zip"]).pick_file(){*src=p.display().to_string();changed=true;}
                });if changed{self.reset();}
                if self.remote {
                    ui.weak("支持稳定的 HTTP Range 时按需读取；不支持时会在临时目录缓存整包，请预留空间。");
                    if ui.motion_check(&mut self.remember,"记住直链（可能含令牌，将明文保存到本机）").changed() && !self.remember{s.payload_url=None;}
                }
                let source=if self.remote {self.url.trim()}else{self.local.trim()}.to_owned();
                if super::tool_sheet::primary(ui,"读取分区",!source.is_empty(),mi).clicked(){
                    self.reset();if self.remote && !source.starts_with("https://") && !source.starts_with("http://"){self.status="请输入 HTTP(S) 地址".into();}
                    else if !self.remote && !Path::new(&source).is_file(){self.status="找不到本地文件".into();}
                    else {if self.remote {s.payload_url=self.remember.then(||source.clone());}self.start(ctx,dock_progress::Kind::Read,move|c,r|payload::inspect(&source,c,r).map(|p|Completed::Inspect(Arc::new(p))));}
                }
                if let Some(p)=self.parsed.clone(){
                    ui.separator();ui.horizontal(|ui|{ui.strong("选择分区");if ui.button("全选").clicked(){self.picks.fill(true);}if ui.button("清空选择").clicked(){self.picks.fill(false);}});
                    egui::ScrollArea::vertical().id_salt("ota-names").max_height(150.).show_rows(ui,22.,p.partitions.len(),|ui,rows|{for i in rows{let part=&p.partitions[i];ui.motion_check(&mut self.picks[i],format!("{}   ·   {}",part.name,human(part.size)));}});
                    ui.separator();ui.horizontal_wrapped(|ui|{ui.label("输出目录");ui.text_edit_singleline(&mut self.dir);if ui.button("浏览…").clicked() && let Some(d)=rfd::FileDialog::new().pick_folder(){self.dir=d.display().to_string();}});
                    ui.horizontal(|ui|{ui.label("并行分区数");ui.add(egui::DragValue::new(&mut self.workers).range(1..=8));});
                    let count=self.picks.iter().filter(|v|**v).count();if super::tool_sheet::primary(ui,format!("提取 {count} 个分区"),count>0,mi).clicked(){
                        let dir=PathBuf::from(self.dir.trim());if !dir.is_dir(){self.status="请选择已存在的输出目录".into();}else{
                            let names=p.partitions.iter().zip(&self.picks).filter(|(_,checked)|**checked).map(|(x,_)|x.name.clone()).collect::<Vec<_>>();
                            let workers=self.workers;s.payload_workers=workers;s.payload_dir=Some(dir.clone());self.start(ctx,dock_progress::Kind::Extract,move|c,r|payload::extract(&p,&names,&dir,workers,c,r).map(Completed::Extract));
                        }
                    }
                }
            });
            if let Some(w) = &self.work {
                if let Ok(m) = w.progress.lock() {
                    ui.label(&m.0);
                    if m.2 > 0 {
                        ui.add(
                            egui::ProgressBar::new((m.1 as f64 / m.2 as f64).clamp(0., 1.) as f32)
                                .show_percentage(),
                        );
                    } else {
                        ui.horizontal(|ui| {
                            ui.spinner();
                            if m.1 > 0 {
                                ui.label(human(m.1));
                            }
                        });
                    }
                }
                if ui
                    .add_enabled(!w.stop.load(Relaxed), egui::Button::new("取消任务"))
                    .clicked()
                {
                    self.cancel();
                }
            }
            if !self.status.is_empty() {
                ui.label(&self.status);
            }
            if let Some(d) = &self.last_dir
                && ui.button("打开输出目录").clicked()
                && let Err(e) = super::workbench::system_open(d)
            {
                self.status = e.to_string();
            }
        });
        self.open = open;
        if !open {
            self.cancel();
            if !self.busy() {
                self.parsed = None;
                self.picks.clear();
            }
        }
    }
}
impl Drop for Tool {
    fn drop(&mut self) {
        self.cancel();
    }
}
