# rdp_timing v22

BAR intro の「ゲーム中の RDP 時間が ares より重いフレームで +2.7%、軽いフレームで +6.7%（CPU を止めた状態）」の内訳を、
3 つの系列で切り分けます。v15 と同じ ROM 構成（8 MB 必須、SD に `rdp_timing/rdp_timing_v22_NNN.txt`、画面に進捗、約 3 分）。

## V: ゲーム中の intro フレームのリストをコマンド種別ごとに NOP にして再生
bar_tasklog を走らせた ares から取り出した **ゲーム中そのものの RDP リスト**（Sync_Full 番号 = tasklog フレーム + 2254）:
`bar_i2274/2279/2284`（フレーム 20/25/30、軽い: ares 0.62〜0.65 VI、実機 +6.7%）、`bar_i2314/2338`（フレーム 60/84、重い、実機 +0.06/+0.09 VI）、
`bar_i2404/2410`（フレーム 150/156、重い、実機 +0.00/+0.01 VI）、比較用に v15 の `bar_2316` と `dk64_797`。
variant（full / no_tri / no_rect / no_load / tri_only / state_only / fetch_only）× VI（game = 0x13012 + VI_WIDTH 320、vi_off）。
`V` 行: list, variant, vi, DPC_CLOCK（3 回の最小）, ticks, model, **読み戻した VI_CTRL, VI_WIDTH, VI プローブ**（RDP 停止後に 4096 回の依存ラインフィルに
かかった ticks: VI オフで約 93K、ゲームの AA モードなら約 100K → 測定中に VI が本当にフェッチしていたかの確認。v15/v17 で VI の効果が途中で消えた
問題の切り分け用）。

## Q: リスト実行中の DPC_CURRENT の進み方
`bar_2316` と `bar_i2314` を流しながら CPU が DPC_CURRENT を読み続け、値が変わった回数と 1 回の進み（バイト）のヒストグラム
（≤8 / 16 / 32 / 64 / 128 / 256 / 512 / 1024 / それ以上）を記録します。F3DEX2 の FIFO 待ち（DPC_CURRENT を見て空きを判断）の粒度の実測。

## W: 細い三角形のスパン単価
ゲームの三角形は 1 スパンあたり 2〜11 ピクセル（BAR 2316: 7.8K スパン / 76K px、DK64: 2〜3 px/スパン）で、コストはスパン数・三角形数で
決まります。shade+texture+z の三角形（22 ワード）で、3 辺とも垂直にしてスパン幅をちょうど w、高さ H スパンにし、320×240 16bit バッファの
ランダムな位置に 4096 スパン分描いて DPC_CLOCK を測ります。
- モード: `2cyc_tex_blend_z`（BAR の主モード）と `1cyc_tex_blend_z`（DK64）は w = 1〜64 × H = 1,2,3,4,16 の全組、`2cyc_tex_blend` / `2cyc_tex_z` /
  `2cyc_tex` / `1cyc_tex_z` は H=3、w = 2/4/8/32。VI オフとゲームモードの両方。
- Z: `pass`（Z バッファを遠にクリア → 全通過）、`fail`（近 → 全部落ちる）、`half`（同じ場所に 2 枚、後の方が奥 → 半分落ちる）。
  ares のテーブルは「全通過」「全落ち」の 2 系統を推定通過率で混ぜているので、短いスパンでの落ちたピクセルの単価と、推定が効く範囲を確かめます。
`W` 行: vi, mode, w, H, z case, triangles, spans, DPC_CLOCK, ticks, clocks/span, clocks/triangle, VI プローブ。

`ares_reference.txt` は ares（series v7）で同じ ROM を走らせた出力です（ares では fail/half が pass と同じ値になるのが現状のモデル）。

作: Nekokabu
