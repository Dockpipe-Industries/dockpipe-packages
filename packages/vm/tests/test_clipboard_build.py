"""Keep declared Linux runtime inputs visible to clipboard closure inspection."""
import importlib.util
import os
from pathlib import Path
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'clipboard/build.py'
SPECIFICATION = importlib.util.spec_from_file_location('clipboard_builder', SCRIPT)
BUILDER = importlib.util.module_from_spec(SPECIFICATION)
SPECIFICATION.loader.exec_module(BUILDER)


class ClipboardBuildTests(unittest.TestCase):
    def test_declared_dependency_prefix_survives_qt_search_precedence(self):
        inherited = {'LD_LIBRARY_PATH': '/declared/gio/usr/lib/x86_64-linux-gnu', 'OTHER': 'retained'}
        original = dict(inherited)
        result = BUILDER.dependency_environment(Path('/selected/qt'), inherited)
        self.assertEqual(result['LD_LIBRARY_PATH'], os.pathsep.join([
            '/selected/qt/lib', '/declared/gio/usr/lib/x86_64-linux-gnu']))
        self.assertEqual(result['OTHER'], 'retained')
        self.assertEqual(inherited, original)

    def test_absent_dependency_prefix_does_not_add_empty_search_entry(self):
        result = BUILDER.dependency_environment(Path('/selected/qt'), {})
        self.assertEqual(result['LD_LIBRARY_PATH'], '/selected/qt/lib')

    def test_missing_library_diagnostic_names_actual_unresolved_dependencies(self):
        with self.assertRaisesRegex(RuntimeError, r'libEGL\.so\.1, libxcb-cursor\.so\.0'):
            BUILDER.require_resolved_dependencies(
                ' libEGL.so.1 => not found\n libxcb-cursor.so.0 => not found\n'
                ' libQt6Core.so.6 => /selected/qt/lib/libQt6Core.so.6 (0x1234)\n')
        BUILDER.require_resolved_dependencies(' libEGL.so.1 => /declared/libEGL.so.1 (0x1234)\n')

    def test_unexpected_or_excessive_missing_output_is_bounded(self):
        for output in ['not found ' + 'x' * 8192, '\n'.join(
                ' libMissing' + str(index) + '.so => not found' for index in range(33))]:
            with self.subTest(length=len(output)):
                with self.assertRaises(RuntimeError) as error:
                    BUILDER.require_resolved_dependencies(output)
                self.assertLess(len(str(error.exception)), 128)


if __name__ == '__main__':
    unittest.main()
