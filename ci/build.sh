#!/bin/bash
set -euo pipefail
ROOT=$PWD
PHASE=${1:-all}
mkdir -p output
exec > >(tee "output/build-$PHASE.log") 2>&1

activate_tools() {
  source "$ROOT/work/venv/bin/activate"
  export LLVM_BIN="$(brew --prefix llvm)/bin"
  export PATH="$ROOT/work/toolbin:$LLVM_BIN:$PATH"
  export XLATE="$ROOT/work/translator/dmsh_xlate"
  export MAIN="$ROOT/work/ExeFS/main"
  export DM_HLE_HEADER="$ROOT/source/dm/host/hle.h"
  export DM_ROOT="$ROOT/work"
  CHAIN=$(cat output/chain.txt)
}

phase_prepare() {
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
  python3 -m pip install lz4 capstone fonttools numpy Pillow zstandard

  if [ ! -x work/toolbin/spirv-cross ]; then
    git clone --depth 1 --branch vulkan-sdk-1.4.357.0 https://github.com/KhronosGroup/SPIRV-Cross.git work/spirv-cross
    cmake -S work/spirv-cross -B work/spirv-cross/build -DCMAKE_BUILD_TYPE=Release
    cmake --build work/spirv-cross/build --parallel 3
    mkdir -p work/toolbin
    cp work/spirv-cross/build/spirv-cross work/toolbin/
  fi
  export PATH="$ROOT/work/toolbin:$(brew --prefix llvm)/bin:$PATH"
  export LLVM_BIN="$(brew --prefix llvm)/bin"

  dotnet publish source/dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj -c Release -o "$ROOT/work/translator" > output/translator.log 2>&1
  export XLATE="$ROOT/work/translator/dmsh_xlate"
  export MAIN="$ROOT/work/ExeFS/main"
  python3 source/dm/gfx/tools/dmsh_chain.py --update-lock
  python3 source/dm/gfx/tools/dmsh_chain.py --header
  CHAIN=$(python3 source/dm/gfx/tools/dmsh_chain.py)
  printf '%s\n' "$CHAIN" > output/chain.txt

  python3 -m unittest discover -s source/dm/gfx/tests -p 'test_shader_*.py' -v > output/shader-tests.log 2>&1
}

phase_verify() {
  for module in sdk subsdk0 main; do
    python3 work/verify/structck.py "$ROOT/work/patched" "$module" "$ROOT/work/ExeFS/$module" > "output/struct-$module.log" 2>&1
  done
}

phase_host() {
  make -C source/dm/host GAME=acnh > output/host-build.log 2>&1
  cp source/dm/host/dmhost output/dmhost
  make -C source/dm/host/tests GAME=acnh build/t_fs > output/fs-build.log 2>&1
  source/dm/host/tests/build/t_fs > output/fs-test.log 2>&1

  clang -O1 -Wno-unused-function -Isource/dm/host/gen/acnh -Isource/dm/games/acnh source/dm/host/tests/t_fs_device.c -o work/t_fs_device > output/fs-device-build.log 2>&1
  work/t_fs_device > output/fs-device-test.log 2>&1
}

phase_shaders() {
  export IOSOUT="$ROOT/output/msl-ios"
  python3 source/dm/gfx/tools/dmsh_build.py "$ROOT/work/shader-pkg" "$ROOT/output/msl-mac" > output/shaders.log 2>&1
  python3 ci/check_shaders.py "output/msl-mac/b-$CHAIN" "output/msl-ios/b-$CHAIN" --bringup
  python3 source/dm/gfx/tools/c1_guard.py "output/msl-mac/b-$CHAIN" source/dm/games/acnh/c1_full.tsv

}

phase_ipa() {
  bash ci/package_ios.sh
}

case "$PHASE" in
  prepare) phase_prepare ;;
  verify|host|shaders|ipa) activate_tools; "phase_$PHASE" ;;
  all) phase_prepare; activate_tools; phase_verify; phase_host; phase_shaders; phase_ipa ;;
  *) echo "Unknown phase: $PHASE" >&2; exit 2 ;;
esac
