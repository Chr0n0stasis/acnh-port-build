"""Compile isolated geometry-lowering probes with both Apple SDKs."""
import json
from pathlib import Path
import subprocess

root = Path.cwd()
inputs = root / 'geometry-probe'
output = root / 'geometry-output'
output.mkdir(exist_ok=True)
results = []
for path in sorted([*(inputs / 'msl').glob('*.metal'), *(inputs / 'original-fragments').glob('*.metal')]):
    for sdk in ('macosx', 'iphoneos'):
        destination = output / sdk
        destination.mkdir(exist_ok=True)
        air = destination / (path.stem + '.air')
        library = destination / (path.stem + '.metallib')
        command = ['xcrun', '-sdk', sdk, 'metal', '-c']
        if sdk == 'iphoneos':
            command += ['-mios-version-min=18.0', '-fmetal-math-mode=relaxed']
        run = subprocess.run([*command, str(path), '-o', str(air)], capture_output=True, text=True)
        (destination / (path.stem + '.log')).write_text(run.stdout + run.stderr)
        if not run.returncode:
            run = subprocess.run(['xcrun', '-sdk', sdk, 'metallib', str(air), '-o', str(library)], capture_output=True, text=True)
            with (destination / (path.stem + '.log')).open('a') as log:
                log.write(run.stdout + run.stderr)
        valid = run.returncode == 0 and library.exists() and library.read_bytes()[:4] == b'MTLB'
        results.append(dict(program=path.stem, sdk=sdk, compiled=valid))
        print(path.stem, sdk, 'compiled' if valid else 'FAILED', flush=True)
probe = subprocess.run(['swift', '-e', 'import Metal; print(MTLCopyAllDevices().map { $0.name })'], capture_output=True, text=True)
(output / 'gpu-devices.log').write_text(probe.stdout + probe.stderr)
report = dict(status='compile probe only; not integrated into the game renderer',
              programs=len(list((inputs / 'msl').glob('*.metal'))), successful=sum(r['compiled'] for r in results),
              attempts=len(results), results=results, gpu_devices=probe.stdout.strip(), gpu_probe_exit=probe.returncode)
(output / 'report.json').write_text(json.dumps(report, indent=2))
assert results and all(r['compiled'] for r in results), 'Apple shader compilation failed'
subprocess.run(['clang', '-O1', '-fobjc-arc', '-fblocks', 'ci/test_geometry.m', '-framework', 'Foundation', '-framework', 'Metal', '-o', str(output / 'test_geometry')], check=True)
gpu = subprocess.run([str(output / 'test_geometry'), str(inputs), str(output / 'macosx')], capture_output=True, text=True)
(output / 'gpu-tests.log').write_text(gpu.stdout + gpu.stderr)
report.update(gpu_test_exit=gpu.returncode, gpu_tests=gpu.stdout)
(output / 'report.json').write_text(json.dumps(report, indent=2))
assert gpu.returncode == 0, 'GPU geometry probe failed; see gpu-tests.log'
