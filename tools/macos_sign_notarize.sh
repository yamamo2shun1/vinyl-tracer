#!/bin/zsh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Shunichi Yamamoto
#
# macOS 用プラグインバンドルを Developer ID で署名し、公証して staple する。
#
#   tools/macos_sign_notarize.sh [バンドル...]
#
# バンドルを省略すると build-mac の VST3 と AU（あれば）を対象にする。
#
# 環境変数
#   VT_SIGN_ID         署名 ID（省略時はキーチェーンの最初の有効な "Developer ID Application"）
#   VT_NOTARY_PROFILE  notarytool のキーチェーンプロファイル名（既定 vinyl-tracer）
#                      事前に一度だけ:
#                        xcrun notarytool store-credentials vinyl-tracer \
#                          --apple-id <Apple ID> --team-id <チームID>
#                      （App 用パスワードを対話で入力）
#   VT_SKIP_NOTARIZE=1 署名だけ行う

set -euo pipefail

root=${0:A:h:h}
profile=${VT_NOTARY_PROFILE:-vinyl-tracer}

if (( $# == 0 )); then
    art="$root/build-mac/VinylTracer_artefacts/Release"
    set -- "$art/VST3/Vinyl Tracer.vst3"
    [[ -d "$art/AU/Vinyl Tracer.component" ]] && set -- "$@" "$art/AU/Vinyl Tracer.component"
fi

sign_id=${VT_SIGN_ID:-}
if [[ -z $sign_id ]]; then
    sign_id=$(security find-identity -v -p codesigning \
              | sed -n 's/.*"\(Developer ID Application:.*\)"/\1/p' | head -n 1)
fi
if [[ -z $sign_id ]]; then
    echo "有効な Developer ID Application 証明書がキーチェーンにありません。" >&2
    exit 1
fi
echo "署名 ID: $sign_id"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

for bundle in "$@"; do
    [[ -d $bundle ]] || { echo "見つかりません: $bundle" >&2; exit 1; }
    name=${bundle:t}
    echo "== $name"

    # Hardened Runtime＋タイムスタンプ（公証の条件）。中身は実行ファイル1つなので --deep は不要。
    codesign --force --options runtime --timestamp --sign "$sign_id" "$bundle"
    codesign --verify --strict --verbose=2 "$bundle"

    [[ ${VT_SKIP_NOTARIZE:-0} == 1 ]] && continue

    zip="$work/${name}.zip"
    ditto -c -k --keepParent "$bundle" "$zip"
    xcrun notarytool submit "$zip" --keychain-profile "$profile" --wait \
        --output-format json | tee "$work/result.json"
    if ! grep -q '"status" *: *"Accepted"' "$work/result.json"; then
        id=$(sed -n 's/.*"id" *: *"\([^"]*\)".*/\1/p' "$work/result.json" | head -n 1)
        echo "公証に失敗しました。ログ:" >&2
        [[ -n $id ]] && xcrun notarytool log "$id" --keychain-profile "$profile" >&2
        exit 1
    fi

    xcrun stapler staple "$bundle"
    xcrun stapler validate "$bundle"
    spctl --assess --type install --verbose=2 "$bundle" || true
done

echo "完了"
