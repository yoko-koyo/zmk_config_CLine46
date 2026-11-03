# CLAUDE.md

CLine46（XIAO BLE 左右分割・トラックボール付き）用の ZMK 設定リポジトリ。

## 構成

- `config/CLine46.keymap` — キーマップ本体（レイヤー・コンボ・behavior）。編集の大半はここ
- `config/CLine46.json` — 物理レイアウト（keymap-editor / keymap-drawer 用）
- `config/west.yml` — 依存モジュール。変更したら `./build_local.sh gitignore`
- `boards/shields/CLine46/` — シールド定義（`.overlay` / `.dtsi` / `.conf`）。トラックボールやポインタ設定もここ
- `build.yaml` — ビルドターゲット（`cline46_r` / `cline46_l` / `reset`）。CI とローカル共通
- `zmk/` `zephyr/` `modules/` `zmk-*/` は west 管理の取得物。**編集しない**（参照のみ）

## ビルド・書き込み

- ビルド: `./build_local.sh -i build cline46_r`（Docker Desktop 起動が前提）
- 書き込み: `./flash_l.sh` / `./flash_r.sh` は実機のリセット操作待ちでブロックするため、**Claude は実行しない**。ユーザーに依頼する
- keymap を変更したら、可能ならビルドで構文エラーがないことを確認してから完了報告する

## keymap 編集ルール

- レイヤー番号は先頭の `#define`（MOUSE, NUMBER, …）で参照し、数値を直書きしない
- レイヤーを追加・並べ替えたら `#define` と `ALL_BUT_MOUSE` を合わせて更新する
- 記号系コンボは `layers = <ALL_BUT_MOUSE>` で MOUSE レイヤーを除外する
- コメントは日本語。配列図コメントやコンボのキー位置コメントは実際の割り当てと一致させる
- 新しい hold-tap を追加するときは、各パラメータの意図をコメントで書く（既存の `hlt` などに倣う）
- 既存のインデント・桁揃えを崩さない

## Git

- このリポジトリは keymap-editor（GitHub 上の Web エディタ）からも `Updated CLine46.keymap` としてコミットされる。編集前に未取り込みのリモート変更がないか確認する
- コミットメッセージは日本語で「何を・なぜ」を1行で（例: 「スクロール量を 1/16 に下げ、ポインタ加速をなめらかにする」）
- コミットはユーザーに頼まれたときだけ。push も明示的に頼まれたときのみ
