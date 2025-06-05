//! Build a Sightglass engine using LibWasm from Ladybird. Usage:
//! ```
//! rustc build.rs
//! ./build [<ladybird repo dir>] [<ladybird build dir>] [<destination dir>]
//! ```
//! Note that at least LibWasm must be built in the ladybird repo (and be present in the build dir)
//! before running this script.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let args: Vec<_> = env::args_os().collect();
    if args.len() < 3 {
        eprintln!("Usage: ./build <ladybird repo dir> <ladybird build dir> [destination dir]");
        std::process::exit(1);
    }

    let ladybird_repo_dir = PathBuf::from(&args[1]);
    let ladybird_build_dir = PathBuf::from(&args[2]);
    let destination_dir = match args.get(3) {
        Some(p) => Path::new(p).canonicalize().expect("the third parameter is not a valid directory"),
        None => env::current_dir().unwrap(),
    };
    if !ladybird_repo_dir.exists() || !ladybird_build_dir.exists() {
        eprintln!("Ladybird repository or build directory does not exist.");
        std::process::exit(1);
    }
    if !destination_dir.exists() {
        fs::create_dir_all(&destination_dir).expect("Failed to create destination directory");
    }
    
    eprintln!("===== Building libengine-libwasm =====");
    let output = Command::new("c++")
        .arg("libengine-libwasm.cpp")
        .arg("-std=c++26")
        .arg(format!("-I{}", ladybird_repo_dir.display()))
        .arg(format!("-I{}/Libraries", ladybird_repo_dir.display()))
        .arg(format!("-I{}/Lagom", ladybird_build_dir.display()))
        .arg(format!("-I{}/Lagom/Libraries", ladybird_build_dir.display()))
        .arg("-Wno-unqualified-std-cast-call")
        .arg("-shared")
        .arg("-fPIC")
        .arg(format!("-L{}/lib", ladybird_build_dir.display()))
        .arg("-Wl,-rpath,")
        .arg(format!("{}/lib", ladybird_build_dir.display()))
        .arg("-llagom-coreminimal")
        .arg("-llagom-ak")
        .arg("-llagom-core")
        .arg("-llagom-wasm")
        .arg("-g3")
        .arg("-o")
        .arg(format!("{}/{}engine-libwasm{}", destination_dir.display(), env::consts::DLL_PREFIX, env::consts::DLL_SUFFIX))
        .output()
        .expect("Failed to execute command");

    if !output.status.success() {
        eprintln!("Failed to build libengine-libwasm:");
        eprintln!("{}", String::from_utf8_lossy(&output.stderr));
        std::process::exit(1);
    }

    eprintln!("libengine-libwasm built successfully at {}", destination_dir.display());
}