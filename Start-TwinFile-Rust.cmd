@echo off
setlocal
cd /d "%~dp0"
if not defined TWINFILE_RUST_BUILD_ROOT set "TWINFILE_RUST_BUILD_ROOT=D:\TwinFile-Rust-Build"
if not defined TWINFILE_RUST_GCC_BIN set "TWINFILE_RUST_GCC_BIN=D:\Qt\Tools\mingw1310_64\bin"
set "RUSTUP_HOME=%TWINFILE_RUST_BUILD_ROOT%\rustup"
set "CARGO_HOME=%TWINFILE_RUST_BUILD_ROOT%\cargo"
set "CARGO_TARGET_DIR=%TWINFILE_RUST_BUILD_ROOT%\target-v2"
set "PATH=%TWINFILE_RUST_GCC_BIN%;%CARGO_HOME%\bin;%PATH%"
if not exist "%CARGO_HOME%\bin\cargo.exe" (
  echo Missing Rust toolchain at "%CARGO_HOME%\bin".
  goto failed
)
if not exist "%TWINFILE_RUST_GCC_BIN%\x86_64-w64-mingw32-gcc.exe" (
  echo Missing MinGW at "%TWINFILE_RUST_GCC_BIN%".
  goto failed
)
"%CARGO_HOME%\bin\cargo.exe" build --locked --release --bin twinfile-rust
if errorlevel 1 goto failed
if /i "%~1"=="--build-only" exit /b 0
start "" "%CARGO_TARGET_DIR%\release\twinfile-rust.exe"
exit /b 0
:failed
if /i not "%~1"=="--build-only" pause
exit /b 1
