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

def wait(processes):
    pc, status, attempts, polls = 0, 0, 0, 0
    while pc < len(lines):
        line = lines[pc]
        pc += 1
        if line in (':REPLACE', ':FAILED'):
            return line[1:], polls
        if line.startswith('set /a wait_attempts='):
            if '+1' in line: attempts += 1
            else: attempts = int(line.split('=')[1])
        elif line.startswith('tasklist '):
            polls += 1
            assert polls <= 31, 'replacement wait is unbounded'
            match = re.search(r'/FI "(PID|IMAGENAME) eq ([^"]+)"', line)
            assert match, line
            mode, value = match.groups()
            status = int(not any((str(pid) == value if mode == 'PID' else name == value)
                                 for pid, name in processes(polls)))
        elif line.startswith('if errorlevel 1 goto '):
            if status: pc = labels[line.split()[-1]]
        elif line.startswith('if %wait_attempts% GEQ '):
            if attempts >= int(line.split()[3]): pc = labels[line.split()[-1]]
        elif line.startswith('goto '):
            pc = labels[line.split()[-1]]
    raise AssertionError('no terminal wait outcome')

assert wait(lambda poll: [(999, 'nosleep.exe')]) == ('REPLACE', 1)
assert wait(lambda poll: [(4242, 'nosleep.exe')] if poll < 3 else []) == ('REPLACE', 3)
assert wait(lambda poll: [(4242, 'nosleep.exe')]) == ('FAILED', 30)
print('PASS: generated updater wait ignores other installations, awaits its process, and is bounded')
PY
