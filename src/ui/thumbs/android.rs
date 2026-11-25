use super::*;
use apk_info_axml::{ARSC, AXML, structs::ResTableEntry};
use apk_info_xml::Element;
use image::{Rgba, RgbaImage};
use std::io::{Cursor, Read};

const SIDE: u32 = 64;
type Archive = zip::ZipArchive<std::io::BufReader<std::fs::File>>;

fn entry(zip: &mut Archive, name: &str, limit: u64) -> Option<Vec<u8>> {
    let f = zip.by_name(name).ok()?;
    if f.size() > limit {
        return None;
    }
    let mut buf = Vec::with_capacity(f.size() as usize);
    f.take(limit + 1).read_to_end(&mut buf).ok()?;
    (buf.len() as u64 <= limit).then_some(buf)
}

// Keep density variants: the default drawable may be XML while another
// configuration contains a finished launcher bitmap under an obfuscated name.
fn variants(table: &ARSC, raw: &str) -> Vec<String> {
    let Some(hex) = raw.strip_prefix('@') else {
        return vec![raw.to_owned()];
    };
    let Ok(id) = u32::from_str_radix(hex, 16) else {
        return vec![];
    };
    let mut values = Vec::new();
    for pkg in table.packages().filter(|p| p.header.id == id >> 24) {
        for (config, types) in &pkg.resources {
            if let Some(ResTableEntry::Default(e)) = types
                .get(&((id >> 16) as u8))
                .and_then(|t| t.find(id as u16))
            {
                let value = if matches!(
                    e.value.data_type,
                    apk_info_axml::structs::ResourceValueType::Reference
                        | apk_info_axml::structs::ResourceValueType::DynamicReference
                ) {
                    format!("@{:08x}", e.value.data)
                } else {
                    table.value_to_string(&e.value)
                };
                let density = config.get_orientation_touchscreen_density().2;
                values.push((value.ends_with(".xml"), density.abs_diff(320), value));
            }
        }
    }
    values.sort();
    let mut seen = BTreeSet::new();
    values
        .into_iter()
        .map(|(_, _, v)| v)
        .filter(|v| seen.insert(v.clone()))
        .take(16)
        .collect()
}

fn rgba(raw: &str) -> Option<Rgba<u8>> {
    let s = raw.strip_prefix('#')?;
    let v = u32::from_str_radix(s, 16).ok()?;
    let a = match s.len() {
        6 => 255,
        8 => (v >> 24) as u8,
        _ => return None,
    };
    Some(Rgba([(v >> 16) as u8, (v >> 8) as u8, v as u8, a]))
}

fn paint(raw: &str, table: &ARSC, depth: u8) -> Option<Rgba<u8>> {
    if let Some(c) = rgba(raw) {
        return Some(c);
    }
    match raw {
        "@android:color/white" | "@0106000b" => return Some(Rgba([255; 4])),
        "@android:color/black" | "@0106000c" => return Some(Rgba([0, 0, 0, 255])),
        "@android:color/transparent" | "@0106000d" => return Some(Rgba([0; 4])),
        _ => {}
    }
    if depth == 0 || !raw.starts_with('@') {
        return None;
    }
    variants(table, raw)
        .iter()
        .find_map(|v| paint(v, table, depth - 1))
}

fn number(n: &Element, key: &str, default: f32) -> f32 {
    n.attr(key)
        .and_then(|v| v.parse::<f32>().ok())
        .filter(|v| v.is_finite())
        .unwrap_or(default)
}

fn vector(root: &Element, table: &ARSC) -> Option<RgbaImage> {
    use std::fmt::Write as _;
    fn draw(n: &Element, table: &ARSC, out: &mut String, depth: u8) -> Option<()> {
        if depth > 16 || out.len() > 512 * 1024 {
            return None;
        }
        match n.name() {
            "path" => {
                let data = n.attr("pathData")?;
                if !data.bytes().all(|b| {
                    b.is_ascii_alphanumeric() || b.is_ascii_whitespace() || b".,+-".contains(&b)
                }) {
                    return None;
                }
                write!(out, "<path d=\"{data}\"").ok()?;
                for (android, svg, alpha) in [
                    ("fillColor", "fill", "fillAlpha"),
                    ("strokeColor", "stroke", "strokeAlpha"),
                ] {
                    let color = n.attr(android).and_then(|s| paint(s, table, 5));
                    if let Some(Rgba([r, g, b, a])) = color {
                        write!(
                            out,
                            " {svg}=\"#{r:02x}{g:02x}{b:02x}\" {svg}-opacity=\"{}\"",
                            number(n, alpha, 1.0) * f32::from(a) / 255.
                        )
                        .ok()?;
                    } else {
                        write!(out, " {svg}=\"none\"").ok()?;
                    }
                }
                let rule = if matches!(n.attr("fillType"), Some("1" | "evenOdd")) {
                    "evenodd"
                } else {
                    "nonzero"
                };
                let cap = match n.attr("strokeLineCap") {
                    Some("1" | "round") => "round",
                    Some("2" | "square") => "square",
                    _ => "butt",
                };
                let join = match n.attr("strokeLineJoin") {
                    Some("1" | "round") => "round",
                    Some("2" | "bevel") => "bevel",
                    _ => "miter",
                };
                write!(
                    out,
                    " fill-rule=\"{rule}\" stroke-linecap=\"{cap}\" stroke-linejoin=\"{join}\" stroke-width=\"{}\"/>",
                    number(n, "strokeWidth", 0.)
                )
                .ok()?;
            }
            "group" => {
                let (x, y) = (number(n, "pivotX", 0.), number(n, "pivotY", 0.));
                write!(
                    out,
                    "<g transform=\"translate({} {}) rotate({}) scale({} {}) translate({} {})\">",
                    x + number(n, "translateX", 0.),
                    y + number(n, "translateY", 0.),
                    number(n, "rotation", 0.),
                    number(n, "scaleX", 1.),
                    number(n, "scaleY", 1.),
                    -x,
                    -y
                )
                .ok()?;
                for child in n.childrens() {
                    draw(child, table, out, depth + 1)?;
                }
                out.push_str("</g>");
            }
            _ => return None,
        }
        Some(())
    }
    let (w, h) = (
        number(root, "viewportWidth", 0.),
        number(root, "viewportHeight", 0.),
    );
    if w <= 0. || h <= 0. {
        return None;
    }
    let mut svg = format!(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\" viewBox=\"0 0 {w} {h}\">"
    );
    for child in root.childrens() {
        draw(child, table, &mut svg, 0)?;
    }
    svg.push_str("</svg>");
    let tree = resvg::usvg::Tree::from_str(&svg, &Default::default()).ok()?;
    let mut px = resvg::tiny_skia::Pixmap::new(SIDE, SIDE)?;
    resvg::render(&tree, Default::default(), &mut px.as_mut());
    if px.pixels().iter().all(|p| p.alpha() == 0) {
        return None;
    }
    Some(RgbaImage::from_fn(SIDE, SIDE, |x, y| {
        let p = px.pixel(x, y).unwrap().demultiply();
        Rgba([p.red(), p.green(), p.blue(), p.alpha()])
    }))
}

struct Renderer<'a> {
    zip: &'a mut Archive,
    table: &'a ARSC,
    budget: usize,
}
impl Renderer<'_> {
    fn resource(&mut self, raw: &str, depth: u8) -> Option<RgbaImage> {
        if depth > 10 || self.budget == 0 {
            return None;
        }
        self.budget -= 1;
        if let Some(c) = rgba(raw) {
            return Some(RgbaImage::from_pixel(SIDE, SIDE, c));
        }
        if raw.starts_with('@') {
            if let Some(c) = paint(raw, self.table, 3) {
                return Some(RgbaImage::from_pixel(SIDE, SIDE, c));
            }
            return variants(self.table, raw)
                .iter()
                .find_map(|v| self.resource(v, depth + 1));
        }
        let buf = entry(self.zip, raw, 4 * 1024 * 1024)?;
        if raw.ends_with(".xml") {
            let xml = AXML::new(&mut &buf[..], None).ok()?;
            return self.element(&xml.root, depth + 1);
        }
        let mut reader = image::ImageReader::new(Cursor::new(buf))
            .with_guessed_format()
            .ok()?;
        let mut limits = image::Limits::default();
        limits.max_image_width = Some(2048);
        limits.max_image_height = Some(2048);
        limits.max_alloc = Some(32 * 1024 * 1024);
        reader.limits(limits);
        Some(
            reader
                .decode()
                .ok()?
                .resize_exact(SIDE, SIDE, image::imageops::FilterType::Triangle)
                .to_rgba8(),
        )
    }
    fn element(&mut self, n: &Element, depth: u8) -> Option<RgbaImage> {
        if depth > 10 {
            return None;
        }
        match n.name() {
            "vector" => vector(n, self.table),
            "bitmap" => self.resource(n.attr("src")?, depth + 1),
            "shape" => Some(RgbaImage::from_pixel(
                SIDE,
                SIDE,
                paint(
                    n.childrens().find(|c| c.name() == "solid")?.attr("color")?,
                    self.table,
                    5,
                )?,
            )),
            "adaptive-icon" => {
                let layer = |this: &mut Self, name: &str| {
                    let node = n.childrens().find(|c| c.name() == name)?;
                    if let Some(s) = node.attr("drawable") {
                        this.resource(s, depth + 1)
                    } else {
                        this.element(node.childrens().next()?, depth + 1)
                    }
                };
                let mut bg = layer(self, "background")?;
                image::imageops::overlay(&mut bg, &layer(self, "foreground")?, 0, 0);
                let crop = image::imageops::crop_imm(&bg, 10, 10, 44, 44).to_image();
                let mut icon = image::imageops::resize(
                    &crop,
                    SIDE,
                    SIDE,
                    image::imageops::FilterType::Triangle,
                );
                for (x, y, p) in icon.enumerate_pixels_mut() {
                    let dx = ((x as f32 + 0.5 - 32.).abs() - 19.).max(0.);
                    let dy = ((y as f32 + 0.5 - 32.).abs() - 19.).max(0.);
                    p[3] = (f32::from(p[3]) * (13.5 - dx.hypot(dy)).clamp(0., 1.)) as u8;
                }
                Some(icon)
            }
            _ => None,
        }
    }
}

pub(super) fn icon(path: &Path) -> Option<egui::ColorImage> {
    let mut zip = Archive::new(std::io::BufReader::new(std::fs::File::open(path).ok()?)).ok()?;
    if zip.len() > 100_000 {
        return None;
    }
    let manifest = entry(&mut zip, "AndroidManifest.xml", 4 * 1024 * 1024)?;
    let xml = AXML::new(&mut &manifest[..], None).ok()?;
    let app = xml.root.childrens().find(|n| n.name() == "application")?;
    let launcher = app.childrens().find(|act| {
        matches!(act.name(), "activity" | "activity-alias")
            && !matches!(act.attr("enabled"), Some("false" | "0"))
            && act.childrens().any(|f| {
                f.name() == "intent-filter"
                    && f.childrens().any(|c| {
                        c.name() == "action" && c.attr("name") == Some("android.intent.action.MAIN")
                    })
                    && f.childrens().any(|c| {
                        c.name() == "category"
                            && c.attr("name") == Some("android.intent.category.LAUNCHER")
                    })
            })
    });
    let bytes = entry(&mut zip, "resources.arsc", 48 * 1024 * 1024)?;
    let table = ARSC::new(&mut &bytes[..]).ok()?;
    let mut renderer = Renderer {
        zip: &mut zip,
        table: &table,
        budget: 64,
    };
    let image = [
        launcher.and_then(|n| n.attr("icon")),
        app.attr("icon"),
        app.attr("roundIcon"),
    ]
    .into_iter()
    .flatten()
    .find_map(|s| renderer.resource(s, 0))?;
    Some(egui::ColorImage::from_rgba_unmultiplied(
        [SIDE as usize; 2],
        image.as_raw(),
    ))
}
