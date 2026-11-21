from __future__ import annotations

import argparse
import json
import math
import struct
from dataclasses import dataclass
from pathlib import Path

from common import LITERT_FEATURES


def sigmoid(z: float) -> float:
    if z >= 20.0:
        return 1.0
    if z <= -20.0:
        return 0.0
    return 1.0 / (1.0 + math.exp(-z))


@dataclass
class Model:
    w: list[float]
    b: float
    lr: float
    l2: float
    threshold: float
    steps: int

    def predict(self, x: list[float]) -> float:
        z = self.b
        for wi, xi in zip(self.w, x):
            z += wi * xi
        return sigmoid(z)

    def train_step(self, x: list[float], label: int) -> None:
        y = 1.0 if label else 0.0
        p = self.predict(x)
        err = y - p
        for i in range(LITERT_FEATURES):
            self.w[i] += self.lr * (err * x[i] - self.l2 * self.w[i])
            self.w[i] = max(-10.0, min(10.0, self.w[i]))
        self.b += self.lr * err
        self.b = max(-10.0, min(10.0, self.b))
        self.steps = min(0xFFFFFFFF, self.steps + 1)


def init_seed_model(lr: float, l2: float, threshold: float) -> Model:
    w = [0.0] * LITERT_FEATURES
    b = -3.0
    w[0] = 0.5
    w[1] = 0.2
    w[2] = -0.15
    w[3] = 0.6
    w[4] = 0.25
    return Model(w=w, b=b, lr=lr, l2=l2, threshold=threshold, steps=0)


def load_jsonl(path: Path) -> list[tuple[list[float], int]]:
    out: list[tuple[list[float], int]] = []
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            x = obj["x"]
            label = int(obj["label"])
            if len(x) != LITERT_FEATURES:
                raise ValueError(f"Bad feature length: {len(x)}")
            out.append((list(map(float, x)), label))
    return out


def save_model_bin(path: Path, m: Model) -> None:
    # Must match the app’s on-disk format:
    # magic u32, ver u32 (=2), n u32 (=12), w[12] f32, b f32, lr f32, l2 f32, threshold f32, steps u32
    magic = 0x54524C4A  # 'J' 'L' 'R' 'T'
    ver = 2
    n = LITERT_FEATURES

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as f:
        f.write(struct.pack("<III", magic, ver, n))
        f.write(struct.pack("<" + "f" * n, *[float(v) for v in m.w]))
        f.write(struct.pack("<f", float(m.b)))
        f.write(struct.pack("<f", float(m.lr)))
        f.write(struct.pack("<f", float(m.l2)))
        f.write(struct.pack("<f", float(m.threshold)))
        f.write(struct.pack("<I", int(m.steps) & 0xFFFFFFFF))


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--data", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--epochs", type=int, default=8)
    p.add_argument("--lr", type=float, default=0.03)
    p.add_argument("--l2", type=float, default=0.0005)
    p.add_argument("--threshold", type=float, default=0.55)
    args = p.parse_args()

    data = load_jsonl(args.data)
    m = init_seed_model(lr=args.lr, l2=args.l2, threshold=args.threshold)

    for _ in range(args.epochs):
        for x, label in data:
            m.train_step(x, label)

    save_model_bin(args.out, m)
    print(f"Wrote model to {args.out} (steps={m.steps})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

