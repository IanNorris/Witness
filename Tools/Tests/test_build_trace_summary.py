import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('summary', Path(__file__).parents[1] / 'summarize_build_trace.py')
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


class BuildTraceSummaryTests(unittest.TestCase):
    def test_nested_phases_headers_and_redaction(self):
        events = [
            {'ph': 'B', 'pid': 1, 'tid': 0, 'ts': 0, 'name': 'CL Invocation 1',
             'args': {'File Input': 'X:/repo/WitnessCamera/Source/Test.cpp', 'Env Var: Path': 'SECRET'}},
            {'ph': 'B', 'pid': 1, 'tid': 0, 'ts': 0, 'name': 'FrontEndPass'},
            {'ph': 'X', 'pid': 1, 'tid': 0, 'ts': 0, 'dur': 500000, 'name': 'X:/repo/WitnessCamera/Include/Test.h'},
            {'ph': 'E', 'pid': 1, 'tid': 0, 'ts': 1000000},
            {'ph': 'B', 'pid': 1, 'tid': 0, 'ts': 1000000, 'name': 'BackEndPass'},
            {'ph': 'E', 'pid': 1, 'tid': 0, 'ts': 1500000},
            {'ph': 'E', 'pid': 1, 'tid': 0, 'ts': 1500000},
        ]
        result = summary.summarize({'traceEvents': events})
        self.assertEqual(result['aggregatePhaseSeconds'], {'FrontEndPass': 1, 'BackEndPass': .5})
        self.assertEqual(result['slowestTranslationUnits'][0]['source'], 'WitnessCamera/Source/Test.cpp')
        self.assertEqual(result['projectHeaders'][0]['inclusiveSeconds'], .5)
        self.assertNotIn('SECRET', str(result))
        self.assertEqual(result['unmatchedBeginEndEvents'], 0)

    def test_distinct_threads_are_not_mixed(self):
        result = summary.summarize([
            {'ph': 'B', 'pid': 1, 'tid': 0, 'ts': 0, 'name': 'FrontEndPass'},
            {'ph': 'B', 'pid': 1, 'tid': 1, 'ts': 0, 'name': 'BackEndPass'},
            {'ph': 'E', 'pid': 1, 'tid': 0, 'ts': 2000000},
            {'ph': 'E', 'pid': 1, 'tid': 1, 'ts': 3000000},
        ])
        self.assertEqual(result['aggregatePhaseSeconds'], {'FrontEndPass': 2, 'BackEndPass': 3})
        self.assertEqual(result['translationUnitCount'], 0)

    def test_incomplete_trace_is_visible(self):
        result = summary.summarize([{'ph': 'B', 'ts': 0}, {'ph': 'E', 'pid': 99, 'ts': 1}])
        self.assertEqual(result['unmatchedBeginEndEvents'], 2)

    def test_mp_per_file_event_attributes_enclosing_frontend_and_later_backend(self):
        result = summary.summarize([
            {'ph': 'B', 'ts': 0, 'name': 'FrontEndPass'},
            {'ph': 'B', 'ts': 1, 'name': 'X:/repo/WitnessCamera/Source/A.cpp'},
            {'ph': 'E', 'ts': 999999}, {'ph': 'E', 'ts': 1000000},
            {'ph': 'B', 'ts': 1000000, 'name': 'BackEndPass'}, {'ph': 'E', 'ts': 2000000},
        ])
        self.assertEqual(result['translationUnitCount'], 1)
        self.assertEqual(result['slowestTranslationUnits'][0]['totalPhaseSeconds'], 2)
        self.assertEqual(result['slowestTranslationUnits'][0]['source'], 'WitnessCamera/Source/A.cpp')

    def test_reused_invocation_thread_and_included_c_file_do_not_steal_attribution(self):
        events = []
        timestamp = 0
        for source in ('A.cpp', 'B.cpp'):
            events.append({'ph': 'B', 'ts': timestamp, 'name': 'CL Invocation 1',
                           'args': {'File Input': 'X:/repo/WitnessCamera/Source/' + source}})
            events.extend([
                {'ph': 'B', 'ts': timestamp, 'name': 'FrontEndPass'},
                {'ph': 'B', 'ts': timestamp, 'name': 'X:/repo/WitnessCamera/Source/' + source},
                {'ph': 'B', 'ts': timestamp, 'name': 'external/applink.c'},
                {'ph': 'E', 'ts': timestamp + 100},
                {'ph': 'E', 'ts': timestamp + 1000000},
                {'ph': 'E', 'ts': timestamp + 1000000},
                {'ph': 'B', 'ts': timestamp + 1000000, 'name': 'BackEndPass'},
                {'ph': 'E', 'ts': timestamp + 2000000},
                {'ph': 'E', 'ts': timestamp + 2000000},
            ])
            timestamp += 2000000
        result = summary.summarize(events)
        self.assertEqual(result['translationUnitCount'], 2)
        self.assertTrue(all(unit['totalPhaseSeconds'] == 2 for unit in result['slowestTranslationUnits']))

    def test_mp_worker_reuse_prefers_current_source_over_invocation_input(self):
        result = summary.summarize([
            {'ph': 'B', 'ts': 0, 'name': 'CL Invocation 1', 'args': {'File Input': 'A.cpp'}},
            {'ph': 'B', 'ts': 0, 'name': 'FrontEndPass'},
            {'ph': 'B', 'ts': 0, 'name': 'A.cpp'},
            {'ph': 'E', 'ts': 1000000}, {'ph': 'E', 'ts': 1000000},
            {'ph': 'B', 'ts': 1000000, 'name': 'BackEndPass'}, {'ph': 'E', 'ts': 2000000},
            {'ph': 'B', 'ts': 2000000, 'name': 'FrontEndPass'},
            {'ph': 'B', 'ts': 2000000, 'name': 'B.cpp'},
            {'ph': 'E', 'ts': 3000000}, {'ph': 'E', 'ts': 3000000},
            {'ph': 'B', 'ts': 3000000, 'name': 'BackEndPass'}, {'ph': 'E', 'ts': 4000000},
            {'ph': 'E', 'ts': 4000000},
        ])
        self.assertEqual(result['translationUnitCount'], 2)
        self.assertTrue(all(unit['totalPhaseSeconds'] == 2 for unit in result['slowestTranslationUnits']))
        self.assertEqual(result['unattributedPhaseSeconds'], {'FrontEndPass': 0, 'BackEndPass': 0})


if __name__ == '__main__':
    unittest.main()
