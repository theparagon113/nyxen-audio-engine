"""Verify ASan catches a deliberately faulty TEST child, not an engine callback."""
from pathlib import Path
import subprocess
import sys

result = subprocess.run([sys.argv[1], '--asan-positive-control'], capture_output=True, text=True, errors='replace')
text = result.stdout + result.stderr
if result.returncode == 0 or 'AddressSanitizer' not in text or 'heap-buffer-overflow' not in text:
    raise AssertionError(f'ASan did not detect positive control: exit={result.returncode}\n{text}')
folder = Path(sys.argv[2])
folder.mkdir(parents=True, exist_ok=True)
(folder / 'asan-positive-control.txt').write_text(text, encoding='utf-8')
print('PASS ASan detector positive control: deliberate heap-buffer-overflow caught in separate test child')
