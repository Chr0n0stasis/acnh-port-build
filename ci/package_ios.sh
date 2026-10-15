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
xcodegen generate -q
xcodebuild -project DmIOS.xcodeproj -scheme DmIOS -configuration Release -destination 'generic/platform=iOS' -derivedDataPath "$ROOT/work/dd" CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= build > "$ROOT/output/ios-build.log" 2>&1
mkdir -p "$ROOT/output/Payload"
cp -R "$ROOT/work/dd/Build/Products/Release-iphoneos/DmIOS.app" "$ROOT/output/Payload/"
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$ROOT/output/Payload/DmIOS.app/Info.plist")" = "$DM_BUNDLE_ID"
cd "$ROOT/output"
zip -qry ACNHMobile-bringup-unsigned.ipa Payload
printf 'Experimental bring-up IPA, not a complete or verified playable port. Eight geometry shaders remain unsupported; see BRINGUP-LIMITATIONS.json. Complete RomFS must be placed in Documents/romfs. No device run has been verified.\n' > IPA-STATUS.txt
