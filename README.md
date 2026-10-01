# MoonShot

## Build

Use an out-of-source build with CMake 3.19 or newer. `Build` is a disposable
directory you create, not a required directory name. CMake places its generated
files, native/Rust outputs, and WASM viewer beneath the selected build directory.
The output architecture comes from the compiler target, not the host CPU.

### Windows

```powershell
mkdir Build
cd Build
cmake ..
cmake --build . --config Debug --parallel
cmake --build . --config Release --parallel
```

`cmake ..` uses the generator's default platform. With Visual Studio, select a
platform explicitly on the **first configure** if needed:

```powershell
cmake .. -G "Visual Studio 17 2022" -A ARM64
# Or, in a different/fresh build directory:
cmake .. -G "Visual Studio 17 2022" -A x64
```

One CMake build tree has one target architecture. Do not change `-A` in an
existing configured tree. To keep both builds, create separate trees inside
`Build` (for example, from the repository root, use
`cmake -S . -B Build/windows-arm64 -A ARM64` and
`cmake -S . -B Build/windows-x64 -A x64`). Do not copy caches or generated files
between them.

Set `VCPKG_ROOT` to enable the vcpkg toolchain automatically. The vcpkg target
triplet must match the C++ architecture (`arm64-windows`, `x64-windows`, or
`x86-windows` for MSVC); mismatches are rejected. Visual Studio also supports
`-A Win32` for x86, provided the corresponding C++ tools and dependencies exist.

CMake passes an explicit matching target to Cargo, including for cross-builds.
Install the Rust standard library for the chosen MSVC target before building:

| CMake platform | Output architecture | Rust target |
| --- | --- | --- |
| `ARM64` | `arm64` | `aarch64-pc-windows-msvc` |
| `x64` | `x64` | `x86_64-pc-windows-msvc` |
| `Win32` | `x86` | `i686-pc-windows-msvc` |

For example: `rustup target add x86_64-pc-windows-msvc`. Cross-builds also
require the corresponding Visual Studio C++ build tools/SDK. Debug uses Cargo's
`dev` profile; Release, RelWithDebInfo, and MinSizeRel use Cargo's `release`
profile, with separate output/cache directories for each CMake configuration.

On Windows ARM64 hosts, the WASM viewer requires LLVM Clang. Its optional
`wasm-opt` pass is skipped because the optimizer has no ARM64 binary. If Clang
is unavailable, configure with `-DMOONSHOT_BUILD_WASM=OFF` to omit the viewer.
The viewer targets `wasm32-unknown-unknown` regardless of the native target.

### Linux/macOS

```bash
mkdir Build
cd Build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
# With the Unix Makefiles generator, "make" also works here.
```

Native builds use the Rust host triple and verify that its architecture matches
C++. For cross-compilation, supply a CMake toolchain and
`-DMOONSHOT_RUST_TARGET=<matching-rust-triple>`, along with the required Rust
target and linker configuration. macOS builds must select one architecture per
build tree; universal binaries are not supported by the Rust executable build.

### Output layout

For a build configured directly in `Build`, only the selected architecture is
created:

```text
Build/
  CMakeCache.txt, CMakeFiles/, generated projects/Makefiles, ...
  <architecture>/          arm64, x64, x86, or arm
    Debug/
    Release/
      ...                 C++ executables/libraries and copied Rust executables
      rust/               Cargo target/profile-specific intermediates
      moon_wasm/          viewer assets and WASM package
  sdk/                    default destination for cmake --install .
```

The same layout applies relative to any other chosen build directory.
Architecture names are lowercase in CMake paths; on Windows, Visual Studio may
create the same case-insensitive directory as `ARM64`.

Examples from the repository root for an x64 Debug build (replace `x64` with
`arm64` for ARM64):

```powershell
.\Build\x64\Debug\moon.exe -i
.\Build\x64\Debug\shennong.exe --port 9000 --index "$env:USERPROFILE\moon.idx"
.\Build\x64\Debug\moon_rs.exe -i
.\Build\x64\Debug\shennong_rs.exe --port 9000 --index "$env:USERPROFILE\moon.idx"
cd .\Build\x64\Debug\moon_wasm; python3 serve.py
```

Standalone Cargo commands default to `Build/cargo` via `.cargo/config.toml`,
without incorrectly assuming x64 or Debug. CMake overrides that default with
the selected build tree's architecture/configuration-specific Rust directory.
`Build/`, `build/`, and root-level `Build-*`/`build-*` trees are ignored by Git.

Architecture-selection regression checks can be run with
`cmake -P Test/CMake/Architecture.cmake` from the repository root.

C:\gitroot\MoonShot\ThirdParty>git submodule add https://github.com/microsoft/mimalloc.git


C:\gitroot\MoonShot\ThirdParty>git status
On branch main
Your branch is up to date with 'origin/main'.

Changes to be committed:
  (use "git restore --staged <file>..." to unstage)
        new file:   ../.gitmodules
        new file:   mimalloc



C:\gitroot\MoonShot>more .gitmodules
[submodule "ThirdParty/mimalloc"]
        path = ThirdParty/mimalloc
        url = https://github.com/microsoft/mimalloc.git


C:\gitroot\MoonShot>git status
On branch main
Your branch is up to date with 'origin/main'.

Changes to be committed:
  (use "git restore --staged <file>..." to unstage)
        new file:   .gitmodules
        new file:   ThirdParty/mimalloc


C:\gitroot\MoonShot>git diff --cached

C:\gitroot\MoonShot\ThirdParty>git commit -am "add submodule"



C:\gitroot\MoonShot>git push origin main
Enumerating objects: 5, done.
Counting objects: 100% (5/5), done.
Delta compression using up to 8 threads
Compressing objects: 100% (3/3), done.
Writing objects: 100% (4/4), 443 bytes | 221.00 KiB/s, done.
Total 4 (delta 1), reused 0 (delta 0), pack-reused 0
remote: Resolving deltas: 100% (1/1), completed with 1 local object.
To https://github.com/honkliu/MoonShot.git
   782735c..c5cee65  main -> main

C:\gitroot\MoonShot>git branch
* main

C:\gitroot\test\MoonShot\ThirdParty\mimalloc>git submodule init
Submodule 'ThirdParty/mimalloc' (https://github.com/microsoft/mimalloc.git) registered for path './'

C:\gitroot\test\MoonShot\ThirdParty\mimalloc>dir
 Volume in drive C is OSDisk
 Volume Serial Number is CE63-1B08

 Directory of C:\gitroot\test\MoonShot\ThirdParty\mimalloc

02/18/2021  11:20 PM    <DIR>          .
02/18/2021  11:20 PM    <DIR>          ..
               0 File(s)              0 bytes
               2 Dir(s)  272,321,204,224 bytes free

C:\gitroot\test\MoonShot\ThirdParty\mimalloc>git submodule update
Cloning into 'C:/gitroot/test/MoonShot/ThirdParty/mimalloc'...
Submodule path './': checked out '15220c684331d1c486550d7a6b1736e0a1773816'


# Install boost library

# sudo apt-get install libboost-dev
# Examples
#include <iostream>
#include<boost/version.hpp>
#include<boost/config.hpp>

using namespace std;

int main() {
    cout << BOOST_VERSION << endl;
    cout << BOOST_LIB_VERSION << endl;
    cout << BOOST_PLATFORM << endl;
    cout << BOOST_COMPILER << endl;
    cout << BOOST_STDLIB << endl;

  return 0;
}

#GRPC

 $ sudo apt-get install build-essential autoconf libtool pkg-config
  $ [sudo] apt-get install clang-5.0 libc++-dev

 $ git clone -b RELEASE_TAG_HERE https://github.com/grpc/grpc
 $ cd grpc
 $ git submodule update --init

 $ mkdir -p cmake/build
 $ cd cmake/build
 $ cmake ../..
 $ make

--BOOST

#wget https://dl.bintray.com/boostorg/release/1.75.0/source/boost_1_75_0.tar.gz
#cd thirdparty
# tar -xzf boost_1_75_0.tar.gz
 
# on windows: Install ICU



$ Install G++ (MSYS2, Cmake)
Find prebuilt MinGW ICU binaries

Some third-party package managers (like MSYS2) provide MinGW builds:
Open MSYS2 MinGW64 shell.
Run: pacman -S mingw-w64-x86_64-icu
The libraries will be installed in /mingw64/lib (e.g., libicuin.a, libicuuc.a, libicudt.a).

$ set ICU_ROOT=C:\msys64\mingw64

$ cmake ..\.. -G "MinGW Makefiles" 

$ mingw32-make
