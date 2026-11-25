use std::{
    io,
    os::windows::ffi::{OsStrExt, OsStringExt},
    path::PathBuf,
    ptr,
    sync::atomic::{AtomicBool, Ordering::Relaxed},
};
use windows_sys::Win32::{
    Foundation::{GlobalFree, HGLOBAL, HWND, LPARAM, LRESULT, WPARAM},
    System::{
        DataExchange::{
            CloseClipboard, EmptyClipboard, GetClipboardData, IsClipboardFormatAvailable,
            OpenClipboard, RegisterClipboardFormatW, SetClipboardData,
        },
        Memory::{GMEM_MOVEABLE, GlobalAlloc, GlobalLock, GlobalSize, GlobalUnlock},
    },
    UI::{
        Input::KeyboardAndMouse::{GetActiveWindow, GetKeyState, VK_CONTROL, VK_MENU},
        Shell::{
            DROPFILES, DefSubclassProc, DragQueryFileW, RemoveWindowSubclass, SetWindowSubclass,
        },
        WindowsAndMessaging::{WM_KEYDOWN, WM_NCDESTROY},
    },
};

const FILES: u32 = 15; // CF_HDROP
const MAX_PATHS: usize = 16_384;
const MAX_BYTES: usize = 16 * 1024 * 1024;
const SUBCLASS: usize = 0x5446_2026;
static PASTE: AtomicBool = AtomicBool::new(false);
static INSTALLED: AtomicBool = AtomicBool::new(false);

unsafe extern "system" fn subclass(
    hwnd: HWND,
    msg: u32,
    w: WPARAM,
    l: LPARAM,
    id: usize,
    _: usize,
) -> LRESULT {
    if msg == WM_KEYDOWN
        && w == b'V' as usize
        && l & (1 << 30) == 0
        && unsafe { GetKeyState(VK_CONTROL as i32) < 0 && GetKeyState(VK_MENU as i32) >= 0 }
    {
        // Winit turns Ctrl+V into a *text* clipboard read before egui sees it.
        // Observe the owned window's key message without intercepting the text event.
        PASTE.store(true, Relaxed);
    } else if msg == WM_NCDESTROY {
        unsafe { RemoveWindowSubclass(hwnd, Some(subclass), id) };
        INSTALLED.store(false, Relaxed);
    }
    unsafe { DefSubclassProc(hwnd, msg, w, l) }
}
pub(super) fn paste_key() -> bool {
    if !INSTALLED.load(Relaxed) {
        let hwnd = unsafe { GetActiveWindow() };
        if !hwnd.is_null() && unsafe { SetWindowSubclass(hwnd, Some(subclass), SUBCLASS, 0) } != 0 {
            INSTALLED.store(true, Relaxed);
        }
    }
    PASTE.swap(false, Relaxed)
}

struct Open;
impl Open {
    fn clipboard(for_write: bool) -> io::Result<Self> {
        let hwnd = unsafe { GetActiveWindow() };
        if for_write && hwnd.is_null() {
            return Err(io::Error::other("请先激活 TwinFile 窗口"));
        }
        if unsafe { OpenClipboard(hwnd) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(Self)
    }
}
impl Drop for Open {
    fn drop(&mut self) {
        unsafe { CloseClipboard() };
    }
}
struct Memory(HGLOBAL);
impl Memory {
    fn allocate(data: &[u8]) -> io::Result<Self> {
        let this = Self(unsafe { GlobalAlloc(GMEM_MOVEABLE, data.len()) });
        if this.0.is_null() {
            return Err(io::Error::last_os_error());
        }
        let dest = unsafe { GlobalLock(this.0) };
        if dest.is_null() {
            return Err(io::Error::last_os_error());
        }
        unsafe {
            ptr::copy_nonoverlapping(data.as_ptr(), dest.cast::<u8>(), data.len());
            GlobalUnlock(this.0);
        }
        Ok(this)
    }
    fn publish(mut self, format: u32) -> io::Result<()> {
        if unsafe { SetClipboardData(format, self.0) }.is_null() {
            return Err(io::Error::last_os_error());
        }
        self.0 = ptr::null_mut();
        Ok(())
    }
}
impl Drop for Memory {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { GlobalFree(self.0) };
        }
    }
}

fn selected_file_block(paths: &[PathBuf]) -> io::Result<Vec<u8>> {
    if paths.is_empty() || paths.len() > MAX_PATHS {
        return Err(io::Error::other("没有选择项目或选择项目过多"));
    }
    let head = size_of::<DROPFILES>();
    let mut data = vec![0; head];
    data[..4].copy_from_slice(&(head as u32).to_le_bytes());
    data[16] = 1; // fWide
    for path in paths {
        if !path.is_absolute() {
            return Err(io::Error::other("剪贴板只能使用本地或共享文件的绝对路径"));
        }
        for x in path.as_os_str().encode_wide() {
            if x == 0 || data.len() >= MAX_BYTES - 4 {
                return Err(io::Error::other("路径列表包含无效字符或太长"));
            }
            data.extend_from_slice(&x.to_le_bytes());
        }
        data.extend_from_slice(&[0, 0]);
    }
    data.extend_from_slice(&[0, 0]);
    Ok(data)
}
fn effect_id() -> io::Result<u32> {
    let pref = "Preferred DropEffect"
        .encode_utf16()
        .chain(Some(0))
        .collect::<Vec<_>>();
    let id = unsafe { RegisterClipboardFormatW(pref.as_ptr()) };
    if id == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(id)
    }
}
pub(super) fn copy(paths: &[PathBuf], moving: bool) -> io::Result<()> {
    let data = Memory::allocate(&selected_file_block(paths)?)?;
    let effect = Memory::allocate(&(if moving { 2u32 } else { 1u32 }).to_le_bytes())?;
    let effect_format = effect_id()?;
    let _open = Open::clipboard(true)?;
    if unsafe { EmptyClipboard() } == 0 {
        return Err(io::Error::last_os_error());
    }
    // Publish the effect first so a failed cut cannot unexpectedly become a file copy.
    effect.publish(effect_format)?;
    data.publish(FILES)
}
pub(super) fn files() -> io::Result<Option<(Vec<PathBuf>, bool)>> {
    if unsafe { IsClipboardFormatAvailable(FILES) } == 0 {
        return Ok(None);
    }
    let _open = Open::clipboard(false)?;
    let raw = unsafe { GetClipboardData(FILES) };
    if raw.is_null() {
        return Err(io::Error::last_os_error());
    }
    let extent = unsafe { GlobalSize(raw) };
    if !(size_of::<DROPFILES>()..=MAX_BYTES).contains(&extent) {
        return Err(io::Error::other("剪贴板文件列表长度无效"));
    }
    let n = unsafe { DragQueryFileW(raw, u32::MAX, ptr::null_mut(), 0) } as usize;
    if n == 0 || n > MAX_PATHS {
        return Err(io::Error::other("剪贴板文件数量无效"));
    }
    let mut paths = Vec::with_capacity(n);
    for idx in 0..n as u32 {
        let len = unsafe { DragQueryFileW(raw, idx, ptr::null_mut(), 0) };
        if len == 0 || len > 32767 {
            return Err(io::Error::other("剪贴板路径长度无效"));
        }
        let mut name = vec![0u16; len as usize + 1];
        if unsafe { DragQueryFileW(raw, idx, name.as_mut_ptr(), len + 1) } != len {
            return Err(io::Error::other("读取剪贴板路径失败"));
        }
        let p = PathBuf::from(std::ffi::OsString::from_wide(&name[..len as usize]));
        if !p.is_absolute() {
            return Err(io::Error::other("剪贴板文件路径不是绝对路径"));
        }
        paths.push(p);
    }
    let e = unsafe { GetClipboardData(effect_id()?) };
    let mut moving = false;
    if !e.is_null() && unsafe { GlobalSize(e) } >= 4 {
        let p = unsafe { GlobalLock(e) };
        if !p.is_null() {
            moving = unsafe { p.cast::<u32>().read_unaligned() == 2 };
            unsafe { GlobalUnlock(e) };
        }
    }
    Ok(Some((paths, moving)))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn unicode_hdrop_uses_full_windows_paths_and_double_nil() {
        let paths = [PathBuf::from("C:/测试 A.txt"), PathBuf::from("D:/B.txt")];
        let b = selected_file_block(&paths).unwrap();
        assert_eq!(b[0], 20);
        assert_eq!(b[16], 1);
        let unit = b[20..]
            .chunks_exact(2)
            .map(|b| u16::from_le_bytes([b[0], b[1]]))
            .collect::<Vec<_>>();
        assert_eq!(
            String::from_utf16(&unit).unwrap(),
            "C:/测试 A.txt\0D:/B.txt\0\0"
        );
        assert!(selected_file_block(&[PathBuf::from("relative")]).is_err());
    }
}
