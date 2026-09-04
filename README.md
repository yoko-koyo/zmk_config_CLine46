# zmk-config-CLine46

## ローカルビルド

Docker でコンテナ化された ZMK ビルド環境を使うので、ローカルに Zephyr SDK を入れる必要はありません。

### 準備

Docker Desktop を起動しておく。初回のみ依存関係（ZMK 本体・各モジュール）を取得します。

```sh
./build_local.sh init     # west init + west update（初回のみ / build 時に自動実行）
```

### ビルド

```sh
./build_local.sh list              # ビルドターゲット一覧
./build_local.sh build             # 全部ビルド（cline46_l / cline46_r / reset）
./build_local.sh build cline46_r   # 右手側だけ
./build_local.sh -i build cline46_r  # 差分ビルド（速い）
./build_local.sh copy              # uf2 を ./artifacts へコピー
```

生成物は `build/<ターゲット名>/zephyr/zmk.uf2` に出力されます。

### 書き込み

ビルドしてから、XIAO のリセットボタンをダブルクリックしてマウントされたら自動でコピーします。

```sh
./flash_l.sh    # 左手側
./flash_r.sh    # 右手側
```

ドライブ名が `XIAO-SENSE` 以外の場合は `DRIVE_NAME="NO NAME" ./flash_l.sh` のように指定してください。

### その他

```sh
./build_local.sh clean        # build/ を削除
./build_local.sh clean_all    # west の依存関係もすべて削除
./build_local.sh update       # west update
./build_local.sh gitignore    # config/west.yml から .gitignore を再生成
./build_local.sh help
```

環境変数 `RUNTIME`（既定 `docker`）、`ZMK_IMAGE`（既定 `zmkfirmware/zmk-build-arm:4.1-branch`）で切り替えできます。
