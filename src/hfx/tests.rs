use super::*;
use std::net::SocketAddr;
use std::thread;
const HOME: &str = "/storage/emulated/0";
fn n<const N: usize>(s: &mut TcpStream) -> [u8; N] {
    let mut b = [0; N];
    s.read_exact(&mut b).unwrap();
    b
}
fn text(s: &mut TcpStream) -> String {
    let l = u16::from_be_bytes(n(s));
    let mut b = vec![0; l as usize];
    s.read_exact(&mut b).unwrap();
    String::from_utf8(b).unwrap()
}
fn string(s: &mut TcpStream, t: &str) {
    let mut b = vec![];
    txt(&mut b, t).unwrap();
    s.write_all(&b).unwrap()
}
fn socket(addr: SocketAddr) -> TcpStream {
    let s = TcpStream::connect_timeout(&addr, Duration::from_secs(3)).unwrap();
    s.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    s.set_write_timeout(Some(Duration::from_secs(5))).unwrap();
    s
}
fn peer(addr: SocketAddr) -> (TcpStream, Vec<TcpStream>) {
    let mut c = socket(addr);
    c.write_all(b"HFXC\0\0\x01\x2c").unwrap();
    assert_eq!(n::<1>(&mut c), [1]);
    assert_eq!(i32::from_be_bytes(n(&mut c)), 2);
    for name in ["USB", "WLAN"] {
        assert_eq!(text(&mut c), name);
        assert_eq!(n::<6>(&mut c), [4, 127, 0, 0, 1, 0]);
    }
    let mut data = vec![];
    for name in ["USB", "WLAN"] {
        data.push(socket(addr));
        c.write_all(&[1]).unwrap();
        string(&mut c, name);
        assert_eq!(n::<1>(&mut c), [1]);
    }
    assert_eq!(i32::from_be_bytes(n(&mut c)), 64);
    c.write_all(&[1]).unwrap();
    assert_eq!(n::<1>(&mut c), [1]);
    c.write_all(&0i32.to_be_bytes()).unwrap();
    string(&mut c, HOME);
    (c, data)
}
fn setup() -> (TcpListener, SocketAddr, Vec<(String, Ipv4Addr)>) {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let a = l.local_addr().unwrap();
    (
        l,
        a,
        vec![
            ("USB".into(), Ipv4Addr::LOCALHOST),
            ("WLAN".into(), Ipv4Addr::LOCALHOST),
        ],
    )
}
fn list(s: &mut TcpStream) {
    assert_eq!(i16::from_be_bytes(n(s)), 1);
    assert_eq!(text(s), HOME);
    s.write_all(&0i32.to_be_bytes()).unwrap();
}
fn pull_req(c: &mut TcpStream, name: &str) -> String {
    assert_eq!(i16::from_be_bytes(n(c)), 11);
    assert_eq!(i32::from_be_bytes(n(c)), 1);
    assert_eq!(text(c), join(HOME, name));
    let path = text(c);
    assert_eq!(i32::from_be_bytes(n(c)), 1);
    assert_eq!(text(c), format!("{HOME}/"));
    path
}
fn block(id: i32, path: &str, dir: bool, size: i64, k: i32, bytes: &[u8]) -> Vec<u8> {
    let mut f = (if dir { 1i16 } else { 0i16 }).to_be_bytes().to_vec();
    f.extend_from_slice(&id.to_be_bytes());
    txt(&mut f, path).unwrap();
    f.extend_from_slice(&123456789i64.to_be_bytes());
    if !dir {
        f.extend_from_slice(&size.to_be_bytes());
        f.extend_from_slice(&k.to_be_bytes());
        f.extend_from_slice(&(bytes.len() as i32).to_be_bytes());
        f.extend_from_slice(bytes);
    }
    f
}
#[test]
fn v300_multilane_round_trip() {
    let (l, addr, lanes) = setup();
    let stop = AtomicBool::new(false);
    let temp = tempfile::tempdir().unwrap();
    let folder = temp.path().join("相册");
    fs::create_dir_all(folder.join("empty")).unwrap();
    let data: Vec<u8> = (0..(3 * BLOCK as usize + 23))
        .map(|i| (i % 251) as u8)
        .collect();
    fs::write(folder.join("sample.bin"), &data).unwrap();
    fs::write(folder.join("zero"), []).unwrap();
    let restore = temp.path().join("out");
    fs::create_dir(&restore).unwrap();
    thread::scope(|scope| {
        let mock = scope.spawn(|| {
            let (mut c, mut channels) = peer(addr);
            list(&mut c);
            list(&mut c);
            assert_eq!(i16::from_be_bytes(n(&mut c)), 10);
            let parts = thread::scope(|s| {
                let mut ts = vec![];
                for channel in &mut channels {
                    ts.push(s.spawn(move || {
                        let mut parts = vec![];
                        loop {
                            let typ = i16::from_be_bytes(n(channel));
                            if typ == 3 {
                                break;
                            }
                            let _ = i32::from_be_bytes(n(channel));
                            let name = text(channel);
                            let _ = i64::from_be_bytes(n(channel));
                            if typ == 1 {
                                parts.push((name, -1, vec![]));
                                continue;
                            }
                            let _size = i64::from_be_bytes(n(channel));
                            let k = i32::from_be_bytes(n(channel));
                            let size = i32::from_be_bytes(n(channel));
                            assert!((0..=BLOCK as i32).contains(&size));
                            let mut b = vec![0; size as usize];
                            channel.read_exact(&mut b).unwrap();
                            parts.push((name, k, b));
                        }
                        parts
                    }))
                }
                ts.into_iter()
                    .flat_map(|t| t.join().unwrap())
                    .collect::<Vec<_>>()
            });
            let mut payloads = parts
                .iter()
                .filter(|(n, _, _)| n.ends_with("/sample.bin"))
                .collect::<Vec<_>>();
            payloads.sort_by_key(|(_, k, _)| k);
            let restored = payloads
                .into_iter()
                .flat_map(|(_, _, b)| b.iter().copied())
                .collect::<Vec<_>>();
            assert_eq!(restored, data);
            assert!(
                parts
                    .iter()
                    .any(|(n, k, _)| n.ends_with("/empty") && *k == -1)
            );
            c.write_all(&[1]).unwrap();
            assert_eq!(n::<1>(&mut c), [1]);
            let stage = pull_req(&mut c, "相册");
            let root = format!("{stage}相册");
            let last = 3 * BLOCK as usize;
            channels[1]
                .write_all(&block(
                    2,
                    &format!("{root}\\sample.bin"),
                    false,
                    data.len() as i64,
                    3,
                    &data[last..],
                ))
                .unwrap();
            channels[0]
                .write_all(&block(1, &format!("{root}\\empty"), true, 0, -1, &[]))
                .unwrap();
            channels[1]
                .write_all(&block(0, &root, true, 0, -1, &[]))
                .unwrap();
            for (i, b) in data[..last].chunks(BLOCK as usize).enumerate() {
                channels[i % 2]
                    .write_all(&block(
                        2,
                        &format!("{root}\\sample.bin"),
                        false,
                        data.len() as i64,
                        i as i32,
                        b,
                    ))
                    .unwrap();
            }
            channels[0]
                .write_all(&block(3, &format!("{root}\\zero"), false, 0, 0, &[]))
                .unwrap();
            for ch in channels.iter_mut() {
                ch.write_all(&3i16.to_be_bytes()).unwrap();
            }
            assert_eq!(n::<1>(&mut c), [1]);
            c.write_all(&[1]).unwrap();
        });
        let mut session = Session::accept(l, &lanes, &stop).unwrap();
        assert_eq!(session.lane_names, ["USB", "WLAN"]);
        assert_eq!(session.home, HOME);
        assert!(session.list(HOME, &stop).unwrap().is_empty());
        session.push(&[folder], HOME, &stop, &|_, _| {}).unwrap();
        session
            .pull(&[join(HOME, "相册")], HOME, &restore, &stop, &|_, _| {})
            .unwrap();
        assert_eq!(fs::read(restore.join("相册/sample.bin")).unwrap(), data);
        assert!(restore.join("相册/empty").is_dir());
        assert_eq!(fs::metadata(restore.join("相册/zero")).unwrap().len(), 0);
        assert!(
            session
                .pull(&[join(HOME, "相册")], HOME, &restore, &stop, &|_, _| {})
                .is_err()
        );
        mock.join().unwrap();
    });
}
#[test]
fn invalid_pull_does_not_publish_partial_data() {
    for mal in ["traversal", "missing", "duplicate", "reserved"] {
        let (l, addr, lanes) = setup();
        let tmp = tempfile::tempdir().unwrap();
        let stop = AtomicBool::new(false);
        thread::scope(|scope| {
            let mock = scope.spawn(|| {
                let (mut c, mut ch) = peer(addr);
                let base = pull_req(&mut c, "folder");
                let p = match mal {
                    "traversal" => format!("{base}folder/../bad"),
                    "reserved" => format!("{base}folder/CON"),
                    _ => format!("{base}folder/ok"),
                };
                let frame = block(1, &p, false, BLOCK as i64 + 1, 1, b"x");
                ch[1].write_all(&frame).unwrap();
                if mal == "duplicate" {
                    let _ = ch[1].write_all(&frame);
                }
                for s in &mut ch {
                    let _ = s.write_all(&3i16.to_be_bytes());
                }
            });
            let mut s = Session::accept(l, &lanes, &stop).unwrap();
            let err = s
                .pull(&[join(HOME, "folder")], HOME, tmp.path(), &stop, &|_, _| {})
                .unwrap_err();
            println!("{mal}: {err}");
            assert_eq!(fs::read_dir(tmp.path()).unwrap().count(), 0);
            mock.join().unwrap();
        });
    }
}
#[test]
fn accept_cancels_without_opening_any_connection() {
    let (l, _, lanes) = setup();
    assert!(Session::accept(l, &lanes, &AtomicBool::new(true)).is_err());
}

#[test]
fn listing_and_cancel_while_waiting_for_remote() {
    let (l, addr, lanes) = setup();
    let stop = AtomicBool::new(false);
    thread::scope(|scope| {
        let (started, waiting) = std::sync::mpsc::sync_channel(0);
        let mock = scope.spawn(move || {
            let (mut c, _data) = peer(addr);
            assert_eq!(i16::from_be_bytes(n(&mut c)), 1);
            assert_eq!(text(&mut c), HOME);
            c.write_all(&1i32.to_be_bytes()).unwrap();
            string(&mut c, "中文");
            string(&mut c, &join(HOME, "中文"));
            c.write_all(&42i64.to_be_bytes()).unwrap();
            c.write_all(&999i64.to_be_bytes()).unwrap();
            c.write_all(&[0]).unwrap();
            assert_eq!(i16::from_be_bytes(n(&mut c)), 1);
            assert_eq!(text(&mut c), HOME);
            started.send(()).unwrap();
            let mut one = [0];
            assert_eq!(c.read(&mut one).unwrap_or(0), 0);
        });
        let mut s = Session::accept(l, &lanes, &stop).unwrap();
        let entries = s.list(HOME, &stop).unwrap();
        assert_eq!(entries.len(), 1);
        assert_eq!(
            (
                entries[0].name.as_str(),
                entries[0].size,
                entries[0].modified_ms,
                entries[0].dir
            ),
            ("中文", 999, 42, false)
        );
        let flag = &stop;
        scope.spawn(move || {
            waiting.recv().unwrap();
            flag.store(true, Relaxed)
        });
        let start = Instant::now();
        assert!(s.list(HOME, &stop).unwrap_err().contains("取消"));
        assert!(start.elapsed() < Duration::from_secs(3));
        mock.join().unwrap();
    });
}
