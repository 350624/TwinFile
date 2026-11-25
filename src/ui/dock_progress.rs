use super::*;
use std::sync::atomic::AtomicU64;

#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum Kind {
    Download,
    Copy,
    Move,
    Send,
    Receive,
    Read,
    Extract,
}
impl Kind {
    fn label(self) -> &'static str {
        match self {
            Self::Download => "下载",
            Self::Copy => "复制",
            Self::Move => "移动",
            Self::Send => "快传发送",
            Self::Receive => "快传接收",
            Self::Read => "读取分区",
            Self::Extract => "分区提取",
        }
    }
}
#[derive(Clone, Copy)]
pub(super) struct Sample {
    kind: Kind,
    bytes: u64,
    fraction: Option<f64>,
}
impl Sample {
    pub(super) fn bytes(kind: Kind, bytes: u64, total: u64) -> Self {
        Self {
            kind,
            bytes,
            fraction: (total > 0).then(|| bytes as f64 / total as f64),
        }
    }
}

pub(super) struct Local {
    moving: bool,
    roots: usize,
    index: AtomicU64,
    base: AtomicU64,
    bytes: AtomicU64,
    total: AtomicU64,
}
impl Local {
    pub(super) fn new(moving: bool, roots: usize) -> Self {
        Self {
            moving,
            roots,
            index: 0.into(),
            base: 0.into(),
            bytes: 0.into(),
            total: 0.into(),
        }
    }
    pub(super) fn kind(&self) -> Kind {
        if self.moving { Kind::Move } else { Kind::Copy }
    }
    pub(super) fn measure(&self, paths: &[PathBuf], stop: &AtomicBool) -> Result<(), String> {
        let t = files::aggregate(paths, stop).map_err(|e| e.to_string())?;
        self.total
            .store(if t.errors == 0 { t.bytes } else { 0 }, Relaxed);
        Ok(())
    }
    pub(super) fn begin_item(&self, index: usize) {
        if self.moving {
            self.total.store(0, Relaxed);
            self.base.store(self.bytes.load(Relaxed), Relaxed);
            self.index.store(index as u64, Relaxed);
        }
    }
    pub(super) fn wrote(&self, n: u64) {
        self.bytes.fetch_add(n, Relaxed);
    }
    pub(super) fn sample(&self) -> Sample {
        let bytes = self.bytes.load(Relaxed);
        let total = self.total.load(Relaxed);
        let mut s = Sample::bytes(self.kind(), bytes, total);
        if self.moving {
            let item = if total == 0 {
                0.
            } else {
                (bytes.saturating_sub(self.base.load(Relaxed)) as f64 / total as f64).min(1.)
            };
            s.fraction = Some((self.index.load(Relaxed) as f64 + item) / self.roots.max(1) as f64);
        }
        s
    }
}

struct Live {
    sample: Sample,
    at: Instant,
    last: u64,
    rate: Option<f64>,
}
#[derive(Clone, Copy)]
pub(super) struct View {
    pub(super) kind: Kind,
    bytes: u64,
    fraction: Option<f32>,
    rate: Option<f64>,
    others: usize,
    pub(super) success: bool,
}
#[derive(Default)]
pub(super) struct Tracker {
    live: Vec<Live>,
    hold: Option<(Instant, View)>,
    last_view: Option<View>,
}
impl Tracker {
    pub(super) fn observe(&mut self, samples: &[Sample], now: Instant) {
        for &s in samples {
            if let Some(l) = self.live.iter_mut().find(|l| l.sample.kind == s.kind) {
                let dt = now.duration_since(l.at).as_secs_f64();
                if s.bytes < l.last {
                    l.rate = None;
                    l.at = now;
                    l.last = s.bytes;
                } else if dt >= 0.35 {
                    l.rate = Some((s.bytes - l.last) as f64 / dt);
                    l.at = now;
                    l.last = s.bytes;
                }
                l.sample = s;
            } else {
                self.hold = None;
                self.live.push(Live {
                    sample: s,
                    at: now,
                    last: s.bytes,
                    rate: None,
                });
            }
        }
    }
    pub(super) fn finish(&mut self, kind: Kind, ok: bool, now: Instant) {
        if let Some(i) = self.live.iter().position(|l| l.sample.kind == kind) {
            let l = self.live.remove(i);
            self.hold = if ok && self.live.is_empty() {
                Some((
                    now + Duration::from_millis(1300),
                    View {
                        kind,
                        bytes: l.sample.bytes,
                        fraction: Some(1.),
                        rate: l.rate,
                        others: 0,
                        success: true,
                    },
                ))
            } else {
                None
            };
        }
    }
    pub(super) fn view(&mut self, now: Instant) -> (bool, Option<View>) {
        let current = self
            .live
            .last()
            .map(|l| View {
                kind: l.sample.kind,
                bytes: l.sample.bytes,
                fraction: l.sample.fraction.map(|x| x.clamp(0., 0.999) as f32),
                rate: l.rate,
                others: self.live.len() - 1,
                success: false,
            })
            .or_else(|| self.hold.filter(|(until, _)| now < *until).map(|(_, v)| v));
        if current.is_some() {
            self.last_view = current;
        }
        (current.is_some(), self.last_view)
    }
}
impl View {
    pub(super) fn info(self, ui: &egui::Ui, r: egui::Rect, alpha: f32) {
        let p = ui.painter().with_clip_rect(r.intersect(ui.clip_rect()));
        let c = crate::theme::miuix::Colors::of(ui);
        let x = r.left() + 12.;
        let y = r.center().y;
        p.vline(
            r.left() + 3.,
            y - 11.0..=y + 11.,
            egui::Stroke::new(1_f32, c.outline.gamma_multiply(alpha)),
        );
        let percent = self.fraction.map_or_else(
            || human(self.bytes),
            |f| format!("{}%", (f * 100.).floor() as u32),
        );
        let others = if self.others == 0 {
            String::new()
        } else {
            format!(" +{}", self.others)
        };
        p.text(
            egui::pos2(x, y - 8.),
            egui::Align2::LEFT_CENTER,
            format!("{}{}  {}", self.kind.label(), others, percent),
            crate::theme::miuix::font(13.),
            c.on_surface.gamma_multiply(alpha),
        );
        let speed = if self.success {
            "已完成".into()
        } else {
            self.rate
                .map_or_else(|| "— /s".into(), |r| format!("{}/s", human(r as u64)))
        };
        p.text(
            egui::pos2(x, y + 9.),
            egui::Align2::LEFT_CENTER,
            speed,
            crate::theme::miuix::font(11.5),
            c.summary.gamma_multiply(alpha),
        );
    }
    pub(super) fn rim(self, ui: &egui::Ui, r: egui::Rect, alpha: f32) {
        let stroke = egui::Stroke::new(
            2.4_f32,
            ui.visuals().selection.stroke.color.gamma_multiply(alpha),
        );
        let r = r.shrink(stroke.width / 2.);
        let phase = ui.input(|i| i.time) * 0.55;
        let reduced = crate::motion::reduced(ui.ctx());
        let (start, len) = self.fraction.map_or(
            (if reduced { 0. } else { phase.fract() as f32 }, 0.12),
            |f| (0., f),
        );
        if len <= 0. {
            return;
        }
        let mut points = outline(r, start, len);
        ui.painter().add(if self.success {
            points.pop(); // closed_line joins the last sample to the first.
            egui::Shape::closed_line(points, stroke)
        } else {
            egui::Shape::line(points, stroke)
        });
    }
}
// Trace the actual capsule by arc length, clockwise from twelve o'clock. Straight
// sides and semicircles share one distance scale, even as the capsule expands.
fn outline(r: egui::Rect, start: f32, len: f32) -> Vec<egui::Pos2> {
    let rad = r.height() / 2.;
    let flat = (r.width() - r.height()).max(0.);
    let arc = std::f32::consts::PI * rad;
    let perimeter = 2. * (flat + arc);
    let steps = ((perimeter * len / 3.).ceil() as usize).max(1);
    (0..=steps)
        .map(|i| {
            let d = ((start + len * i as f32 / steps as f32) * perimeter + flat / 2.)
                .rem_euclid(perimeter);
            let cx = r.left() + rad;
            let cy = r.center().y;
            if d < flat {
                egui::pos2(cx + d, r.top())
            } else if d < flat + arc {
                let a = (d - flat) / rad - std::f32::consts::FRAC_PI_2;
                egui::pos2(cx + flat + rad * a.cos(), cy + rad * a.sin())
            } else if d < flat * 2. + arc {
                egui::pos2(cx + flat - (d - flat - arc), r.bottom())
            } else {
                let a = (d - flat * 2. - arc) / rad + std::f32::consts::FRAC_PI_2;
                egui::pos2(cx + rad * a.cos(), cy + rad * a.sin())
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn speed_and_ring_wait_for_committed_success() {
        let mut d = Tracker::default();
        let t = Instant::now();
        let k = Kind::Download;
        d.observe(&[Sample::bytes(k, 0, 1024)], t);
        d.observe(
            &[Sample::bytes(k, 512, 1024)],
            t + Duration::from_millis(500),
        );
        let v = d.view(t).1.unwrap();
        assert_eq!(v.rate, Some(1024.));
        assert_eq!(v.fraction, Some(0.5));
        d.observe(&[Sample::bytes(k, 1024, 1024)], t + Duration::from_secs(1));
        assert!(d.view(t).1.unwrap().fraction.unwrap() < 1.);
        d.finish(k, true, t + Duration::from_secs(1));
        let (on, v) = d.view(t + Duration::from_secs(2));
        assert!(on && v.unwrap().success);
        assert_eq!(v.unwrap().fraction, Some(1.));
        assert!(!d.view(t + Duration::from_secs(3)).0);
    }
    #[test]
    fn unknown_size_and_cancellation_never_report_success() {
        let mut d = Tracker::default();
        let t = Instant::now();
        d.observe(
            &[
                Sample::bytes(Kind::Download, 32, 128),
                Sample::bytes(Kind::Receive, 13, 0),
            ],
            t,
        );
        let v = d.view(t).1.unwrap();
        assert!(v.fraction.is_none());
        assert_eq!(v.others, 1);
        d.finish(Kind::Receive, false, t);
        let v = d.view(t).1.unwrap();
        assert!(v.kind == Kind::Download && v.others == 0);
        d.finish(Kind::Download, false, t);
        assert!(!d.view(t).0);
    }
    #[test]
    fn perimeter_follows_capsule_instead_of_its_bounding_box() {
        let r = egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(390., 52.));
        let full = outline(r, 0., 1.);
        assert!(full.first().unwrap().distance(*full.last().unwrap()) < 0.01);
        assert!(full[0].distance(r.center_top()) < 0.01);
        let half = outline(r, 0., 0.5);
        assert!(half.last().unwrap().distance(r.center_bottom()) < 0.01);
        for p in full {
            let axis = egui::pos2(p.x.clamp(r.left() + 26., r.right() - 26.), r.center().y);
            assert!((p.distance(axis) - 26.).abs() < 0.01);
        }
    }
    #[test]
    fn actual_local_copy_reports_each_chunk_and_rename_counts_no_io() {
        let t = tempfile::tempdir().unwrap();
        let src = t.path().join("source");
        let out = t.path().join("dest");
        fs::create_dir(&src).unwrap();
        fs::create_dir(&out).unwrap();
        let n = 690000;
        fs::write(src.join("a"), vec![8; n]).unwrap();
        let m = Local::new(false, 1);
        let stop = AtomicBool::new(false);
        m.measure(std::slice::from_ref(&src), &stop).unwrap();
        let calls = AtomicU64::new(0);
        files::transfer_metered(&src, &out, false, &stop, &|| Ok(()), &|delta| {
            m.wrote(delta);
            calls.fetch_add(1, Relaxed);
        })
        .unwrap();
        assert!(calls.load(Relaxed) > 1);
        assert_eq!(m.sample().bytes, n as u64);
        assert_eq!(m.sample().fraction, Some(1.));
        let other = t.path().join("move");
        fs::create_dir(&other).unwrap();
        files::transfer_metered(
            &src,
            &other,
            true,
            &stop,
            &|| panic!("same volume move must skip tree scan"),
            &|_| panic!("no copied bytes"),
        )
        .unwrap();
    }
}

#[cfg(test)]
mod ui_tests {
    use super::*;
    use std::io::{BufRead, BufReader, Write};
    // Exercise the actual downloader and painter; the fixture pauses at exactly 50%.
    #[test]
    fn downloading_expands_the_idle_dock_and_only_published_files_get_a_closed_ring() {
        let tmp = tempfile::tempdir().unwrap();
        let ctx = egui::Context::default();
        let cc = eframe::CreationContext::_new_kittest(ctx.clone());
        let mut app = App::new(&cc, false, Some(tmp.path().into()), false, None);
        app.s.theme = Theme::Miuix;
        app.s.reduce_motion = false;
        crate::theme::apply(&ctx, &app.s);
        let mut clock = 0.;
        let mut paint = |app: &mut App| {
            clock += 0.033;
            let out = ctx.run(
                egui::RawInput {
                    time: Some(clock),
                    screen_rect: Some(egui::Rect::from_min_size(
                        egui::Pos2::ZERO,
                        egui::vec2(760., 560.),
                    )),
                    ..Default::default()
                },
                |ctx| app.workbench(ctx),
            );
            let r = ctx
                .data(|d| d.get_temp::<egui::Rect>(egui::Id::new("test-transfer-dock-rect")))
                .unwrap();
            let accent = ctx.style().visuals.selection.stroke.color;
            let mut arc = None;
            for sh in out.shapes {
                if let egui::Shape::Path(p) = sh.shape {
                    if (p.stroke.width - 2.4).abs() < 0.005
                        && p.stroke.color == egui::epaint::ColorMode::Solid(accent)
                    {
                        arc = Some(p.closed);
                    }
                }
            }
            (r, arc)
        };
        for _ in 0..4 {
            paint(&mut app);
        }
        let (idle, edge) = paint(&mut app);
        assert!(edge.is_none());
        let server = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let url = format!("http://{}/fixture", server.local_addr().unwrap());
        let (go, wait) = mpsc::channel();
        let server = std::thread::spawn(move || {
            let (mut sock, _) = server.accept().unwrap();
            let mut rd = BufReader::new(&sock);
            loop {
                let mut l = String::new();
                if rd.read_line(&mut l).unwrap() == 0 || l == "\r\n" {
                    break;
                }
            }
            drop(rd);
            write!(
                sock,
                "HTTP/1.1 200 OK\r\nContent-Length: 65536\r\nConnection: close\r\n\r\n"
            )
            .unwrap();
            sock.write_all(&[3u8; 32768]).unwrap();
            sock.flush().unwrap();
            wait.recv_timeout(Duration::from_secs(10)).unwrap();
            sock.write_all(&[4u8; 32768]).unwrap();
        });
        let p = Arc::new(download::Progress::default());
        let (tx, rx) = mpsc::channel();
        app.dl.work = Some((p.clone(), rx));
        let dest = tmp.path().join("test.bin");
        let published = dest.clone();
        let watched = p.clone();
        let worker = std::thread::spawn(move || {
            tx.send(
                download::run(&url, &dest, 1, watched)
                    .map(|_| "ok".to_owned())
                    .map_err(|e| e.to_string()),
            )
            .unwrap();
        });
        let deadline = Instant::now() + Duration::from_secs(5);
        while p.received.load(Relaxed) < 32768 && Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(10));
        }
        assert_eq!(p.received.load(Relaxed), 32768);
        app.poll(&ctx);
        let mut widths = vec![];
        for _ in 0..14 {
            widths.push(paint(&mut app).0.width());
        }
        let (full, edge) = paint(&mut app);
        assert!(!published.exists());
        assert_eq!(edge, Some(false));
        assert!(
            full.width() > idle.width() + 100.
                && widths
                    .iter()
                    .any(|w| *w > idle.width() + 1. && *w < full.width() - 1.)
        );
        assert!((full.center().x - idle.center().x).abs() < 2.);
        go.send(()).unwrap();
        worker.join().unwrap();
        server.join().unwrap();
        app.poll(&ctx);
        assert_eq!(fs::metadata(&published).unwrap().len(), 65536);
        assert_eq!(paint(&mut app).1, Some(true));
        app.dock.hold = None;
        for _ in 0..16 {
            paint(&mut app);
        }
        let (rest, edge) = paint(&mut app);
        assert_eq!(edge, None);
        assert!((rest.width() - idle.width()).abs() < 0.1);
    }
}
