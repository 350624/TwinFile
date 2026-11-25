//! HybridFileXfer Android v300: one control socket, 1–8 independently scheduled
//! USB-reverse / IPv4 data sockets. No listener is created until the caller asks.
use crate::files;
use std::{
    collections::{HashMap, HashSet},
    fs::{self, File, OpenOptions},
    io::{self, Read, Seek, SeekFrom, Write},
    net::{IpAddr, Ipv4Addr, Shutdown, TcpListener, TcpStream},
    path::{Path, PathBuf},
    sync::{
        Mutex,
        atomic::{AtomicBool, AtomicU64, Ordering::Relaxed},
    },
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
const BLOCK: u64 = 1 << 20;
const MAX_FILES: usize = 200_000;
const MAX_FILE: u64 = 256 << 30;
const MAX_META: usize = 48 << 20;
fn bad(s: impl Into<String>) -> io::Error {
    io::Error::other(s.into())
}
fn require(b: bool, s: &str) -> io::Result<()> {
    if b { Ok(()) } else { Err(bad(s)) }
}
fn remote_name(s: &str) -> bool {
    !s.is_empty() && s != "." && s != ".." && !s.contains(['\\', '/', '\0'])
}
fn directory(p: &str) -> io::Result<()> {
    require(
        p == "/"
            || p.strip_prefix('/')
                .is_some_and(|rest| rest.split('/').all(remote_name)),
        "无效的手机目录",
    )
}
fn join(d: &str, n: &str) -> String {
    format!("{}/{n}", d.trim_end_matches('/'))
}
fn txt(v: &mut Vec<u8>, s: &str) -> io::Result<()> {
    require(
        s.len() <= 65535 && !s.contains('\0'),
        "HFX 字符串超长或包含 NUL",
    )?;
    v.extend_from_slice(&(s.len() as u16).to_be_bytes());
    v.extend_from_slice(s.as_bytes());
    Ok(())
}
type R<T> = io::Result<T>;
struct Signals<'a> {
    stop: &'a AtomicBool,
    abort: AtomicBool,
    tick: Mutex<Instant>,
    initial: Instant,
}
impl<'a> Signals<'a> {
    fn new(stop: &'a AtomicBool) -> Self {
        Self {
            stop,
            abort: AtomicBool::new(false),
            tick: Mutex::new(Instant::now()),
            initial: Instant::now(),
        }
    }
    fn check(&self) -> R<()> {
        files::check_cancel(self.stop)?;
        require(!self.abort.load(Relaxed), "其它 HFX 通道已中止")
    }
    fn deadline(&self, long: bool) -> R<()> {
        self.check()?;
        if long && self.initial.elapsed() < Duration::from_secs(180) {
            return Ok(());
        }
        require(
            self.tick.lock().unwrap().elapsed() < Duration::from_secs(60),
            "HFX 手机响应超时",
        )
    }
    fn activity(&self) {
        *self.tick.lock().unwrap() = Instant::now();
    }
}
struct Wire<'a, 'b> {
    s: &'a TcpStream,
    sig: &'a Signals<'b>,
    first: bool,
}
impl<'a, 'b> Wire<'a, 'b> {
    fn new(s: &'a TcpStream, sig: &'a Signals<'b>) -> Self {
        Self {
            s,
            sig,
            first: false,
        }
    }
    fn get(&mut self, mut dst: &mut [u8]) -> R<()> {
        while !dst.is_empty() {
            self.sig.deadline(self.first)?;
            match self.s.read(dst) {
                Ok(0) => return Err(bad("手机已断开连接")),
                Ok(n) => {
                    self.first = false;
                    self.sig.activity();
                    dst = &mut dst[n..];
                }
                Err(e)
                    if matches!(
                        e.kind(),
                        io::ErrorKind::Interrupted
                            | io::ErrorKind::TimedOut
                            | io::ErrorKind::WouldBlock
                    ) => {}
                Err(e) => return Err(e),
            }
        }
        Ok(())
    }
    fn send(&mut self, mut src: &[u8]) -> R<()> {
        while !src.is_empty() {
            self.sig.deadline(false)?;
            match self.s.write(src) {
                Ok(0) => return Err(bad("手机已断开连接")),
                Ok(n) => {
                    self.sig.activity();
                    src = &src[n..];
                }
                Err(e)
                    if matches!(
                        e.kind(),
                        io::ErrorKind::Interrupted
                            | io::ErrorKind::TimedOut
                            | io::ErrorKind::WouldBlock
                    ) => {}
                Err(e) => return Err(e),
            }
        }
        Ok(())
    }
    fn read<const N: usize>(&mut self) -> R<[u8; N]> {
        let mut b = [0; N];
        self.get(&mut b)?;
        Ok(b)
    }
    fn flag(&mut self) -> R<bool> {
        match self.read::<1>()?[0] {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err(bad("HFX 布尔值无效")),
        }
    }
    fn string(&mut self) -> R<String> {
        let n = u16::from_be_bytes(self.read()?) as usize;
        let mut b = vec![0; n];
        self.get(&mut b)?;
        let s = String::from_utf8(b).map_err(|_| bad("HFX UTF-8 无效"))?;
        require(!s.contains('\0'), "HFX 字符串包含 NUL")?;
        Ok(s)
    }
    fn i32(&mut self) -> R<i32> {
        Ok(i32::from_be_bytes(self.read()?))
    }
    fn i64(&mut self) -> R<i64> {
        Ok(i64::from_be_bytes(self.read()?))
    }
}
#[derive(Debug, Clone)]
pub struct Entry {
    pub name: String,
    pub path: String,
    pub dir: bool,
    pub size: u64,
    pub modified_ms: i64,
}
pub struct Session {
    pub home: String,
    pub lane_names: Vec<String>,
    control: TcpStream,
    lanes: Vec<TcpStream>,
}
impl Drop for Session {
    fn drop(&mut self) {
        self.close();
    }
}
impl Session {
    fn close(&self) {
        let _ = self.control.shutdown(Shutdown::Both);
        for s in &self.lanes {
            let _ = s.shutdown(Shutdown::Both);
        }
    }
    pub fn accept(
        listener: TcpListener,
        lanes: &[(String, Ipv4Addr)],
        stop: &AtomicBool,
    ) -> Result<Self, String> {
        Self::attach(listener, lanes, stop).map_err(|e| e.to_string())
    }
    fn attach(listener: TcpListener, lanes: &[(String, Ipv4Addr)], stop: &AtomicBool) -> R<Self> {
        require((1..=8).contains(&lanes.len()), "需要 1–8 条传输通道")?;
        listener.set_nonblocking(true)?;
        let accept = |seconds: u64, allowed: &[Ipv4Addr]| -> R<TcpStream> {
            let deadline = Instant::now() + Duration::from_secs(seconds);
            loop {
                files::check_cancel(stop)?;
                require(Instant::now() < deadline, "等待手机连接超时")?;
                match listener.accept() {
                    Ok((s, peer)) => {
                        let ip = s.local_addr()?.ip();
                        if !allowed.iter().any(|&a| ip == IpAddr::V4(a))
                            || (ip.is_loopback() && !peer.ip().is_loopback())
                        {
                            continue;
                        }
                        s.set_nonblocking(false)?;
                        s.set_read_timeout(Some(Duration::from_millis(250)))?;
                        s.set_write_timeout(Some(Duration::from_millis(250)))?;
                        s.set_nodelay(true)?;
                        return Ok(s);
                    }
                    Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                        std::thread::sleep(Duration::from_millis(25))
                    }
                    Err(e) => return Err(e),
                }
            }
        };
        let addrs: Vec<_> = lanes.iter().map(|x| x.1).collect();
        let usb: Vec<_> = addrs.iter().filter(|a| a.is_loopback()).copied().collect();
        let control = accept(90, if usb.is_empty() { &addrs } else { &usb })?;
        let sig = Signals::new(stop);
        let mut w = Wire::new(&control, &sig);
        require(w.read::<4>()? == *b"HFXC", "不是 HFX 控制连接")?;
        if w.i32()? != 300 {
            w.send(&[0])?;
            w.send(&300i32.to_be_bytes())?;
            return Err(bad("HFX 版本不匹配，需要 v300"));
        }
        let mut h = vec![1];
        h.extend_from_slice(&(lanes.len() as i32).to_be_bytes());
        for (name, addr) in lanes {
            txt(&mut h, name)?;
            h.push(4);
            h.extend_from_slice(&addr.octets());
            h.push(0)
        }
        w.send(&h)?;
        let mut sockets = vec![];
        let mut names = vec![];
        for (n, ip) in lanes {
            let ok = w.flag()?;
            let name = w.string()?;
            if !ok {
                return Err(bad(format!("手机不能连接通道 {n}: {name}")));
            }
            sockets.push(accept(15, &[*ip])?);
            names.push(name);
            w.send(&[1])?;
        }
        w.send(&64i32.to_be_bytes())?;
        require(w.flag()?, "手机缓冲区分配失败")?;
        w.send(&[1])?;
        require(w.i32()? == 0, "需要 Android/UNIX 文件系统")?;
        let home = w.string()?;
        directory(&home)?;
        Ok(Self {
            home,
            lane_names: names,
            control,
            lanes: sockets,
        })
    }
    fn read_dir(&self, remote: &str, stop: &AtomicBool) -> R<Vec<Entry>> {
        directory(remote)?;
        let sig = Signals::new(stop);
        let mut w = Wire::new(&self.control, &sig);
        let mut request = 1i16.to_be_bytes().to_vec();
        txt(&mut request, remote)?;
        w.send(&request)?;
        let count = w.i32()?;
        require(
            (0..=MAX_FILES as i32).contains(&count),
            "手机未返回有效的目录列表",
        )?;
        let mut entries = Vec::new();
        let mut names = HashSet::new();
        let mut bytes = 0;
        for _ in 0..count {
            let name = w.string()?;
            let path = w.string()?;
            let modified_ms = w.i64()?;
            let size = w.i64()?;
            let dir = w.flag()?;
            bytes += name.len() + path.len();
            require(
                bytes <= MAX_META
                    && size >= 0
                    && remote_name(&name)
                    && path == join(remote, &name)
                    && names.insert(name.clone()),
                "手机目录返回重复、超限或越界条目",
            )?;
            entries.push(Entry {
                name,
                path,
                dir,
                size: size as u64,
                modified_ms,
            })
        }
        Ok(entries)
    }
    pub fn list(&mut self, remote: &str, stop: &AtomicBool) -> Result<Vec<Entry>, String> {
        self.read_dir(remote, stop).map_err(|e| {
            self.close();
            e.to_string()
        })
    }
    pub fn push(
        &mut self,
        local: &[PathBuf],
        remote: &str,
        stop: &AtomicBool,
        progress: &(dyn Fn(u64, u64) + Sync),
    ) -> Result<(), String> {
        self.send_files(local, remote, stop, progress).map_err(|e| {
            self.close();
            e.to_string()
        })
    }
    pub fn pull(
        &mut self,
        remote: &[String],
        source: &str,
        local: &Path,
        stop: &AtomicBool,
        progress: &(dyn Fn(u64, u64) + Sync),
    ) -> Result<(), String> {
        self.receive_files(remote, source, local, stop, progress)
            .map_err(|e| {
                self.close();
                e.to_string()
            })
    }
}
#[cfg(test)]
mod tests;
mod transfer;
