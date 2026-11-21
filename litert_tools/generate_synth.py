from __future__ import annotations

import argparse
import json
import random
from pathlib import Path

from common import Sample, extract_features, map_en_to_ru, map_ru_to_en


EN_WORDS = [
    "hello",
    "world",
    "scandal",
    "constitution",
    "book",
    "market",
    "python",
    "keyboard",
    "language",
    "switch",
    "download",
    "windows",
    "developer",
    "project",
    "feature",
    "random",
    "training",
    "model",
]

RU_WORDS = [
    "привет",
    "мир",
    "книга",
    "рынок",
    "проект",
    "функция",
    "модель",
    "обучение",
    "конституция",
    "раньше",
    "больше",
    "шерегеше",
    "время",
    "слово",
    "раскладка",
]


def gen_positive_samples(rng: random.Random, n: int) -> list[Sample]:
    out: list[Sample] = []
    for _ in range(n):
        if rng.random() < 0.5:
            corrected = rng.choice(EN_WORDS)
            original = map_en_to_ru(corrected)  # typed in RU layout
            to_english = 1
        else:
            corrected = rng.choice(RU_WORDS)
            original = map_ru_to_en(corrected)  # typed in EN layout
            to_english = 0
        x = extract_features(original, corrected, bool(to_english))
        out.append(Sample(label=1, to_english=to_english, original=original, corrected=corrected, x=x))
    return out


def gen_negative_samples(rng: random.Random, n: int) -> list[Sample]:
    out: list[Sample] = []
    for _ in range(n):
        if rng.random() < 0.5:
            # Correctly typed word (no correction).
            if rng.random() < 0.5:
                original = rng.choice(EN_WORDS)
                corrected = map_en_to_ru(original)
                to_english = 0
            else:
                original = rng.choice(RU_WORDS)
                corrected = map_ru_to_en(original)
                to_english = 1
        else:
            # Random letters (often not a word); still label as "do not autocorrect".
            alphabet = "abcdefghijklmnopqrstuvwxyz"
            k = rng.randint(3, 10)
            original = "".join(rng.choice(alphabet) for _ in range(k))
            corrected = map_en_to_ru(original)
            to_english = 0
        x = extract_features(original, corrected, bool(to_english))
        out.append(Sample(label=0, to_english=to_english, original=original, corrected=corrected, x=x))
    return out


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--n", type=int, default=5000)
    p.add_argument("--seed", type=int, default=1)
    args = p.parse_args()

    rng = random.Random(args.seed)
    pos = gen_positive_samples(rng, args.n // 2)
    neg = gen_negative_samples(rng, args.n - len(pos))
    data = pos + neg
    rng.shuffle(data)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8") as f:
        for s in data:
            f.write(
                json.dumps(
                    {
                        "label": s.label,
                        "to_english": s.to_english,
                        "original": s.original,
                        "corrected": s.corrected,
                        "x": s.x,
                    },
                    ensure_ascii=False,
                )
                + "\n"
            )

    print(f"Wrote {len(data)} samples to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

