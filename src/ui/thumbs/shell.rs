use super::*;
use windows::Win32::{
    Foundation::SIZE,
    Graphics::Gdi::*,
    System::Com::{COINIT_APARTMENTTHREADED, CoInitializeEx, CoUninitialize},
    UI::Shell::*,
};
use windows::core::HSTRING;

struct Apartment;
impl Drop for Apartment {
    fn drop(&mut self) {
        unsafe { CoUninitialize() }
    }
}

pub(super) fn file_icon(path: &Path, side: u32) -> Option<egui::ColorImage> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::UI::WindowsAndMessaging::PrivateExtractIconsW;
    unsafe {
        let ext = path.extension().and_then(|e| e.to_str()).unwrap_or("");
        if ext.eq_ignore_ascii_case("exe") || ext.eq_ignore_ascii_case("dll") {
            let wide: Vec<_> = path.as_os_str().encode_wide().chain(Some(0)).collect();
            let mut h = std::ptr::null_mut();
            if PrivateExtractIconsW(
                wide.as_ptr(),
                0,
                side as i32,
                side as i32,
                &mut h,
                std::ptr::null_mut(),
                1,
                0,
            ) == 1
                && !h.is_null()
                && let Some(im) = owned_icon(h)
            {
                return Some(im);
            }
        }
        CoInitializeEx(None, COINIT_APARTMENTTHREADED).ok().ok()?;
        let _com = Apartment;
        let factory: IShellItemImageFactory =
            SHCreateItemFromParsingName(&HSTRING::from(path.as_os_str()), None).ok()?;
        let bitmap = factory
            .GetImage(
                SIZE {
                    cx: side as i32,
                    cy: side as i32,
                },
                SIIGBF_ICONONLY,
            )
            .ok()?;
        let result = bitmap_image(bitmap);
        let _ = DeleteObject(bitmap.into());
        result
    }
}

pub(super) fn type_icon(extension: &str, side: u32) -> Option<egui::ColorImage> {
    use windows_sys::Win32::{Storage::FileSystem::FILE_ATTRIBUTE_NORMAL, UI::Shell::*};
    // Association lookup can return no icon when first invoked concurrently by list workers.
    static LOOKUP: std::sync::Mutex<()> = std::sync::Mutex::new(());
    let _guard = LOOKUP.lock().unwrap_or_else(|e| e.into_inner());
    let name: Vec<u16> = format!("file.{extension}")
        .encode_utf16()
        .chain(Some(0))
        .collect();
    unsafe {
        CoInitializeEx(None, COINIT_APARTMENTTHREADED).ok().ok()?;
        let _com = Apartment;
        let mut info = SHFILEINFOW::default();
        if SHGetFileInfoW(
            name.as_ptr(),
            FILE_ATTRIBUTE_NORMAL,
            &mut info,
            size_of::<SHFILEINFOW>() as u32,
            SHGFI_ICON | SHGFI_SYSICONINDEX | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES,
        ) == 0
        {
            return None;
        }
        let mut best = owned_icon(info.hIcon)?;
        let base = best.size[0];
        let base_ink = ink_span(&best);
        for list_size in [SHIL_EXTRALARGE, SHIL_JUMBO] {
            if best.size[0] >= side as usize {
                break;
            }
            use windows::Win32::UI::Controls::{IImageList, ILD_TRANSPARENT};
            let Ok(list) =
                windows::Win32::UI::Shell::SHGetImageList::<IImageList>(list_size as i32)
            else {
                continue;
            };
            let Ok(icon) = list.GetIcon(info.iIcon, ILD_TRANSPARENT.0) else {
                continue;
            };
            if let Some(im) = owned_icon(icon.0) {
                // Jumbo lists can pad a tiny icon with transparent pixels. Do not
                // downscale that empty canvas into a microscopic list glyph.
                let ink = ink_span(&im);
                if im.size[0] > best.size[0]
                    && ink > ink_span(&best)
                    && ink * base * 5 >= base_ink * im.size[0] * 4
                {
                    best = im;
                }
            }
        }
        Some(super::resample(best, side, true))
    }
}

fn ink_span(im: &egui::ColorImage) -> usize {
    let [w, h] = im.size;
    let (mut x0, mut y0, mut x1, mut y1) = (w, h, 0, 0);
    for (i, _) in im.pixels.iter().enumerate().filter(|(_, p)| p.a() >= 32) {
        let (x, y) = (i % w, i / w);
        x0 = x0.min(x);
        y0 = y0.min(y);
        x1 = x1.max(x + 1);
        y1 = y1.max(y + 1);
    }
    x1.saturating_sub(x0).max(y1.saturating_sub(y0))
}

fn owned_icon(
    icon: windows_sys::Win32::UI::WindowsAndMessaging::HICON,
) -> Option<egui::ColorImage> {
    use windows_sys::Win32::{
        Graphics::Gdi::DeleteObject as delete_object,
        UI::WindowsAndMessaging::{DestroyIcon, GetIconInfo, ICONINFO},
    };
    unsafe {
        let mut bits = ICONINFO::default();
        let ok = GetIconInfo(icon, &mut bits) != 0;
        DestroyIcon(icon);
        if !ok {
            return None;
        }
        let image = bitmap_image(HBITMAP(bits.hbmColor));
        if !bits.hbmColor.is_null() {
            delete_object(bits.hbmColor);
        }
        if !bits.hbmMask.is_null() {
            delete_object(bits.hbmMask);
        }
        image
    }
}

fn bitmap_image(bitmap: HBITMAP) -> Option<egui::ColorImage> {
    unsafe {
        let mut bm = BITMAP::default();
        if GetObjectW(
            bitmap.into(),
            size_of::<BITMAP>() as i32,
            Some((&raw mut bm).cast()),
        ) == 0
            || !(1..=256).contains(&bm.bmWidth)
            || !(1..=256).contains(&bm.bmHeight)
        {
            return None;
        }
        let (w, h) = (bm.bmWidth, bm.bmHeight);
        let mut info = BITMAPINFO::default();
        info.bmiHeader = BITMAPINFOHEADER {
            biSize: size_of::<BITMAPINFOHEADER>() as u32,
            biWidth: w,
            biHeight: -h,
            biPlanes: 1,
            biBitCount: 32,
            biCompression: BI_RGB.0,
            ..Default::default()
        };
        let mut px = vec![0u8; (w * h * 4) as usize];
        let dc = GetDC(None);
        if dc.is_invalid() {
            return None;
        }
        let lines = GetDIBits(
            dc,
            bitmap,
            0,
            h as u32,
            Some(px.as_mut_ptr().cast()),
            &raw mut info,
            DIB_RGB_COLORS,
        );
        ReleaseDC(None, dc);
        if lines != h {
            return None;
        }
        let opaque = px.chunks_exact(4).all(|p| p[3] == 0);
        for p in px.chunks_exact_mut(4) {
            p.swap(0, 2);
            if opaque {
                p[3] = 255;
            }
        }
        Some(egui::ColorImage::from_rgba_premultiplied(
            [w as usize, h as usize],
            &px,
        ))
    }
}
