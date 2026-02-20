# tangnano20k_vdp_cartridge-verilator

このリポジトリは、tangnano20k VDP カートリッジの Verilator ベースのテストハーネスを提供します。主な機能：

- 生の VDP ピクセル出力（screen_pos_x / screen_pos_y と vdp_r/g/b）をトップレベルで公開
- C++ ラッパーが生ピクセルをサンプリングして PPM フレームを生成
- CSV シナリオから実行時に VCD のダンプを開閉・ON/OFF 制御可能
- VCD 出力量を抑えるための trace depth 制御やダンプ抑止フラグを実装

以下はビルド方法、使い方、デバッグのヒントです。

---

## 必要環境

- Linux / WSL（WSL 上でも動作しますが、ファイルロックや I/O に注意）
- Verilator（推奨：比較的新しい安定版）
- g++（C++11 以上）
- make
- Python 3（任意：VCD を後処理する場合）

Debian/Ubuntu の例：
```bash
sudo apt update
sudo apt install verilator g++ make python3
```

---

## ビルド（簡易）

リポジトリのルートから：

1. クリーン（オプション）:
```bash
make clean || rm -rf obj_dir
```

2. ビルド:
```bash
make msx2logo
```
または
```bash
make
```

Notes:
- Makefile は `verilator --cc --exe --build --trace ...` を使ってビルドします。
- 環境や設定によっては `-DVM_TRACE` を Verilator コンパイル定義に追加する必要があります。その場合は Makefile に反映するか、`make VERILATOR_FLAGS+=" -DVM_TRACE"` のようにして下さい。

---

## 実行例

ビルド後、実行バイナリ（通常は `obj_dir/Vwrapper_top`）を使って次のように起動できます：

```bash
./obj_dir/Vwrapper_top --vcd=trace.vcd --csv=tests/csv/msx2_logo.csv --dump-screen
```

主要オプション：
- `--vcd=PATH` : VCD トレースを PATH に出力
- `--csv=PATH` : CSV シナリオを実行
- `--dump-screen` : raw VDP ピクセルを PPM として保存する（フレーム毎）
- `--vramtest` : VRAM テストモード
- `--debug` : ラッパーのデバッグログを有効化

---

## CSV コマンド（ランタイムでの VCD 操作）

CSV 実行中に VCD のファイル open/close とダンプ ON/OFF を制御できます。

- `VCD_OPEN,<path?>`  
  トレースファイルを開きます。パス省略時は `dump.vcd` 。

- `VCD_CLOSE`  
  トレースファイルを閉じます（物理的に close）。

- `VCD_ON,<0|1?>`  
  トレースファイルが open の状態で `g_tfp->dump()` によるダンプを有効化します（引数省略なら `1`）。

- `VCD_OFF`  
  ファイルを閉じずにダンプを停止します（ダンプ呼び出しを抑止）。

典型的な使い方（CSV 内）：
```
VCD_OPEN,trace.vcd
CYCLE,1000
VCD_OFF
CYCLE,50000
VCD_ON,1
VCD_CLOSE
```

この仕組みは「ファイル再オープンのコスト」を避けつつ、ダンプを必要な箇所だけ行うためのものです。

---

## raw VDP ピクセルのキャプチャ（PPM）

ラッパーは VDP の「生の」出力（加工前）をサンプリングし、フレーム境界で PPM を出力できます。

- 有効化方法：
  - 実行時に `--dump-screen` を付ける、または `vdp_cartridge_set_dump_screen(1)` を呼びます。
- 出力ファイル：
  - `display_000000.ppm`, `display_000001.ppm`, ... のように生成されます。
- 注意：
  - HDMI/最終表示出力とは異なり、これは VDP 生出力です（openMSX 等に渡す目的向け）。
  - サンプリング位相（posedge/ネゲエッジ）はデザイン依存です。結果が正しくない場合は wrapper のサンプリング位相を調整してください（`vdp_cartridge_wrapper.cpp` 内で調整可能）。

---

## VCD の範囲（trace depth）とファイルサイズ制御

`g_top->trace(g_tfp, depth)` でトレースする階層の深さを指定できますが、注意点：

- `depth` は「どこまで階層をたどって signal を登録するか」を指定します。ただし wrapper_top が多数の DUT 信号をトップポートとして露出していると、`depth=0` でも大量の信号が出力されることがあります。
- Verilator のバージョンや生成��ードにより `depth` の挙動に差が出ることがあります。

ファイルサイズ削減の方法：
1. trace depth を 0 または 1 に設定して試す（`vdp_cartridge_set_vcd_depth(0)` を main で呼ぶ）。
2. CSV の `VCD_OFF` / `VCD_ON` を使って長時間の不必要なダンプを抑止する。
3. 生成済み VCD を後処理して必要なスコープだけ抽出する（Python スクリプト例をブランチに用意しています）。

---

## トラブルシューティング

- `cannot open output file Vwrapper_top: No such file or directory`  
  - `obj_dir` の権限、ファイルロック、WSL の Windows 側アンチウィルスの干渉などが原因です。`rm -rf obj_dir` 後再ビルド、または WSL を再起動してから再試行してください。

- VCD が依然フルトレースになる（depth=0 でも下位階層が含まれる）  
  - wrapper_top が多くの信号をトップポートとして再露出しているためです。根本解決は RTL 側でトップ露出を減らすことですが、暫定的に VCD post-filter（ツール）で必要なスコープだけ抽出してください。

- PPM が「一行だけ」になる（全ピクセルが同じ y 座標でサンプルされる）  
  - サンプリング位相（どのクロックエッジでサンプルするか）が誤っている可能性があります。`vdp_cartridge_wrapper.cpp` のサンプリングタイミングを posedge/negedge やサンプルフェーズで調整してください。

---

## 開発者向けメモ

- 主要 API（`vdp_cartridge_wrapper.h`）:
  - `vdp_cartridge_set_vcd_depth(int depth)`
  - `int vdp_cartridge_set_vcd_enabled(int enable, const char* path)`
  - `void vdp_cartridge_set_vcd_dump(int enable)` — open 状態で dump を有効/無効
  - `void vdp_cartridge_set_dump_screen(int enable)` — raw ピクセルの PPM 出力制御
  - `void vdp_render_frame_rgb(uint8_t* dst, int pitch)` — display_* 出力からフレームを取得

- VCD の `dump()` 呼び出しは頻度を抑えるよう `step_halfcycle()` の制御点（たとえば posedge のみ）で行う実装にしています。これにより I/O 負荷を低減します。必要ならさらに `g_vcd_dump_enabled` を CSV から切り替え可能にしてあります。

---

## 動作例

ビルドして実行：
```bash
make msx2logo
./obj_dir/Vwrapper_top --csv=tests/csv/msx2_logo.csv --vcd=trace.vcd --dump-screen
```

CSV の一例：
```
VCD_OPEN,trace.vcd
CYCLE,1000
VCD_OFF
CYCLE,50000
VCD_ON,1
VCD_CLOSE
```

VCD をトップのみ抽出（後処理の例；ツールが別途必要）：
```bash
python3 tools/filter_vcd.py dump.vcd wrapper_top trimmed.vcd
```

---

## ライセンス

MIT（詳しくはリポジトリの LICENSE を参照）

