"""Train public baseline. Personal logs are used only with explicit --feedback."""
import argparse
import hashlib
import json
import math
import random
from pathlib import Path
from local_model import ALPHABETS, N, SIZE, Model, language, source_language, mapped, split, sigmoid, metrics

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--words', type=int, default=50000)
    p.add_argument('--feedback', type=Path, help='Explicitly reviewed developer feedback JSONL')
    args = p.parse_args()
    from wordfreq import top_n_list, zipf_frequency
    model = Model()
    vocabulary = {}
    for lang, code in enumerate(('en','ru')):
        for w in top_n_list(code, args.words):
            if 3 <= len(w) <= 64 and language(w) == lang:
                vocabulary[w] = zipf_frequency(w, code)
    # The public lexical resource is fixed for all splits. Holdout word labels and
    # character sequences never train the classifier or character language model.
    # This measures unseen decisions with dictionary access, not unseen vocabulary.
    model.words = dict(vocabulary)
    for lang in range(2):
        counts = [0.] * SIZE
        for word, freq in model.words.items():
            if language(word) != lang or split(word) < 2:
                continue
            weight = 10 ** (min(freq, 5.0)-3.0)
            a = b = 33
            for char in word + '\0':
                c = 33 if char == '\0' else ALPHABETS[lang].index(char)
                counts[(a*N+b)*N+c] += weight
                a, b = b, c
        for start in range(0, SIZE, N):
            total = sum(counts[start:start+N]) + .25*N
            model.tables[lang][start:start+N] = [math.log((v+.25)/total) for v in counts[start:start+N]]
    train, validation, test = [], [], []
    for word in vocabulary:
        other = mapped(word)
        if source_language(other) is None:
            continue
        bucket = test if split(word) == 0 else validation if split(word) == 1 else train
        bucket.append((word, 0))
        # Do not label an existing word in the other language as a guaranteed mistake.
        if vocabulary.get(other, 0) < 3:
            bucket.append((other, 1))
    if args.feedback:
        latest = {}
        for line in args.feedback.read_text(encoding='utf-8-sig').splitlines():
            row = json.loads(line)
            word, label = row['original'].lower(), int(row['label'])
            if label not in (0,1) or source_language(word) is None or language(mapped(word)) is None:
                raise ValueError('Invalid reviewed feedback')
            latest[word] = label
        for word, label in latest.items():
            train = [(w,y) for w,y in train if w != word]
            validation = [(w,y) for w,y in validation if w != word]
            test = [(w,y) for w,y in test if w != word]
            bucket = test if split(word) == 0 else validation if split(word) == 1 else train
            bucket.append((word, label))
    # Word features + learned spelling probabilities, weighted against unwanted changes.
    samples = [(model.features(w, mapped(w)), y) for w,y in train]
    rng = random.Random(42)
    for epoch in range(12):
        rng.shuffle(samples)
        for x,y in samples:
            prob = sigmoid(model.bias + sum(a*b for a,b in zip(model.weights,x)))
            err = (y-prob) * (4 if y == 0 else 1)
            for i in range(8):
                model.weights[i] += .025 * (err*x[i] - .0002*model.weights[i])
            model.bias += .025*err
    args.out.parent.mkdir(parents=True, exist_ok=True)
    model.save(args.out)
    # Evaluate held-out spelling classification separately from production's dictionary gate.
    def classifier_stats(rows):
        tp=fp=tn=fn=0
        for w,y in rows:
            yes = model.predict(w,mapped(w)) >= model.threshold
            tp+=bool(yes and y); fp+=bool(yes and not y)
            tn+=bool(not yes and not y); fn+=bool(not yes and y)
        return dict(tp=tp,fp=fp,tn=tn,fn=fn,precision=tp/max(1,tp+fp),recall=tp/max(1,tp+fn))
    report = {'format':1, 'source':'wordfreq 3.1.1', 'seed':42,
              'vocabulary':len(vocabulary), 'bundled_words':len(model.words),
              'train_samples':len(train), 'threshold':model.threshold,
              'reviewed_developer_feedback_used':bool(args.feedback),
              'validation_classifier':classifier_stats(validation),
              'test_classifier':classifier_stats(test),
              'test_production_policy':metrics(model,test),
              'note':'Word-pair grouped label holdout; fixed public dictionary available in all splits. Character LM trained on training words only. Synthetic layout errors, not real-user accuracy.',
              'sha256':hashlib.sha256(args.out.read_bytes()).hexdigest()}
    args.out.with_suffix('.report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__ == '__main__':
    main()
