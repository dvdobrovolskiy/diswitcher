import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'litert_tools'))
from local_model import Model, mapped, language, source_language

MODEL = ROOT/'models/language_model.bin'
PROBE = ROOT/'build-msvc-Release/model_probe.exe'

class LocalModelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.model = Model.load(MODEL)

    def probe(self, rows, model=MODEL):
        p = subprocess.run([str(PROBE),str(model)],input='\n'.join(rows)+'\n',
                           capture_output=True,encoding='utf-8',check=True)
        return [line.split() for line in p.stdout.splitlines()]

    def test_c_python_policy_and_probability_match(self):
        rng = random.Random(45)
        words = rng.sample(list(self.model.words),1000)
        samples = []
        for word in words:
            other = mapped(word)
            if source_language(other) is not None:
                samples.append(other)
            if language(other) is not None:
                samples.append(word)
        native = self.probe([f'score\t{w}\t{mapped(w)}' for w in samples])
        for w,row in zip(samples,native):
            apply,p,reason = self.model.decide(w)
            self.assertEqual(int(row[0]),int(apply),w)
            self.assertEqual(row[2],reason,w)
            if reason in ('model_accept','uncertain'):
                self.assertAlmostEqual(float(row[1]),p,places=5,msg=w)

    def test_latest_feedback_wins_without_duplicates(self):
        original='ghbdtn'; target='привет'
        rows=self.probe([f'learn\t{original}\t{target}\t0',
                         f'score\t{original}\t{target}',
                         f'learn\t{original}\t{target}\t1',
                         f'learn\t{original}\t{target}\t1'])
        self.assertEqual([r[0] for r in rows],['0','0','1','1'])
        self.assertTrue(all(r[3]=='1' for r in rows))
        self.assertEqual(rows[2],rows[3])

    def test_regression_cases(self):
        cases=[json.loads(line) for line in (ROOT/'tests/regressions.jsonl').read_text(encoding='utf-8').splitlines()]
        native=self.probe([f"score\t{c['original']}\t{mapped(c['original']) or 'invalid'}" for c in cases])
        for case,row in zip(cases,native):
            self.assertEqual(int(row[0]),case['label'],case['original'])

    def test_truncated_and_wrong_format_models_fail_closed(self):
        raw=MODEL.read_bytes()
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'bad.bin'
            for data in (b'',raw[:80],raw[:-1],b'BADMAGIC'+raw[8:],raw+b'extra'):
                path.write_bytes(data)
                result=subprocess.run([str(PROBE),str(path)],capture_output=True)
                self.assertEqual(result.returncode,4)

if __name__=='__main__':
    unittest.main()
