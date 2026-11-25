use super::*;

// Clockwise quarter turns followed by a horizontal flip in display coordinates.
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
struct Pose {
    turns: usize,
    flipped: bool,
}
impl Pose {
    fn rotate(self) -> Self {
        Self {
            turns: (self.turns + if self.flipped { 3 } else { 1 }) % 4,
            ..self
        }
    }
    fn flip(self) -> Self {
        Self {
            flipped: !self.flipped,
            ..self
        }
    }
    fn corners(self) -> [usize; 4] {
        let mut c = [0, 1, 2, 3];
        c.rotate_right(self.turns);
        if self.flipped {
            c.swap(0, 1);
            c.swap(2, 3);
        }
        c
    }
}
#[derive(Default)]
struct History {
    current: Pose,
    undo: Vec<Pose>,
    redo: Vec<Pose>,
}
impl History {
    fn change(&mut self, to: Pose) {
        self.redo.clear();
        if self.undo.len() == 8 {
            self.undo.remove(0);
        }
        self.undo.push(self.current);
        self.current = to;
    }
    fn undo(&mut self) {
        if let Some(p) = self.undo.pop() {
            self.redo.push(self.current);
            self.current = p;
        }
    }
    fn redo(&mut self) {
        if let Some(p) = self.redo.pop() {
            self.undo.push(self.current);
            self.current = p;
        }
    }
}
fn neighbors(path: &Path) -> Vec<PathBuf> {
    let mut list = vec![];
    if let Some(dir) = path.parent().and_then(|d| fs::read_dir(d).ok()) {
        for e in dir.flatten() {
            let p = e.path();
            if p.extension().and_then(|s| s.to_str()).is_some_and(|s| {
                matches!(
                    s.to_ascii_lowercase().as_str(),
                    "png" | "jpg" | "jpeg" | "gif" | "webp" | "bmp" | "ico"
                )
            }) && e
                .file_type()
                .is_ok_and(|t| t.is_file() || (t.is_symlink() && p.is_file()))
            {
                list.push(p);
            }
        }
    }
    list.sort_by_cached_key(|p| (name(p).to_lowercase(), name(p)));
    list.truncate(5000);
    if !list.iter().any(|p| p == path) {
        return vec![path.to_owned()];
    }
    list
}

pub(super) struct Preview {
    list: Vec<PathBuf>,
    index: usize,
    tex: egui::TextureHandle,
    zoom: f32,
    history: History,
    error: Option<String>,
}
impl Preview {
    pub(super) fn open(ctx: &egui::Context, path: PathBuf) -> Option<Self> {
        let image = dialogs::load_image(&path, 2048)?;
        let list = neighbors(&path);
        let index = list.iter().position(|p| p == &path)?;
        Some(Self {
            list,
            index,
            tex: ctx.load_texture("preview", image, Default::default()),
            zoom: 1.,
            history: History::default(),
            error: None,
        })
    }
    fn step(&mut self, by: isize) {
        let next = (self.index as isize + by).rem_euclid(self.list.len() as isize) as usize;
        if next == self.index {
            return;
        }
        if let Some(im) = dialogs::load_image(&self.list[next], 2048) {
            self.tex.set(im, Default::default());
            self.history = History::default();
            self.zoom = 1.;
            self.index = next;
            self.error = None;
        } else {
            self.error = Some(format!(
                "无法打开 {}，已保留当前预览",
                name(&self.list[next])
            ));
        }
    }
    fn show(&mut self, ui: &mut egui::Ui) {
        ui.horizontal_wrapped(|ui| {
            if ui
                .add_enabled(self.list.len() > 1, egui::Button::new("上一张"))
                .clicked()
            {
                self.step(-1);
            }
            ui.label(format!("{} / {}", self.index + 1, self.list.len()));
            if ui
                .add_enabled(self.list.len() > 1, egui::Button::new("下一张"))
                .clicked()
            {
                self.step(1);
            }
            if ui.button("旋转 90°").clicked() {
                self.history.change(self.history.current.rotate());
            }
            if ui.button("水平翻转").clicked() {
                self.history.change(self.history.current.flip());
            }
            if ui
                .add_enabled(!self.history.undo.is_empty(), egui::Button::new("撤销"))
                .clicked()
            {
                self.history.undo();
            }
            if ui
                .add_enabled(!self.history.redo.is_empty(), egui::Button::new("重做"))
                .clicked()
            {
                self.history.redo();
            }
            if ui.button("适应窗口").clicked() {
                self.zoom = 1.;
            }
            ui.add(egui::Slider::new(&mut self.zoom, 0.10..=4.00).text("缩放"));
        });
        if let Some(err) = &self.error {
            ui.colored_label(ui.visuals().error_fg_color, err);
        }
        let view = ui.available_size().max(egui::vec2(32., 32.));
        let pose = self.history.current;
        let mut sz = self.tex.size_vec2();
        if pose.turns % 2 == 1 {
            sz = egui::vec2(sz.y, sz.x);
        }
        let scale = (view.x / sz.x).min(view.y / sz.y) * self.zoom;
        egui::ScrollArea::both()
            .auto_shrink([false, false])
            .show(ui, |ui| {
                let (rect, _) = ui.allocate_exact_size(sz * scale, egui::Sense::hover());
                let uv = [
                    egui::pos2(0., 0.),
                    egui::pos2(1., 0.),
                    egui::pos2(1., 1.),
                    egui::pos2(0., 1.),
                ];
                let mut mesh = egui::Mesh::with_texture(self.tex.id());
                for (pos, i) in [
                    rect.left_top(),
                    rect.right_top(),
                    rect.right_bottom(),
                    rect.left_bottom(),
                ]
                .into_iter()
                .zip(pose.corners())
                {
                    mesh.vertices.push(egui::epaint::Vertex {
                        pos,
                        uv: uv[i],
                        color: egui::Color32::WHITE,
                    });
                }
                mesh.indices.extend_from_slice(&[0, 1, 2, 0, 2, 3]);
                ui.painter().add(mesh);
            });
    }
}
impl App {
    pub(super) fn preview(&mut self, ctx: &egui::Context) {
        if let Some(im) = &mut self.preview {
            let mut open = true;
            egui::Window::new(format!("图片预览：{}", name(&im.list[im.index])))
                .id(egui::Id::new("local-image-view"))
                .open(&mut open)
                .default_size([600., 485.])
                .show(ctx, |ui| im.show(ui));
            if !open {
                self.preview = None;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn transformations_apply_in_display_order_and_undo_redo() {
        let mut h = History::default();
        h.change(h.current.rotate());
        assert_eq!(h.current.corners(), [3, 0, 1, 2]);
        h.change(h.current.flip());
        assert_eq!(h.current.corners(), [0, 3, 2, 1]);
        h.change(h.current.rotate());
        assert_eq!(h.current.corners(), [1, 0, 3, 2]);
        h.undo();
        assert_eq!(h.current.corners(), [0, 3, 2, 1]);
        h.redo();
        assert_eq!(h.current.corners(), [1, 0, 3, 2]);
        h.undo();
        h.change(h.current.flip());
        assert!(h.redo.is_empty());
    }
    #[test]
    fn opens_local_neighbors_wraps_and_retains_preview_on_failure() {
        let d = tempfile::tempdir().unwrap();
        let a = d.path().join("a.png");
        let b = d.path().join("B.PNG");
        for p in [&a, &b] {
            image::RgbaImage::new(8, 4)
                .save_with_format(p, image::ImageFormat::Png)
                .unwrap();
        }
        fs::write(d.path().join("ignore.txt"), "x").unwrap();
        fs::create_dir(d.path().join("dir.png")).unwrap();
        let original = fs::read(&a).unwrap();
        let ctx = egui::Context::default();
        let mut v = Preview::open(&ctx, a.clone()).unwrap();
        assert_eq!(v.list, vec![a.clone(), b.clone()]);
        v.history.change(v.history.current.rotate());
        v.step(-1);
        assert_eq!(v.index, 1);
        assert!(v.history.undo.is_empty());
        v.step(1);
        assert_eq!(v.index, 0);
        fs::remove_file(b).unwrap();
        v.step(1);
        assert!(v.error.is_some());
        assert_eq!(v.index, 0);
        assert_eq!(fs::read(a).unwrap(), original);
    }
}
