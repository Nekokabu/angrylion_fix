# rdp_timing v24

v23 の結果: ゲームのリストの三角形が v22 の合成三角形より 1〜2 割高い原因は、**モードビットではない**（AA / パースペクティブ / LOD /
ディザ / ポイントサンプル / TLUT を落としても ±0.1%、BAR のモードそのものを合成三角形に付けても同じ）し、**三角形コマンドの大きさでも
属性の傾きでもない**（0F→09 で +0.3%、傾き 0 で −0.6%）。一方で `same_place`（全部同じ場所）は −44%、`strip` −2〜5%、`apex` −2〜4% と
**メモリの局所性**が大きく効き、Z 系（`om_no_zcmp` −14〜20%、`om_no_zupd` −2.5〜7%）は ares のテーブルの想定（−27〜40% / −25〜33%）と
大きく違います。v23 の `z_near` は Z クリアの Fill を取り除けていなかった（バグ）ので full と同じ値でした。
また v23 は両機とも Y の `state_every8` の後（`syncpipe_every1`〜`ci4_tlut` のどれか）で止まってしまい、残りのケースが取れていません。

v24 はそこを詰めます（8 MB 必須、SD に `rdp_timing/rdp_timing_v24_NNN.txt`、約 2 分、**Y はケースごとに SD へ保存**するので、
止まった場合はどのケースで止まったか画面の進捗と一緒に教えてください）。

## X（VI オフのみ。v23 で VI はどちらの機でも最初のケースから止まっていました）
| variant | 内容 |
|---|---|
| full / tri_only | v22/v23 と同じ |
| z_far / z_near | Z クリアの Fill を取り除き（今度は本当に）、Z バッファを最遠 / 最近で初期化 → 最初の描画は全部通る（落ちるのは同じフレーム内のオーバードローだけ）/ 全部落ちる |
| tri_only_zfar / tri_only_znear | 同じことを tri_only に |
| om_no_zcmp | Z_COMPARE オフ（比較無し、Z 書き込みあり） |
| om_no_atomic | ATOMIC_PRIM オフ（DK64 が立てている。v23 の `om_wlike` で DK64 だけ −7.5% だった原因の候補） |
| om_no_aa / om_no_cvg | v23 と同じ（再現性） |
| color_low | 描画先カラーバッファを 0x0F5000（RDRAM の最初の 1 MB）へ移す（BAR は再生時 0x300000、DK64 は 0x11D240） |
| z_up1m | Z バッファを 1 MB 上へ（BAR のみ: 0x4DA800 → 0x5DA800） |

`full − z_far` = フレーム内オーバードローで落ちた分、`z_far − z_near` = 全通過と全落ちの差（= テーブルの pass/fail 差の実測）。

## Y（2cyc_tex_blend_z、H=8、w = 4 / 12 / 32、ランダム配置、VI オフ）
- `base`（v22 W / v23 base と同じ）、`zfail`（Z を最近でクリア）、`no_zcmp` / `no_zupd` / `no_imrd`
- `fbzb_*`: カラー／Z バッファの RDRAM 上の位置。`adjacent`（Z をカラーの直後 0x300000/0x325800）、`300k`（v22 の間隔）、`1m`、`bar`（ゲームどおり 0x200000/0x3DA800）、
  `bar_reloc`（X の再生どおり 0x300000/0x4DA800）、`2m`、`4m`、`dk_like`（Z 0x6D2E00、1 サイクル）、`adjacent2m`、`bar_om`（BAR のモードで bar_reloc 配置）
- v23 で取れなかった分: `syncpipe_every1`、`tile_every1`、`load_every8`、`ci4_tlut`、`rgba32_tex`、`i8_tex`、`op_0b/09/08`、`1cyc_base/apex/aa`、`dk_om`、`dk_om_apex`
- `1cyc_atomic` / `2cyc_atomic` / `dk_om_noatomic`: ATOMIC_PRIM の単価
- `random_rows`（x 固定、行だけランダム）/ `random_cols`（行固定、x だけランダム）/ `same_place`

`ares_reference.txt` は ares v7 モデル（`T_ZPASS=1`）、`ares_raster_wmodel.txt` は実機エッジウォーカー + v22 W 単価。`compare.py 実機ファイル v7z rastwz`。

作: Nekokabu
