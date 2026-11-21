from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Sequence


TOKEN_MAX_CHARS = 64
LITERT_FEATURES = 12


# Physical-keyboard mapping for QWERTY <-> ЙЦУКЕН (lowercase).
RU_TO_EN = {
    "й": "q",
    "ц": "w",
    "у": "e",
    "к": "r",
    "е": "t",
    "н": "y",
    "г": "u",
    "ш": "i",
    "щ": "o",
    "з": "p",
    "х": "[",
    "ъ": "]",
    "ф": "a",
    "ы": "s",
    "в": "d",
    "а": "f",
    "п": "g",
    "р": "h",
    "о": "j",
    "л": "k",
    "д": "l",
    "ж": ";",
    "э": "'",
    "я": "z",
    "ч": "x",
    "с": "c",
    "м": "v",
    "и": "b",
    "т": "n",
    "ь": "m",
    "б": ",",
    "ю": ".",
    "ё": "`",
}

EN_TO_RU = {v: k for k, v in RU_TO_EN.items()}


EN_COMMON_BIGRAMS: Sequence[str] = (
    "th",
    "he",
    "in",
    "er",
    "an",
    "re",
    "on",
    "at",
    "en",
    "nd",
    "ti",
    "es",
    "or",
    "te",
    "of",
    "ed",
    "is",
    "it",
    "al",
    "ar",
    "st",
    "to",
    "nt",
    "ng",
    "se",
    "ha",
    "as",
    "ou",
    "io",
    "le",
    "oo",
    "ck",
    "ok",
    "bo",
    "ee",
    "ly",
    "gh",
    "ht",
    "ig",
    "sl",
    "li",
)

RU_COMMON_BIGRAMS: Sequence[str] = (
    "ст",
    "но",
    "то",
    "на",
    "ен",
    "ов",
    "ни",
    "ра",
    "во",
    "ко",
    "пр",
    "по",
    "ер",
    "ро",
    "ос",
    "ал",
    "та",
    "ва",
    "не",
    "ли",
    "ре",
)

RU_BAD_BIGRAMS: Sequence[str] = (
    "щщ",
    "ъъ",
    "ыы",
    "йй",
    "ьъ",
    "ъь",
    "жы",
    "шы",
    "ьш",
    "ьп",
    "ьр",
    "ьт",
    "ьн",
    "ьс",
    "ьк",
    "ьг",
    "ьм",
    "ьд",
    "ьж",
    "ьз",
    "ьх",
    "ьц",
    "ьч",
    "ьщ",
    "ьф",
    "ьл",
    "ьв",
    "ьб",
)


def is_latin_letter(ch: str) -> bool:
    return ("A" <= ch <= "Z") or ("a" <= ch <= "z")


def is_cyrillic_letter(ch: str) -> bool:
    code = ord(ch)
    return (0x0400 <= code <= 0x04FF) or (0x0500 <= code <= 0x052F)


def to_lower_invariant(ch: str) -> str:
    return ch.lower()


def ends_with(s: str, suffix: str) -> bool:
    return s.endswith(suffix)


def find_bigram_score(token_lower: str, pairs: Sequence[str]) -> int:
    hits = 0
    for i in range(len(token_lower) - 1):
        bg = token_lower[i : i + 2]
        if bg in pairs:
            hits += 1
    return hits


def count_bad_bigrams(token_lower: str, bad_pairs: Sequence[str]) -> int:
    hits = 0
    for i in range(len(token_lower) - 1):
        if token_lower[i : i + 2] in bad_pairs:
            hits += 1
    return hits


def vowel_ratio_en(token_lower: str) -> float:
    vowels = set("aeiouy")
    letters = [ch for ch in token_lower if is_latin_letter(ch)]
    if not letters:
        return 0.0
    v = sum(1 for ch in letters if ch in vowels)
    return v / len(letters)


def vowel_ratio_ru(token_lower: str) -> float:
    vowels = set("аеёиоуыэюя")
    letters = [ch for ch in token_lower if is_cyrillic_letter(ch)]
    if not letters:
        return 0.0
    v = sum(1 for ch in letters if ch in vowels)
    return v / len(letters)


def score_english(token_lower: str) -> int:
    latin = 0
    non_latin_letters = 0
    for ch in token_lower:
        if is_latin_letter(ch):
            latin += 1
        elif ch.isalpha():
            non_latin_letters += 1
    if latin == 0:
        return -1000
    if non_latin_letters > 0:
        return -500

    hits = find_bigram_score(token_lower, EN_COMMON_BIGRAMS)
    n = len(token_lower)
    vr = vowel_ratio_en(token_lower)

    score = 0
    score += hits * 3
    if ends_with(token_lower, "ly"):
        score += 8
    if ends_with(token_lower, "ing"):
        score += 8
    if ends_with(token_lower, "ed"):
        score += 5
    if ends_with(token_lower, "ion"):
        score += 5
    if ends_with(token_lower, "tion"):
        score += 6
    if ends_with(token_lower, "ment"):
        score += 6
    if ends_with(token_lower, "ness"):
        score += 6
    if ends_with(token_lower, "able"):
        score += 6
    if ends_with(token_lower, "less"):
        score += 6
    if n >= 4 and vr < 0.20:
        score -= 6
    if vr > 0.75:
        score -= 3
    if n >= 6 and vr < 0.15:
        score -= 10
    score += n
    return score


def score_russian(token_lower: str) -> int:
    cyr = 0
    non_cyr_letters = 0
    for ch in token_lower:
        if is_cyrillic_letter(ch):
            cyr += 1
        elif ch.isalpha():
            non_cyr_letters += 1
    if cyr == 0:
        return -1000
    if non_cyr_letters > 0:
        return -500

    hits = find_bigram_score(token_lower, RU_COMMON_BIGRAMS)
    bad_hits = count_bad_bigrams(token_lower, RU_BAD_BIGRAMS)
    n = len(token_lower)
    vr = vowel_ratio_ru(token_lower)

    score = 0
    score += hits * 3
    score -= bad_hits * 8
    if ends_with(token_lower, "ут"):
        score += 6
    elif ends_with(token_lower, "ют"):
        score += 6
    elif ends_with(token_lower, "ет"):
        score += 5
    elif ends_with(token_lower, "ит"):
        score += 5
    elif ends_with(token_lower, "ть"):
        score += 4
    elif ends_with(token_lower, "ый"):
        score += 4
    elif ends_with(token_lower, "ая"):
        score += 4
    elif ends_with(token_lower, "ое"):
        score += 4
    elif ends_with(token_lower, "ые"):
        score += 4
    if n >= 4 and vr < 0.20:
        score -= 6
    if vr > 0.80:
        score -= 3
    if n >= 6 and vr < 0.15:
        score -= 10
    score += n
    return score


def map_ru_to_en(s: str) -> str:
    out: list[str] = []
    for ch in s:
        lower = to_lower_invariant(ch)
        mapped = RU_TO_EN.get(lower, lower)
        if ch != lower and "a" <= mapped <= "z":
            mapped = mapped.upper()
        out.append(mapped)
    return "".join(out)


def map_en_to_ru(s: str) -> str:
    out: list[str] = []
    for ch in s:
        upper = "A" <= ch <= "Z"
        lower = ch.lower() if upper else ch
        mapped = EN_TO_RU.get(lower, lower)
        if upper:
            mapped = mapped.upper()
        out.append(mapped)
    return "".join(out)


def extract_features(token: str, mapped: str, to_english: bool) -> list[float]:
    token = token[:TOKEN_MAX_CHARS]
    mapped = mapped[:TOKEN_MAX_CHARS]
    lower = "".join(to_lower_invariant(ch) for ch in token)
    mapped_lower = "".join(to_lower_invariant(ch) for ch in mapped)

    latin = sum(1 for ch in lower if is_latin_letter(ch))
    cyr = sum(1 for ch in lower if is_cyrillic_letter(ch))
    digits = sum(1 for ch in lower if ch.isdigit())

    score_en = score_english(lower)
    score_ru = score_russian(lower)
    base = score_ru if cyr > 0 else score_en

    mapped_score = score_english(mapped_lower) if to_english else score_russian(mapped_lower)
    diff = mapped_score - base

    vr_base = vowel_ratio_ru(lower) if cyr > 0 else vowel_ratio_en(lower)
    vr_mapped = vowel_ratio_en(mapped_lower) if to_english else vowel_ratio_ru(mapped_lower)

    bad_hits_base = count_bad_bigrams(lower, RU_BAD_BIGRAMS) if cyr > 0 else 0
    bad_hits_mapped = count_bad_bigrams(mapped_lower, RU_BAD_BIGRAMS) if not to_english else 0

    x = [0.0] * LITERT_FEATURES
    x[0] = diff / 10.0
    x[1] = mapped_score / 20.0
    x[2] = base / 20.0
    x[3] = float(bad_hits_base)
    x[4] = len(lower) / 10.0
    x[5] = float(vr_base)
    x[6] = float(vr_mapped)
    x[7] = latin / 10.0
    x[8] = cyr / 10.0
    x[9] = float(bad_hits_mapped)
    x[10] = 1.0 if to_english else 0.0
    x[11] = 1.0 if digits > 0 else 0.0
    return x


@dataclass(frozen=True)
class Sample:
    label: int
    to_english: int
    original: str
    corrected: str
    x: list[float]

