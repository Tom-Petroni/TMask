use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=NUKE_SOURCE_PATH");
    println!("cargo:rerun-if-env-changed=PLATFORM_NAME");
    println!("cargo:rerun-if-env-changed=CPP_VERSION");
    println!("cargo:rerun-if-changed=src/tmask_base.cpp");

    let nuke_root = if let Ok(sources) = std::env::var("NUKE_SOURCE_PATH") {
        PathBuf::from(sources)
    } else {
        return;
    };
    let nuke_path = nuke_root.join("include");

    let platform_name = if let Ok(name) = std::env::var("PLATFORM_NAME") {
        name
    } else {
        return;
    };

    let cpp_version = std::env::var("CPP_VERSION").unwrap_or_else(|_| "17".to_string());

    let mut builder = cc::Build::new();
    builder
        .cpp(true)
        .std(&format!("c++{cpp_version}"))
        .file("src/tmask_base.cpp")
        .include(&nuke_path)
        .flag_if_supported("-DGLEW_NO_GLU");

    if platform_name == "linux" {
        builder
            .flag("-fPIC")
            .flag_if_supported("-Wno-deprecated-copy-with-user-provided-copy")
            .flag_if_supported("-Wno-ignored-qualifiers")
            .flag_if_supported("-Wno-date-time")
            .flag_if_supported("-Wno-unused-parameter");

        if std::env::var("USE_CXX11_ABI").is_ok() {
            builder.flag("-D_GLIBCXX_USE_CXX11_ABI=1");
        }

        if std::env::var("USING_ZIG").is_ok() {
            builder.define("__gnu_cxx", "std");
        }
    } else if platform_name == "macos" {
        builder
            .flag_if_supported("-Wno-deprecated-copy-with-user-provided-copy")
            .flag_if_supported("-Wno-ignored-qualifiers")
            .flag_if_supported("-Wno-date-time")
            .flag_if_supported("-Wno-unused-parameter");
    } else if platform_name == "windows" {
        builder
            .define("_CPPUNWIND", "1")
            .define("NOMINMAX", "1")
            .define("_USE_MATH_DEFINES", "1")
            .flag("/EHsc");
    }

    builder.compile("tmask-nuke");

    println!("cargo:rustc-link-search=all={}", nuke_root.display());
    println!("cargo:rustc-link-lib=dylib=DDImage");
}

