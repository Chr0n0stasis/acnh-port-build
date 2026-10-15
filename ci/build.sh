#!/bin/bash
set -euo pipefail
ROOT=$PWD
mkdir -p output
exec > >(tee output/build.log) 2>&1
trap 'find output -type f -name "*.tmp" -delete' EXIT

git init source
git -C source remote add origin https://github.com/seedds/smbw-mobile.git
git -C source fetch --depth 1 origin 1b70789d001e289bbf23c5f8c38f0938caf5cdef
git -C source checkout --detach FETCH_HEAD
git -C source apply "$ROOT/source.patch"
cp -R overlay/. source/

xcodebuild -version
uname -m
if ! xcrun -sdk iphoneos metal --version; then
  xcodebuild -downloadComponent MetalToolchain
fi
xcrun -sdk iphoneos metal --version
brew install xcodegen llvm
python3 -m venv "$ROOT/work/venv"
source "$ROOT/work/venv/bin/activate"
python3 -m pip install lz4 fonttools numpy Pillow zstandard

git clone --depth 1 --branch vulkan-sdk-1.4.357.0 https://github.com/KhronosGroup/SPIRV-Cross.git work/spirv-cross
cmake -S work/spirv-cross -B work/spirv-cross/build -DCMAKE_BUILD_TYPE=Release
cmake --build work/spirv-cross/build --parallel 3
export PATH="$ROOT/work/spirv-cross/build:$(brew --prefix llvm)/bin:$PATH"
export LLVM_BIN="$(brew --prefix llvm)/bin"

dotnet publish source/dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj -c Release -o "$ROOT/work/translator" > output/translator.log 2>&1
export XLATE="$ROOT/work/translator/dmsh_xlate"
export MAIN="$ROOT/work/ExeFS/main"
python3 source/dm/gfx/tools/dmsh_chain.py --update-lock
python3 source/dm/gfx/tools/dmsh_chain.py --header
CHAIN=$(python3 source/dm/gfx/tools/dmsh_chain.py)
printf '%s\n' "$CHAIN" > output/chain.txt

export DM_HLE_HEADER="$ROOT/source/dm/host/hle.h"
export DM_ROOT="$ROOT/work"
for module in sdk subsdk0 main; do
  python3 work/verify/structck.py "$ROOT/work/patched" "$module" "$ROOT/work/ExeFS/$module" > "output/struct-$module.log" 2>&1
done
make -C source/dm/host GAME=acnh > output/host-build.log 2>&1
cp source/dm/host/dmhost output/dmhost

export IOSOUT="$ROOT/output/msl-ios"
python3 source/dm/gfx/tools/dmsh_build.py "$ROOT/work/shader-pkg" "$ROOT/output/msl-mac" > output/shaders.log 2>&1
python3 ci/check_shaders.py "output/msl-mac/b-$CHAIN" "output/msl-ios/b-$CHAIN"
python3 source/dm/gfx/tools/c1_guard.py "output/msl-mac/b-$CHAIN" source/dm/games/acnh/c1_full.tsv

bash ci/package_ios.sh
