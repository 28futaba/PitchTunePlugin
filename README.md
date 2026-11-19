# PitchTunePlugin (ARA 2.0 手動ピッチ補正 VST3 プラグイン)

JUCE 8 と Celemony ARA 2.0 SDK を用いて開発されたオフライン手動ピッチ補正 VST3 プラグインです。
※AI製(Gemini Argon)です。

---

## 🌟 主な特徴 (v1.2.0)

- **ARA 2.0 (Audio Random Access) 完全対応**
  - DAW（Studio One, Reaper, Cubase, Logic Pro 等）のオーディオトラックやクリップと直接同期。
  - 音声クリップを自動解析し、ノート情報と詳細なピッチ軌跡を取得。レガート歌唱の自動分割にも対応。
  - DAW タイムライン上のクリップ移動・分割・トリミング位置に合わせて複数クリップを正確に配置表示。
  - 編集したピッチ・ノート分割内容は DAW プロジェクト保存時に自動で保存・復元。
- **直感的なピアノロール GUI エディタ**
  - C2〜C7 のピアノキーボードと秒単位のタイムライングリッド（長尺トラックに合わせた自動幅拡張・再生ヘッド表示・リサイズ対応）。
  - 起動時は実用的なボーカル中央音域（C4 付近）に自動スクロール表示。
  - 解析された音符がノートブロックおよびピッチ軌跡（黄色曲線）として可視化。
- **多彩なピッチ編集操作**
  - **上下ドラッグ**: 半音単位での転調・音高変更（元の位置をゴースト表示）。
  - **`Alt + 上下ドラッグ`**: セント単位での微調整（`+15c` のように差分表示）。
  - **右クリック**: クリックした位置でノートを手動 2 分割。
  - **ダブルクリック**: 編集したノートの音程・セントを元の位置へワンタッチリセット。
  - **Center スライダー (0〜100%)**: ノート内の音程の揺れ・ズレを目標音程の中心へ自動補正（ピッチ軌跡も連動表示）。
- **位相連続ピッチシフター (`PitchShifter`)**
  - 補正あり・補正なし（0半音）の境界でも常に一定レイテンシを保ち、ノート境界のタイミング段差やクリックノイズを低減。
- **リアルタイムフォールバックモード**
  - ARA 非対応の DAW ホストや通常インサート時でも、自動ピッチ補正（オートチューン）として動作するデュアルモード設計。

---

## 📂 ファイル構成

- `Source/PluginProcessor.h` / `Source/PluginProcessor.cpp`:
  - `juce::AudioProcessorARAExtension` を継承したメインプロセッサ。
  - `PitchTunePluginPlaybackRegion`: DAW クリップごとの解析・ノート管理・手動分割・セント補正計算。
  - `PitchTunePluginPlaybackRenderer`: 再生時のピッチシフトレンダリング（タイムライン／トリミング対応）。
  - `PitchTunePluginDocumentController`: ドキュメント全体の統括、ARA アーカイブ保存・復元、バックグラウンド解析スレッド。
- `Source/PluginEditor.h` / `Source/PluginEditor.cpp`:
  - `juce::AudioProcessorEditorARAExtension` を継承したリサイズ可能なプラグイン GUI（Center スライダー・操作ヒント付き）。
- `Source/PianoRollComponent.h` / `Source/PianoRollComponent.cpp`:
  - ピアノロール描画、ドラッグ転調、`Alt+ドラッグ` セント調整、右クリック分割、ダブルクリックリセット、再生ヘッド描画。
- `Source/PitchDetector.h` / `Source/PitchDetector.cpp`: 自己差分 (YIN/NSDF) ベースの高精度基本周波数 (F0) 推定エンジン。
- `Source/PitchShifter.h` / `Source/PitchShifter.cpp`: 定遅延クロスフェード型ディレイライン・ピッチシフター DSP。
- `Source/ARALibraryIncludes.cpp`: JUCE 8 標準バンドルとの重複リンク警告を防ぐための ARA SDK スタブ（ライセンス注記保持）。
- `install_vst3.bat`: ビルドされた VST3 プラグインをシステム共通プラグインフォルダにワンクリックで昇格コピーするインストーラー。

---

## 🛠️ ビルド方法

### 必要環境
- Windows 10 / 11 (x64)
- Visual Studio 2022 / 2026 (MSVC C++17)
- JUCE 8.0.14 以降
- ARA SDK 2.3.0 (`C:\SDKs\ARA_SDK` または任意の配置先)

### ビルド手順

#### 方法 1: Projucer を使用する（推奨）
1. [JUCE Projucer](https://juce.com/get-juce/) を起動します。
2. `PitchTunePlugin.jucer` を開きます。
3. 必要に応じて「Modules」パスおよび ARA SDK のヘッダーパス (`C:\SDKs\ARA_SDK`) をご自身の環境に合わせて設定します。
4. 「Save and Open in IDE」を実行し、Visual Studio でソリューションを開いて `Release / x64` でビルドします。

#### 方法 2: MSBuild コマンドライン
Visual Studio Developer PowerShell 等で以下を実行します：
```powershell
MSBuild.exe Builds\VisualStudio2026\PitchTunePlugin.sln /p:Configuration=Release /p:Platform=x64
```

出力先:
`Builds\VisualStudio2026\x64\Release\VST3\PitchTunePlugin.vst3`

---

## 🎧 DAW での使用方法

1. **プラグインの配置**:
   - ビルド完了後、`install_vst3.bat` を右クリックして「管理者として実行」するか、手動で `PitchTunePlugin.vst3` を DAW の VST3 フォルダ（`C:\Program Files\Common Files\VST3\`）にコピーします。
2. **DAW での読み込み**:
   - Studio One / Reaper / Cubase 等の ARA 2.0 対応 DAW で、ボーカルオーディオイベントに対して **ARA プラグイン / Melodyne 拡張スロット** として `PitchTunePlugin` を挿入します。
3. **編集**:
   - エディタ画面にボーカルのノートブロックとピッチ軌跡が表示されます。
   - **ドラッグ**: 半音移動 / **`Alt + ドラッグ`**: セント微調整 / **右クリック**: ノート分割 / **ダブルクリック**: リセット / **Center スライダー**: ノート内の揺れ補正強度を調整します。
4. **再生**:
   - DAW の再生ボタンを押すと、補正された音声がリアルタイムで再生されます。

---

## 📄 ライセンス・著作権表示 (License & Notices)

本プロジェクトのソースコードは **[GNU General Public License v3.0 (GPL-3.0)](LICENSE)** の下で公開されています。

### サードパーティライセンスと互換性について
本プラグインのビルドおよび動作には、以下のサードパーティライブラリおよび SDK を使用しています：

| コンポーネント | 著作権者 | ライセンス | 備考 |
|---|---|---|---|
| **PitchTunePlugin (本プロジェクト)** | Copyright (c) 2026 28futaba | **GPL-3.0** | 完全なソースコードを本リポジトリで公開しています。 |
| **JUCE 8** | Copyright (c) 2024 Raw Material Software Limited / PACE Anti-Piracy, Inc. | **AGPL-3.0** / 商用ライセンス | オープンソース版 JUCE 8 に準拠。静的リンクされた配布バイナリは AGPL-3.0 の条件に従います。 |
| **Celemony ARA SDK** | Copyright (c) Celemony Software GmbH | **Apache 2.0** | GPL-3.0 と互換。詳細は [NOTICE](NOTICE) を参照してください。 |
| **Steinberg VST3 SDK** | Copyright (c) 2025 Steinberg Media Technologies GmbH | **MIT** | GPL-3.0 と互換。 |

### 商標に関する注記
- **VST** は Steinberg Media Technologies GmbH の登録商標です。
- **ARA** (Audio Random Access) は Celemony Software GmbH の技術および商標です。
- **Melodyne** は Celemony Software GmbH の登録商標です。
- **Waves Tune** は Waves Audio Ltd. の登録商標です。

### バイナリ配布に関するご案内
本プラグインのビルド済みバイナリ（`.vst3`）を再配布または利用する場合、GPL-3.0 / AGPL-3.0 の条項に従い、対応する完全なソースコード（本 GitHub リポジトリ: https://github.com/28futaba/PitchTunePlugin ）へのアクセスを提供する必要があります。
