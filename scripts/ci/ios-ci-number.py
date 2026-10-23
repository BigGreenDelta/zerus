#!/usr/bin/env python3
"""Keep TestFlight numbers in the main CI sequence, including nightly uploads."""
import json
import os
from pathlib import Path
import re
import subprocess


def number(runs, commit):
    if not re.fullmatch('[0-9a-f]{40}', commit):
        raise ValueError('Exact source commit required')
    valid = []
    for run in runs:
        if (run.get('path') == '.github/workflows/ci.yml' and run.get('event') == 'push'
                and run.get('head_branch') == 'main' and run.get('head_sha') == commit
                and run.get('head_repository', {}).get('full_name') == 'ufna/zerus'
                and run.get('repository', {}).get('full_name') == 'ufna/zerus'
                and run.get('status') == 'completed' and run.get('conclusion') == 'success'
                and type(run.get('run_number')) is int and 0 < run['run_number'] < 10**9):
            valid.append(run['run_number'])
    if not valid:
        raise ValueError('No successful exact-source upstream main CI sequence')
    return max(valid)


def main():
    commit = os.environ['GITHUB_SHA']
    if os.environ['GITHUB_REPOSITORY'] != 'ufna/zerus' or os.environ['GITHUB_REF'] != 'refs/heads/main':
        raise ValueError('Upstream main required')
    if not re.fullmatch('[0-9a-f]{40}', commit):
        raise ValueError('Exact source commit required')
    result = subprocess.check_output(['gh', 'api', 'repos/ufna/zerus/actions/workflows/ci.yml/runs'
                                     f'?head_sha={commit}&event=push&branch=main&status=success&per_page=100'], timeout=30)
    if len(result) > 2 * 1024**2:
        raise ValueError('CI inventory exceeds bounds')
    data = json.loads(result)
    if type(data.get('total_count')) is not int or not 0 <= data['total_count'] <= 100:
        raise ValueError('Unknown bounded CI inventory')
    chosen = number(data['workflow_runs'], commit)
    with Path(os.environ['GITHUB_OUTPUT']).open('a') as out:
        out.write(f'number={chosen}\n')
    print(f'Using exact-source main CI build number {chosen}.')


if __name__ == '__main__':
    main()
