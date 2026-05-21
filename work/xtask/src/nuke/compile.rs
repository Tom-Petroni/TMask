use std::path::{Path, PathBuf};

use anyhow::{Context as _, Error, Result};
use duct::cmd;

use crate::{
    TargetPlatform,
    nuke::sources::{dll_prefix, dll_suffix, get_sources, nuke_source_directory},
    util::{crate_root, path_to_string, target_directory},
};

fn build_dir(version: &str, target: TargetPlatform) -> PathBuf {
    target_directory()
        .join("nuke")
        .join("builds")
        .join(version)
        .join(format!("{}", target))
}

pub async fn compile_nuke(
    versions: &[String],
    target: TargetPlatform,
    limit_threads: bool,
    use_zig: bool,
) -> Result<Vec<PathBuf>> {
    let mut missing_versions: Vec<String> = Vec::new();
    for version in versions {
        let ddimage_path = nuke_source_directory(version).join(format!(
            "{}DDImage.{}",
            dll_prefix(target),
            dll_suffix(target)
        ));
        if !ddimage_path.exists() {
            missing_versions.push(version.clone());
        }
    }

    if !missing_versions.is_empty() {
        get_sources(vec![target], &missing_versions, limit_threads).await?;
    }

    let mut binaries = vec![];
    for version in versions {
        let ddimage_path = nuke_source_directory(&version).join(format!(
            "{}DDImage.{}",
            dll_prefix(target),
            dll_suffix(target)
        ));
        if !ddimage_path.exists() {
            log::warn!(
                "Skipping {version} as no sources could be found for this version on {target} (expected '{}')",
                ddimage_path.display()
            );
            continue;
        }

        let macos_deployment_target = if target == TargetPlatform::MacosAarch64
            || target == TargetPlatform::MacosX86_64
        {
            Some(get_macos_deployment_target(&version)?)
        } else {
            None
        };

        if use_zig {
            binaries.push(compile_zig(&version, target, macos_deployment_target.as_deref()).await?);
            continue;
        }

        if is_crosscompiling(target) {
            return Err(Error::msg("Cannot cross compile without using zig or xwin"));
        }

        binaries.push(compile_native(&version, target, macos_deployment_target.as_deref()).await?);
    }

    Ok(binaries)
}

fn is_crosscompiling(target: TargetPlatform) -> bool {
    match target {
        TargetPlatform::Linux => std::env::consts::OS != "linux",
        TargetPlatform::Windows => std::env::consts::OS != "windows",
        TargetPlatform::MacosAarch64 => {
            std::env::consts::OS != "macos" || std::env::consts::ARCH != "aarch64"
        }
        TargetPlatform::MacosX86_64 => {
            std::env::consts::OS != "macos" || std::env::consts::ARCH != "x86_64"
        }
    }
}

fn parse_nuke_version(version: &str) -> Result<(u32, u32)> {
    let (major_str, minor_str) = version
        .split_once('.')
        .ok_or_else(|| Error::msg(format!("Invalid Nuke version format: {version}")))?;
    let major = major_str
        .parse::<u32>()
        .map_err(|_| Error::msg(format!("Invalid major version: {major_str}")))?;
    let minor = minor_str
        .parse::<u32>()
        .map_err(|_| Error::msg(format!("Invalid minor version: {minor_str}")))?;
    Ok((major, minor))
}

fn get_cpp_version(version: &str) -> Result<usize> {
    let (major, _) = parse_nuke_version(version)?;
    if major < 13 {
        return Ok(11);
    } else if major < 14 {
        return Ok(14);
    }
    Ok(17)
}

fn use_cxx11_abi(version: &str) -> Result<bool> {
    let (major, minor) = parse_nuke_version(version)?;
    Ok(major > 14 || (major == 14 && minor >= 1))
}

fn get_macos_deployment_target(version: &str) -> Result<String> {
    let (major, _) = parse_nuke_version(version)?;
    if major < 13 {
        return Ok("11.0".to_owned());
    } else if major < 14 {
        return Ok("12.0".to_owned());
    }
    Ok("14.0".to_owned())
}

fn get_platform_name(target: TargetPlatform) -> String {
    match target {
        TargetPlatform::Linux => "linux",
        TargetPlatform::Windows => "windows",
        _ => "macos",
    }
    .to_string()
}

fn get_zig_target(target: TargetPlatform) -> String {
    match target {
        TargetPlatform::Linux => "x86_64-unknown-linux-gnu.2.17",
        TargetPlatform::Windows => "x86_64-pc-windows-msvc",
        TargetPlatform::MacosAarch64 => "aarch64-apple-darwin",
        TargetPlatform::MacosX86_64 => "x86_64-apple-darwin",
    }
    .to_string()
}

fn get_rust_target(target: TargetPlatform) -> String {
    match target {
        TargetPlatform::Linux => "x86_64-unknown-linux-gnu",
        TargetPlatform::Windows => "x86_64-pc-windows-msvc",
        TargetPlatform::MacosAarch64 => "aarch64-apple-darwin",
        TargetPlatform::MacosX86_64 => "x86_64-apple-darwin",
    }
    .to_string()
}

fn packaged_binary_name(target: TargetPlatform) -> String {
    match target {
        TargetPlatform::Windows => "TMask.dll",
        TargetPlatform::Linux => "libTMask.so",
        TargetPlatform::MacosAarch64 | TargetPlatform::MacosX86_64 => "libTMask.dylib",
    }
    .to_string()
}

async fn compile_native(
    version: &str,
    target: TargetPlatform,
    macos_deployment_target: Option<&str>,
) -> Result<PathBuf, anyhow::Error> {
    let sources_directory = nuke_source_directory(version);
    let crates_path = path_to_string(
        &crate_root()
            .join("crates")
            .join("tmask-nuke")
            .join("Cargo.toml"),
    )?;

    if target == TargetPlatform::Windows
        && !nuke_source_directory(version).join("DDImage.lib").exists()
    {
        create_lib_from_dll(&nuke_source_directory(version).join("DDImage.dll")).await?;
    }

    let mut expression = cmd!(
        "cargo",
        "build",
        "--manifest-path",
        &crates_path,
        "--release",
        "--target",
        get_rust_target(target),
    )
    .env("CPP_VERSION", format!("{}", get_cpp_version(version)?))
    .env("NUKE_SOURCE_PATH", &sources_directory)
    .env("PLATFORM_NAME", get_platform_name(target));

    if let Some(deployment_target) = macos_deployment_target {
        expression = expression.env("MACOSX_DEPLOYMENT_TARGET", deployment_target);
    }

    if use_cxx11_abi(version)? {
        expression = expression.env("USE_CXX11_ABI", "1");
    }

    expression.run()?;

    let out_dir = build_dir(version, target);
    if !out_dir.is_dir() {
        tokio::fs::create_dir_all(&out_dir).await?;
    }

    let output_dylib = out_dir.join(packaged_binary_name(target));
    let build_lib = path_to_string(
        &target_directory()
            .join(get_rust_target(target))
            .join("release")
            .join(format!(
                "{}tmask_nuke.{}",
                dll_prefix(target),
                dll_suffix(target)
            )),
    )?;

    if output_dylib.exists() {
        tokio::fs::remove_file(&output_dylib).await?;
    }
    tokio::fs::rename(build_lib, &output_dylib).await?;
    Ok(output_dylib)
}

async fn compile_zig(
    version: &str,
    target: TargetPlatform,
    macos_deployment_target: Option<&str>,
) -> Result<PathBuf, anyhow::Error> {
    let sources_directory = nuke_source_directory(version);

    let crates_path = path_to_string(
        &crate_root()
            .join("crates")
            .join("tmask-nuke")
            .join("Cargo.toml"),
    )?;

    let mut expression = cmd!(
        "cargo",
        "zigbuild",
        "--manifest-path",
        &crates_path,
        "--release",
        "--target",
        get_zig_target(target),
    )
    .env("CPP_VERSION", format!("{}", get_cpp_version(version)?))
    .env("NUKE_SOURCE_PATH", &sources_directory)
    .env("PLATFORM_NAME", get_platform_name(target))
    .env("USING_ZIG", "1");

    if let Some(deployment_target) = macos_deployment_target {
        expression = expression.env("MACOSX_DEPLOYMENT_TARGET", deployment_target);
    }

    if use_cxx11_abi(version)? {
        expression = expression.env("USE_CXX11_ABI", "1");
    }

    expression.run()?;

    let out_dir = build_dir(version, target);
    if !out_dir.is_dir() {
        tokio::fs::create_dir_all(&out_dir).await?;
    }

    let output_dylib = out_dir.join(packaged_binary_name(target));
    let build_lib = path_to_string(
        &target_directory()
            .join(get_rust_target(target))
            .join("release")
            .join(format!(
                "{}tmask_nuke.{}",
                dll_prefix(target),
                dll_suffix(target)
            )),
    )?;

    if output_dylib.exists() {
        tokio::fs::remove_file(&output_dylib).await?;
    }
    tokio::fs::rename(build_lib, &output_dylib).await?;
    Ok(output_dylib)
}

pub async fn create_lib_from_dll(dll: &Path) -> Result<()> {
    let dumpbin = find_msvc_tools::find("x86_64", "dumpbin.exe")
        .context("Failed to locate dumpbin.exe via find_msvc_tools")?;
    let dll_parent = dll.parent().context("DLL path has no parent directory")?;
    let exports = cmd!(dumpbin.get_program(), "/EXPORTS", dll)
        .dir(dll_parent)
        .read()?;

    let mut lines = vec!["EXPORTS\n".to_string()];
    for (i, line) in exports.lines().enumerate() {
        if i < 19 {
            continue;
        }
        if line.len() > 26 {
            lines.push(line[26..].to_string());
            lines.push("\n".to_string());
        }
    }

    let exports = lines.iter().cloned().collect::<String>();

    let def = PathBuf::from("DDImage.def");
    tokio::fs::write(dll_parent.join(&def), exports).await?;

    let lib = find_msvc_tools::find("x86_64", "lib.exe")
        .context("Failed to locate lib.exe via find_msvc_tools")?;
    cmd!(
        lib.get_program(),
        "/def:DDImage.def",
        "/machine:x64",
        "/out:DDImage.lib"
    )
    .dir(dll_parent)
    .run()?;

    Ok(())
}
