# Vinyl Tracer
<img width="360" alt="vinyl-tracer-logo" src="https://github.com/user-attachments/assets/9246e534-5226-47a9-b0ea-7e0f56ad269d" />

## Screenshot
<img width="640" alt="image" src="https://github.com/user-attachments/assets/64bdb228-0261-4450-9cb5-51c13218a4b4" />

## Description
Ableton Live などのDAWで、Serato CV02 / CV02.5 のコントロール・ヴァイナルを使い、プラグインに読み込んだサンプルをスクラッチするための VST3 / Audio Unit（AU）プラグインです。

- **Windows**（10 / 11、64ビット）：VST3
- **macOS**（11 以降、Apple シリコン / Intel）：VST3 / AU（署名・公証済みのインストーラ）

配布版は [Releases](https://github.com/yamamo2shun1/vinyl-tracer/releases) からダウンロードできます。インストールの方法は [INSTALL_ja.md](INSTALL_ja.md) を見てください。

DAWで曲を作りながら、「あ、このフレーズ、スクラッチしたらどうなるだろう？」という思いつきを、すぐに試したい、そんな思いで開発しました。

既存のプラグインで問題になっている次の3点を、設計の段階で避けることを目標にしています。

| 問題 | 原因の推定 | この実装での対策 |
|---|---|---|
| 数分スクラッチすると位置がずれる | 速度を積算して位置を出している（誤差が溜まる） | 位置は「搬送波の位相の測定値＋LFSRによる絶対位置」から直接求める。積算しないので原理的にずれが溜まらない。サイクルスリップはビット列で検出して自動補正 |
| 低速でレコードが動いているのに音が止まる | ゼロクロスの間隔から速度を求めるため、低速ではゼロクロスがほとんど来ない | L/Rの直交信号の位相（atan2）を毎サンプル測り、3次PLLで追従する。ゼロクロスを待たずに位置が動く |
| CPU負荷が高い | （M4L由来の可能性） | C++で実装。1デッキ・48kHzで解析0.3％＋再生0.2％（1コア比、テスト環境） |

## フォルダ構成

```
core/      解析と再生の本体（JUCEに依存しない C++17）
  TimecodeDef.*      Serato CV02 の定義とLFSRの位置テーブル
  TimecodeDecoder.*  タイムコード解析
  ScratchPlayer.*    位置に従ってサンプルを再生（補間＋エイリアス対策）
plugin/    JUCE のプラグイン層（Processor / Editor）
tests/     合成タイムコードによる精度テスト
tools/     tc_analyze：実機で録音したWAVを解析するコマンド
           macos_sign_notarize.sh / macos_package.sh：macOS 版の署名・公証と .pkg の作成
docs/      テスト結果
```

## 解析の仕組み

1. **DC除去とL/R補正**：L/Rのゲイン差と位相のずれ（直交誤差）を常に推定して補正します。推定は搬送波が十分速いとき（0.2x以上）だけ更新します。
2. **二倍角の位相追従**：`atan2(2LR, R²−L²)`（2θ）を3次PLLで追います。
   - 全体のレベル変化に影響されません。カートリッジの出力は速度が下がると小さくなります。
   - 逆回転時の極性反転にも影響されません。磁気カートリッジは速度に比例した電圧を出すので、逆回転すると信号が反転します。
3. **LFSRで絶対位置**：ビット読み取りは、チャンネル順・極性・回転方向の8通りの組み合わせを並行して試し、自動で判定します。L/Rの配線が逆でも動きます。
   - 24ビット連続で一致したら、絶対位置を「追従位相＋オフセット」として確定します。
   - 補正に使うのは、順方向かつ0.6〜2.0xのときの読み取りだけです。この範囲ではRIAAの位相特性がほぼ平らです。それより低い50〜500Hz帯ではプリアンプが積分器のように働き、読み取り位置が半サイクルずれるためです。
4. **補正**：小さなずれ（〜4サイクル）は25サイクル/秒でゆっくり戻します。ピッチ変化は2.5％以内で、クリックは出ません。大きなずれ（針の載せ替え）は即座にジャンプします。
5. **ゲート**：ユーザー設定のしきい値と、ノイズフロア＋8dBのうち高い方を使います。それより小さい信号のときは追従を止め、ゆっくり停止させます。

## 合成信号でのテスト結果

テスト信号は実機を模して作っています。カートリッジ（速度比例・逆回転で反転）、RIAAのデエンファシス、L/Rゲイン差0.85、直交誤差8°、DCオフセット、ハム、ノイズ（−60dB）を入れ、48kHz・128サンプルブロックで実行しました（`docs/results_decoder.txt`）。

| シナリオ | 結果 |
|---|---|
| 1x再生 20秒 | ロック61ms、ばらつき p99 0.025ms、ドリフト 0.000ms |
| 低速（0.2x→0.01x、往復） | 最大誤差 1.0ms、追従停止 0％ |
| ランダムなスクラッチ 5分（最大4x、加速度大） | p99 0.79ms、最大 2.0ms、**5分後のドリフト −0.17ms** |
| 同 5分（最大8xのフリック） | p99 1.0ms、最大 2.4ms、ドリフト −0.05ms |
| 針の載せ替え（0.3秒浮かせて別の位置へ） | 63msで新しい位置にロック |
| L/R入れ替え・極性反転（4通り） | すべて自動判定してロック |
| 44.1kHz / 96kHz / 1024サンプルブロック | 同等の精度 |
| ノイズ・ハム −40dB（劣悪な環境） | p99 7.8ms。実機ではThresholdを上げて対処 |

`docs/results_player.txt` は、解析から再生までを通したテストの結果です。
- 位置ジッタ（100Hz以上）は16µs RMSで、1サンプル（20.8µs）未満でした。
- 0.2ms程度の一定オフセットがあります。カートリッジとRIAAの位相によるもので、固定の遅延と同じなので聴感上の問題はありません。

**注意**：上の数値は合成信号での結果です。実機の Serato 盤（CV02 / CV02.5）でも、ロックしてスクラッチ再生できることを確認しています。お使いの環境の信号は、次の「実機WAVでの確認」の方法で確かめられます。

## ビルド

必要なもの：CMake 3.22以上と、C++17のコンパイラ（macOSはXcode、WindowsはVisual Studio 2022）。

```bash
cd vinyl-tracer
git clone --depth 1 https://github.com/juce-framework/JUCE.git   # 置いておけばFetchContentを使いません
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

- VST3は `build/VinylTracer_artefacts/Release/VST3/Vinyl Tracer.vst3` にできます。
  - macOSは `~/Library/Audio/Plug-Ins/VST3/` へコピーします。
  - Windowsは `C:\Program Files\Common Files\VST3\` へコピーします。
- macOSではAU（`.component`）もできます。
  - 配布用のユニバーサルバイナリ（Apple シリコン＋Intel）は、`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0` を付けて `build-mac` にビルドします。
  - 署名・公証は `tools/macos_sign_notarize.sh`、インストーラ（.pkg）の作成は `tools/macos_package.sh` で行います。Apple Developer Program の Developer ID 証明書が必要です（使い方は各スクリプトの先頭を参照）。
- テストとツールだけをビルドする場合は `-DVT_BUILD_PLUGIN=OFF` を付けます（JUCE不要）。

## Ableton Live での使い方

1. オーディオトラックを作り、入力をターンテーブルがつながっているオーディオIFのチャンネルにします（例：JUMBLEQのUSB入力 1/2）。モニターは **In** にします。
2. トラックに「Vinyl Tracer」を挿します。
3. `Load Sample...` を押すか、ドラッグ＆ドロップでサンプルを読み込みます。
4. レコードを再生します。スコープに円が出て、`SIGNAL ok / CODE ok / ABS ok` になれば準備完了です。
5. 何もしなければ、最初にロックした位置がサンプルの頭になります。好きな位置から始めたいときは、Cue の 1〜6 のボタンを使います。
   - Ctrl＋クリック（macOS は Cmd＋クリック）：今の再生位置をキューとして登録（上書き）します。
   - クリック：そのキューの位置から再生します（空のボタンでは何も起きません）。
   - Alt＋クリック（macOS は Option＋クリック）：キューを削除します。
6. Liveのバッファサイズは64〜128サンプルにしてください。遅延はほぼバッファサイズで決まります。

各パラメータの意味は次のとおりです。

| パラメータ | 内容 |
|---|---|
| Timecode | Side A / Side B |
| Mode | Absolute（針位置＝サンプル位置。針を載せ替えると追従）/ Relative（動きだけを使い、針の載せ替えは無視） |
| RPM | 33 1/3 / 45 |
| Threshold | ゲートのしきい値（公称レベル比、dB）。ハムやノイズが多いときは上げる |
| Output | 出力ゲイン |
| Timecode Thru | 入力のタイムコードをそのまま出力する（配線確認用） |
| Loop | サンプルをループさせる |
| Wave view | 波形のスクロールビューの表示幅 |
| Cue | キューポイントの登録・呼び出し・削除（6個） |

## 実機WAVでの確認（おすすめ）

プラグインを試す前に、実機の信号でも解析が動くかを確かめられます。

1. ターンテーブルのタイムコードを録音します。48kHzのステレオWAVで、エフェクトなしにします。次の4種類を録っておくと便利です。
   - 通常の再生（30秒）
   - 低速で前後に動かす
   - 激しいスクラッチ
   - 針の載せ替え
2. `tc_analyze` で解析します。

```bash
./build/tc_analyze rec_play.wav --side A --csv play.csv
```

ロックするまでの時間、ロックしている割合、補正の回数、検出した配線の設定が表示されます。CSVには1msごとのレコード位置・速度・ロック状態が出るので、グラフにすれば位置の飛びや低速での追従を確認できます。

## 未対応・次の作業

- Serato CV02 以外のレコード（Traktor MK1/MK2、djay Control Vinyl）の絶対位置。Relativeモードは1kHzの直交トーンなら原理的に動きますが、回転方向の自動判定にはLFSRが必要です。
- タイムコードCD、45回転の実機確認
- 高速時の音質の詰め。現在は半分のレートに落としたコピーを速度に応じてクロスフェードしてエイリアスを抑えています。
- 長いファイルの読み込みを別スレッドにすること
- 小さなMax for Liveデバイスとの連携（Liveのクリップの音源を扱う）

## ライセンス

Vinyl Tracer は [GNU Affero General Public License v3.0](LICENSE.md)（またはそれ以降のバージョン、`AGPL-3.0-or-later`）で公開しています。`core/`・`plugin/`・`tests/`・`tools/` のすべてが対象です。

- `core/` はサードパーティのコードを含みません。
- プラグインは [JUCE](https://juce.com/) を AGPLv3 の条件で使っています。
- VST3 SDK（JUCE に同梱）は MIT ライセンスです。
- プラグインに埋め込んでいるフォント（`fonts/`）は、どちらも SIL Open Font License 1.1 です。
  - [Space Grotesk](https://github.com/floriankarsten/space-grotesk)（プラグイン名）：[fonts/OFL-SpaceGrotesk.txt](fonts/OFL-SpaceGrotesk.txt)
  - [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono)（そのほかの文字）：[fonts/OFL-JetBrainsMono.txt](fonts/OFL-JetBrainsMono.txt)

Serato、CV02 はSerato社の商標です。Vinyl Tracer は同社とは関係がなく、対応するタイムコード盤を示すためだけに名前を使っています。
