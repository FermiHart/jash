"""Real shell regression: bounded active tail, retained archive and hysteresis."""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BIN=Path(os.environ.get('JASH_TEST_BINARY',str(Path(__file__).resolve().parents[1]/'build/jash')))


class LogRetention(unittest.TestCase):
    def run_shell(self,home,allow_degraded=True):
        environment=dict(os.environ,HOME=str(home),JASH_SIGIL='1',JASH_SESSION='retention-test',
                         JASH_SIGIL_ALLOW_DEGRADED='1' if allow_degraded else '0')
        environment.pop('JASH_SIGIL_TEST_TRUNCATE_FAIL',None)
        # This fixture exercises the core LOG fallback. The suite runner's
        # optional development modules would instead hand logging to Sigil.
        # The explicit degraded opt-in is local to this synthetic LOG fixture;
        # the default missing-module refusal is checked separately below.
        environment.pop('JASH_MODULE_DEV_ROOT',None)
        return subprocess.run([str(BIN),'-c','echo retention-ok'],cwd=home,
                              env=environment,capture_output=True,timeout=15)

    def seed(self,home,count,attested=False):
        directory=home/'.jash';directory.mkdir(mode=0o700)
        field='1'*64 if attested else '0'*64
        data=b''.join((f'{index+1} 1700000000.000000000 {field} tag=- rc=0 cmd='+'x'*300+'\n').encode()
                      for index in range(count))
        chain=directory/'chain.log';chain.write_bytes(data);chain.chmod(0o600)
        return chain,data

    def test_missing_module_refuses_without_degraded_opt_in(self):
        with tempfile.TemporaryDirectory(prefix='sigil-retention-') as name:
            home=Path(name);chain,before=self.seed(home,11001)
            result=self.run_shell(home,allow_degraded=False)
            self.assertEqual(result.returncode,125,result.stderr)
            self.assertEqual(result.stdout,b'')
            self.assertIn(b'refusing to run without attestation module',result.stderr)
            self.assertEqual(chain.read_bytes(),before)
            self.assertEqual(list(chain.parent.glob('chain.log.archive.*')),[])

    def test_long_records_below_line_trigger_do_not_rotate(self):
        with tempfile.TemporaryDirectory(prefix='sigil-retention-') as name:
            home=Path(name);chain,before=self.seed(home,10000)
            result=self.run_shell(home)
            self.assertEqual((result.returncode,result.stdout),(0,b'retention-ok\n'))
            self.assertTrue(chain.read_bytes().startswith(before))
            self.assertEqual(list(chain.parent.glob('chain.log.archive.*')),[])
            self.assertNotIn(b'active LOG retained',result.stderr)

    def test_rotation_preserves_full_archive_and_following_commands_do_not_rotate(self):
        with tempfile.TemporaryDirectory(prefix='sigil-retention-') as name:
            home=Path(name);chain,before=self.seed(home,11001)
            first=self.run_shell(home)
            self.assertEqual(first.returncode,0,first.stderr)
            archives=list(chain.parent.glob('chain.log.archive.*'))
            self.assertEqual(len(archives),1)
            archived=archives[0].read_bytes()
            self.assertTrue(archived.startswith(before))
            self.assertEqual(len(chain.read_bytes().splitlines()),5000)
            second=self.run_shell(home)
            self.assertEqual((second.returncode,second.stdout),(0,b'retention-ok\n'))
            self.assertEqual(len(list(chain.parent.glob('chain.log.archive.*'))),1)
            self.assertEqual(hashlib.sha256(archives[0].read_bytes()).digest(),hashlib.sha256(archived).digest())
            self.assertNotIn(b'active LOG retained',second.stderr)

    def test_attested_record_remains_untrimmed(self):
        with tempfile.TemporaryDirectory(prefix='sigil-retention-') as name:
            home=Path(name);chain,before=self.seed(home,11001)
            data=before.splitlines(keepends=True)
            data[0]=data[0].replace(b'0'*64,b'1'*64,1).replace(b'tag=-',b'tag='+b'2'*64,1)
            before=b''.join(data);chain.write_bytes(before)
            result=self.run_shell(home)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertTrue(chain.read_bytes().startswith(before))
            self.assertEqual(list(chain.parent.glob('chain.log.archive.*')),[])


if __name__=='__main__':
    unittest.main()
