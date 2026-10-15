"""Run the shader compiler with live counts instead of a silent CI step."""
from pathlib import Path
import os
import subprocess
import sys
import time

chain = sys.argv[1]
scratch = Path('work/shader-tmp').resolve()
scratch.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, WORK=str(scratch))
started = time.monotonic()

def progress():
    spv = sum(1 for _ in scratch.glob('dmsh_*/*.spv'))
    counts = [sum(1 for _ in Path(f'output/msl-{sdk}/b-{chain}').glob('*.metallib'))
              for sdk in ('mac', 'ios')]
    print(f'Shader progress: elapsed={int(time.monotonic()-started)}s '
          f'SPIR-V={spv} macOS libraries={counts[0]} iOS libraries={counts[1]}', flush=True)

with Path('output/shaders.log').open('w') as log:
    process = subprocess.Popen([sys.executable, 'source/dm/gfx/tools/dmsh_build.py',
                                str(Path('work/shader-pkg').resolve()),
                                str(Path('output/msl-mac').resolve())],
                               env=env, stdout=log, stderr=subprocess.STDOUT)
    progress()
    while True:
        try:
            code = process.wait(timeout=30)
            break
        except subprocess.TimeoutExpired:
            progress()
progress()
print('\n'.join(Path('output/shaders.log').read_text(errors='replace').splitlines()[-20:]), flush=True)
raise SystemExit(code)
