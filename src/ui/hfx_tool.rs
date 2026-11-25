use super::dock_progress::{Kind, Sample};
use crate::motion::ChoiceUi as _;
use eframe::egui;
use std::{
    collections::BTreeSet,
    net::{Ipv4Addr, TcpListener},
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicU64, Ordering::Relaxed},
        mpsc::{self, Receiver, TryRecvError},
    },
    thread,
    time::Duration,
};
use twinfile_rust::hfx::{Entry, Session};
mod platform;

struct Link {
    session: Session,
    _reverse: Option<platform::Reverse>,
}
enum Update {
    Adapters(Vec<Ipv4Addr>),
    Devices(Vec<(String, String)>),
    Connected(Link, String, Vec<Entry>, String),
    Message(String),
}
type ResultMsg = Result<Update, String>;
#[derive(Default)]
struct Progress {
    stop: AtomicBool,
    done: AtomicU64,
    total: AtomicU64,
}
impl Progress {
    fn record(&self, done: u64, total: u64) {
        self.done.store(done, Relaxed);
        self.total.store(total, Relaxed);
    }
}
struct Job {
    rx: Receiver<ResultMsg>,
    flag: Arc<Progress>,
    label: &'static str,
    transfer: Option<Kind>,
}
enum Action {
    Adapters,
    Devices,
    Install,
    Connect,
    List(String),
    Push(Vec<PathBuf>),
    Pull(PathBuf),
    Disconnect,
    Cancel,
}
#[derive(Debug, Default, Copy, Clone, PartialEq, Eq)]
enum Mode {
    #[default]
    Hybrid,
    Usb,
    Wifi,
}
impl Mode {
    fn usb(self) -> bool {
        self != Self::Wifi
    }
    fn wifi(self) -> bool {
        self != Self::Usb
    }
    fn connection(
        self,
        serial: &str,
        wifi: &str,
        trusted: bool,
    ) -> Result<Vec<(String, Ipv4Addr)>, String> {
        if self.usb() && serial.is_empty() {
            return Err("请用数据线连接手机、确认 USB 调试授权，再点刷新。".into());
        }
        let v = lanes(
            if self.usb() { serial } else { "" },
            if self.wifi() { wifi } else { "" },
        )?;
        if self.wifi() && !v.iter().any(|(_, ip)| !ip.is_loopback()) {
            return Err("请选择电脑的 Wi-Fi 地址。".into());
        }
        if self.wifi() && !trusted {
            return Err("请确认当前网络可信。".into());
        }
        Ok(v)
    }
}
#[derive(Default)]
pub(super) struct Tool {
    pub(super) open: bool,
    mode: Mode,
    devices: Vec<(String, String)>,
    serial: String,
    wifi: String,
    adapters: Vec<Ipv4Addr>,
    trusted: bool,
    confirm_install: bool,
    link: Option<Link>,
    remote: String,
    entries: Vec<Entry>,
    selected: BTreeSet<String>,
    job: Option<Job>,
    status: String,
}
fn lanes(serial: &str, wifi: &str) -> Result<Vec<(String, Ipv4Addr)>, String> {
    let mut v = Vec::new();
    if !serial.is_empty() {
        v.push(("USB".into(), Ipv4Addr::LOCALHOST));
    }
    for value in wifi
        .split(|c: char| c.is_whitespace() || c == ',' || c == '，' || c == ';')
        .filter(|t| !t.is_empty())
    {
        let ip = value
            .parse::<Ipv4Addr>()
            .map_err(|_| format!("无效的电脑 IPv4 地址：{value}"))?;
        if !platform::valid_ip(ip) {
            return Err(format!("不可用的 WLAN 地址：{ip}"));
        }
        if !v.iter().any(|(_, a)| *a == ip) {
            v.push((format!("WiFi {ip}"), ip));
        }
    }
    if v.is_empty() || v.len() > 8 {
        return Err("请选择 USB 手机或填写本机 WLAN 地址（总共 1–8 条通道）".into());
    }
    Ok(v)
}
fn within_home(home: &str, current: &str) -> Option<String> {
    let home = home.trim_end_matches('/');
    let current = current.trim_end_matches('/');
    if current == home
        || (!home.is_empty()
            && !current
                .strip_prefix(home)
                .is_some_and(|s| s.starts_with('/')))
    {
        return None;
    }
    let parent = current.rsplit_once('/')?.0;
    Some(if parent.is_empty() { "/" } else { parent }.into())
}
fn sort_entries(v: &mut [Entry]) {
    v.sort_by_cached_key(|e| (!e.dir, e.name.to_lowercase(), e.name.clone()));
}
fn refreshed(mut link: Link, dir: String, status: String, flag: &Progress) -> ResultMsg {
    let mut rows = link.session.list(&dir, &flag.stop)?;
    sort_entries(&mut rows);
    Ok(Update::Connected(link, dir, rows, status))
}
impl Tool {
    pub(super) fn busy(&self) -> bool {
        self.job.is_some()
    }
    pub(super) fn cancel(&mut self) {
        self.open = false;
        self.confirm_install = false;
        if let Some(j) = &self.job {
            j.flag.stop.store(true, Relaxed);
        } else if self.link.is_some() {
            self.act(Action::Disconnect);
        }
    }
    fn start<F>(&mut self, label: &'static str, f: F)
    where
        F: FnOnce(&Progress) -> ResultMsg + Send + 'static,
    {
        self.start_kind(label, None, f);
    }
    fn start_kind<F: FnOnce(&Progress) -> ResultMsg + Send + 'static>(
        &mut self,
        label: &'static str,
        transfer: Option<Kind>,
        f: F,
    ) {
        let (flag, (tx, rx)) = (Arc::new(Progress::default()), mpsc::channel());
        self.status.clear();
        self.job = Some(Job {
            rx,
            transfer,
            flag: flag.clone(),
            label,
        });
        thread::spawn(move || {
            let result = f(&flag);
            if !flag.stop.load(Relaxed) {
                let _ = tx.send(result);
            } else {
                drop(result);
                let _ = tx.send(Err("已取消，连接已关闭".into()));
            }
        });
    }
    pub(super) fn dock_sample(&self) -> Option<Sample> {
        let j = self.job.as_ref()?;
        Some(Sample::bytes(
            j.transfer?,
            j.flag.done.load(Relaxed),
            j.flag.total.load(Relaxed),
        ))
    }
    pub(super) fn poll(&mut self, ctx: &egui::Context) -> Option<(Kind, bool)> {
        let job = self.job.as_ref()?;
        let msg = match job.rx.try_recv() {
            Ok(v) => v,
            Err(TryRecvError::Disconnected) => Err("HFX 后台任务意外结束".into()),
            Err(TryRecvError::Empty) => {
                ctx.request_repaint_after(Duration::from_millis(90));
                return None;
            }
        };
        let cancelled = job.flag.stop.load(Relaxed);
        let outcome = job.transfer.map(|k| (k, msg.is_ok() && !cancelled));
        self.job = None;
        match msg {
            Ok(Update::Devices(v)) => {
                if !v.iter().any(|(s, _)| *s == self.serial) {
                    self.serial = v.first().map(|v| v.0.clone()).unwrap_or_default();
                }
                self.devices = v;
                self.status = format!(
                    "发现 {} 台已授权 USB 手机；未授权设备请在手机上确认 USB 调试后刷新。",
                    self.devices.len()
                );
            }
            Ok(Update::Adapters(v)) => {
                self.adapters = v;
                self.status = "点击地址可添加通道；也可以手动输入本机 IPv4 地址。".into();
            }
            Ok(Update::Message(s)) => self.status = s,
            Ok(Update::Connected(link, dir, rows, s)) => {
                self.link = Some(link);
                self.remote = dir;
                self.entries = rows;
                self.selected.clear();
                self.status = s;
                if cancelled || !self.open {
                    self.act(Action::Disconnect);
                }
            }
            Err(e) => {
                self.status = e;
                if self.link.is_none() {
                    self.selected.clear();
                    self.entries.clear();
                }
            }
        }
        outcome
    }
    fn act(&mut self, action: Action) {
        if self.busy() {
            if matches!(action, Action::Cancel | Action::Disconnect) {
                if let Some(j) = &self.job {
                    j.flag.stop.store(true, Relaxed);
                }
            }
            return;
        }
        match action {
            Action::Adapters => self.start("枚举本机 IPv4", |f| {
                platform::addresses(&f.stop).map(Update::Adapters)
            }),
            Action::Devices => self.start("枚举已授权 USB 手机", |f| {
                platform::devices(&f.stop).map(Update::Devices)
            }),
            Action::Install => {
                let s = self.serial.clone();
                if s.is_empty() {
                    return;
                }
                self.start("正在安装 HFX 手机端", move |f| {
                    platform::install(&s, &f.stop)?;
                    Ok(Update::Message(
                        "安装成功；请在手机设置授予 HFX 文件访问权限。".into(),
                    ))
                });
            }
            Action::Connect => {
                let list = match self.mode.connection(&self.serial, &self.wifi, self.trusted) {
                    Ok(v) => v,
                    Err(e) => {
                        self.status = e;
                        return;
                    }
                };
                let serial = if self.mode.usb() {
                    self.serial.clone()
                } else {
                    String::new()
                };
                self.start(
                    "等待 HFX 手机端连接（5740），请解锁手机",
                    move |f| {
                        // Verify requested local interfaces before opening a multihomed listener.
                        for (_, ip) in &list {
                            TcpListener::bind((*ip, 0))
                                .map_err(|e| format!("不能使用本机地址 {ip}：{e}"))?;
                        }
                        let bind = if list.len() == 1 {
                            list[0].1
                        } else {
                            Ipv4Addr::UNSPECIFIED
                        };
                        let listener = TcpListener::bind((bind, 5740))
                            .map_err(|e| format!("监听 {bind}:5740 失败：{e}"))?;
                        let guard = if serial.is_empty() {
                            None
                        } else {
                            Some(platform::Reverse::setup(&serial, &f.stop)?)
                        };
                        let session = Session::accept(listener, &list, &f.stop)?;
                        let home = session.home.clone();
                        let summary = format!("已连接：{}", session.lane_names.join(" + "));
                        refreshed(
                            Link {
                                session,
                                _reverse: guard,
                            },
                            home,
                            summary,
                            f,
                        )
                    },
                );
            }
            Action::Disconnect | Action::Cancel => {
                let link = self.link.take();
                self.entries.clear();
                self.selected.clear();
                self.remote.clear();
                if link.is_some() {
                    self.start("断开 HFX 并释放 USB 映射", move |_| {
                        drop(link);
                        Ok(Update::Message("HFX 已断开".into()))
                    });
                }
            }
            Action::List(dir) => {
                if let Some(l) = self.link.take() {
                    self.start("读取手机目录", move |f| {
                        refreshed(l, dir, "目录已刷新".into(), f)
                    });
                }
            }
            Action::Push(paths) => {
                if let Some(mut l) = self.link.take() {
                    let dir = self.remote.clone();
                    self.start_kind("电脑 → 手机", Some(Kind::Send), move |f| {
                        l.session
                            .push(&paths, &dir, &f.stop, &|a, b| f.record(a, b))?;
                        refreshed(l, dir, "发送完成".into(), f)
                    });
                }
            }
            Action::Pull(dest) => {
                if let Some(mut l) = self.link.take() {
                    let paths = self.selected.iter().cloned().collect::<Vec<_>>();
                    let dir = self.remote.clone();
                    // The peer does not announce a batch size for directories. Listed plain files do have lengths.
                    let expected = paths
                        .iter()
                        .try_fold(0u64, |sum, path| {
                            self.entries
                                .iter()
                                .find(|e| &e.path == path && !e.dir)
                                .and_then(|e| sum.checked_add(e.size))
                        })
                        .unwrap_or(0);
                    self.start_kind("手机 → 电脑", Some(Kind::Receive), move |f| {
                        f.record(0, expected);
                        l.session.pull(&paths, &dir, &dest, &f.stop, &|a, b| {
                            f.record(a, if b == 0 { expected } else { b })
                        })?;
                        refreshed(l, dir, format!("接收完成：{}", dest.display()), f)
                    });
                }
            }
        }
    }
    pub(super) fn show(&mut self, ctx: &egui::Context, mi: bool) {
        if !self.open {
            return;
        }
        let mut action = None;
        let mut collapse = false;
        let ready = !self.busy();
        let open = super::tool_sheet::show(ctx, "hfx-open", "HFX 多轨传输", mi, |ui| {
            if let Some(j) = &self.job {
                ui.horizontal_wrapped(|ui| {
                    ui.spinner();
                    ui.label(j.label);
                    if mi && j.transfer.is_some() && ui.button("收起到底栏").clicked() {
                        collapse = true;
                    }
                    if ui.button("取消").clicked() {
                        action = Some(Action::Cancel);
                    }
                });
                let (done, total) = (j.flag.done.load(Relaxed), j.flag.total.load(Relaxed));
                if total > 0 {
                    ui.add(
                        egui::ProgressBar::new((done as f64 / total as f64).clamp(0., 1.) as f32)
                            .text(format!("{} / {}", super::human(done), super::human(total))),
                    );
                } else if done > 0 {
                    ui.label(format!("已传输 {}", super::human(done)));
                }
            }
            if let Some(link) = &self.link {
                ui.label(format!("通道：{}", link.session.lane_names.join(" + ")));
                ui.label(format!("手机目录：{}", self.remote));
                ui.horizontal_wrapped(|ui| {
                    let parent = within_home(&link.session.home, &self.remote);
                    if ui
                        .add_enabled(parent.is_some(), egui::Button::new("上一级"))
                        .clicked()
                    {
                        action = parent.map(Action::List);
                    }
                    if super::workbench::refresh_button(ui, "刷新目录", false).clicked() {
                        action = Some(Action::List(self.remote.clone()));
                    }
                    if ui.button("断开").clicked() {
                        action = Some(Action::Disconnect);
                    }
                });
                ui.horizontal_wrapped(|ui| {
                    if ui.button("发送文件…").clicked() {
                        action = rfd::FileDialog::new().pick_files().map(Action::Push);
                    }
                    if ui.button("发送文件夹…").clicked() {
                        action = rfd::FileDialog::new()
                            .pick_folder()
                            .map(|p| Action::Push(vec![p]));
                    }
                    if ui
                        .add_enabled(
                            !self.selected.is_empty(),
                            egui::Button::new("接收选中项到…"),
                        )
                        .clicked()
                    {
                        action = rfd::FileDialog::new().pick_folder().map(Action::Pull);
                    }
                });
                ui.separator();
                ui.small("勾选要接收的手机文件或目录；双击目录进入。");
                egui::ScrollArea::vertical()
                    .id_salt("hfx-dir")
                    .max_height(198.)
                    .show_rows(ui, 24., self.entries.len(), |ui, range| {
                        for entry in &self.entries[range] {
                            ui.horizontal(|ui| {
                                let mut sel = self.selected.contains(&entry.path);
                                if ui.motion_check(&mut sel, "").changed() {
                                    if sel {
                                        self.selected.insert(entry.path.clone());
                                    } else {
                                        self.selected.remove(&entry.path);
                                    }
                                }
                                let r = ui.motion_label(
                                    false,
                                    if entry.dir {
                                        format!("[目录] {}", entry.name)
                                    } else {
                                        format!("{}   ·   {}", entry.name, super::human(entry.size))
                                    },
                                );
                                if entry.dir && r.double_clicked() {
                                    action = Some(Action::List(entry.path.clone()));
                                }
                            });
                        }
                    });
                if self.entries.is_empty() {
                    ui.weak("空目录");
                }
            } else if ready {
                ui.horizontal_wrapped(|ui| {
                    ui.strong("连接方式");
                    for (mode, label) in [
                        (Mode::Hybrid, "USB + Wi-Fi"),
                        (Mode::Usb, "仅 USB"),
                        (Mode::Wifi, "仅 Wi-Fi"),
                    ] {
                        if ui.motion_value(&mut self.mode, mode, label).changed() {
                            self.confirm_install = false;
                            self.status.clear();
                        }
                    }
                });
                if self.mode.usb() {
                    ui.horizontal_wrapped(|ui| {
                        ui.label("USB 手机");
                        ui.add_enabled_ui(!self.devices.is_empty(), |ui| {
                            egui::ComboBox::from_id_salt("hfx-usb-device")
                                .width(142.)
                                .selected_text(
                                    self.devices
                                        .iter()
                                        .find(|x| x.0 == self.serial)
                                        .map(|x| x.1.as_str())
                                        .unwrap_or("未发现已授权手机"),
                                )
                                .show_ui(ui, |ui| {
                                    for (s, n) in &self.devices {
                                        ui.motion_value(&mut self.serial, s.clone(), n);
                                    }
                                });
                        });
                        if super::workbench::refresh_button(ui, "刷新", false).clicked() {
                            action = Some(Action::Devices);
                        }
                        if ui
                            .add_enabled(!self.serial.is_empty(), egui::Button::new("安装手机端…"))
                            .clicked()
                        {
                            self.confirm_install = true;
                        }
                    });
                }
                if self.mode.wifi() {
                    ui.strong("Wi-Fi 通道");
                    ui.horizontal_wrapped(|ui| {
                        ui.add(
                            egui::TextEdit::singleline(&mut self.wifi)
                                .desired_width((ui.available_width() - 110.).clamp(70., 260.))
                                .hint_text("本机 IPv4，多个用逗号分隔"),
                        );
                        if ui.button("读取本机地址").clicked() {
                            action = Some(Action::Adapters);
                        }
                    });
                    if !self.adapters.is_empty() {
                        ui.horizontal_wrapped(|ui| {
                            for ip in &self.adapters {
                                if ui.small_button(ip.to_string()).clicked() {
                                    if !self.wifi.trim().is_empty() {
                                        self.wifi.push_str(", ");
                                    }
                                    self.wifi.push_str(&ip.to_string());
                                }
                            }
                        });
                    }
                    if !self.wifi.trim().is_empty() {
                        ui.motion_check(
                            &mut self.trusted,
                            "Wi-Fi 传输不加密、无认证；我确认网络可信",
                        );
                    }
                }
                let available = self
                    .mode
                    .connection(&self.serial, &self.wifi, self.trusted)
                    .is_ok();
                let hint = match self.mode {
                    Mode::Hybrid if self.serial.is_empty() => {
                        "用数据线连接并授权手机，点刷新；再选择电脑的 Wi-Fi 地址。"
                    }
                    Mode::Hybrid => "手机与电脑连同一局域网，连接后 USB 和 Wi-Fi 同时传输。",
                    Mode::Usb => "用数据线连接并授权手机，再点刷新。",
                    Mode::Wifi => "在手机 HFX 中连接电脑的 Wi-Fi 地址，端口 5740。",
                };
                ui.weak(egui::RichText::new(hint).small());
                if super::tool_sheet::primary(
                    ui,
                    match self.mode {
                        Mode::Hybrid => "连接双通道",
                        _ => "连接手机",
                    },
                    available && !self.confirm_install,
                    mi,
                )
                .clicked()
                {
                    action = Some(Action::Connect);
                }
                if self.confirm_install {
                    ui.group(|ui|{ui.label(format!("将配套 HybridFileXfer v3.0 安装到 USB 设备 {}。这会修改手机上的应用，不会自动授予文件权限。",self.serial));ui.horizontal(|ui|{if ui.button("确认安装").clicked(){self.confirm_install=false;action=Some(Action::Install);}if ui.button("返回").clicked(){self.confirm_install=false;}});});
                }
            }
            if !self.status.is_empty() {
                ui.separator();
                ui.label(&self.status);
            }
        });
        if collapse {
            self.open = false;
        } else if !open {
            self.cancel();
        } else if let Some(a) = action {
            self.act(a);
        }
        if self.busy() {
            ctx.request_repaint_after(Duration::from_millis(90));
        }
    }
}
impl Drop for Tool {
    fn drop(&mut self) {
        if let Some(j) = &self.job {
            j.flag.stop.store(true, Relaxed);
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn mixed_mode_requires_both_transports_and_never_downgrades() {
        let ip = "192.168.3.18";
        assert_eq!(Mode::default(), Mode::Hybrid);
        for (serial, wifi, trust) in [
            ("", ip, true),
            ("abc", "", true),
            ("abc", ",", true),
            ("abc", ip, false),
        ] {
            assert!(Mode::Hybrid.connection(serial, wifi, trust).is_err());
        }
        let both = Mode::Hybrid.connection("abc", ip, true).unwrap();
        assert_eq!(both.len(), 2);
        assert!(both[0].1.is_loopback());
        assert_eq!(both[1].1.to_string(), ip);
        assert_eq!(
            Mode::Usb.connection("abc", "bad address", false).unwrap(),
            both[..1]
        );
        assert_eq!(Mode::Wifi.connection("abc", ip, true).unwrap(), both[1..]);
    }
    #[test]
    fn filters_and_preserves_lane_order() {
        assert_eq!(lanes("abc", "192.168.1.2,192.168.1.2").unwrap().len(), 2);
        assert_eq!(lanes("abc", "").unwrap()[0].1, Ipv4Addr::LOCALHOST);
        for s in [
            "",
            "0.0.0.0",
            "127.0.0.1",
            "255.255.255.255",
            "224.0.0.1",
            "not.ip",
        ] {
            assert!(lanes("", s).is_err());
        }
    }
    #[test]
    fn only_browse_within_home() {
        assert_eq!(
            within_home("/storage/emulated/0", "/storage/emulated/0/DCIM"),
            Some("/storage/emulated/0".into())
        );
        assert!(within_home("/storage/emulated/0", "/storage/emulated/00/x").is_none());
        assert!(within_home("/storage/emulated/0", "/storage/emulated/0").is_none());
    }
}
