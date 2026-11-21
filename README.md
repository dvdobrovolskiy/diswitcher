# DiSwitcher

Windows tray app for English/Russian keyboard layout correction, written in C.
Automatic correction runs locally on Space. **Pause** converts the current/last
word or undoes/redoes the latest conversion while the caret is still in place.

The app uses a local character-trigram language model, a public word-frequency
dictionary and a trained classifier. Personal corrections train a bounded local
adapter and a dictionary of explicit keep/convert preferences. Nothing is uploaded.

## Run the new local-AI version

Run `build-msvc-Release\Diswitcher.exe` after building. Keep
`language_model.bin` and `model-licenses\` next to the EXE. The old root-level
`DiSwitcher.EXE` is not updated by the build script.

The tray menu shows **Local AI ready**. If the model is missing or invalid,
automatic switching is disabled; manual Pause conversion remains available.
Use **Switching enabled** to pause all conversions.

Choose **AI learning statistics** in the tray menu to see the local model,
personal keep/convert preferences, feedback history, recent learned words and
current-session correction counts. The window refreshes every five seconds.
Counts and classifier thresholds are not accuracy measurements. Closing this
window leaves the tray app running. `Diswitcher.exe --stats` also opens it when
starting the app.

Only one instance can run. Exit an older instance before launching a new build.

## Independent local learning

- The release model is `language_model.bin` beside the EXE; it is read-only at runtime.
- Personal labels live in `%LOCALAPPDATA%\DiSwitcher\diswitcher.sqlite3`, table
  `local_feedback`. Up to 1,024 latest word preferences are retained.
- Undo with Pause means “keep the original”; manual conversion/redo means
  “convert this word”. Repeated toggles replace the previous label. Not undoing
  a correction is **not** treated as approval.
- On startup, the app refits its small bounded adapter from these labels against
  the shipped baseline. Updating the EXE/model preserves personal preferences.
- The old `litert_model.bin` is retained for compatibility, but does not decide
  automatic corrections when this version runs.

## Diagnostics and privacy

**Record local diagnostics (contains typed words)** is off on each startup.
Enable it when investigating missed or unwanted corrections. Decisions, reasons,
scores and the model ID are saved locally in `decisions.jsonl`; the file rotates
at approximately 5 MB with one previous file retained. Prediction rows have no
ground-truth label and are never used automatically as training data.

Explicit Pause feedback is saved to `ml_samples.jsonl` and SQLite. Existing
**Export DB** / **Export Model Log** actions save local files only. No network
client, analytics, telemetry upload or automatic sharing is included in the app.
The separate `webapp/` downloader is not used by the keyboard switcher.

Use **Open local learning folder** to find these files. Review exports before
sharing: they contain typed words. To exclude an application, create UTF-8
`excluded_apps.txt` in that folder with one executable name per line (for example
`WindowsTerminal.exe`); `#` starts a comment. Restart to reload exclusions.

Native Windows password edit controls are skipped. Browser/custom password fields
cannot all be recognized by this keyboard-hook approach; pause switching or
exclude those applications. Diagnostics should only be enabled during a deliberate
test session.

## Current conservative policy

An uppercase input can qualify for correction when it has at least six characters,
is absent from the dictionary, and maps to a word with Zipf frequency of at least 4.
The usual classifier threshold still applies. Other uppercase inputs, known words, mixed scripts, digits, URL/path/identifier segments and
uncertain candidates are left unchanged unless an applicable explicit personal
word preference exists. Automatic conversion requires a known target word and a
high classifier score. Three-letter words have a stricter frequency requirement;
one/two-letter words require manual conversion. Enter, Tab, navigation and
punctuation do not trigger automatic correction. Focus changes, mouse clicks and
shortcuts clear stale typing/undo state.

This is a token-level model, not a sentence-understanding LLM. The score is a
classifier output, not a calibrated guarantee. Real-world improvements should be
measured with reviewed feedback, not just synthetic examples.

## Build (Windows)

### Prerequisites
- Install a C toolchain:
  - **MSVC Build Tools** (cl.exe) + Windows SDK (recommended), or
  - **MSYS2 MinGW-w64** (gcc.exe).

### PowerShell (auto-detects compiler)
```powershell
.\scripts\build.ps1 -Config Release
.\build-msvc-Release\Diswitcher.exe
```

### Verify changes
```powershell
.\scripts\build.ps1 -Config Release -BuildTests
python -m unittest discover -s tests -v
.\build-msvc-Release\typing_tests.exe .\models\language_model.bin
```
The tests run the actual C decision engine and a mocked keyboard callback. They
do not type into desktop applications or use the user's local data.

See `litert_tools/README.md` for model training, evaluation and the developer-only
workflow for improving future releases from explicitly reviewed local feedback.

Create a distributable ZIP with `scripts/package.ps1`. It uses an explicit file
allowlist and checks the model/report hashes; personal learning files are never
included. The ZIP contains the EXE, public model, attribution and documentation.

### CMake + MSVC
```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
.\build\Release\Diswitcher.exe
```

### Notes
- Production builds do not emit raw key-down/up events to DebugView.
- Some environments may require running elevated for global hooks, depending on target processes and integrity levels.
