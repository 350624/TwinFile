use std::{
    io,
    os::windows::{io::AsRawHandle, process::CommandExt},
    process::{Command, Stdio},
    sync::mpsc,
    time::Duration,
};
use twinfile_rust::bridge::{self, Request, Response};
use windows_sys::Win32::Foundation::{
    HANDLE_FLAG_INHERIT, INVALID_HANDLE_VALUE, SetHandleInformation,
};

fn detach_browser_pipes() -> io::Result<()> {
    // The downloader owns separate pipes; do not let it keep the browser's host
    // streams alive after this process exits.
    for handle in [
        io::stdin().as_raw_handle(),
        io::stdout().as_raw_handle(),
        io::stderr().as_raw_handle(),
    ] {
        if !handle.is_null()
            && handle != INVALID_HANDLE_VALUE
            && unsafe { SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0) } == 0
        {
            return Err(io::Error::last_os_error());
        }
    }
    Ok(())
}

fn run() -> io::Result<()> {
    let request = bridge::recv::<Request>(io::stdin().lock())?.validated()?;
    detach_browser_pipes()?;
    let exe = std::env::current_exe()?.with_file_name("TwinFileRustDownloader.exe");
    let mut child = Command::new(exe)
        .arg("--browser-stdin")
        .creation_flags(0x08000000)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()?;
    let result = (|| {
        bridge::send(
            child
                .stdin
                .take()
                .ok_or_else(|| io::Error::other("无法连接下载器"))?,
            &request,
        )?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| io::Error::other("下载器无响应通道"))?;
        let (tx, rx) = mpsc::sync_channel(1);
        std::thread::spawn(move || {
            let _ = tx.send(bridge::recv::<Response>(stdout));
        });
        if !rx
            .recv_timeout(Duration::from_secs(12))
            .map_err(|_| io::Error::other("下载器窗口未就绪"))??
            .accepted
        {
            return Err(io::Error::other("下载器没有接收任务"));
        }
        Ok(())
    })();
    if result.is_err() {
        let _ = child.kill();
        let _ = child.wait();
    }
    result
}
fn main() {
    let _ = bridge::send(
        io::stdout().lock(),
        &Response {
            accepted: run().is_ok(),
        },
    );
}
