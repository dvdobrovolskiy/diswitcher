# Local model training and evaluation

The current app uses `models/language_model.bin` (DSLM0001), not the legacy
80-byte `litert_model.bin`, for automatic correction. This file includes English
and Russian trigram log probabilities, approximately 96,000 frequency entries,
eight classifier weights, a bias and a decision threshold. No runtime Python,
GPU, Internet connection or paid service is required.

## Rebuild the public baseline

```powershell
python -m venv .venv-model
.\.venv-model\Scripts\python.exe -m pip install -r litert_tools/requirements-model.txt
.\.venv-model\Scripts\python.exe litert_tools/train_local.py --out models/language_model.bin
.\scripts\build.ps1 -Config Release -BuildTests
python -m unittest discover -s tests -v
.\build-msvc-Release\typing_tests.exe models/language_model.bin
```

Only the development environment needs wordfreq. Package the model with
`models/licenses/`; the derived data is CC BY-SA 4.0. The model is written
atomically. The JSON report records its SHA-256, training parameters and metrics.

The corpus is wordfreq 3.1.1's top 50,000 entries per language, filtered to
single-script words of length 3–64. The public dictionary is a fixed resource
available to every split. A stable hash groups both layout spellings of a word
into 80% training / 10% validation / 10% test. Held-out labels and character
sequences do not train the classifier/trigram tables. Both positive synthetic
layout errors and correctly typed negative examples are evaluated. The report
does **not** measure real-world accuracy or unknown-vocabulary generalization.

The threshold is conservatively fixed at 0.995. It is not selected on the test
set. Unknown target words and known originals are protected by additional rules.
Review precision, unwanted changes and missed changes separately.

## Improve releases using the developer's own app

1. Run the app and use Pause to correct mistakes. Optionally enable local
   decision diagnostics to capture rejected candidates and their reasons.
2. Use **Open local learning folder** or **Export Model Log**. Copy the examples
   you want to inspect to `local-data/` (git-ignored). Nothing is sent anywhere.
3. Review a subset into JSONL rows such as
   `{"original":"ghbdtn","label":1}` or `{"original":"hello","label":0}`.
   Label 1 means a layout conversion is wanted; 0 means leave it unchanged.
   Keep unrelated private words out of the reviewed dataset. Do not label an
   uncorrected prediction as successful just because it was not undone.
4. Establish a baseline with the replay tool, then train a candidate separately:

```powershell
python litert_tools/evaluate_local.py --model models/language_model.bin --data local-data/reviewed.jsonl
.\.venv-model\Scripts\python.exe litert_tools/train_local.py --feedback local-data/reviewed.jsonl --out local-data/candidate.bin
python litert_tools/evaluate_local.py --model local-data/candidate.bin --data local-data/holdout.jsonl
```

Keep `holdout.jsonl` independent from the reviewed training file. Stable split
assignment also applies to developer examples during training. The latest
explicit label for each word replaces its earlier synthetic training label.
Replaying training examples alone is not evidence of generalization. Only
promote a candidate after checking the independent holdout, regression tests,
and ordinary typing in representative applications.

5. To promote an approved candidate, copy `candidate.bin` to
   `models/language_model.bin`, retain its matching report and attribution, and
   rebuild. Ship the EXE, baseline model and licenses. Do **not** package SQLite,
   diagnostic exports, `ml_samples.jsonl`, `local-data/`, or personal exceptions.

`--feedback` is explicit and developer-only. The trainer never scans LocalAppData
or imports a user's records automatically. Reviewed feedback adjusts classifier
parameters; it does not add private word strings to the shipped dictionary.
Ordinary installations learn independently through their local SQLite records.

## Legacy prototype tools

The following scripts are retained for comparison with the old implementation.
Their output no longer controls automatic correction in the new app.

This folder contains a small Python project that:
- Generates synthetic wrong-layout samples (EN<->RU) for DiSwitcher.
- Trains a tiny logistic-regression model compatible with the app’s `litert_model.bin`.

### Create synthetic dataset
```powershell
python .\litert_tools\generate_synth.py --out .\litert_tools\synth.jsonl --n 5000 --seed 1
```

### Train model (writes `litert_model.bin` v2)
```powershell
python .\litert_tools\train_model.py --data .\litert_tools\synth.jsonl --out .\litert_tools\litert_model.bin --epochs 8 --lr 0.03 --l2 0.0005 --threshold 0.55
```

### Bundle into the app

Copy the trained model next to the EXE as `litert_model.bin`:
```powershell
Copy-Item .\litert_tools\litert_model.bin .\build-msvc-Release\litert_model.bin -Force
```

On startup, if `%LOCALAPPDATA%\DiSwitcher\litert_model.bin` does not exist, the app will copy the bundled model from the EXE directory.

