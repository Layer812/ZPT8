# ZPT8 (M5Cardputer用 Zepto-8 移植版)

[English Version](README.md)

ZPT8は、カスタマイズされたZepto-8コアをベースに構築された、M5Cardputer専用に高度に最適化されたPICO-8ファンタジーコンソールエミュレータです。

静的バッファの再利用、メモリ割り当てオーバーヘッドゼロのカスタムLuaアロケータ、リアルタイムの積極的なガベージコレクション（GC）など、アグレッシブなベアメタルメモリハックを採用することにより、ESP32-S3の制限された320KB RAMの壁を打ち破ります。これにより、スタンドアロンのシステムBIOSの起動や、最適化された`.pc8c`フォーマットに変換された重く複雑なPICO-8カートリッジを、手のひらの上で動かすことができます。<br>
<img width="480" height="270" alt="Image" src="https://github.com/user-attachments/assets/10904ae2-a344-4af6-b236-2014e23407d8" />

---

## 💾 M5Burner を使用したクイックインストール

PlatformIOをインストールしたり、ソースコードを自分でコンパイルしたりすることなく、ZPT8をM5Cardputerに直接書き込むことができます。

1. PCで **M5Burner** を開きます。
2. ユーザー公開ファームウェアカタログでカスタム共有コードを検索します：
   * **共有コード (Share Code)**: `Uv0jV9Mo8hxCK7Gf`
3. USB経由でM5Cardputerを接続し、COMポートを選択して、**Burn**をクリックします。

---

## ✨ 特徴

* **画面の最適化**: 専用のファストパスレンダリングユーティリティを使用して、ネイティブの128x128 PICO-8キャンバスをM5Cardputer画面の中央に直接マップします。
* **メモリ使用量の極限削減**:
  * 固定の64KB静的コードバッファ（BSSセクション）を再利用し、カートリッジのスワップ中のヒープ割り当てオーバーヘッドを正確に**0バイト**に抑えます。
  * 標準C++の `std::string` コピーを排除し、一時的なヒープ保持を無くします。
* **ベアメタルLuaアロケータ**: `z8lua` の内部メモリ制限をオーバーライドし、ESP32の生の空きヒープを100%直接Luaステートに開放します。
* **アグレッシブなガベージコレクション**: Luaエンジンを強制的に高頻度リサイクルモード（`LUA_GCSETPAUSE`を100、`LUA_GCSETSTEPMUL`を500に設定）で動かし、30KB〜90KBという極めて厳しい動作マージン内で揮発性プロセスを安全に実行します。
* **専用オーディオパイプライン**: サウンド合成処理をFreeRTOSタスク経由でCore 0にオフロードし、メインフレームループを阻害することなく、安定したダブルバッファリング11025Hzオーディオのダウンサンプリングを実現します。

---

## 📂 SDカードのファイルレイアウト

microSDカードのルートディレクトリを以下のように構成してください。メモリマップへのダイレクト書き込みとRAMオーバーヘッドの最小化のため、すべてのカートリッジは事前に最適化された `.pc8c` バイナリフォーマットにコンパイルされている必要があります。

```text
SD Card Root/
├── bios.pc8c          # ZPT8コンソール環境用のシステムBIOS
├── jelpi.pc8c         # デフォルトの動作確認用ゲーム (推奨)
├── 31991.pc8c         # 「El Dorado」の最適化済みバイナリカートリッジ
└── any_other_game.pc8c # その他のゲームも直接ルートに配置します
```

---

## 💻 開発環境の構築手順

ZPT8エミュレータ本体やカートリッジコンパイラをビルドするための手順です。

### 必要な環境
1. **Visual Studio Code (VSCode)**
2. **PlatformIO IDE 拡張機能**（VSCode 内でインストール）
3. **M5Cardputer 本体**（およびPC接続用のUSB-Cケーブル）

### 依存・参照リポジトリおよびライブラリ
本プロジェクトでは、以下の主要な外部リポジトリを使用しています：
* **Zepto-8 Core**: [samhocevar/zepto8](https://github.com/samhocevar/zepto8) (PICO-8 エミュレータコア)
* **M5Unified**: [m5stack/M5Unified](https://github.com/m5stack/M5Unified) (M5Stack統合ハードウェア抽象化レイヤー)
* **M5Cardputer**: [m5stack/M5Cardputer](https://github.com/m5stack/M5Cardputer) (M5Cardputer用ラッパーライブラリ)

### 便利なオプションツール
* **Shrinko8**: PICO-8 カートリッジ用の最適化・コード縮小（ミニファイ）ツールです。ZPT8自体のビルドには必須ではありませんが、Luaコードのサイズを圧縮して `.pc8c` へコンパイルする際に役立ちます。
  以下のコマンドでリポジトリから取得して使用できます：
  ```bash
  git clone https://github.com/thisistherong/shrinko8.git
  ```


### ビルドおよび書き込み手順

#### 1. リポジトリのクローンと依存関係の取得
本リポジトリを `ZPT8` フォルダ以下にクローンし、そのフォルダ内で依存する `zepto8` リポジトリを手動で再帰的クローンします：
```bash
# ZPT8本体のクローン
git clone https://github.com/Layer812/ZPT8.git ZPT8
cd ZPT8

# 依存する zepto8 コアをZPT8フォルダ内に再帰的クローン
git clone --recursive https://github.com/samhocevar/zepto8.git zepto8
```


#### 2. プロジェクトの読み込み
VSCodeを起動し、クローンした `ZPT8` フォルダを開きます。PlatformIOが自動的にプロジェクトを認識します。

#### 3. エミュレータ本体のビルドと書き込み
本プロジェクトは、M5Cardputer（内部基板: M5Stack StampS3）をターゲットにしています。
* **VSCode GUI から行う場合**:
  1. VSCode の左メニューにある PlatformIO アイコン（アリのマーク）をクリックします。
  2. **Project Tasks** から `env:m5stack-stamps3` ➔ **General** ➔ **Upload** を選択して実行します。
  3. 必要に応じて **Monitor** を実行し、シリアルデバッグ出力を確認します。
* **CLI (コマンドライン) から行う場合**:
  ```bash
  # ビルドのみ
  pio run -e m5stack-stamps3
  
  # ビルド ＆ M5Cardputerへの書き込み
  pio run -e m5stack-stamps3 --target upload
  
  # シリアルモニターの起動
  pio run -e m5stack-stamps3 --target monitor
  ```

#### 4. カートリッジコンパイラ (pc8_compile) のビルド
PC上で `.p8` から `.pc8c` へ変換するためのツールをビルドします。
* **CLI (コマンドライン) から行う場合**:
  ```bash
  # PCネイティブ向けにビルド
  pio run -e native_tool
  ```
  ビルドが成功すると、実行バイナリが生成されます。Windows環境の場合は、生成された実行ファイルを `tools/pc8_compile.exe` に配置して使用してください。

---

## 🛠️ カートリッジの変換手順 (`.p8.png` ➔ `.pc8c`)

ZPT8はSDカードから高度に最適化された `.pc8c` フォーマットのカートリッジを読み込んで実行します。
`.p8.png` ファイルから `.pc8c` バイナリカートリッジへ変換するには、以下のステップを実行します。

### 前提条件
* Python 3.x がインストールされていること。
* 画像処理ライブラリ `Pillow` がインストールされていること。
  ```bash
  pip install Pillow
  ```

### ステップ 1: `.p8.png` から `.p8` テキストの抽出
`p28.py` スクリプトを使用して、PICO-8 カートリッジ画像（`.p8.png`）からソースコードとグラフィックスデータを含む `.p8` テキストファイルを復元します。
```bash
python p28.py <入力ファイル名.p8.png> <出力ファイル名.p8>
```
* **実行例**:
  ```bash
  python p28.py jelpi.p8.png jelpi.p8
  ```

### ステップ 2: `.p8` から `.pc8c` へのコンパイル
ビルドした `pc8_compile` ツールを使用して、`.p8` テキストを最適化済みのバイナリフォーマット `.pc8c` へコンパイルします。

#### コマンド形式:
```bash
tools/pc8_compile.exe <mode> <入力ファイル名.p8> <出力ファイル名.pc8c>
```
* `<mode>`: ゲームカートリッジの場合は `game`、システムBIOSの場合は `bios` を指定します。

#### 実行例 (ゲームの場合):
```bash
tools/pc8_compile.exe game jelpi.p8 jelpi.pc8c
```
#### 実行例 (BIOSの場合):
```bash
tools/pc8_compile.exe bios bios.p8 bios.pc8c
```

生成された `.pc8c` ファイルを microSD カードのルートディレクトリへコピーしてください。

---

## 🎮 操作方法

M5Cardputerの物理キーボードおよびサイドキーは、PICO-8のプレイヤー1入力にバインドされています。

| ZPT8キー (M5Cardputer) | PICO-8 ボタン | ゲーム内アクション |
| :--- | :---: | :--- |
| **矢印キー (↑ / ↓ / ⬅️ / ➡️)** | ⬆️ / ⬇️ / ⬅️ / ➡️ | 移動 / 方向キー |
| **`O` キー** または **`Z` キー** | 🅾️ (ボタン 4) | ジャンプ / 決定 / プライマリアクション |
| **`X` キー** または **`Space` キー** | ❎ (Button 5) | ダッシュ / キャンセル / メニューオーバーレイ |
| **全英数字キー** | テキスト入力 | PICO-8 BIOS内でのネイティブコマンド入力 |

### 🛠️ 内蔵起動ファイルセレクターの操作方法
* **`↑` / `↓` 矢印キー**: 利用可能なファイルリストを上下にブラウズします。
* **`O` キー**: 選択したカートリッジをロードして自動実行（`run`）します。
* **`X` キー**: 選択をキャンセル、またはコンソールへ戻ります。

---

## 🔧 トラブルシューティング

#### Q. カートリッジはロードされるが、画面が黒いまま、またはフリーズする。
A. `main.cpp` の `loop()` 内のパイプラインルーティングを確認してください。カートリッジが高速レンダリング（`g_vm->render_fast() == false`）から外れた場合、フォールバックピクセル配列が正しくコピーされ、Big-Endian RGB565へバイトスワップされて、`g_hal.pushScreenBuffer()` によってプッシュされているか確認してください。

#### Q. リアルタイムループ中に `*** BIOS LUA ERROR 4: not enough memory` が発生する。
A. `src/pico8/vm.cpp` をダブルチェックしてください。`vm::vm()` コンストラクタが、通常のLuaメモリ設定をバイパスして `lua_newstate(baremetal_lua_alloc, nullptr)` を呼び出し、コア初期化ルーチンを実行する前にアグレッシブなGCステップを設定しているか確認してください。

#### Q. カートリッジがエラーでクラッシュしたり、デバイスが突然リセット（再起動）される。
A. ESP32-S3の極めて限られたメモリ制限（320KB RAM）のなかで動作しているため、複雑なカートリッジやメモリ消費の激しいカートリッジではメモリ不足（OutOfMemory）が発生することがあります。もしクラッシュや強制リセットが発生するカートリッジを見つけた場合は、GitHubの Issue でやさしく教えていただけると幸いです！ ZPT8をより良くするためのご協力に感謝いたします。

---

## 📜 ライセンスと謝辞

* **Zepto-8 Core**: Copyright © 2016–2024 Sam Hocevar (Do What the Fuck You Want to Public License - WTFPL).
* **z8lua 拡張**: カスタマイズされた Lua 5.2 組み込みサブシステム。
* **LodePNG**: Copyright © 2005–2020 Lode Vandevenne (zlibライセンス).
* **改変および新規追加部分**: Copyright © 2026 Layer8. MITライセンスに準拠します。
* **Jelpi サンプルアセット**: `jelpi.pc8c` は Lexaloffle Games が公式に作成したデモカートリッジ「Jelpi Adventures」を最適化変換したものであり、ハードウェアおよびパフォーマンスの検証目的でのみ提供されています。
