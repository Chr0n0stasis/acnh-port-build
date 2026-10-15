#!/bin/bash
set -euo pipefail
ROOT=$PWD
CHAIN=$(cat output/chain.txt)
export DM_BUNDLE_ID=$(python3 source/dm/games/game.py acnh bundle_id)
export DM_DISPLAY_NAME='Animal Crossing' DM_APP_CATEGORY=public.app-category.games DM_TEAM_ID=0000000000
cd source/dm/ios
mkdir -p Gen/dm Gen/msl Gen/Assets.xcassets
python3 tools/gen_build.py ../host Gen > Gen/dm.xcconfig
LINK_FLAGS=$(python3 tools/gen_embed.py "$ROOT/work/patched" Gen/embed.S main subsdk0 sdk)
printf 'OTHER_LDFLAGS = $(inherited) $(DM_FRAMEWORKS) %s\n' "$LINK_FLAGS" >> Gen/dm.xcconfig
cp "$ROOT"/work/patched/*.tsv Gen/dm/
cp ../games/acnh/{shader_map.tsv,pso_manifest.tsv,c1_full.tsv} Gen/dm/
cp -R "$ROOT/output/msl-ios/b-$CHAIN" Gen/msl/
mkdir -p Gen/dm/font
cp '/System/Library/Fonts/Supplemental/Arial Unicode.ttf' Gen/dm/font/standard.ttf
python3 ../tools/make_ext_font.py Gen/dm/font/nintendo_ext.ttf
printf '{"info":{"author":"xcode","version":1}}\n' > Gen/Assets.xcassets/Contents.json
# 保留用户原图背景，生成无透明通道的满版图标；圆角由系统遮罩生成。
# Preserve the user's original opaque background; the system supplies the corner mask.
ICON=../games/acnh/app-icon.jpg
test -f "$ICON"
AI=Gen/Assets.xcassets/AppIcon.appiconset
mkdir -p "$AI"
sips -s format png -z 1024 1024 "$ICON" --out "$AI/icon.png" > /dev/null
printf '{"images":[{"filename":"icon.png","idiom":"universal","platform":"ios","size":"1024x1024"}],"info":{"author":"xcode","version":1}}\n' > "$AI/Contents.json"
printf 'DM_APPICON = AppIcon\n' >> Gen/dm.xcconfig
python3 -c 'from PIL import Image; i=Image.open("Gen/Assets.xcassets/AppIcon.appiconset/icon.png"); assert i.size == (1024,1024) and i.mode == "RGB", (i.size,i.mode)'
xcodegen generate -q
xcodebuild -project DmIOS.xcodeproj -scheme DmIOS -configuration Release -destination 'generic/platform=iOS' -derivedDataPath "$ROOT/work/dd" CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= build > "$ROOT/output/ios-build.log" 2>&1
mkdir -p "$ROOT/output/Payload"
cp -R "$ROOT/work/dd/Build/Products/Release-iphoneos/DmIOS.app" "$ROOT/output/Payload/"
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$ROOT/output/Payload/DmIOS.app/Info.plist")" = "$DM_BUNDLE_ID"
python3 -c 'import plistlib,sys; from pathlib import Path; p=Path(sys.argv[1]); i=plistlib.loads((p/"Info.plist").read_bytes()); assert i["CFBundleIcons"]["CFBundlePrimaryIcon"]["CFBundleIconName"] == "AppIcon"; assert (p/"Assets.car").is_file()' "$ROOT/output/Payload/DmIOS.app"
cd "$ROOT/output"
zip -qry ACNHMobile-bringup-unsigned.ipa Payload
printf 'Experimental bring-up IPA, not a complete or verified playable port. Geometry lowering programs are compiled, but renderer integration remains incomplete; see BRINGUP-LIMITATIONS.json. Complete RomFS belongs in Documents/romfs. Returned device logs establish only the startup stages they reach; gameplay still needs device verification.\n' > IPA-STATUS.txt
