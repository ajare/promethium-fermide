"""Inventory/selection regressions and deliberate artifact collision detection."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import smoke_lane as lane

build, config, probe = sys.argv[1:]
sys.argv = sys.argv[:1]
source = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('selector', source / 'scripts/validate_ctest_lane.py')
selector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(selector)


class Lanes(unittest.TestCase):
    def test_inventory_and_selection(self):
        inventory = json.loads(subprocess.check_output(
            ['ctest', '--test-dir', build, '-C', config, '--show-only=json-v1'],
            text=True, timeout=30))['tests']
        tests = {t['name']: t for t in inventory}
        def props(name):
            return {p['name']: p['value'] for p in tests[name]['properties']}
        for module in lane.REPRESENTATIVES:
            functional = props(f'smoke-{module}')
            self.assertIn('functional', functional['LABELS'])
            self.assertIn('validation-fast', functional['LABELS'])
            stress = props(f'smoke-{module}-contract')
            self.assertIn('validation-stress', stress['LABELS'])
            self.assertNotIn('validation-fast', stress['LABELS'])
            if module not in ('world', 'agent'):
                self.assertEqual(stress['PROCESSORS'], 8)
            for kind in ('cli', 'concurrency'):
                properties = props(f'smoke-{module}-{kind}-contract')
                self.assertIn('validation-fast', properties['LABELS'])
                self.assertNotIn('validation-stress', properties['LABELS'])
                if kind == 'concurrency':
                    self.assertEqual(properties['PROCESSORS'], 4)
        selected = json.loads(subprocess.check_output(
            ['ctest', '--test-dir', build, '-C', config, '-L', '^validation-fast$',
             '--show-only=json-v1'], text=True, timeout=30))['tests']
        self.assertTrue(selected)
        self.assertFalse(any('validation-stress' in props(t['name'])['LABELS'] for t in selected))
        fast_timeouts = {'stabilityLongRun': 10, 'stabilityPathEquivalence': 40,
                       'stabilityResetIsolation': 30,
                       'stabilityMultiWorldIsolation': 40}
        for check in ('stabilityLongRun', 'stabilityPathEquivalence',
                      'stabilityResetIsolation', 'stabilityMultiWorldIsolation'):
            full = props(f'station-{check}')
            fast = props(f'station-{check}Fast')
            self.assertIn('validation-stress', full['LABELS'])
            self.assertNotIn('validation-fast', full['LABELS'])
            self.assertIn('validation-fast', fast['LABELS'])
            self.assertNotIn('validation-stress', fast['LABELS'])
            self.assertEqual(fast['TIMEOUT'], fast_timeouts[check])
        # The scheduled pathway command queue was retired at schema 106 (daab06fa),
        # so its checks - pathwayCommands, pathwayCommandOrdering/Reset/Validation
        # and pathwayExteriorCommands - are no longer registered and are not
        # inventoried here. Openness became a live parameter in its place.
        for check in ('pathwayDiffusion', 'pathwayValidation', 'pathwayCounterFlow',
                      'pathwayVacuumEqualization', 'pathwayExteriorDiffusion',
                      'pathwayReferenceConfigurations',
                      'pathwayClosed', 'pathwayOpenness', 'pathwayParallelAdditivity',
                      'pathwayChainTopology', 'pathwayThermalNeutrality', 'pathwayCondensation',
                      'pathwayPersistence',
                      'advectedSignDirection', 'advectedEqualTemperatureInert',
                      'advectedVentClosedForm', 'advectedFlimsyVersusVault',
                      'advectedMassUntouched', 'advectedLedgerCloses',
                      'advectedConductedIndependence', 'advectedVentLossBooked',
                      'advectedValidation',
                      'thermalLinkConservation', 'thermalLinkAdr0026Contrast', 'thermalLinkIdle',
                      'thermalLinkCapacityWeighting', 'thermalLinkNoCrossing',
                      'thermalLinkParallelAdditivity', 'thermalLinkDeclarationOrder',
                      'thermalLinkMassUntouched', 'thermalLinkPathwayCoexistence',
                      'thermalLinkPathwayIndependence',
                      'thermalLinkReservoirCoexistence', 'thermalLinkValidation',
                      'thermalLinkResetIsolation', 'thermalLinkPersistence',
                      'thermalLinkSameTickCondensation',
                      'thermalLinkReverseNoSpuriousCondensation',
                      'thermalLinkParallelWallAdditivity', 'thermalLinkChainConvergence',
                      'thermalLinkChainLedgerAttribution',
                      'thermalLinkDeclarationOrderIndependence',
                      'thermalLinkSharedNeighbourIndependence',
                      'heatEngineTracerBullet', 'heatEngineClosure', 'heatEngineCarnotBound',
                      'heatEngineIdle', 'heatEngineNoCrossing', 'heatEngineSinkTrip',
                      'heatEngineValidation', 'heatEnginePersistence',
                      'heatEngineResetIsolation', 'heatEnginePlantCoexistence'):
            self.assertIn('validation-fast', props(f'station-{check}')['LABELS'])
            self.assertNotIn('validation-stress', props(f'station-{check}')['LABELS'])
        stress_timeouts = {'pathwayLongHorizon': 20, 'pathwayAbsurdGeometry': 10,
                         'thermalLinkChainHomogenization': 30, 'thermalLinkAbsurdGeometry': 20,
                         'advectedVentNearEmpty': 10, 'advectedAbsurdSpecificHeat': 10}
        for check in ('pathwayLongHorizon', 'pathwayAbsurdGeometry',
                      'thermalLinkChainHomogenization', 'thermalLinkAbsurdGeometry',
                      'advectedVentNearEmpty', 'advectedAbsurdSpecificHeat'):
            properties = props(f'station-{check}')
            self.assertIn('validation-stress', properties['LABELS'])
            self.assertNotIn('validation-fast', properties['LABELS'])
            self.assertEqual(properties['TIMEOUT'], stress_timeouts[check])
            self.assertNotIn(f'station-{check}', {t['name'] for t in selected})
        # #801: both thermal-link stress extremes re-run under the step-ratio
        # runner at both bounds and a non-divisor. Each ratio variant is
        # stress-only, carries the timestep-invariance label, and must not
        # leak into the fast selection.
        stress_ratio_timeouts = {
            'station-thermalLink-stress-thermalLinkAbsurdGeometry-1': 10,
            'station-thermalLink-stress-thermalLinkAbsurdGeometry-7': 10,
            'station-thermalLink-stress-thermalLinkAbsurdGeometry-60': 20,
            'station-thermalLink-stress-thermalLinkChainHomogenization-1': 90,
            'station-thermalLink-stress-thermalLinkChainHomogenization-7': 20,
            'station-thermalLink-stress-thermalLinkChainHomogenization-60': 10,
        }
        for name, timeout in stress_ratio_timeouts.items():
            properties = props(name)
            self.assertIn('validation-stress', properties['LABELS'])
            self.assertIn('station-timestep-invariance', properties['LABELS'])
            self.assertNotIn('validation-fast', properties['LABELS'])
            self.assertEqual(properties['TIMEOUT'], timeout)
            self.assertNotIn(name, {t['name'] for t in selected})
        for ratio in (1, 7, 60):
            for check in ('pathwayThermalNeutrality', 'pathwayCondensation', 'thermalLinkConservation'):
                labels = props(f'station-pathway-thermal-{check}-{ratio}')['LABELS']
                self.assertIn('validation-fast', labels)
                self.assertNotIn('validation-stress', labels)
            for check in ('pathwayDiffusion', 'pathwayCounterFlow', 'pathwayVacuumEqualization',
                          'pathwayExteriorDiffusion', 'pathwayClosed', 'pathwayOpenness'):
                labels = props(f'station-timestep-components-{check}-{ratio}')['LABELS']
                self.assertIn('validation-exhaustive', labels)
                self.assertNotIn('validation-fast', labels)
        for module in lane.REPRESENTATIVES:
            self.assertEqual(sum(t['name'] == f'smoke-{module}' for t in selected), 1)
        self.assertIn('headless-tools-cli-contract', {t['name'] for t in selected})
        self.assertNotIn('headless-tools-contract', {t['name'] for t in selected})
        for kind in ('fast', 'final'):
            cmd = selector.command(SimpleNamespace(build_tree=build, config=config,
                                                   parallel=8, lane=kind))
            self.assertEqual('-L' in cmd, kind == 'fast')
            self.assertNotIn('-E', cmd)
            self.assertNotIn('-R', cmd)
            self.assertIn(config, cmd)

    def test_selector_cli(self):
        script = str(source / 'scripts/validate_ctest_lane.py')
        help_result = subprocess.run([sys.executable, script, '--help'],
                                     capture_output=True, text=True, timeout=15)
        self.assertEqual(help_result.returncode, 0)
        self.assertIn('expensive', help_result.stdout)
        for arguments in ([], ['--lane', 'unknown'],
                          ['--build-tree', build, '--config', config,
                           '--lane', 'fast', '--parallel', '0']):
            result = subprocess.run([sys.executable, script, *arguments],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 2)
            self.assertIn('error:', result.stderr)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux helper')
    def test_linux_helper_lane_help_and_misuse(self):
        script = str(source / 'scripts/validate_linux_smoke.sh')
        help_result = subprocess.run(['bash', script, '--list'],
                                     capture_output=True, text=True, timeout=15)
        self.assertEqual(help_result.returncode, 0)
        self.assertIn('--lane fast|final', help_result.stdout)
        self.assertIn('expensive', help_result.stdout)
        result = subprocess.run(['bash', script, '--config', 'Release',
                                 '--lane', 'absent', 'routing'],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 2)
        self.assertIn('--lane must be fast or final', result.stderr)

    def test_actual_probe_cleanup_and_failure_reporting(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([probe, '--check', 'failed-paths'], cwd=directory,
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 1)
            self.assertIn('SUMMARY harness pass=0 fail=1 skip=0', result.stdout)
            root = Path(result.stdout.splitlines()[0][5:])
            self.assertFalse(root.exists())

    def test_deliberate_fixed_path_collision_is_detected(self):
        # Real concurrent children overwrite a fixed artifact and report the same
        # root. They claim success: the isolation contract must still reject them.
        with tempfile.TemporaryDirectory() as directory:
            fake = Path(directory) / 'colliding.py'
            fake.write_text('''import os, pathlib, sys, time
check = sys.argv[-1]
if check == 'paths':
    root = pathlib.Path(os.environ['TMPDIR']) / 'fixed-root'
    try:
        root.mkdir(exist_ok=True)
        (root / 'artifact').write_text('overwritten')
        time.sleep(0.1)
        (root / 'artifact').unlink()
        root.rmdir()
    except OSError:
        pass # Deliberately hides collision errors, just as the mutant claims PASS.
    print('ROOT ' + str(root))
    print('PASS harness paths')
    print('SUMMARY harness pass=1 fail=0 skip=0')
else:
    print('PASS simulation ' + check)
    print('SUMMARY simulation pass=1 fail=0 skip=0')
''')
            args = SimpleNamespace(module='simulation', lane='concurrency', timeout=15,
                                   names=lane.REPRESENTATIVES['simulation'],
                                   binary=[sys.executable, str(fake)],
                                   probe=[sys.executable, str(fake)])
            with self.assertRaisesRegex(AssertionError, 'roots collide'):
                lane.contract(args)


if __name__ == '__main__':
    unittest.main()
