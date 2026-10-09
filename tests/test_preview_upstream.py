"""Verify upstream previews preserve PAX changes and never write active sources."""
from pathlib import Path
import json, shutil, subprocess, sys, tempfile, unittest

class PreviewTests(unittest.TestCase):
    def run_preview(self, ours, theirs):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); repo = root / 'upstream/demo'; repo.mkdir(parents=True)
            def git(*args):
                return subprocess.check_output(['git', '-C', str(repo), *args], stderr=subprocess.DEVNULL).decode().strip()
            git('init'); git('config', 'user.name', 'Test'); git('config', 'user.email', 'test@example.invalid')
            (repo / 'core').mkdir(); source = repo / 'core/main.c'
            source.write_text('one\ntwo\nthree\n'); git('add', '.'); git('commit', '-m', 'base'); base = git('rev-parse', 'HEAD')
            source.write_text(theirs); git('add', '.'); git('commit', '-m', 'update')
            active = root / 'apps/demo/core/main.c'; active.parent.mkdir(parents=True); active.write_text(ours)
            (root / 'tools').mkdir(); shutil.copy2(Path(__file__).resolve().parents[1] / 'tools/preview_upstream.py', root / 'tools/preview_upstream.py')
            (root / 'upstream-bases.json').write_text(json.dumps({'demo': dict(repository='upstream/demo', ref=base, upstream_path='core', active_path='apps/demo/core', files=['main.c'])}))
            out = root / 'preview'
            result = subprocess.run([sys.executable, str(root / 'tools/preview_upstream.py'), 'demo', '--ref', 'HEAD', '--out', str(out)], capture_output=True, text=True)
            self.assertEqual(active.read_text(), ours)
            return result.returncode, json.loads((out / 'report.json').read_text()), (out / 'update.patch').read_text() if (out / 'update.patch').exists() else None, (out / 'candidates/main.c').read_text()

    def test_clean_merge_keeps_local_changes(self):
        code, report, patch, candidate = self.run_preview('PAX one\ntwo\nthree\n', 'one\ntwo\nupstream three\n')
        self.assertEqual(code, 0); self.assertFalse(report['conflicts'])
        self.assertEqual(candidate, 'PAX one\ntwo\nupstream three\n')
        self.assertIn('apps/demo/core/main.c', patch)

    def test_conflict_has_no_applicable_patch(self):
        code, report, patch, candidate = self.run_preview('PAX one\ntwo\nthree\n', 'upstream one\ntwo\nthree\n')
        self.assertEqual(code, 1); self.assertEqual(report['conflicts'], ['main.c'])
        self.assertIsNone(patch); self.assertIn('<<<<<<< PAX', candidate)
