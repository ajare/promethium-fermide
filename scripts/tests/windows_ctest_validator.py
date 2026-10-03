"""Focused regression tests for the MSVC evidence validator; no native children."""
import copy
import sys
import importlib.util
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
spec = importlib.util.spec_from_file_location(
    'validator', Path(__file__).resolve().parents[1] / 'validate_windows_ctest.py')
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)


class InventoryTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.TemporaryDirectory()
        self.addCleanup(self.root.cleanup)
        self.inventory = {'tests': []}
        for module in sorted(v.CORE | {'render', 'editor', 'metrics'}):
            exe = Path(self.root.name) / 'Debug' / f'pf-smoke-{module}.exe'
            exe.parent.mkdir(exist_ok=True)
            exe.touch()
            labels = ['smoke', 'functional', 'validation-fast',
                      'core' if module in v.CORE else module]
            if module == 'metrics':
                labels.append('http')
            self.inventory['tests'].append({'name': f'smoke-{module}', 'command': [str(exe)],
                                           'properties': [{'name': 'LABELS', 'value': labels}]})

    def test_complete_direct_inventory(self):
        self.assertEqual(len(v.audit_inventory(self.inventory, False, 'Debug')), 12)

    def test_missing_product_fails(self):
        Path(self.inventory['tests'][0]['command'][0]).unlink()
        with self.assertRaisesRegex(RuntimeError, 'missing required'):
            v.audit_inventory(self.inventory, False, 'Debug')

    def test_missing_startup_fails_gui_inventory(self):
        with self.assertRaisesRegex(RuntimeError, 'missing direct modules'):
            v.audit_inventory(self.inventory, True, 'Debug')

    def test_wrong_configuration_fails(self):
        with self.assertRaisesRegex(RuntimeError, 'wrong product configuration'):
            v.audit_inventory(self.inventory, False, 'Release')

    def test_labels_and_duplicate_coverage_fail(self):
        changed = copy.deepcopy(self.inventory)
        changed['tests'][0]['properties'][0]['value'].append('editor')
        with self.assertRaisesRegex(RuntimeError, 'labels'):
            v.audit_inventory(changed, False, 'Debug')
        self.inventory['tests'].append(copy.deepcopy(self.inventory['tests'][0]))
        with self.assertRaisesRegex(RuntimeError, 'duplicate'):
            v.audit_inventory(self.inventory, False, 'Debug')


class ResultTests(unittest.TestCase):
    def audit(self, output, skipped=False, name='smoke-persistence'):
        suite = ET.Element('testsuite')
        case = ET.SubElement(suite, 'testcase', name=name)
        ET.SubElement(case, 'system-out').text = output
        if skipped:
            ET.SubElement(case, 'skipped')
        return v.audit_results(suite, {'tests': [{'name': name}]},
                               {'persistence': ['transactional-permissions']})

    def test_optional_check_skip_has_reason(self):
        result = self.audit('SKIP persistence transactional-permissions: POSIX symlink/permission semantics unavailable\n')
        self.assertEqual(len(result['skipped_checks']), 1)

    def test_missing_required_capability_cannot_skip(self):
        with self.assertRaisesRegex(RuntimeError, 'unexpected check skip'):
            self.audit('SKIP persistence transactional-permissions: missing fixture\n')

    def test_optional_egl_skip(self):
        result = self.audit('Could not load EGL library', True, 'willpower_resource_manager_gui_smoke')
        self.assertEqual(len(result['skipped_tests']), 1)

    def test_other_test_skip_fails(self):
        with self.assertRaisesRegex(RuntimeError, 'unexpected CTest skip'):
            self.audit('missing executable', True)

    def test_unexecuted_registry_entry_fails(self):
        with self.assertRaisesRegex(RuntimeError, 'disagrees with registry'):
            self.audit('')


if __name__ == '__main__':
    unittest.main()
