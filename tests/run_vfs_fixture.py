"""Give each CTest VFS invocation a fresh synthetic directory, including reruns."""
import subprocess
import sys
import tempfile
with tempfile.TemporaryDirectory(prefix='fixer-vfs-') as directory:
    result=subprocess.run([sys.argv[1],sys.argv[2],directory],timeout=20)
    sys.exit(result.returncode)
