# PitchTunePlugin (ARA 2.0 手動ピッチ補正 VST3 プラグイン)

JUCE 8 と Celemony ARA 2.0 SDK を用いて開発されたオフライン手動ピッチ補正 VST3 プラグインです。

---

## 🌟 主な特徴

- **ARA 2.0 (Audio Random Access) 完全対応**
  - DAW（Studio One, Reaper, Cubase, Logic Pro 等）のオーディオトラックやクリップと直接同期。
  - 音声クリップを自動解析し、ノート情報と詳細なピッチ軌跡（セント単位）を取得。
- **直感的なピアノロール GUI エディタ**
  - C2〜C7 のピアノキーボードと秒単位のタイムライングリッド。
  - 解析された音符がノートブロックおよびピッチ軌跡（黄色曲線）として可視化。
- **ドラッグ＆ドロップによる音高変更（転調・ピッチ補正）**
  - ノートブロックを上下にドラッグするだけで、瞬時に目的の音階（MIDI Note）へピッチシフト。
  - 移動前の音高はゴーストブロックとして残るため、変更差分を視覚的に把握可能。
- **リアルタイム再生レンダリング (`ARAPlaybackRenderer`)**
  - DAW の再生ヘッドと同期し、ユーザーの編集内容に合わせてピッチシフト DSP を適用したオーディオをリアルタイム出力。
- **リアルタイムフォールバックモード**
  - ARA 非対応の DAW ホストや通常インサート時でも、自動ピッチ補正（オートチューン）として動作するデュアルモード設計。

---

## 📂 ファイル構成

- `Source/PluginProcessor.h` / `Source/PluginProcessor.cpp`:
  - `juce::AudioProcessorARAExtension` を継承したメインプロセッサ。
  - `PitchTunePluginPlaybackRegion`: DAW クリップごとの解析・ノート管理。
  - `PitchTunePluginPlaybackRenderer`: 再生時のピッチシフトレンダリング。
  - `PitchTunePluginDocumentController`: ドキュメント全体の統括とバックグラウンド解析スレッド。
- `Source/PluginEditor.h` / `Source/PluginEditor.cpp`:
  - `juce::AudioProcessorEditorARAExtension` を継承したプラグイン GUI。
  - スクロール可能な Viewport とピアノロールを統合。
- `Source/PianoRollComponent.h` / `Source/PianoRollComponent.cpp`:
  - ピアノロール描画、マウスドラッグによる音符移動、ピッチカーブのリアルタイム描画。
- `Source/PitchDetector.h` / `Source/PitchDetector.cpp`: 自乗差分 (YIN/NSDF) ベースの高精度基本周波数 (F0) 推定エンジン。
- `Source/PitchShifter.h` / `Source/PitchShifter.cpp`: ピッチシフター DSP (Overlap-Add / Phase Vocoder)。
- `Source/ARALibraryIncludes.cpp`: Celemony ARA SDK 2.3.0 の実装ソースバンドル。
- `install_vst3.bat` / `install_vst3.vbs`: ビルドされた VST3 プラグインをシステム共通プラグインフォルダにワンクリックで昇格コピーするインストーラー。

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
   - プラグイン画面にボーカルのノートブロックとピッチ軌跡が表示されます。
   - ノートブロックを上下にドラッグしてピッチを補正・転調します。
4. **再生**:
   - DAW の再生ボタンを押すと、補正された音声がリアルタイムで再生されます。

---

## 📄 ライセンス

本プロジェクトは [GNU General Public License v3.0 (GPL-3.0)](LICENSE) の下で公開されています。
JUCE 8 および Steinberg VST3 SDK、Celemony ARA SDK のオープンソースライセンス条項に準拠しています。
