use std::{
    fs,
    io::{BufRead, BufReader, Write},
    net::{TcpListener, TcpStream},
    path::Path,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    thread,
    time::Duration,
};
use tempfile::tempdir;
use twinfile_rust::{
    archive,
    config::{Location, Settings, Store, Theme},
    download::{self, Progress},
    files, text,
};

#[test]
fn settings_restart_theme_workspace_and_lock() {
    let dir = tempdir().unwrap();
    let (store, mut s, error) = Store::open(dir.path()).unwrap();
    assert!(error.is_none());
    s.theme = Theme::Miuix;
    s.dark = true;
    s.reduce_motion = true;
    s.accent = 4;
    s.active = 1;
    s.panes[0].tabs = vec![Location::Local(dir.path().join("甲")), Location::Computer];
    s.panes[0].selected = 1;
    s.favorites.push(dir.path().join("收藏"));
    s.download_connections = 512;
    store.save(&s).unwrap();
    assert!(Store::open(dir.path()).is_err());
    drop(store);
    let (_store, loaded, error) = Store::open(dir.path()).unwrap();
    assert!(error.is_none());
    assert_eq!(loaded, s);
}

#[test]
fn legacy_local_tabs_and_virtual_computer_have_distinct_encodings() {
    let legacy = r#"{"panes":[{"tabs":["C:/Users","D:/"]},{"tabs":["C:/"]}]}"#;
    let mut s: Settings = serde_json::from_str(legacy).unwrap();
    assert_eq!(s.panes[0].tabs[1].local(), Some(Path::new("D:/")));
    s.panes[1].tabs.push(Location::Computer);
    let encoded = serde_json::to_value(&s).unwrap();
    assert_eq!(encoded["panes"][0]["tabs"][0], "C:/Users");
    assert_eq!(encoded["panes"][1]["tabs"][1], ":twinfile-computer:");
    let old: Vec<std::path::PathBuf> =
        serde_json::from_value(encoded["panes"][1]["tabs"].clone()).unwrap();
    assert!(old[0].is_absolute() && !old[1].is_absolute());
    assert!(s.panes[1].tabs[1].local().is_none());
    assert_eq!(serde_json::from_value::<Settings>(encoded).unwrap(), s);
}

#[test]
fn settings_invalid_file_never_replaced() {
    let dir = tempdir().unwrap();
    let file = dir.path().join("settings.json");
    fs::write(&file, b"{ broken").unwrap();
    let (store, mut s, error) = Store::open(dir.path()).unwrap();
    assert!(error.is_some());
    s.dark = true;
    assert!(store.save(&s).is_err());
    assert_eq!(fs::read(&file).unwrap(), b"{ broken");
}
#[test]
fn copy_move_no_clobber_and_cancel() {
    let root = tempdir().unwrap();
    let left = root.path().join("左");
    let right = root.path().join("右");
    fs::create_dir_all(left.join("folder/empty")).unwrap();
    fs::create_dir(&right).unwrap();
    let src = left.join("folder/x.txt");
    fs::write(&src, b"from-left").unwrap();
    fs::write(right.join("x.txt"), b"keep").unwrap();
    let c = AtomicBool::new(false);
    assert!(files::transfer(&src, &right, true, &c).is_err());
    assert_eq!(fs::read(&src).unwrap(), b"from-left");
    assert_eq!(fs::read(right.join("x.txt")).unwrap(), b"keep");
    let dir = left.join("folder");
    assert!(files::transfer(&dir, &dir, false, &c).is_err());
    c.store(true, Ordering::Relaxed);
    assert!(files::transfer(&dir, &right, false, &c).is_err());
    assert!(!right.join("folder").exists());
    c.store(false, Ordering::Relaxed);
    let copy = files::transfer(&dir, &right, false, &c).unwrap();
    assert!(copy.join("empty").is_dir());
    assert_eq!(fs::read(copy.join("x.txt")).unwrap(), b"from-left");
    let moved = files::transfer(&left, &right, true, &c).unwrap();
    assert!(!left.exists());
    assert!(moved.join("folder/empty").is_dir());
}
#[test]
fn reserved_and_streaming_checksum() {
    for name in [
        "",
        "..",
        "NUL.txt",
        "LPT²",
        "file.",
        "C:drive",
        "a/b",
        "a\\b",
        "a\u{0000}b",
    ] {
        assert!(files::safe_name(name).is_err(), "{name}")
    }
    let root = tempdir().unwrap();
    let p = root.path().join("字.txt");
    fs::write(&p, b"abc").unwrap();
    assert_eq!(
        files::checksum(&p, &AtomicBool::new(false)).unwrap(),
        [
            "900150983cd24fb0d6963f7d28e17f72",
            "a9993e364706816aba3e25717850c26c9cd0d89d",
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        ]
    );
}
#[test]
fn text_bom_crlf_and_external_edit() {
    let r = tempdir().unwrap();
    let p = r.path().join("编辑.txt");
    fs::write(&p, b"\xef\xbb\xbfa\r\nb\r\n").unwrap();
    let mut d = text::Document::open(&p, None).unwrap();
    assert!(d.bom);
    assert_eq!(d.body, "a\nb\n");
    d.body = "a\nz\n".into();
    d.save(&p).unwrap();
    assert_eq!(fs::read(&p).unwrap(), b"\xef\xbb\xbfa\r\nz\r\n");
    fs::write(&p, b"external").unwrap();
    d.body = "new".into();
    assert!(d.save(&p).is_err());
    assert_eq!(fs::read(&p).unwrap(), b"external");
}
#[test]
fn zip_round_trip_and_unsafe_paths() {
    let r = tempdir().unwrap();
    let base = r.path().join("原始");
    fs::create_dir_all(base.join("空目录")).unwrap();
    fs::write(base.join("测试.txt"), b"example").unwrap();
    let z = r.path().join("测试.zip");
    let c = AtomicBool::new(false);
    archive::create(&[base], &z, &c).unwrap();
    assert_eq!(archive::list(&z).unwrap().len(), 3);
    let out = r.path().join("解压");
    archive::extract(&z, &out, &c).unwrap();
    assert!(out.join("原始/空目录").is_dir());
    assert_eq!(fs::read(out.join("原始/测试.txt")).unwrap(), b"example");
    assert!(archive::extract(&z, &out, &c).is_err());
    let bad = r.path().join("bad.zip");
    let mut w = zip::ZipWriter::new(fs::File::create(&bad).unwrap());
    w.start_file("../outside.txt", zip::write::SimpleFileOptions::default())
        .unwrap();
    w.write_all(b"bad").unwrap();
    w.finish().unwrap();
    let dst = r.path().join("no");
    assert!(archive::extract(&bad, &dst, &c).is_err());
    assert!(!dst.exists());
    assert!(!r.path().join("outside.txt").exists());
}

#[test]
fn directory_properties_include_hidden_and_dedup_descendants() {
    let r = tempdir().unwrap();
    let root = r.path().join("selected");
    let sub = root.join("sub");
    fs::create_dir_all(&sub).unwrap();
    let one = sub.join(".hidden");
    let two = root.join("second.txt");
    fs::write(&one, b"123").unwrap();
    fs::write(&two, b"45678").unwrap();
    let stop = AtomicBool::new(false);
    let totals = files::aggregate(&[one, root.clone(), two, root], &stop).unwrap();
    assert_eq!(totals.files, 2);
    assert_eq!(totals.directories, 1);
    assert_eq!(totals.bytes, 8);
    assert_eq!(totals.errors, 0);
    stop.store(true, Ordering::Relaxed);
    assert!(files::aggregate(&[sub], &stop).is_err());
}

const DATA: &[u8] = b"0123456789-abcdefghijklmnopqrstuvwxyz";
#[derive(Clone, Copy, Debug)]
enum Mode {
    Segments,
    Fallback,
    Change,
    Stall,
    Retry,
}
struct Server {
    url: String,
    stop: Arc<AtomicBool>,
    calls: Arc<Mutex<Vec<String>>>,
    task: Option<thread::JoinHandle<()>>,
}
impl Server {
    fn new(mode: Mode) -> Self {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let url = format!("http://{}/test", listener.local_addr().unwrap());
        listener.set_nonblocking(true).unwrap();
        let stop = Arc::new(AtomicBool::new(false));
        let signal = stop.clone();
        let calls = Arc::new(Mutex::new(Vec::new()));
        let observed = calls.clone();
        let retry = Arc::new(AtomicBool::new(false));
        let task = thread::spawn(move || {
            while !signal.load(Ordering::Relaxed) {
                match listener.accept() {
                    Ok((socket, _)) => {
                        let seen = observed.clone();
                        let retry = retry.clone();
                        thread::spawn(move || serve(socket, mode, seen, retry));
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                        thread::sleep(Duration::from_millis(4))
                    }
                    Err(e) => panic!("{e}"),
                }
            }
        });
        Self {
            url,
            stop,
            calls,
            task: Some(task),
        }
    }
}
impl Drop for Server {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        self.task.take().unwrap().join().unwrap();
    }
}
fn serve(mut s: TcpStream, mode: Mode, seen: Arc<Mutex<Vec<String>>>, retry: Arc<AtomicBool>) {
    s.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
    s.set_nodelay(true).unwrap();
    let mut reader = BufReader::new(&mut s);
    let mut req = String::new();
    loop {
        let mut line = String::new();
        if reader.read_line(&mut line).unwrap_or(0) == 0 {
            return;
        }
        if line == "\r\n" {
            break;
        }
        req.push_str(&line);
    }
    seen.lock().unwrap().push(req.clone());
    if matches!(mode, Mode::Stall) {
        let _ = write!(
            s,
            "HTTP/1.1 200 OK\r\nContent-Length: 200\r\nConnection: close\r\n\r\nabc"
        );
        thread::sleep(Duration::from_millis(600));
        return;
    }
    if !matches!(mode, Mode::Fallback)
        && let Some(range) = req
            .lines()
            .filter_map(|x| x.split_once(':'))
            .find_map(|(k, v)| k.eq_ignore_ascii_case("range").then(|| v.trim()))
            .and_then(|v| v.strip_prefix("bytes="))
    {
        let (a, b) = range.split_once('-').unwrap();
        let (a, b) = (a.parse::<usize>().unwrap(), b.parse::<usize>().unwrap());
        let etag = if matches!(mode, Mode::Change) && (a, b) != (0, 0) {
            "v2"
        } else {
            "v1"
        };
        let _ = write!(
            s,
            "HTTP/1.1 206 Partial Content\r\nContent-Length: {}\r\nContent-Range: bytes {a}-{b}/{}\r\nETag: \"{etag}\"\r\nConnection: close\r\n\r\n",
            b - a + 1,
            DATA.len()
        );
        let lose =
            matches!(mode, Mode::Retry) && (a, b) != (0, 0) && !retry.swap(true, Ordering::Relaxed);
        let _ = s.write_all(if lose { &DATA[a..a + 1] } else { &DATA[a..=b] });
        close_response(&mut s);
        return;
    }
    let _ = write!(
        s,
        "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        DATA.len()
    );
    let _ = s.write_all(DATA);
    close_response(&mut s);
}
fn close_response(s: &mut TcpStream) {
    use std::io::Read;
    let _ = s.shutdown(std::net::Shutdown::Write);
    let _ = s.read(&mut [0; 1]);
}
fn assert_clear(p: &Path) {
    assert!(!fs::read_dir(p).unwrap().any(|e| {
        e.unwrap()
            .file_name()
            .to_string_lossy()
            .starts_with(".twinfile-download-")
    }));
}
#[test]
fn downloads_segmented_fallback_retry_and_conflict() {
    for mode in [Mode::Segments, Mode::Fallback, Mode::Retry] {
        let r = tempdir().unwrap();
        let dst = r.path().join("测试.bin");
        let s = Server::new(mode);
        let p = Arc::new(Progress::default());
        download::run(&s.url, &dst, 4, p.clone())
            .unwrap_or_else(|e| panic!("{mode:?}: {e:?}; calls: {:?}", s.calls.lock().unwrap()));
        assert_eq!(fs::read(&dst).unwrap(), DATA);
        let calls = s.calls.lock().unwrap();
        assert_eq!(
            calls.len(),
            match mode {
                Mode::Segments => 5,
                Mode::Fallback => 2,
                Mode::Retry => 6,
                _ => unreachable!(),
            }
        );
        if !matches!(mode, Mode::Fallback) {
            assert!(
                calls
                    .iter()
                    .any(|r| r.to_ascii_lowercase().contains("if-range: \"v1\""))
            );
        }
        if matches!(mode, Mode::Retry) {
            // The listener proves one request was replayed after the intentional
            // truncation. The application also counts connection attempts that
            // failed before reaching the listener, which can vary on Windows.
            let ranges: Vec<_> = calls
                .iter()
                .skip(1)
                .filter_map(|r| {
                    let value = r
                        .lines()
                        .filter_map(|l| l.split_once(':'))
                        .find_map(|(k, v)| k.eq_ignore_ascii_case("range").then_some(v.trim()))?;
                    let (start, end) = value.strip_prefix("bytes=")?.split_once('-')?;
                    Some((start.parse::<u64>().ok()?, end.parse::<u64>().ok()?))
                })
                .collect();
            assert_eq!(ranges.len(), 5);
            assert_eq!(
                ranges
                    .iter()
                    .map(|&(_, end)| end)
                    .collect::<std::collections::BTreeSet<_>>()
                    .len(),
                4
            );
            assert!(p.retries.load(Ordering::Relaxed) >= 1);
        }
        assert!(download::run(&s.url, &dst, 4, Arc::new(Progress::default())).is_err());
        assert_eq!(fs::read(&dst).unwrap(), DATA);
        assert_clear(r.path());
    }
}
#[test]
fn download_rejects_entity_change_and_cancel_cleans_up() {
    let r = tempdir().unwrap();
    let dst = r.path().join("reject.bin");
    let s = Server::new(Mode::Change);
    let e = download::run(&s.url, &dst, 4, Arc::new(Progress::default())).unwrap_err();
    assert!(
        e.to_string().contains("不一致"),
        "{e:?}; calls {:?}",
        s.calls.lock().unwrap()
    );
    assert!(!dst.exists());
    assert_clear(r.path());
    let s = Server::new(Mode::Stall);
    let p = Arc::new(Progress::default());
    let signal = p.clone();
    let t = thread::spawn(move || {
        for _ in 0..200 {
            if signal.received.load(Ordering::Relaxed) > 0 {
                break;
            }
            thread::sleep(Duration::from_millis(10));
        }
        signal.cancel.store(true, Ordering::Relaxed);
    });
    let e = download::run(&s.url, &dst, 1, p).unwrap_err();
    assert!(e.to_string().contains("取消"), "{e:?}");
    t.join().unwrap();
    assert!(!dst.exists());
    assert_clear(r.path());
}
#[test]
fn download_permission_error_before_network() {
    let r = tempdir().unwrap();
    let dir = r.path().join("denied");
    fs::create_dir(&dir).unwrap();
    let who = std::process::Command::new("whoami.exe")
        .args(["/user", "/fo", "csv", "/nh"])
        .output()
        .unwrap();
    let sid = String::from_utf8_lossy(&who.stdout)
        .trim()
        .split(',')
        .next_back()
        .unwrap()
        .trim_matches('"')
        .to_owned();
    assert!(sid.starts_with("S-1-"));
    let grant = format!("*{sid}:(WD)");
    let trustee = format!("*{sid}");
    let run = |op: &str, v: &str| {
        std::process::Command::new("icacls.exe")
            .arg(&dir)
            .args([op, v])
            .output()
            .unwrap()
    };
    assert!(run("/deny", &grant).status.success());
    let dst = dir.join("test.bin");
    let s = Server::new(Mode::Segments);
    let result = download::run(&s.url, &dst, 4, Arc::new(Progress::default()));
    let restore = run("/remove:d", &trustee);
    assert!(restore.status.success());
    let e = result.unwrap_err();
    let io = e.downcast_ref::<std::io::Error>().unwrap();
    assert_eq!(io.kind(), std::io::ErrorKind::PermissionDenied);
    assert!(io.to_string().contains("创建下载临时文件"));
    assert!(s.calls.lock().unwrap().is_empty());
    assert_clear(&dir);
}

#[test]
fn retired_themes_migrate_without_losing_workspace() {
    assert_eq!(
        Theme::ALL,
        [Theme::TwinFile, Theme::Miuix, Theme::Soft, Theme::Keynote]
    );
    for old in ["Mt", "Cream", "Bata", "MacOs", "Fluent"] {
        let mut cfg = serde_json::to_value(Settings::default()).unwrap();
        cfg["theme"] = old.into();
        cfg["dark"] = true.into();
        cfg["payload_workers"] = 5.into();
        let loaded: Settings = serde_json::from_value(cfg.clone()).unwrap();
        let expected = if old == "Mt" {
            Theme::TwinFile
        } else {
            Theme::Miuix
        };
        assert_eq!(loaded.theme, expected);
        cfg["theme"] = serde_json::to_value(expected).unwrap();
        assert_eq!(serde_json::to_value(loaded).unwrap(), cfg);
    }
}

#[test]
fn soft_theme_survives_restart() {
    let d = tempdir().unwrap();
    let (st, mut s, e) = Store::open(d.path()).unwrap();
    assert!(e.is_none());
    s.theme = Theme::Soft;
    s.reduce_motion = true;
    st.save(&s).unwrap();
    drop(st);
    let (_, s, e) = Store::open(d.path()).unwrap();
    assert!(e.is_none());
    assert!(s.theme == Theme::Soft && s.reduce_motion);
}

#[test]
fn keynote_survives_restart_without_changing_other_appearance_preferences() {
    let tmp = tempdir().unwrap();
    let (store, mut s, _) = Store::open(tmp.path()).unwrap();
    s.theme = Theme::Keynote;
    s.dark = false;
    s.accent = 4;
    store.save(&s).unwrap();
    drop(store);
    let (_, loaded, error) = Store::open(tmp.path()).unwrap();
    assert!(error.is_none());
    assert_eq!(s, loaded);
}

// A real loopback response, not a guessed name or a separate HEAD request.
fn named_responses(responses: Vec<String>) -> (String, thread::JoinHandle<()>) {
    let socket = TcpListener::bind("127.0.0.1:0").unwrap();
    let base = format!("http://{}", socket.local_addr().unwrap());
    (
        base,
        thread::spawn(move || {
            for response in responses {
                let (mut stream, _) = socket.accept().unwrap();
                let mut reader = BufReader::new(&stream);
                let mut request = String::new();
                reader.read_line(&mut request).unwrap();
                assert!(request.starts_with("GET "));
                loop {
                    let mut line = String::new();
                    reader.read_line(&mut line).unwrap();
                    if line.trim().is_empty() {
                        break;
                    }
                }
                stream.write_all(response.as_bytes()).unwrap();
                stream.flush().unwrap();
            }
        }),
    )
}
#[test]
fn direct_names_follow_headers_redirects_and_user_choice() {
    let dir = tempdir().unwrap();
    let reply = |extra: &str| {
        format!("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 3\r\n{extra}\r\nabc")
    };
    let redirect = "HTTP/1.1 302 Found\r\nLocation: /%E7%A4%BA%E4%BE%8B%20v2.zip\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    let named = reply(
        "Content-Disposition: attachment; filename=ascii.zip; filename*=UTF-8'zh'%E8%B5%84%E6%96%99.zip\r\n",
    );
    for (responses, name, manual) in [
        (vec![named.clone()], "资料.zip", false),
        (vec![redirect.into(), reply("")], "示例 v2.zip", false),
        (
            vec![reply(
                "Content-Disposition: attachment; filename=\"one; two.txt\"\r\n",
            )],
            "one; two.txt",
            false,
        ),
        (vec![named.clone()], "自己命名.txt", true),
        (
            vec![reply(
                "Content-Disposition: attachment; filename*=UTF-8''..%2Fescape.txt\r\n",
            )],
            "id",
            false,
        ),
    ] {
        let (base, task) = named_responses(responses);
        let p = Arc::new(Progress::default());
        let path = dir.path().join(name);
        let url = format!("{base}/id");
        let actual = if manual {
            download::run(&url, &path, 1, p.clone())
        } else {
            download::run_auto(&url, dir.path(), None, 1, p.clone())
        }
        .unwrap();
        task.join().unwrap();
        assert_eq!(actual, path);
        assert_eq!(fs::read(&actual).unwrap(), b"abc");
        assert_eq!(p.resolved_path.lock().unwrap().as_ref(), Some(&path));
    }
    let (base, task) = named_responses(vec![named]);
    let error = download::run_auto(&base, dir.path(), None, 1, Arc::default())
        .unwrap_err()
        .to_string();
    assert!(error.contains("未覆盖"));
    task.join().unwrap();
    assert_clear(dir.path());
    let (base, task) = named_responses(vec![reply("")]);
    assert!(
        download::run_auto(&base, dir.path(), None, 1, Arc::default())
            .unwrap_err()
            .to_string()
            .contains("另存为")
    );
    task.join().unwrap();
    assert!(!dir.path().join("download.bin").exists());
    assert_clear(dir.path());
}
#[test]
fn direct_name_from_accepted_range_probe_applies_to_all_chunks() {
    let tmp = tempdir().unwrap();
    let range = |start, end, text: &str, header: &str| {
        format!(
            "HTTP/1.1 206 Partial Content\r\nETag: \"v1\"\r\nContent-Range: bytes {start}-{end}/2\r\nContent-Length: 1\r\nConnection: close\r\n{header}\r\n{text}"
        )
    };
    // One chunk per worker, ordered dynamically by its Range header.
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("http://{}/id", listener.local_addr().unwrap());
    let worker = thread::spawn(move || {
        for i in 0..3 {
            let (mut s, _) = listener.accept().unwrap();
            let mut req = String::new();
            let mut reader = BufReader::new(&s);
            loop {
                let mut line = String::new();
                reader.read_line(&mut line).unwrap();
                if line.trim().is_empty() {
                    break;
                }
                req.push_str(&line.to_ascii_lowercase());
            }
            let idx = usize::from(req.contains("range: bytes=1-1"));
            let text = if idx == 0 { "a" } else { "b" };
            s.write_all(
                range(
                    idx,
                    idx,
                    text,
                    if i == 0 {
                        "Content-Disposition: attachment; filename=segmented.zip\r\n"
                    } else {
                        ""
                    },
                )
                .as_bytes(),
            )
            .unwrap();
        }
    });
    let p = download::run_auto(&url, tmp.path(), None, 2, Arc::default()).unwrap();
    worker.join().unwrap();
    assert_eq!(p, tmp.path().join("segmented.zip"));
    assert_eq!(fs::read(p).unwrap(), b"ab");
}
