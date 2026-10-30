"""Keep catalog test discovery connected to the archive-owned bulk suite."""
import importlib.util
from pathlib import Path

TEST_PATH = (Path(__file__).resolve().parents[1]
             / "resolvers/r2/dockpipe.cloudflare.r2bulk/tests/test_r2_bulk.py")
spec = importlib.util.spec_from_file_location("r2_bulk_package_tests", TEST_PATH)
package_tests = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package_tests)
BulkTest = package_tests.BulkTest
