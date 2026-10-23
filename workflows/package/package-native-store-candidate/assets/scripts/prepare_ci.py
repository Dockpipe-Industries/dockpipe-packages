"""Resolve source identity after an accepted commit exists; tracked dependency pins stay immutable."""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import urllib.parse

import native_store
from publish_candidate import command

RUNNERS = {'linux-amd64': 'ubuntu-24.04', 'windows-amd64': 'windows-2022'}
MAX_SOURCE_BYTES = 64 * 1024 * 1024


def download_source(url, path):
    # Source is selected by the accepted immutable Git commit URL. Its actual
    # transport digest is computed once and shared with every native job.
    environment = {key: value for key, value in os.environ.items()
                   if key not in {'GH_TOKEN', 'GITHUB_TOKEN', 'GH_ENTERPRISE_TOKEN', 'GITHUB_ENTERPRISE_TOKEN'}}
    final_url = command(['curl', '--disable', '--silent', '--show-error', '--fail', '--location',
                         '--proto', '=https', '--proto-redir', '=https', '--max-redirs', '3',
                         '--max-filesize', str(MAX_SOURCE_BYTES), '--connect-timeout', '15',
                         '--max-time', '300', '--output', str(path), '--write-out', '%{url_effective}', url], environment)
    final = urllib.parse.urlsplit(final_url.strip())
    if final.scheme != 'https' or final.hostname not in {'github.com', 'codeload.github.com'}:
        raise ValueError('Committed source archive redirected outside its owning public hosts')
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= MAX_SOURCE_BYTES:
        raise ValueError('Committed source archive exceeds its bounded transport contract')


def prepare(template, output, environment=None, downloader=download_source):
    environment = os.environ if environment is None else environment
    source = environment.get('GITHUB_SHA', '')
    if environment.get('GITHUB_REPOSITORY') != native_store.REPOSITORY or not re.fullmatch('[0-9a-f]{40}', source):
        raise ValueError('CI source export requires the exact owning repository commit')
    document = native_store.read_json(template)
    for target in document.get('accepted_platforms', []):
        native_store.load_lock(template, target, source_template=True)
    if not document.get('accepted_platforms') or output.exists():
        raise ValueError('Use a complete dependency template and fresh source descriptor output')
    output.mkdir(parents=True)
    url = 'https://github.com/' + native_store.REPOSITORY + '/archive/' + source + '.tar.gz'
    archive = output / 'source-archive.tar.gz'
    downloader(url, archive)
    source_asset = {'url': url, 'sha256': native_store.sha256(archive), 'size_bytes': archive.stat().st_size,
                    'format': 'tar.gz', 'root': 'dockpipe-packages-' + source}
    effective = copy.deepcopy(document)
    effective['source_commit'] = source
    for profile in effective['profiles'].values():
        profile['inputs']['source'] = source_asset
    lock = output / 'effective-lock.json'
    lock.write_text(json.dumps(effective, sort_keys=True, indent=2) + '\n')
    for target in effective['accepted_platforms']:
        native_store.load_lock(lock, target)
    matrix = {'include': [{'platform': target, 'runner': (
        'ubuntu-22.04' if target == 'linux-amd64' and 'native_inputs' in effective['profiles'][target]
        else RUNNERS[target])} for target in effective['accepted_platforms']]}
    return matrix


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--template', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    matrix = prepare(args.template, args.output)
    target = os.environ.get('GITHUB_OUTPUT')
    if target:
        with Path(target).open('a') as stream:
            stream.write('matrix=' + json.dumps(matrix, separators=(',', ':')) + '\n')
    else:
        print(json.dumps(matrix))
