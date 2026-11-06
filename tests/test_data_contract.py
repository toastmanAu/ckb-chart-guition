import json, subprocess, sys, pathlib, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class ContractTests(unittest.TestCase):
    def test_mock_sample_bounds(self):
        rows=json.loads(subprocess.check_output([sys.executable,str(ROOT/'tools/mock_metrics.py')]))
        self.assertEqual(len(rows),120)
        self.assertTrue(all(x['source']=='DEMO' for x in rows))
        self.assertTrue(all(x['value']>=1 for x in rows))
        self.assertTrue(all(rows[i]['block_height']<rows[i+1]['block_height'] for i in range(len(rows)-1)))
if __name__=='__main__': unittest.main()
