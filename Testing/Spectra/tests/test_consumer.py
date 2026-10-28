"""Consumer contract tests against exported synthetic C++ recorder output."""
from pathlib import Path
import copy,json,sys,unittest
C=Path(__file__).resolve().parents[1];sys.path.insert(0,str(C/'tools'))
from parse_marker_sidecar import read_records,validate_record,validate_summary,describe
class ConsumerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.rows=read_records(C/'fixtures/synthetic-measurements.jsonl')
    def test_fixture_summary_and_values(self):
        validate_summary(json.loads((C/'fixtures/synthetic-summary.json').read_text()),self.rows)
        replies=[r for r in self.rows if r['record_type']=='measurement_reply']
        self.assertEqual(len(replies),8)
        self.assertEqual(replies[0]['tools'][0]['markers'][0]['xyz_mm'],[7,8,1000])
        self.assertEqual(replies[0]['tools'][0]['pose']['translation_mm'],[100,200,300])
        self.assertIsNone(replies[2]['tools'][0]['markers'][2]['xyz_mm'])
        self.assertEqual(describe(self.rows)['source_provenance'],'synthetic_fixture')
    def test_stale_missing_xyz_rejected(self):
        r=copy.deepcopy(self.rows[1]);r['tools'][0]['markers'][1]['xyz_mm']=[1,2,3]
        with self.assertRaisesRegex(ValueError,'missing marker'):validate_record(r)
    def test_crc_tampering_rejected(self):
        r=copy.deepcopy(self.rows[1]);raw=bytearray.fromhex(r['raw_reply_hex']);raw[20]^=1;r['raw_reply_hex']=raw.hex()
        with self.assertRaisesRegex(ValueError,'CRC mismatch'):validate_record(r)
    def test_fake_marker_timestamp_rejected(self):
        r=copy.deepcopy(self.rows[1]);r['tools'][0]['marker_device_timestamp']=123
        with self.assertRaisesRegex(ValueError,'unavailable'):validate_record(r)
if __name__=='__main__':unittest.main(verbosity=2)
