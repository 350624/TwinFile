use serde::{Deserialize, Serialize};
use std::io::{self, Read, Write};
const CAP: usize = 65536;

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Request {
    pub url: String,
    #[serde(default)]
    pub filename: String,
}
impl Request {
    pub fn validated(mut self) -> io::Result<Self> {
        if self.url.chars().any(char::is_control)
            || self.url.contains('#')
            || self.url.len() > 24 * 1024
        {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "无效直链"));
        }
        let u = crate::download::url(&self.url).map_err(|_| {
            io::Error::new(io::ErrorKind::InvalidInput, "仅支持不含凭据的 HTTP(S) URL")
        })?;
        self.url = u.into();
        self.filename = self
            .filename
            .rsplit(['\\', '/'])
            .next()
            .filter(|n| n.len() <= 240 && crate::files::safe_name(n).is_ok())
            .unwrap_or("")
            .to_owned();
        Ok(self)
    }
}
#[derive(Serialize, Deserialize)]
pub struct Response {
    pub accepted: bool,
}

pub fn recv<T: serde::de::DeserializeOwned>(mut stream: impl Read) -> io::Result<T> {
    let mut n = [0; 4];
    stream.read_exact(&mut n)?;
    let n = u32::from_le_bytes(n) as usize;
    if !(1..=CAP).contains(&n) {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "无效消息帧长度"));
    }
    let mut bytes = vec![0; n];
    stream.read_exact(&mut bytes)?;
    serde_json::from_slice(&bytes).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))
}
pub fn send<T: Serialize>(mut stream: impl Write, message: &T) -> io::Result<()> {
    let data = serde_json::to_vec(message).map_err(io::Error::other)?;
    if !(1..=CAP).contains(&data.len()) {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "无效消息帧长度"));
    }
    stream.write_all(&(data.len() as u32).to_le_bytes())?;
    stream.write_all(&data)?;
    stream.flush()
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn binary_frame_round_trip_and_bounds() {
        let mut b = vec![];
        let r = Request {
            url: "https://host.test/a?b=c".into(),
            filename: "文件.zip".into(),
        };
        send(&mut b, &r).unwrap();
        assert_eq!(
            u32::from_le_bytes(b[..4].try_into().unwrap()) as usize,
            b.len() - 4
        );
        assert_eq!(recv::<Request>(&b[..]).unwrap(), r);
        assert!(recv::<Request>(&b[..b.len() - 1]).is_err());
        assert!(recv::<Request>(&65537u32.to_le_bytes()[..]).is_err());
    }
    #[test]
    fn validates_url_and_removes_directory_from_suggestion() {
        let r = Request {
            url: "https://host.test/x".into(),
            filename: r"D:\test\外部.zip".into(),
        }
        .validated()
        .unwrap();
        assert_eq!(r.filename, "外部.zip");
        for url in [
            "https://user:pass@host.test/a",
            "file:///C:/test",
            "https://host.test/#frag",
        ] {
            assert!(
                Request {
                    url: url.into(),
                    filename: String::new()
                }
                .validated()
                .is_err()
            );
        }
        assert_eq!(
            Request {
                url: "http://host.test/".into(),
                filename: "NUL.txt".into()
            }
            .validated()
            .unwrap()
            .filename,
            ""
        );
    }
}
