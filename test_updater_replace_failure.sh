#!/bin/bash
# Exercise the generated replacement script under partial writes and rename failures.
set -euo pipefail
python3 - "${1:-src/updater.c}" <<'PY'
import re
import subprocess
import sys
import tempfile
from pathlib import Path

source = Path(sys.argv[1]).read_text()
start = source.index('    char script_content[32768];')
end = source.index('    free(escaped_current_exe_path);', start)
block = source[start:end]
with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'script.c'
    c.write_text('#include <stdio.h>\nstatic unsigned long GetCurrentProcessId(void) { return 4242; }\nint main(void) {\n' +
        'char *escaped_exe_name="nosleep.exe", *escaped_current_exe_path="C:\\\\app\\\\nosleep.exe", '
        '*escaped_downloaded_path="C:\\\\temp\\\\download.exe", *escaped_arguments_path="args", '
        '*escaped_internal_marker="relay", *downloaded_path_error_line="";\n' +
        block + '\n(void)written; puts(script_content); return 0; }\n')
    binary = Path(directory) / 'script'
    subprocess.run(['cc', '-Wall', '-Wextra', str(c), '-o', str(binary)], check=True)
    script = subprocess.check_output([str(binary)], text=True)

lines = [line.strip() for line in script.splitlines()]
labels = {line[1:]: i for i, line in enumerate(lines) if line.startswith(':')}
current = r'C:\app\nosleep.exe'
download = r'C:\temp\download.exe'

def run(failure=None, collision=None, relay_status=0):
    files = {current: b'old executable', download: b'new executable', 'args': b'args'}
    if collision:
        files[current + collision] = b'existing unrelated file'
    status, pc, steps = 0, labels['REPLACE'], 0
    while pc < len(lines):
        steps += 1
        assert steps < 200, 'script did not terminate'
        line = lines[pc]
        pc += 1
        match = re.match(r'if exist "([^"]+)" goto (\w+)', line, re.I)
        if match:
            if match[1] in files: pc = labels[match[2]]
            continue
        match = re.match(r'if errorlevel (\d+) goto (\w+)', line, re.I)
        if match:
            if status >= int(match[1]): pc = labels[match[2]]
            continue
        if line.startswith('if not "%errorlevel%"=="0" goto '):
            if status != 0: pc = labels[line.split()[-1]]
            continue
        match = re.match(r'goto (\w+)', line, re.I)
        if match:
            pc = labels[match[1]]
            continue
        match = re.match(r'(copy|move) /Y "([^"]+)" "([^"]+)"', line, re.I)
        if match:
            operation, src, dst = match.groups()
            status = 0
            if operation.lower() == 'copy' and failure == 'copy':
                files[dst] = b'partial'
                status = 1
            elif operation.lower() == 'move' and (
                failure == 'backup' and src == current or
                failure in ('install', 'rollback') and src.endswith('.update') or
                failure == 'rollback' and src.endswith('.backup')):
                status = 1
            else:
                files[dst] = files[src]
                if operation.lower() == 'move': del files[src]
            continue
        match = re.match(r'del "([^"]+)"', line, re.I)
        if match:
            files.pop(match[1], None)
            continue
        if line.startswith('start '):
            status = relay_status
            continue
        if line.startswith('exit /b'): break
    return files

for failure in ('copy', 'backup', 'install'):
    files = run(failure)
    assert files.get(current) == b'old executable', (failure, files)
    assert download in files, 'failure discarded the retry download'
    assert current + '.update' not in files, 'partial stage was retained'
files = run('rollback')
assert files.get(current + '.backup') == b'old executable', 'failed rollback lost backup'
for suffix in ('.update', '.backup'):
    files = run(collision=suffix)
    assert files[current] == b'old executable'
    assert files[current + suffix] == b'existing unrelated file', 'collision was overwritten'
files = run()
assert files[current] == b'new executable', 'successful install did not replace executable'
assert current + '.backup' not in files, 'successful install retained backup'
files = run(relay_status=1)
assert files[current] == b'old executable', 'failed relay start did not restore original executable'
assert download in files, 'failed relay start discarded the recovery download'
files = run(relay_status=2)
assert files[current] == b'new executable', 'failed child stop attempted a rollback over a running process'
assert files.get(current + '.backup') == b'old executable', 'failed child stop discarded the rollback backup'
print('PASS: generated updater preserves executable across copy/install/rollback failures and collisions')
PY
