# Vinyl Tracer のインストール

- [Windows（VST3）](#windows)
- [macOS（VST3 / AU）](#macos)

## 必要なもの

- Windows 10 / 11（64ビット）、または macOS 11 以降（Apple シリコン / Intel）
- VST3 または Audio Unit に対応した DAW（動作確認は Windows 11 ＋ Ableton Live 12、macOS 26 ＋ Ableton Live 12）
- Serato CV02 / CV02.5 のコントロール・ヴァイナルと、ターンテーブル
- ターンテーブルの信号をステレオでパソコンに入力できるオーディオインターフェイス

## Windows

### インストール

1. ZIP を展開します。
2. 中にある `Vinyl Tracer.vst3` フォルダを、**フォルダごと**次の場所へコピーします（管理者の許可を求められたら許可してください）。

   ```
   C:\Program Files\Common Files\VST3\
   ```

3. DAW を起動し、プラグインを再スキャンします。
   - Ableton Live：`Options` → `Settings`（Live 11 以前は `Preferences`）→ `Plug-Ins` で `Use VST3 Plug-In System Folders` をオンにし、`Rescan` を押します。
4. プラグインの一覧に `Yamamoto Works` → `Vinyl Tracer` が出れば完了です。

### アップデート

`C:\Program Files\Common Files\VST3\` にある古い `Vinyl Tracer.vst3` を削除してから、新しいものをコピーしてください。保存した Live セットの設定はそのまま引き継がれます。

### アンインストール

`C:\Program Files\Common Files\VST3\Vinyl Tracer.vst3` フォルダを削除します。

## macOS

インストーラ（`VinylTracer-<バージョン>-macOS.pkg`）で、VST3 と Audio Unit（AU）を入れます。インストーラもプラグインも Apple の公証済みです。

### インストール

1. `VinylTracer-<バージョン>-macOS.pkg` をダブルクリックし、画面の指示に従います（最後に管理者のパスワードを求められます）。
   - 次の場所に入ります。VST3 と AU のどちらか一方だけにしたい場合は、途中の `カスタマイズ` で選べます。

   | 形式 | インストール先 |
   | --- | --- |
   | VST3 | `/Library/Audio/Plug-Ins/VST3/Vinyl Tracer.vst3` |
   | AU | `/Library/Audio/Plug-Ins/Components/Vinyl Tracer.component` |

2. DAW を起動し、プラグインを再スキャンします。
   - Ableton Live：`Live` → `Settings` → `Plug-Ins` で、VST3 を使う場合は `Use VST3 Plug-In System Folders`、AU を使う場合は `Use Audio Units` をオンにし、`Rescan` を押します。
3. プラグインの一覧に `Yamamoto Works` → `Vinyl Tracer` が出れば完了です。

うまくいかないとき：

- **入力に信号が来ない**：`システム設定` → `プライバシーとセキュリティ` → `マイク` で、DAW（Ableton Live など）を許可してください。
- **AU が一覧に出ない**：一度ログアウトしてログインし直すか、Mac を再起動してから再スキャンしてください。
- **Vinyl Tracer が2つ出る**：以前に手で `~/Library/Audio/Plug-Ins/`（ホームフォルダのライブラリ）へコピーしたものが残っています。そちらを削除してください。

### アップデート

新しいバージョンのインストーラを実行すると、上書きされます。保存した Live セットの設定はそのまま引き継がれます。

### アンインストール

Finder で `移動` → `フォルダへ移動…`（⇧⌘G）を選び、次の2つを削除します。

- `/Library/Audio/Plug-Ins/VST3/Vinyl Tracer.vst3`
- `/Library/Audio/Plug-Ins/Components/Vinyl Tracer.component`

## 最初の設定（Ableton Live）

1. オーディオトラックを作り、入力をターンテーブルがつながっているチャンネル（ステレオ）にします。モニターは **In** にします。
2. トラックに `Vinyl Tracer` を挿します。
3. `Load Sample...` を押すか、オーディオファイルをウィンドウにドラッグ＆ドロップしてサンプルを読み込みます。
4. `Timecode` を使うレコードの面（Side A / Side B）に合わせます。
5. レコードを再生します。左のスコープに円が出て、下の表示が `SIGNAL ok / CODE ok / ABS ok` になれば準備完了です。
   - 入力レベルのメーターが緑の範囲に入るように、オーディオインターフェイスの入力ゲインを調整してください。
6. Live のバッファサイズは 64〜128 サンプルをおすすめします。遅延はほぼバッファサイズで決まります。

各パラメータとキューの使い方は [README.md](README.md) の「Ableton Live での使い方」を見てください。

## ライセンス

Vinyl Tracer は GNU Affero General Public License v3.0 またはそれ以降のバージョン（`AGPL-3.0-or-later`）で配布しています。本文は [LICENSE.md](LICENSE.md)、埋め込んでいるフォントのライセンスは `fonts/` フォルダにあります。ソースコードは https://github.com/yamamo2shun1/vinyl-tracer で公開しています。
