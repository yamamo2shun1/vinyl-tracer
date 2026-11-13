#!/bin/zsh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Shunichi Yamamoto
#
# 公証済みの VST3 / AU から配布用のインストーラ（.pkg）を作り、署名・公証・staple する。
#
#   tools/macos_package.sh
#
# 先に build-mac で VinylTracer_VST3 / VinylTracer_AU をビルドし、
# tools/macos_sign_notarize.sh でバンドルを署名・公証しておくこと。
# 出力: build-mac/dist/VinylTracer-<版>-macOS.pkg
#
# 環境変数
#   VT_INSTALLER_ID    インストーラの署名 ID（省略時はキーチェーンの最初の "Developer ID Installer"）
#   VT_NOTARY_PROFILE  notarytool のキーチェーンプロファイル名（既定 vinyl-tracer）
#   VT_SKIP_SIGN=1     署名・公証をしない（中身の確認用）

set -euo pipefail

root=${0:A:h:h}
art="$root/build-mac/VinylTracer_artefacts/Release"
version=$(sed -n 's/^project(VinylTracer VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
profile=${VT_NOTARY_PROFILE:-vinyl-tracer}
dist="$root/build-mac/dist"
out="$dist/VinylTracer-${version}-macOS.pkg"
idbase=com.yamamotoworks.vinyltracer

vst3="$art/VST3/Vinyl Tracer.vst3"
au="$art/AU/Vinyl Tracer.component"
for b in "$vst3" "$au"; do
    [[ -d $b ]] || { echo "見つかりません: $b" >&2; exit 1; }
    if [[ ${VT_SKIP_SIGN:-0} != 1 ]]; then
        xcrun stapler validate -q "$b" || { echo "公証されていません: $b" >&2; exit 1; }
    fi
done

sign_args=()
if [[ ${VT_SKIP_SIGN:-0} != 1 ]]; then
    inst_id=${VT_INSTALLER_ID:-}
    if [[ -z $inst_id ]]; then
        inst_id=$(security find-identity -v \
                  | sed -n 's/.*"\(Developer ID Installer:.*\)"/\1/p' | head -n 1)
    fi
    [[ -n $inst_id ]] || { echo "有効な Developer ID Installer 証明書がキーチェーンにありません。" >&2; exit 1; }
    echo "署名 ID: $inst_id"
    sign_args=(--sign "$inst_id" --timestamp)
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/vst3" "$work/au" "$work/scripts-au" "$work/res" "$work/pkgs" "$dist"

# --- コンポーネントパッケージ ------------------------------------------------
# BundleIsRelocatable=false：同じバンドル ID のものが別の場所（ビルドフォルダ等）にあっても、
# そちらを上書きせず、必ず /Library/Audio/Plug-Ins に入れる。
cat > "$work/component.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<array>
	<dict>
		<key>BundleHasStrictIdentifier</key><true/>
		<key>BundleIsRelocatable</key><false/>
		<key>BundleIsVersionChecked</key><false/>
		<key>BundleOverwriteAction</key><string>upgrade</string>
		<key>RootRelativeBundlePath</key><string>BUNDLE</string>
	</dict>
</array>
</plist>
EOF

ditto "$vst3" "$work/vst3/${vst3:t}"
ditto "$au"   "$work/au/${au:t}"
sed "s|BUNDLE|${vst3:t}|" "$work/component.plist" > "$work/vst3.plist"
sed "s|BUNDLE|${au:t}|"   "$work/component.plist" > "$work/au.plist"

# AU を入れた後、ログインし直さなくても DAW から見えるように登録キャッシュを更新する
cat > "$work/scripts-au/postinstall" <<'EOF'
#!/bin/sh
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
exit 0
EOF
chmod +x "$work/scripts-au/postinstall"

pkgbuild --quiet --root "$work/vst3" --component-plist "$work/vst3.plist" \
    --identifier "$idbase.vst3" --version "$version" \
    --install-location "/Library/Audio/Plug-Ins/VST3" "$work/pkgs/vst3.pkg"
pkgbuild --quiet --root "$work/au" --component-plist "$work/au.plist" \
    --scripts "$work/scripts-au" \
    --identifier "$idbase.au" --version "$version" \
    --install-location "/Library/Audio/Plug-Ins/Components" "$work/pkgs/au.pkg"

# --- インストーラの画面 ------------------------------------------------------
cat > "$work/res/welcome.html" <<EOF
<!doctype html>
<html><head><meta charset="utf-8">
<style>body{font-family:-apple-system,sans-serif;font-size:13px;line-height:1.5}code{font-size:12px}</style>
</head><body>
<p><b>Vinyl Tracer ${version}</b> をインストールします。</p>
<p>Serato CV02 / CV02.5 のタイムコードレコードで、読み込んだサンプルをスクラッチ再生するオーディオプラグインです。</p>
<p>インストール先：<br>
VST3 … <code>/Library/Audio/Plug-Ins/VST3/</code><br>
Audio Unit … <code>/Library/Audio/Plug-Ins/Components/</code></p>
<p>「カスタマイズ」で VST3 と Audio Unit のどちらか一方だけを選ぶこともできます。インストールが終わったら、DAW でプラグインを再スキャンしてください。</p>
<hr>
<p>This installs the <b>Vinyl Tracer ${version}</b> VST3 and Audio Unit plug-ins into
<code>/Library/Audio/Plug-Ins/</code>. Rescan plug-ins in your DAW afterwards.</p>
<p>Source code: https://github.com/yamamo2shun1/vinyl-tracer</p>
</body></html>
EOF

# ライセンス：本体（AGPL）と、埋め込んでいるフォント（OFL）
{
    cat "$root/LICENSE.md"
    for f in "$root"/fonts/OFL-*.txt; do
        printf '\n\n==============================================================================\n'
        printf 'Embedded font license: %s\n' "${f:t}"
        printf '==============================================================================\n\n'
        cat "$f"
    done
} > "$work/res/license.txt"

cat > "$work/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Vinyl Tracer ${version}</title>
    <organization>com.yamamotoworks</organization>
    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_localSystem="true" enable_anywhere="false" enable_currentUserHome="false"/>
    <volume-check><allowed-os-versions><os-version min="11.0"/></allowed-os-versions></volume-check>
    <welcome file="welcome.html" mime-type="text/html"/>
    <license file="license.txt" mime-type="text/plain"/>
    <choices-outline>
        <line choice="vst3"/>
        <line choice="au"/>
    </choices-outline>
    <choice id="vst3" title="VST3" description="/Library/Audio/Plug-Ins/VST3/Vinyl Tracer.vst3">
        <pkg-ref id="$idbase.vst3"/>
    </choice>
    <choice id="au" title="Audio Unit" description="/Library/Audio/Plug-Ins/Components/Vinyl Tracer.component">
        <pkg-ref id="$idbase.au"/>
    </choice>
    <pkg-ref id="$idbase.vst3" version="$version" onConclusion="none">vst3.pkg</pkg-ref>
    <pkg-ref id="$idbase.au" version="$version" onConclusion="none">au.pkg</pkg-ref>
</installer-gui-script>
EOF

rm -f "$out"
productbuild --quiet --distribution "$work/distribution.xml" \
    --resources "$work/res" --package-path "$work/pkgs" \
    "${sign_args[@]}" "$out"

if [[ ${VT_SKIP_SIGN:-0} == 1 ]]; then
    echo "$out（署名なし）"
    exit 0
fi

pkgutil --check-signature "$out"

# --- 公証・staple -----------------------------------------------------------
xcrun notarytool submit "$out" --keychain-profile "$profile" --wait \
    --output-format json | tee "$work/result.json"
echo
if ! grep -q '"status" *: *"Accepted"' "$work/result.json"; then
    id=$(sed -n 's/.*"id" *: *"\([^"]*\)".*/\1/p' "$work/result.json" | head -n 1)
    echo "公証に失敗しました。ログ:" >&2
    [[ -n $id ]] && xcrun notarytool log "$id" --keychain-profile "$profile" >&2
    exit 1
fi
xcrun stapler staple "$out"
xcrun stapler validate "$out"
spctl --assess --type install --verbose=2 "$out"

shasum -a 256 "$out"
