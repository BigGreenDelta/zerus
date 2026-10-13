fn main() {
    println!("cargo:rerun-if-changed=src/macos_session.c");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos") {
        cc::Build::new().file("src/macos_session.c")
            .flag_if_supported("-Wno-deprecated-declarations")
            .compile("hgs_macos_session");
        for library in ["framework=Security", "framework=CoreFoundation", "bsm"] {
            println!("cargo:rustc-link-lib={library}");
        }
    }
    // hgs.exe gets the application manifest hgs-tray.exe has: UTF-8 as the ANSI
    // code page, long paths and the Windows 10 and 11 compatibility section.
    // link.exe embeds it; other targets link nothing extra.
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows")
        && std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc")
    {
        println!("cargo:rerun-if-changed=src/hgs.manifest");
        let manifest = std::path::Path::new(&std::env::var("CARGO_MANIFEST_DIR").unwrap())
            .join("src")
            .join("hgs.manifest");
        println!("cargo:rustc-link-arg-bins=/MANIFEST:EMBED");
        println!("cargo:rustc-link-arg-bins=/MANIFESTINPUT:{}", manifest.display());
    }
}
