# CLAUDE.md — Vinyl Tracer

回答・コメントのやりとりは日本語で行うこと。

## これは何か
Ableton Live 等で使う VST3 / AU プラグイン。Serato CV02 / CV02.5 のタイムコードレコードで、プラグインに読み込んだサンプルをスクラッチ再生する。
目標は既存品（SCRATCHabl、Ms. Pinky）の3つの欠点を解消すること：
1. 長時間スクラッチでサンプル位置がずれる
2. 低速でレコードが動いているのに音が止まる
3. CPU負荷が高い

将来：小さな Max for Live デバイスと組み合わせて Live のクリップ音源を操作する（精度が確認できてから）。

## 構成
- `core/` JUCE非依存 C++17（名前空間 `vt`、ライブラリ `vt_core`）
  - `TimecodeDef.*` Serato CV02 定義（20bit LFSR, seed/taps）と状態→位置LUT
  - `TimecodeDecoder.*` 解析本体
  - `ScratchPlayer.*` 位置駆動の再生（Hermite補間＋半レートのミップでエイリアス対策、出力DCブロッカー）
- `plugin/` JUCE層（`VinylTracerProcessor` / `VinylTracerEditor`）
- `tests/` 合成タイムコード（カートリッジ速度比例＋逆回転で極性反転、RIAAデエンファシス、L/R不均衡、直交誤差、DC、ハム、ノイズ）による精度テスト
- `tools/tc_analyze.cpp` 実機録音WAVを解析しCSV出力

## 解析の設計判断（変更時は理由を踏まえること）
- 位置 = 搬送波の位相（追従）＋ LFSR由来の絶対オフセット。速度の積算はしない → ドリフトしない。
- 位相は二倍角 `atan2(2LR, R²−L²)` を3次PLL（位置・速度・加速度、極 -ω の三重根）で追う。二倍角なのはカートリッジが逆回転で極性反転するため。帯域 = 300 Hz + 300 Hz×|速度|（上限2.5 kHz）。
- DC除去は 0.3 Hz を常時、0.2x 以上では 2 Hz。速度で凍結すると過渡で誤ったDCを掴む／通常のHPFは低速搬送波の位相を最大90°・方向符号付きで動かし逆転時に半周期飛ぶ、の両方を避けるため。
- L/Rゲイン・直交補正の統計は 0.2x 以上のときだけ更新（低速の部分周期で偏る）。
- ビット読み取りは (主チャンネル L/R) × (極性) × (方向) の8仮説を並行。24ビット連続一致でロック。選択はスティッキー（両極性のピークが同じビットを持つ場合に交互選択で0.5周期の誤補正が起きたため）。
- 絶対位置のアンカー／補正は **順方向かつ 0.6〜2.0x** の読み取りのみ。RIAAの平坦域外（50–500 Hz）ではプリアンプが積分的に働き読み取りが隣の半周期にずれる、逆回転は速度域で極性が変わる、ため。
- 補正：差が0.25周期超を3ビット連続で検出 → 4周期以下は 25 cycles/s でスルー（ピッチ2.5%以内）、それ以上（針の載せ替え）は即ジャンプ。ゲートが30 ms以上閉じた後の最初のロックは即ジャンプ。
- ゲート = max(ユーザー閾値×公称レベル, ノイズフロア×2.5)。ノイズフロアは10 ms RMSの最小追従（下降30 ms、上昇+6 dB/s、公称比-34 dBで頭打ち）。

## 現状（2026-11-12時点）
- `core/` `tests/` `tools/` は Linux g++13 でビルド・テスト済み。結果は `docs/results_*.txt`。
  - 5分スクラッチ: p99 0.8–1.0 ms、最大 2–2.4 ms、ドリフト ≈0.1 ms以下。低速0.01xでも停止なし。CPU 解析0.3%＋再生0.2%/1コア。
  - 残課題: 激しいスクラッチで二倍角PLLの0.5周期スリップが約0.5回/秒起き、次の順方向ストロークで自動補正される。
- `plugin/` は **未コンパイル**（作成環境にJUCEが無かった）。最初のビルドでのAPI修正が想定される。
- 実機Serato盤では未検証。特にAMビットがどちらのピークに載っているか、チャンネル割り当ては実録音で確認が必要（自動判定はある）。

## ビルドとテスト
```
git clone --depth 1 https://github.com/juce-framework/JUCE.git   # .gitignore 済み
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\vt_test_decoder.exe --quick     # 解析の精度テスト（--quick なしで5分版）
build\Release\vt_test_player.exe              # 解析→再生の通し・CPU測定
build\Release\tc_analyze.exe rec.wav --side A --csv out.csv
```
JUCEなしで core/テストだけ: `-DVT_BUILD_PLUGIN=OFF`

解析ロジックを変えたら必ず `vt_test_decoder` を流し、変更前後の数値（p99 / max / drift / stuck / reanchor）を比較すること。

## 次の作業候補
1. プラグインのビルドを通す（Windows / VS2022、次に macOS）
2. Ableton Live で動作確認（入力モニター In、バッファ 64–128）
3. JUMBLEQ経由で実機タイムコードを録音（通常再生 / 低速 / スクラッチ / 針の載せ替え）→ `tc_analyze` で検証・調整
4. 長いサンプル読み込みのバックグラウンド化、高速時の音質
5. Relativeモードの方向反転スイッチ（非Serato盤用）、djay / Traktor 盤対応
6. Max for Live 連携

## 方針メモ
- xwax 等 GPL コードは使っていない（core はサードパーティコードなし）。ライセンス未決定。
- PLUGIN_CODE `Vytr` / MANUFACTURER `Ymwk` は公開後に変えない（保存済みLiveセットの互換性）。
