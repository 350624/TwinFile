use super::*;
use std::io::Cursor;
fn vi(mut n: u64) -> Vec<u8> {
    let mut v = vec![];
    while n > 127 {
        v.push((n as u8) | 128);
        n >>= 7
    }
    v.push(n as u8);
    v
}
fn num(k: u8, n: u64) -> Vec<u8> {
    [vi((k as u64) << 3), vi(n)].concat()
}
fn bytes(k: u8, b: &[u8]) -> Vec<u8> {
    [vi((k as u64) << 3 | 2), vi(b.len() as u64), b.to_vec()].concat()
}
fn fixture(kind: u64, compressed: &[u8], name: &str, badhash: bool) -> (Vec<u8>, Vec<u8>) {
    let raw = b"ABCD";
    let expected = b"\0\0\0\0ABCD".to_vec();
    let mut ph = Sha256::digest(&expected);
    if badhash {
        ph[0] ^= 0xff
    }
    let part_info = [num(1, 8), bytes(2, &ph)].concat();
    let ext = [num(1, 1), num(2, 1)].concat();
    let op = [
        num(1, kind),
        num(3, compressed.len() as u64),
        bytes(6, &ext),
        bytes(8, &Sha256::digest(compressed)),
    ]
    .concat();
    let zero = [num(1, 6), bytes(6, &num(2, 1))].concat();
    let part = [
        bytes(1, name.as_bytes()),
        bytes(7, &part_info),
        bytes(8, &op),
        bytes(8, &zero),
    ]
    .concat();
    let man = [num(3, raw.len() as u64), bytes(13, &part)].concat();
    let all = [
        b"CrAU".as_slice(),
        &2u64.to_be_bytes(),
        &(man.len() as u64).to_be_bytes(),
        &[0; 4],
        &man,
        compressed,
    ]
    .concat();
    (all, expected)
}
fn nop(_: &str, _: u64, _: u64) {}
#[test]
fn all_output_codecs_offsets_zero_sha_and_no_clobber() {
    let mut bzip = bzip2::write::BzEncoder::new(vec![], bzip2::Compression::default());
    bzip.write_all(b"ABCD").unwrap();
    let mut xz = xz2::write::XzEncoder::new(vec![], 1);
    xz.write_all(b"ABCD").unwrap();
    let c = AtomicBool::new(false);
    for (kind, data) in [
        (0, b"ABCD".to_vec()),
        (1, bzip.finish().unwrap()),
        (8, xz.finish().unwrap()),
        (14, zstd::stream::encode_all(b"ABCD".as_slice(), 1).unwrap()),
    ] {
        let tmp = tempfile::tempdir().unwrap();
        let f = tmp.path().join("a.bin");
        let (v, expected) = fixture(kind, &data, "boot", false);
        fs::write(&f, v).unwrap();
        let p = inspect(f.to_str().unwrap(), &c, &nop).unwrap();
        assert_eq!(p.partitions[0].name, "boot");
        let names = ["boot".into()];
        let out = extract(&p, &names, tmp.path(), 4, &c, &nop).unwrap();
        assert_eq!(fs::read(&out[0]).unwrap(), expected);
        assert!(extract(&p, &names, tmp.path(), 1, &c, &nop).is_err());
        assert_eq!(fs::read(&out[0]).unwrap(), expected);
    }
}
#[test]
fn ota_zip_stored_deflate_and_hash_failure_leave_no_output() {
    let c = AtomicBool::new(false);
    for method in [
        zip::CompressionMethod::Stored,
        zip::CompressionMethod::Deflated,
    ] {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("ota.zip");
        let (v, _) = fixture(0, b"ABCD", "boot", true);
        let mut z = zip::ZipWriter::new(File::create(&path).unwrap());
        z.start_file(
            "payload.bin",
            zip::write::SimpleFileOptions::default().compression_method(method),
        )
        .unwrap();
        z.write_all(&v).unwrap();
        z.finish().unwrap();
        let p = inspect(path.to_str().unwrap(), &c, &nop).unwrap();
        assert!(
            extract(&p, &["boot".into()], dir.path(), 2, &c, &nop)
                .unwrap_err()
                .contains("SHA256")
        );
        assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 1);
    }
}
#[test]
fn unsafe_manifest_delta_and_cancel_rejected() {
    let tmp = tempfile::tempdir().unwrap();
    let f = tmp.path().join("ota");
    for (kind, name) in [(0, "../evil"), (5, "boot"), (0, "CON")] {
        fs::write(&f, fixture(kind, b"ABCD", name, false).0).unwrap();
        assert!(inspect(f.to_str().unwrap(), &AtomicBool::new(false), &nop).is_err())
    }
    assert!(
        inspect(f.to_str().unwrap(), &AtomicBool::new(true), &nop)
            .unwrap_err()
            .contains("取消")
    );
}
#[test]
fn remote_ota_zip_reads_selected_ranges_only() {
    use std::net::{TcpListener, TcpStream};
    use std::sync::atomic::AtomicU64;
    let (v, expected) = fixture(0, b"ABCD", "boot", false);
    let mut z = zip::ZipWriter::new(Cursor::new(Vec::new()));
    let opt =
        zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Stored);
    z.start_file("payload.bin", opt).unwrap();
    z.write_all(&v).unwrap();
    z.start_file("unrelated", opt).unwrap();
    z.write_all(&vec![0; 8 << 20]).unwrap();
    let ota = z.finish().unwrap().into_inner();
    let lim = ota.len();
    let http = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = http.local_addr().unwrap();
    let stop = AtomicBool::new(false);
    let network = AtomicU64::new(0);
    std::thread::scope(|s| {
        s.spawn(||{for mut stream in http.incoming().map(|x|x.unwrap()){if stop.load(Relaxed){break}let mut head=BufReader::new(&stream);let mut range=None;let mut line=String::new();loop{line.clear();if head.read_line(&mut line).unwrap()==0||line=="\r\n"{break}if let Some(v)=line.to_ascii_lowercase().strip_prefix("range: bytes="){let(a,b)=v.trim().split_once('-').unwrap();range=Some((a.parse::<usize>().unwrap(),b.parse::<usize>().unwrap()))}}
 let(a,b)=range.unwrap();write!(stream,"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes {a}-{b}/{lim}\r\nContent-Length: {}\r\nETag: \"v1\"\r\nConnection: close\r\n\r\n",b-a+1).unwrap();let _=stream.write_all(&ota[a..=b]);network.fetch_add((b-a+1)as u64,Relaxed);}});
        let run = std::panic::catch_unwind(|| {
            let c = AtomicBool::new(false);
            let p = inspect(&format!("http://{addr}/ota.zip"), &c, &nop).unwrap();
            let temp = tempfile::tempdir().unwrap();
            let paths = extract(&p, &["boot".into()], temp.path(), 1, &c, &nop).unwrap();
            assert_eq!(fs::read(&paths[0]).unwrap(), expected);
            assert!(network.load(Relaxed) < lim as u64);
        });
        stop.store(true, Relaxed);
        let _ = TcpStream::connect(addr);
        run.unwrap();
    });
}
#[test]
fn remote_without_range_caches_verified_file_and_rejects_changed_range_version() {
    use std::net::{TcpListener, TcpStream};
    let (fixture, expected) = fixture(0, b"ABCD", "boot", false);
    for ranges in [false, true] {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let stop = AtomicBool::new(false);
        std::thread::scope(|scope| {
            scope.spawn(||{
                for mut stream in listener.incoming().map(Result::unwrap){
                    if stop.load(Relaxed){break}
                    let mut hdr=BufReader::new(&stream);let mut range=None;let mut line=String::new();
                    loop {line.clear();if hdr.read_line(&mut line).unwrap()==0||line=="\r\n"{break}if let Some(v)=line.to_ascii_lowercase().strip_prefix("range: bytes="){let(a,b)=v.trim().split_once('-').unwrap();range=Some((a.parse::<usize>().unwrap(),b.parse::<usize>().unwrap()));}}
                    let len=fixture.len();if ranges {let(a,b)=range.unwrap();let tag=if b==0{"old"}else{"changed"};write!(stream,"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes {a}-{b}/{len}\r\nETag: \"{tag}\"\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",b-a+1).unwrap();let _=stream.write_all(&fixture[a..=b]);}
                    else{write!(stream,"HTTP/1.1 200 OK\r\nContent-Length: {len}\r\nConnection: close\r\n\r\n").unwrap();let _=stream.write_all(&fixture);}
                }
            });
            let res = std::panic::catch_unwind(|| {
                let c = AtomicBool::new(false);
                let p = inspect(&format!("http://{addr}/ota"), &c, &nop);
                if ranges {
                    assert!(p.unwrap_err().contains("远程文件已变化"))
                } else {
                    let p = p.unwrap();
                    let tmp = tempfile::tempdir().unwrap();
                    let files = extract(&p, &["boot".into()], tmp.path(), 1, &c, &nop).unwrap();
                    assert_eq!(fs::read(&files[0]).unwrap(), expected);
                }
            });
            stop.store(true, Relaxed);
            let _ = TcpStream::connect(addr);
            res.unwrap();
        });
    }
}
