"""Portable v1 language model; standard library only at runtime/evaluation."""
from __future__ import annotations
import hashlib
import json
import math
import struct
from pathlib import Path
from common import map_en_to_ru, map_ru_to_en

ALPHABETS = ('abcdefghijklmnopqrstuvwxyz', 'абвгдеёжзийклмнопрстуфхцчшщъыьэюя')
N = 34
SIZE = N ** 3
FEATURES = 8
MAGIC = b'DSLM0001'

def language(word):
    word = word.lower()
    for lang, alphabet in enumerate(ALPHABETS):
        if word and all(c in alphabet for c in word):
            return lang
    return None

def mapped(word):
    lang = source_language(word)
    return map_en_to_ru(word) if lang == 0 else map_ru_to_en(word) if lang == 1 else ''

def source_language(word):
    lang = language(word)
    if lang is None and any(c in ALPHABETS[0] for c in word.lower()) and all(c in ALPHABETS[0]+"[];',.`" for c in word.lower()):
        return 0
    return lang

def group(word):
    return min(word.lower(), mapped(word).lower())

def split(word):
    return int(hashlib.sha256(group(word).encode()).hexdigest()[:8], 16) % 10

def sigmoid(z):
    return 1 / (1 + math.exp(-max(-30, min(30, z))))

class Model:
    def __init__(self):
        self.tables = [[0.] * SIZE for _ in range(2)]
        self.words = {}
        self.weights = [0.] * FEATURES
        self.bias = 0.
        self.threshold = .995

    def score(self, word, lang):
        a = b = 33
        total = 0.
        for char in word + '\0':
            c = 33 if char == '\0' else ALPHABETS[lang].find(char)
            if c < 0:
                return -20.
            total += self.tables[lang][(a*N+b)*N+c]
            a, b = b, c
        return total / (len(word)+1)

    def features(self, original, target):
        original, target = original.lower(), target.lower()
        lang = source_language(original)
        s = self.score(original, lang)
        t = self.score(target, 1-lang)
        f, g = self.words.get(original, 0.), self.words.get(target, 0.)
        return [(t-s)/4, s/8, t/8, f/8, g/8, (g-f)/8,
                min(len(original), 20)/20, float(lang)]

    def predict(self, original, target):
        x = self.features(original, target)
        return sigmoid(self.bias + sum(w*v for w,v in zip(self.weights,x)))

    def decide(self, original):
        target = mapped(original)
        lower = original.lower()
        if not target or len(original) < 3 or len(original) > 64 or language(target) is None:
            return False, 0., 'unsupported'
        if original.isupper() and (len(original) < 6 or self.words.get(lower, 0) > 0
                                   or self.words.get(target.lower(), 0) < 4):
            return False, 0., 'acronym'
        # A known original and ambiguous short words are deliberately protected.
        if self.words.get(lower, 0) >= 3.0:
            return False, 0., 'known_original'
        if self.words.get(target.lower(), 0) < (4.5 if len(original) <= 3 else 3.0):
            return False, 0., 'unknown_target'
        p = self.predict(original, target)
        return p >= self.threshold, p, 'model_accept' if p >= self.threshold else 'uncertain'

    def save(self, path):
        path = Path(path)
        temporary = path.with_suffix(path.suffix+'.tmp')
        with temporary.open('wb') as f:
            f.write(MAGIC)
            f.write(struct.pack('<II', FEATURES, len(self.words)))
            f.write(struct.pack('<10f', *self.weights, self.bias, self.threshold))
            for table in self.tables:
                f.write(struct.pack('<' + 'f'*SIZE, *table))
            for word, freq in sorted(self.words.items()):
                raw = word.encode('utf-16-le')
                f.write(struct.pack('<Hf', len(raw)//2, freq))
                f.write(raw)
        temporary.replace(path)

    @classmethod
    def load(cls, path):
        m = cls()
        with Path(path).open('rb') as f:
            if f.read(8) != MAGIC:
                raise ValueError('Bad language model magic')
            n, count = struct.unpack('<II', f.read(8))
            if n != FEATURES or count > 200000:
                raise ValueError('Invalid language model header')
            vals = struct.unpack('<10f', f.read(40))
            m.weights, m.bias, m.threshold = list(vals[:8]), vals[8], vals[9]
            m.tables = [list(struct.unpack('<'+'f'*SIZE, f.read(SIZE*4))) for _ in range(2)]
            for _ in range(count):
                length, freq = struct.unpack('<Hf', f.read(6))
                if not 1 <= length <= 64:
                    raise ValueError('Invalid word length')
                word = f.read(length*2).decode('utf-16-le')
                m.words[word] = freq
            if f.read(1):
                raise ValueError('Trailing model data')
        return m

def metrics(model, samples):
    tp = fp = tn = fn = 0
    for original, label in samples:
        yes, _, _ = model.decide(original)
        tp += bool(yes and label)
        fp += bool(yes and not label)
        tn += bool(not yes and not label)
        fn += bool(not yes and label)
    return dict(samples=len(samples), correct_changes=tp, unwanted_changes=fp,
                correct_keeps=tn, missed_changes=fn,
                precision=tp/max(1,tp+fp), recall=tp/max(1,tp+fn))
