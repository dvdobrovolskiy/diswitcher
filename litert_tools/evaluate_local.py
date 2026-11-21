"""Replay reviewed JSONL against the actual production decision policy."""
import argparse
import json
from local_model import Model, metrics

p = argparse.ArgumentParser()
p.add_argument('--model', required=True)
p.add_argument('--data', required=True)
args = p.parse_args()
model = Model.load(args.model)
latest = {}
with open(args.data, encoding='utf-8-sig') as f:
    for line in f:
        row = json.loads(line)
        if row.get('label') not in (0,1):
            continue  # Predictions are not ground truth.
        latest[row['original']] = int(row['label'])
print(json.dumps(metrics(model,list(latest.items())),indent=2))
for word,label in latest.items():
    yes, prob, reason = model.decide(word)
    if yes != bool(label):
        print(json.dumps(dict(original=word,expected=label,predicted=int(yes),p=prob,reason=reason),ensure_ascii=True))
