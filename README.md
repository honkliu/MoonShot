# MoonShot

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
