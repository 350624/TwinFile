use std::{env, fs, path::PathBuf, process::Command};

fn main() {
    println!("cargo:rerun-if-changed=assets/app.ico");
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    // MinGW resource tools also need to work when the checkout has a non-ASCII name.
    fs::copy("assets/app.ico", out.join("app.ico")).expect("stage application icon");
    fs::write(out.join("app.rc"), "1 ICON \"app.ico\"\n").unwrap();
    let msvc = env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc");
    let (tool, args, file) = if msvc {
        ("rc", vec!["/nologo", "/foapp.res", "app.rc"], "app.res")
    } else {
        (
            "windres",
            vec!["-i", "app.rc", "-O", "coff", "-o", "app-icon.o"],
            "app-icon.o",
        )
    };
    let status = Command::new(tool)
        .args(args)
        .current_dir(&out)
        .status()
        .expect("Windows resource compiler not found");
    assert!(
        status.success(),
        "{tool} failed to compile the Windows icon"
    );
    println!(
        "cargo:rustc-link-arg-bin=twinfile-rust={}",
        out.join(file).display()
    );
}
