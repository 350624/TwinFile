use super::*;
use std::collections::{HashMap, VecDeque};
use std::time::SystemTime;
type ThumbKey = (PathBuf, Option<SystemTime>, u64, u32);
type ThumbImage = (ThumbKey, Option<egui::ColorImage>);
mod android;
mod shell;

fn apk_name(name: &str) -> bool {
    let name = name.to_ascii_lowercase();
    name.ends_with(".apk")
        || name
            .rsplit_once(".apk.")
            .is_some_and(|(_, n)| !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()))
}

fn side_px(path: &Path, ppp: f32) -> u32 {
    let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("");
    let ext = path
        .extension()
        .and_then(|e| e.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    // Keep previews unchanged; match the 36-point folder tile for associated file icons.
    let points = if apk_name(name)
        || matches!(
            ext.as_str(),
            "png" | "jpg" | "jpeg" | "gif" | "webp" | "bmp" | "ico" | "exe" | "dll" | "lnk" | "msi"
        ) {
        32.0
    } else {
        36.0
    };
    (points * ppp).round().clamp(1., 256.) as u32
}

pub(super) fn icon_rect(center: egui::Pos2, pixels: [usize; 2], ppp: f32) -> egui::Rect {
    let size = egui::vec2(pixels[0] as f32, pixels[1] as f32);
    let corner = center.to_vec2() * ppp - size * 0.5;
    let origin = egui::pos2(corner.x.round() / ppp, corner.y.round() / ppp);
    egui::Rect::from_min_size(origin, size / ppp)
}

fn fit(im: egui::ColorImage, side: u32) -> egui::ColorImage {
    resample(im, side, false)
}

fn resample(im: egui::ColorImage, side: u32, enlarge: bool) -> egui::ColorImage {
    let [w, h] = im.size.map(|n| n as u32);
    let long = w.max(h);
    if long == side || (long < side && !enlarge) {
        return im;
    }
    let dim = |n: u32| ((n * side + long / 2) / long).max(1);
    // Filter premultiplied channels to keep transparent edges free of dark halos.
    let src = image::RgbaImage::from_fn(w, h, |x, y| {
        image::Rgba(im[(x as usize, y as usize)].to_array())
    });
    let mut dst =
        image::imageops::resize(&src, dim(w), dim(h), image::imageops::FilterType::Lanczos3);
    if long < side {
        // A bounded detail boost on opaque strokes, not alpha: preserve lettering
        // without ringing outside the silhouette or adding fringes on dark themes.
        let soft = image::imageops::blur(&dst, 0.8);
        for (p, low) in dst.pixels_mut().zip(soft.pixels()) {
            let alpha = p[3] as i16;
            for c in 0..3 {
                let value = p[c] as i16;
                let detail = if alpha >= 245 && low[3] >= 245 {
                    ((value - low[c] as i16) / 2).clamp(-12, 12)
                } else {
                    0
                };
                p[c] = (value + detail).clamp(0, alpha) as u8;
            }
        }
    }
    egui::ColorImage::from_rgba_premultiplied(
        [dst.width() as usize, dst.height() as usize],
        dst.as_raw(),
    )
}

fn thumbnail(path: &Path, side: u32) -> Option<egui::ColorImage> {
    source_image(path, side).map(|im| fit(im, side))
}

fn source_image(path: &Path, side: u32) -> Option<egui::ColorImage> {
    let extension = path
        .extension()
        .and_then(|s| s.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    if path
        .file_name()
        .and_then(|s| s.to_str())
        .is_some_and(apk_name)
    {
        return android::icon(path).or_else(|| shell::type_icon("apk", side));
    }
    if matches!(
        extension.as_str(),
        "png" | "jpg" | "jpeg" | "gif" | "webp" | "bmp" | "ico"
    ) && let Some(image) = dialogs::load_image(path, side.max(64))
    {
        return Some(image);
    }
    if matches!(extension.as_str(), "exe" | "dll" | "lnk" | "msi")
        && let Some(image) = shell::file_icon(path, side)
    {
        return Some(image);
    }
    shell::type_icon(&extension, side)
}
pub(super) struct Cache {
    map: HashMap<ThumbKey, Option<egui::TextureHandle>>,
    order: VecDeque<ThumbKey>,
    inflight: BTreeSet<ThumbKey>,
    tx: mpsc::Sender<ThumbImage>,
    rx: Receiver<ThumbImage>,
}
impl Default for Cache {
    fn default() -> Self {
        let (tx, rx) = mpsc::channel();
        Self {
            map: HashMap::new(),
            order: VecDeque::new(),
            inflight: BTreeSet::new(),
            tx,
            rx,
        }
    }
}
impl Cache {
    pub(super) fn image(
        &mut self,
        e: &files::Entry,
        ppp: f32,
    ) -> Option<(egui::TextureId, [usize; 2])> {
        if e.dir {
            return None;
        }
        let key = (e.path.clone(), e.modified, e.size, side_px(&e.path, ppp));
        if let Some(t) = self.map.get(&key) {
            return t.as_ref().map(|t| (t.id(), t.size()));
        }
        if self.inflight.len() < 4 && self.inflight.insert(key.clone()) {
            let tx = self.tx.clone();
            std::thread::spawn(move || {
                let image = std::panic::catch_unwind(|| thumbnail(&key.0, key.3))
                    .ok()
                    .flatten();
                let _ = tx.send((key, image));
            });
        }
        None
    }
    pub(super) fn poll(&mut self, ctx: &egui::Context) {
        while let Ok((key, im)) = self.rx.try_recv() {
            self.inflight.remove(&key);
            let tex =
                im.map(|im| ctx.load_texture(key.0.to_string_lossy(), im, Default::default()));
            self.map.insert(key.clone(), tex);
            self.order.push_back(key);
            while self.order.len() > 160 {
                if let Some(old) = self.order.pop_front() {
                    self.map.remove(&old);
                }
            }
        }
        if !self.inflight.is_empty() {
            ctx.request_repaint_after(Duration::from_millis(70));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zip_icons_are_drawn_texel_for_pixel_at_each_scale() {
        for (ppp, side) in [(1.0, 36), (1.25, 45), (1.5, 54), (2.0, 72)] {
            let path = Path::new("DeskBox.zip");
            assert_eq!(side_px(path, ppp), side);
            let icon = thumbnail(path, side).unwrap();
            assert!(icon.pixels.iter().any(|p| p.a() > 0));
            assert_eq!(icon.size, [side as usize; 2]);
            let r = icon_rect(egui::pos2(23.27, 47.6), icon.size, ppp);
            for coord in [r.min.x, r.min.y, r.max.x, r.max.y] {
                assert!((coord * ppp - (coord * ppp).round()).abs() < 0.0001);
            }
            assert!((r.width() * ppp - side as f32).abs() < 0.0001);
            println!(
                "DeskBox.zip scale={ppp}: texture={:?}, painted={}px",
                icon.size,
                r.width() * ppp
            );
        }
    }

    #[test]
    fn previews_keep_aspect_and_never_enlarge_small_textures() {
        let image = egui::ColorImage::filled([60, 30], egui::Color32::WHITE);
        assert_eq!(fit(image.clone(), 32).size, [32, 16]);
        assert_eq!(fit(image, 96).size, [60, 30]);
    }

    #[test]
    fn tiny_association_art_is_enlarged_without_transparent_edge_fringe() {
        let mut art = egui::ColorImage::filled([16, 12], egui::Color32::TRANSPARENT);
        for y in 2..10 {
            for x in 3..13 {
                art[(x, y)] = egui::Color32::WHITE;
            }
        }
        let out = resample(art, 36, true);
        assert_eq!(out.size, [36, 27]);
        assert_eq!(out[(0, 0)], egui::Color32::TRANSPARENT);
        assert_eq!(out[(18, 13)], egui::Color32::WHITE);
        assert!(
            out.pixels
                .iter()
                .all(|p| p.r() <= p.a() && p.g() <= p.a() && p.b() <= p.a())
        );
    }

    #[test]
    fn numbered_apk_names() {
        for n in ["base.apk", "BASE.APK.1", "file.apk.42"] {
            assert!(apk_name(n));
        }
        for n in ["a.apk.", "a.apk.jpg", "a.apk.1.exe"] {
            assert!(!apk_name(n));
        }
    }

    #[test]
    fn bad_packages_do_not_prevent_other_previews() {
        let f = tempfile::NamedTempFile::new().unwrap();
        assert!(android::icon(f.path()).is_none());
        assert!(shell::type_icon("zip", 36).is_some());
    }

    #[test]
    #[ignore = "Set TWINFILE_ICON_FIXTURES and TWINFILE_ICON_OUTPUT to local test directories"]
    fn real_file_icons() {
        let source = PathBuf::from(std::env::var_os("TWINFILE_ICON_FIXTURES").unwrap());
        let out = PathBuf::from(std::env::var_os("TWINFILE_ICON_OUTPUT").unwrap());
        let mut failed = Vec::new();
        for item in fs::read_dir(source).unwrap().flatten() {
            let p = item.path();
            if !p.is_file() {
                continue;
            }
            let n = name(&p);
            let im = if apk_name(&n) {
                android::icon(&p)
            } else if n.to_lowercase().ends_with(".exe") {
                shell::file_icon(&p, 32)
            } else if n.to_lowercase().ends_with(".zip") {
                shell::type_icon("zip", 36)
            } else {
                continue;
            };
            let Some(im) = im else {
                failed.push(n);
                continue;
            };
            assert!(im.pixels.iter().any(|p| p.a() > 0));
            let buf: Vec<_> = im
                .pixels
                .iter()
                .flat_map(|p| p.to_srgba_unmultiplied())
                .collect();
            image::save_buffer(
                out.join(format!("{n}.png")),
                &buf,
                im.size[0] as u32,
                im.size[1] as u32,
                image::ColorType::Rgba8,
            )
            .unwrap();
            println!("{n}: {}x{}", im.size[0], im.size[1]);
        }
        assert!(failed.is_empty(), "Missing icons: {failed:?}");
    }
}
