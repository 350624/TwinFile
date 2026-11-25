#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
mod motion;
mod theme;
mod ui;
use eframe::egui;
fn main() {
    let mut args = std::env::args_os().skip(1);
    let mode = args.next();
    let smoke = mode.as_deref() == Some(std::ffi::OsStr::new("--smoke"));
    let visual = mode.as_deref() == Some(std::ffi::OsStr::new("--theme-smoke"));
    let downloader = mode.as_deref() == Some(std::ffi::OsStr::new("--downloader"))
        || std::env::current_exe()
            .ok()
            .and_then(|p| {
                p.file_stem().map(|s| {
                    s.to_string_lossy()
                        .eq_ignore_ascii_case("TwinFileRustDownloader")
                })
            })
            .unwrap_or(false);
    let smoke_dir = if smoke || visual {
        args.next().map(std::path::PathBuf::from)
    } else {
        None
    };
    let initial = if downloader && mode.as_deref() == Some(std::ffi::OsStr::new("--browser-stdin"))
    {
        use twinfile_rust::bridge::{self, Request};
        match bridge::recv::<Request>(std::io::stdin().lock()).and_then(Request::validated) {
            Ok(v) => Some(v),
            Err(_) => std::process::exit(2),
        }
    } else {
        None
    };
    let icon = image::load_from_memory_with_format(
        include_bytes!("../assets/app.png"),
        image::ImageFormat::Png,
    )
    .ok()
    .map(|im| {
        let rgba = im.to_rgba8();
        let (width, height) = rgba.dimensions();
        egui::IconData {
            rgba: rgba.into_raw(),
            width,
            height,
        }
    });
    let mut viewport = egui::ViewportBuilder::default()
        .with_inner_size([1180.0, 740.0])
        .with_min_inner_size([680.0, 460.0]);
    if let Some(icon) = icon {
        viewport = viewport.with_icon(icon);
    }
    let opts = eframe::NativeOptions {
        viewport,
        centered: true,
        ..Default::default()
    };
    let name = if downloader {
        "TwinFile Rust · 直链下载"
    } else {
        "TwinFile Rust"
    };
    if let Err(e) = eframe::run_native(
        name,
        opts,
        Box::new(move |cc| {
            Ok(Box::new(ui::App::new(
                cc, downloader, smoke_dir, visual, initial,
            )))
        }),
    ) {
        if !smoke {
            let _ = rfd::MessageDialog::new()
                .set_title(name)
                .set_description(e.to_string())
                .set_level(rfd::MessageLevel::Error)
                .show();
        }
        std::process::exit(1);
    }
}
