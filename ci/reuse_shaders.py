"""Reuse compiled output only with matching successful-run provenance and inputs.

The normal shader/interface validator still runs after this check. Missing cache
or a different translation chain falls back to a complete shader build.
"""
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def api(path):
    return json.loads(subprocess.check_output(['gh', 'api', path]))


def main():
    run, chain = sys.argv[1:]
    assert run.isdecimal() and re.fullmatch('[0-9a-f]{12}', chain)
    repo = os.environ['GITHUB_REPOSITORY']
    previous = api(f'repos/{repo}/actions/runs/{run}')
    assert previous['conclusion'] == 'success' and previous['name'] == 'ACNH Apple build'
    assert previous['head_repository']['full_name'] == repo
    old_input = api(f'repos/{repo}/contents/input-sha256.txt?ref={previous["head_sha"]}')
    assert base64.b64decode(old_input['content']).split()[0] == Path('input-sha256.txt').read_bytes().split()[0]
    artifacts = api(f'repos/{repo}/actions/runs/{run}/artifacts')['artifacts']
    names = [a['name'] for a in artifacts if a['name'].startswith('acnh-diagnostics-') and not a['expired']]
    assert len(names) == 1
    destination = Path('work/shader-provenance')
    subprocess.run(['gh', 'run', 'download', run, '--repo', repo, '--name', names[0], '--dir', str(destination)], check=True)
    headers = list(destination.rglob('dmsh_chain.h'))
    assert len(headers) == 1 and f'"{chain}"' in headers[0].read_text()
    for sdk in ('mac', 'ios'):
        root = Path(f'output/msl-{sdk}/b-{chain}')
        assert root.is_dir()
        if sdk == 'mac': assert (root / '_b_build.json').is_file()
        files = list(root.glob('*.metallib'))
        assert files
        for path in files:
            with path.open('rb') as stream: assert stream.read(4) == b'MTLB'
        for path in root.glob('*_b.json'):
            assert json.loads(path.read_text())['chain'] == chain
    report = dict(producer_run=int(run), producer_commit=previous['head_sha'], chain=chain,
                  input_sha256=Path('input-sha256.txt').read_text().split()[0],
                  current_commit=os.environ.get('GITHUB_SHA'),
                  validation='same inputs and chain; compiled library headers; full interface validation follows')
    Path('output/shader-reuse.json').write_text(json.dumps(report, indent=2))
    # Record complete cached-library digests for later artifact audits.
    for sdk in ('mac', 'ios'):
        root = Path(f'output/msl-{sdk}/b-{chain}')
        report[sdk + '_libraries'] = {p.name: hashlib.file_digest(p.open('rb'), 'sha256').hexdigest() for p in sorted(root.glob('*.metallib'))}
    Path('output/shader-reuse.json').write_text(json.dumps(report, indent=2))
    print(f'Reusing compiled shader chain {chain} from successful run {run}; validating all interfaces next')


try:
    main()
except Exception as error:
    print(f'Compiled shader reuse unavailable ({type(error).__name__}: {error}); stopping rather than silently rebuilding')
    raise SystemExit(2)
