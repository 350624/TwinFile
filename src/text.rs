use crate::config::atomic_replace;
use encoding_rs::{BIG5, GB18030, GBK, WINDOWS_1252};
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::{self, Read},
    path::Path,
};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Encoding {
    Utf8,
    Utf16Le,
    Utf16Be,
    Gb18030,
    Gbk,
    Big5,
    Windows1252,
}
impl Encoding {
    pub const ALL: [Self; 7] = [
        Self::Utf8,
        Self::Utf16Le,
        Self::Utf16Be,
        Self::Gb18030,
        Self::Gbk,
        Self::Big5,
        Self::Windows1252,
    ];
    pub fn name(self) -> &'static str {
        match self {
            Self::Utf8 => "UTF-8",
            Self::Utf16Le => "UTF-16 LE",
            Self::Utf16Be => "UTF-16 BE",
            Self::Gb18030 => "GB18030",
            Self::Gbk => "GBK",
            Self::Big5 => "Big5",
            Self::Windows1252 => "Windows-1252",
        }
    }
    fn legacy(self) -> &'static encoding_rs::Encoding {
        match self {
            Self::Gb18030 => GB18030,
            Self::Gbk => GBK,
            Self::Big5 => BIG5,
            _ => WINDOWS_1252,
        }
    }
    fn bom(self) -> &'static [u8] {
        match self {
            Self::Utf8 => b"\xef\xbb\xbf",
            Self::Utf16Le => b"\xff\xfe",
            Self::Utf16Be => b"\xfe\xff",
            _ => b"",
        }
    }
}
#[derive(Debug)]
pub struct Document {
    pub body: String,
    pub encoding: Encoding,
    pub bom: bool,
    baseline: [u8; 32],
    original: String,
    original_lines: Vec<(String, &'static str)>,
    eol: &'static str,
}
fn invalid() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "无法无损转换此编码")
}
fn read_bounded(p: &Path) -> io::Result<Vec<u8>> {
    const MAX: u64 = 16 * 1024 * 1024;
    let mut bytes = Vec::new();
    fs::File::open(p)?.take(MAX + 1).read_to_end(&mut bytes)?;
    if bytes.len() as u64 > MAX {
        return Err(io::Error::other("文本超过 16 MiB"));
    }
    Ok(bytes)
}
impl Document {
    pub fn open(p: &Path, chosen: Option<Encoding>) -> io::Result<Self> {
        let bytes = read_bounded(p)?;
        let encoding = chosen.unwrap_or_else(|| {
            Encoding::ALL
                .into_iter()
                .find(|e| !e.bom().is_empty() && bytes.starts_with(e.bom()))
                .unwrap_or(if std::str::from_utf8(&bytes).is_ok() {
                    Encoding::Utf8
                } else {
                    Encoding::Gb18030
                })
        });
        let bom = !encoding.bom().is_empty() && bytes.starts_with(encoding.bom());
        let input = if bom {
            &bytes[encoding.bom().len()..]
        } else {
            &bytes
        };
        let raw = match encoding {
            Encoding::Utf8 => std::str::from_utf8(input)
                .map_err(|_| invalid())?
                .to_owned(),
            Encoding::Utf16Le | Encoding::Utf16Be => {
                if input.len() % 2 != 0 {
                    return Err(invalid());
                }
                let words = input
                    .chunks_exact(2)
                    .map(|p| {
                        if encoding == Encoding::Utf16Le {
                            u16::from_le_bytes([p[0], p[1]])
                        } else {
                            u16::from_be_bytes([p[0], p[1]])
                        }
                    })
                    .collect::<Vec<_>>();
                String::from_utf16(&words).map_err(|_| invalid())?
            }
            _ => encoding
                .legacy()
                .decode_without_bom_handling_and_without_replacement(input)
                .ok_or_else(invalid)?
                .into_owned(),
        };
        let eol = if raw.contains("\r\n") {
            "\r\n"
        } else if raw.contains('\r') {
            "\r"
        } else {
            "\n"
        };
        let original_lines = split_original(&raw);
        let body = raw.replace("\r\n", "\n").replace('\r', "\n");
        Ok(Self {
            original: body.clone(),
            body,
            encoding,
            bom,
            baseline: Sha256::digest(&bytes).into(),
            original_lines,
            eol,
        })
    }
    pub fn dirty(&self) -> bool {
        self.body != self.original
    }
    pub fn save(&mut self, p: &Path) -> io::Result<()> {
        if !self.dirty() {
            return Ok(());
        }
        let current: [u8; 32] = Sha256::digest(read_bounded(p)?).into();
        if current != self.baseline {
            return Err(io::Error::other("文件已被其他程序修改，未覆盖"));
        }
        let lines = self.body.split('\n').collect::<Vec<_>>();
        let raw = lines
            .iter()
            .enumerate()
            .fold(String::new(), |mut out, (i, line)| {
                out.push_str(line);
                if i + 1 < lines.len() {
                    out.push_str(
                        self.original_lines
                            .get(i)
                            .filter(|(s, e)| s == line && !e.is_empty())
                            .map_or(self.eol, |(_, e)| *e),
                    );
                }
                out
            });
        let mut out = if self.bom {
            self.encoding.bom().to_vec()
        } else {
            vec![]
        };
        match self.encoding {
            Encoding::Utf8 => out.extend_from_slice(raw.as_bytes()),
            Encoding::Utf16Le | Encoding::Utf16Be => {
                for w in raw.encode_utf16() {
                    out.extend(if self.encoding == Encoding::Utf16Le {
                        w.to_le_bytes()
                    } else {
                        w.to_be_bytes()
                    });
                }
            }
            _ => {
                let (bytes, _, loss) = self.encoding.legacy().encode(&raw);
                if loss {
                    return Err(invalid());
                }
                out.extend_from_slice(&bytes);
            }
        }
        atomic_replace(p, &out)?;
        self.baseline = Sha256::digest(&out).into();
        self.original.clone_from(&self.body);
        self.original_lines = split_original(&raw);
        Ok(())
    }
}
fn split_original(mut s: &str) -> Vec<(String, &'static str)> {
    let mut lines = vec![];
    while let Some(i) = s.find(['\r', '\n']) {
        let tail = &s[i..];
        let end = if tail.starts_with("\r\n") {
            "\r\n"
        } else if tail.starts_with('\r') {
            "\r"
        } else {
            "\n"
        };
        lines.push((s[..i].to_owned(), end));
        s = &tail[end.len()..];
    }
    lines.push((s.to_owned(), ""));
    lines
}
