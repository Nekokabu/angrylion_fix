# rdp_timing v23

v22 の結果（W 系列）で、**孤立した三角形**の単価は「三角形 1 個あたり約 45 clk の固定分 + スパンあたりテーブル値（+15 の spanPenalty は不要）」と
分かりましたが、その単価をゲームのリスト（V 系列）に当てはめると実機より **10〜20% 安く**なってしまいます（BAR 軽いフレームで実機/モデル 1.21、
重いフレーム 1.10、DK64 1.06）。つまりゲームの三角形には、v22 の合成三角形に無い「何か」が乗っています。v23 はその何かを 2 方向から切り分けます。
v15/v22 と同じ ROM 構成（8 MB 必須、SD に `rdp_timing/rdp_timing_v23_NNN.txt`、画面に進捗、約 2 分）。

## X: ゲームのリストのモードビット／三角形コマンドを書き換えて再生
v22 の bar_i2274（軽）/ bar_i2314（重）/ bar_i2404（重・後半）/ bar_2316 / dk64_797 の 5 本。VI オフで（full と tri_only はゲームの VI モードでも）:

| variant | 変更 |
|---|---|
| full / no_tri / tri_only | v22 と同じ |
| om_no_zcmp | 全 Set_Other_Modes の Z_COMPARE を落とす → Z テストが全部通る（Z 書き込みは残る） |
| om_no_zupd | Z_UPDATE を落とす（Z 読み出し・比較だけ） |
| z_near | Z バッファを消す Fill を取り除き、Z バッファを 0（最も手前）で初期化 → Z テストが全部落ちる |
| om_no_aa / om_no_persp / om_no_lod / om_no_dither / om_point / om_no_tlut / om_no_cvg / om_no_imrd | AA_EN / TEX_PERSP / TEX_LOD+SHARPEN+DETAIL / ディザ / バイリニア→ポイント / EN_TLUT / カバレッジ系 (ALPHA_CVG_SEL, CVG_X_ALPHA, CVG_DST, COLOR_ON_CVG, ALPHA_COMPARE) / IMAGE_READ を落とす |
| om_1cyc | 2 サイクルのモードを 1 サイクルに |
| om_wlike | v22 W の三角形と同じモードビット（サイクル種別・ブレンダ・Z・IMAGE_READ だけ残す） |
| tri_flat | 0F 三角形のシェード／テクスチャ／Z の傾き（DxDx, DxDe, DxDy）を全部 0 に |
| tri_noshade / tri_notex / tri_edgez | 0F → 0B（テクスチャ+Z, 14 語）/ 0D（シェード+Z）/ 09（エッジ+Z, 6 語）。落とした語は NOP にしてリスト長は同じ |

`X` 行: list, variant, vi, DPC_CLOCK（3 回の最小）, ticks, model, VI_CTRL 読み戻し, VI_WIDTH, VI プローブ。
`om_no_zcmp` と `z_near` と `full` の差から **実際の Z 通過率**が、`om_*` の差からモードビットごとの単価が、`tri_*` から
三角形コマンドのサイズ／属性の傾きの効果が出ます。

## Y: 合成三角形で、ゲームの三角形の性質を 1 つずつ足す
v22 W の `2cyc_tex_blend_z`（垂直エッジ、ランダム配置、H=8 スパン、w = 4 / 12 / 32、4096 スパン、VI オフ、Z 全通過）を基準に:

- 形: `frac`（エッジを x+0.5 に）, `shear_half` / `shear_two`（全エッジを 0.5 / 2 px/行 傾ける。幅は w のまま）, `apex`（頂点が上、底辺 2w の本物の三角形。平均幅 w）
- モードビット: `aa`, `persp`, `lod`（tile 1 = tile 0）, `dither`, `point`, `cvg`, `bar_om`（BAR の 0x182c0f/0x0c182078 そのもの）, `dk_om`（DK64 の 1 サイクル）
- 属性: `slopes`（シェード／テクスチャ 1 texel/px／Z の傾き）
- 配置: `strip`（行に沿って隣接）, `same_place`（全部同じ場所）
- 状態の挟み込み: `state_every1/8`（Set_Other_Modes + Set_Combine）, `syncpipe_every1`, `tile_every1`（Set_Tile + Set_Tile_Size + Sync_Tile）, `load_every8`（Sync_Load + Load_Block 64 B + Sync_Tile）
- テクスチャ形式: `ci4_tlut`, `rgba32_tex`, `i8_tex`
- コマンド種別: `op_0b_texz` / `op_0d_shadez` / `op_09_z` / `op_08_plain`
- 1 サイクル: `1cyc_base` / `1cyc_apex` / `1cyc_aa`, `bar_om_apex` / `bar_om_lod` / `dk_om_apex`（モード + 形 + 傾きの組み合わせ）

`Y` 行: case, w, H, triangles, spans, DPC_CLOCK, ticks, clocks/span, clocks/triangle, VI プローブ。

`ares_*.txt` は ares（series v7 のモデル、`T_ZPASS=1` = Z 全通過固定）で同じ ROM を走らせた出力。`ares_raster.txt` は
ハーネスに入れた「実機のエッジウォーカー（4 サブスキャンライン、floor(左端)〜floor(右端) を含む）」での値です。

作: Nekokabu

`ares_reference.txt`: ares v7 モデル（`T_ZPASS=1`）、`ares_raster.txt`: + 実機のエッジウォーカー（`T_RASTER=1`）、`ares_wmodel.txt`: v7 + v22 W の単価
（`T_TRIOV=45 T_TRISP=3 T_SPWIDE=0.8`）、`ares_raster_wmodel.txt`: 両方。`compare.py 実機ファイル v7z rastz wmodz rastwz` で表にできます
（`v23runs/TAG.txt` を参照するので、ares_*.txt をそのパスに置いてください）。
