#[cfg(test)]
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::{Read, Write},
    net::Ipv4Addr,
    os::windows::process::CommandExt,
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::atomic::{AtomicBool, Ordering},
    thread,
    time::{Duration, Instant},
};
const ADB: &[u8] = include_bytes!("../../../assets/hfx/adb.exe");
const API: &[u8] = include_bytes!("../../../assets/hfx/AdbWinApi.dll");
const USB: &[u8] = include_bytes!("../../../assets/hfx/AdbWinUsbApi.dll");
const APK: &[u8] = include_bytes!("../../../assets/hfx/HybridFileXfer-phone.apk");
const PKG: &str = "top.weixiansen574.hybridfilexfer";
const ACTIVITY: &str = "top.weixiansen574.hybridfilexfer/.ClientActivity";
fn fail(e: impl std::fmt::Display) -> String {
    e.to_string()
}
fn cache() -> Result<PathBuf, String> {
    std::env::var_os("LOCALAPPDATA")
        .map(|p| PathBuf::from(p).join("TwinFileRust/hfx-v300-adb37.0.1"))
        .ok_or_else(|| "未找到 LOCALAPPDATA".into())
}
fn check_stop(stop: &AtomicBool) -> Result<(), String> {
    if stop.load(Ordering::Relaxed) {
        Err("已取消".into())
    } else {
        Ok(())
    }
}
fn unpack(dir: &Path, name: &str, bytes: &[u8]) -> Result<PathBuf, String> {
    let target = dir.join(name);
    let verify = || fs::read(&target).is_ok_and(|b| b.as_slice() == bytes);
    if verify() {
        return Ok(target);
    }
    let mut tmp = tempfile::NamedTempFile::new_in(dir).map_err(fail)?;
    tmp.write_all(bytes).map_err(fail)?;
    tmp.flush().map_err(fail)?;
    if let Err(e) = tmp.persist(&target) {
        if !verify() {
            return Err(format!("无法准备配套组件 {name}：{e}"));
        }
    }
    Ok(target)
}
fn adb_path() -> Result<PathBuf, String> {
    let d = cache()?;
    fs::create_dir_all(&d).map_err(fail)?;
    unpack(&d, "AdbWinApi.dll", API)?;
    unpack(&d, "AdbWinUsbApi.dll", USB)?;
    unpack(&d, "adb.exe", ADB)
}
fn run(exe: &Path, args: &[&str], timeout: u64, stop: &AtomicBool) -> Result<String, String> {
    check_stop(stop)?;
    let capture = tempfile::NamedTempFile::new().map_err(fail)?;
    let log = capture.reopen().map_err(fail)?;
    let mut child = Command::new(exe)
        .args(args)
        .current_dir(exe.parent().ok_or("工具路径无效")?)
        .creation_flags(0x08000000)
        .stdin(Stdio::null())
        .stdout(Stdio::from(log.try_clone().map_err(fail)?))
        .stderr(Stdio::from(log))
        .spawn()
        .map_err(fail)?;
    let start = Instant::now();
    loop {
        if let Some(status) = child.try_wait().map_err(fail)? {
            let mut bytes = Vec::new();
            capture
                .reopen()
                .map_err(fail)?
                .take(1024 * 1024)
                .read_to_end(&mut bytes)
                .map_err(fail)?;
            let text = String::from_utf8_lossy(&bytes).trim().to_owned();
            return if status.success() {
                Ok(text)
            } else {
                Err(format!("{}：{}", status, text))
            };
        }
        if stop.load(Ordering::Relaxed) || start.elapsed() >= Duration::from_secs(timeout) {
            let _ = child.kill();
            let _ = child.wait();
            return Err(if stop.load(Ordering::Relaxed) {
                "操作已取消"
            } else {
                "工具执行超时"
            }
            .into());
        }
        thread::sleep(Duration::from_millis(80));
    }
}
pub(super) fn devices(stop: &AtomicBool) -> Result<Vec<(String, String)>, String> {
    let text = run(&adb_path()?, &["devices", "-l"], 15, stop)?;
    Ok(parse_devices(&text))
}
fn parse_devices(text: &str) -> Vec<(String, String)> {
    let mut found = vec![];
    for line in text.lines() {
        let mut words = line.split_whitespace();
        let (Some(serial), Some("device")) = (words.next(), words.next()) else {
            continue;
        };
        if serial.starts_with("emulator-") || serial.contains(':') || serial.contains("_adb-tls-") {
            continue;
        }
        let model = words
            .find_map(|s| s.strip_prefix("model:"))
            .unwrap_or("Android");
        found.push((serial.into(), format!("{model} · {serial}")));
    }
    found
}
pub(super) fn addresses(stop: &AtomicBool) -> Result<Vec<Ipv4Addr>, String> {
    let dir = std::env::var_os("SystemRoot").ok_or("无法定位 Windows")?;
    let text = run(
        &PathBuf::from(dir).join("System32/ipconfig.exe"),
        &[],
        12,
        stop,
    )?;
    let mut ips = Vec::new();
    for line in text.lines().filter(|s| s.contains("IPv4")) {
        for piece in line.split(|c: char| !c.is_ascii_digit() && c != '.') {
            if let Ok(ip) = piece.parse::<Ipv4Addr>()
                && valid_ip(ip)
                && !ips.contains(&ip)
            {
                ips.push(ip);
            }
        }
    }
    Ok(ips)
}
pub(super) fn valid_ip(ip: Ipv4Addr) -> bool {
    !ip.is_unspecified()
        && !ip.is_loopback()
        && !ip.is_multicast()
        && !ip.is_broadcast()
        && ip.octets()[0] != 0
        && ip.octets()[0] < 224
}
pub(super) fn install(serial: &str, stop: &AtomicBool) -> Result<(), String> {
    let adb = adb_path()?;
    let apk = unpack(adb.parent().unwrap(), "HybridFileXfer-phone.apk", APK)?;
    let txt = run(
        &adb,
        &[
            "-s",
            serial,
            "install",
            "-r",
            apk.to_str().ok_or("APK 路径不可用")?,
        ],
        300,
        stop,
    )?;
    if txt.lines().any(|s| s.trim() == "Success") {
        Ok(())
    } else {
        Err(format!("未收到安装成功回执：{txt}"))
    }
}
pub(super) struct Reverse {
    adb: PathBuf,
    serial: String,
}
impl Reverse {
    pub(super) fn setup(serial: &str, stop: &AtomicBool) -> Result<Self, String> {
        let adb = adb_path()?;
        let check = run(&adb, &["-s", serial, "shell", "pm", "path", PKG], 12, stop)?;
        if !check.lines().any(|s| s.starts_with("package:")) {
            return Err("手机未安装配套 HFX；请先点“安装配套手机端”".into());
        }
        let sdk = run(
            &adb,
            &["-s", serial, "shell", "getprop", "ro.build.version.sdk"],
            12,
            stop,
        )?;
        if sdk.trim().parse::<u32>().is_ok_and(|v| v >= 30) {
            let mode = run(
                &adb,
                &[
                    "-s",
                    serial,
                    "shell",
                    "cmd",
                    "appops",
                    "get",
                    PKG,
                    "MANAGE_EXTERNAL_STORAGE",
                ],
                12,
                stop,
            )?;
            if denied(&mode) {
                return Err(
                    "请在手机系统设置 → 特殊应用权限 → 所有文件访问权限，允许 HFX 后重新连接"
                        .into(),
                );
            }
        }
        // Never take over a reverse mapping owned by another tool or session.
        run(
            &adb,
            &[
                "-s",
                serial,
                "reverse",
                "--no-rebind",
                "tcp:5740",
                "tcp:5740",
            ],
            15,
            stop,
        )?;
        let guard = Self {
            adb,
            serial: serial.into(),
        };
        let started = run(
            &guard.adb,
            &[
                "-s",
                serial,
                "shell",
                "am",
                "start",
                "-S",
                "-n",
                ACTIVITY,
                "--ei",
                "io_mode",
                "0",
                "--es",
                "controller_ip",
                "127.0.0.1",
                "--es",
                "home_dir",
                "/storage/emulated/0",
            ],
            20,
            stop,
        )?;
        if started
            .lines()
            .any(|s| s.starts_with("Error") || s.contains("SecurityException"))
        {
            return Err(started);
        }
        Ok(guard)
    }
}
impl Drop for Reverse {
    fn drop(&mut self) {
        let _ = run(
            &self.adb,
            &["-s", &self.serial, "reverse", "--remove", "tcp:5740"],
            7,
            &AtomicBool::new(false),
        );
    }
}
fn denied(text: &str) -> bool {
    let mut uid = None;
    let mut pkg = None;
    for l in text.lines() {
        let l = l.trim();
        if let Some(v) = l.strip_prefix("Uid mode: MANAGE_EXTERNAL_STORAGE:") {
            uid = v.split([';', ' ']).find(|s| !s.is_empty())
        } else if let Some(v) = l.strip_prefix("MANAGE_EXTERNAL_STORAGE:") {
            pkg = v.split([';', ' ']).find(|s| !s.is_empty())
        }
    }
    let mode = uid.filter(|s| *s != "default").or(pkg).or(uid);
    matches!(mode, Some("ignore" | "deny" | "errored" | "default"))
        || text.trim() == "No operations."
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn usb_authorized_only() {
        assert_eq!(
            parse_devices(
                "List of devices attached\nfoo device model:Pixel_7 transport_id:1\nbar unauthorized\n192.168.1.2:5555 device\nemulator-5554 device"
            ),
            vec![("foo".into(), "Pixel_7 · foo".into())]
        );
    }
    #[test]
    fn access_modes() {
        assert!(!denied(
            "Uid mode: MANAGE_EXTERNAL_STORAGE: allow\nMANAGE_EXTERNAL_STORAGE: deny; rejectTime=3"
        ));
        assert!(denied("MANAGE_EXTERNAL_STORAGE: ignore; time=1"));
        assert!(denied("No operations."));
        assert!(!denied("MANAGE_EXTERNAL_STORAGE: allow"));
    }
    #[test]
    fn bundled_adb_version_without_device_access() {
        let version = run(
            &adb_path().unwrap(),
            &["version"],
            10,
            &AtomicBool::new(false),
        )
        .unwrap();
        assert!(version.contains("1.0.41"), "{version}");
        assert_eq!(
            format!("{:x}", Sha256::digest(APK)),
            "02104107e2ec157898fb34661ead279dfaa4b3cc146b40c221cb9d018b0b4f71"
        );
    }
}
