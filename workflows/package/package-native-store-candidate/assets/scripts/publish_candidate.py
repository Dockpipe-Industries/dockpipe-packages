"""Create/resume an immutable native catalog prerelease and verify unauthenticated public bytes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import urllib.parse

import native_store
import toolchain_inputs
import publication_plan
import catalog_transaction
from catalog_transaction import inventory, verify_reference

REPOSITORY = native_store.REPOSITORY
METADATA_NAME = 'native-store-release.json'
CHECKS = ['public-locked-inputs', 'owning-native-source-hook', 'native-display-build-and-ctest',
          'exact-noncore-ownership', 'flat-store-integrity']


def command(arguments, environment=None):
    result = subprocess.run(arguments, env=environment, capture_output=True, text=True, timeout=300)
    if result.returncode:
        status = re.search(r'\bHTTP ([1-5][0-9]{2})\b', result.stderr or '')
        detail = '; HTTP ' + status.group(1) if status else ''
        raise ValueError('Candidate operation failed: ' + arguments[0] + ' ' + arguments[1]
                         + ' (exit ' + str(result.returncode) + detail + ')')
    if len(result.stdout) > 2 * 1024 * 1024:
        raise ValueError('Candidate metadata response exceeds capacity')
    return result.stdout


class GitHub:
    def api(self, endpoint):
        return json.loads(command(['gh', 'api', '--hostname', 'github.com', endpoint]))

    def reference(self, name):
        query = '''query($owner:String!,$name:String!,$ref:String!){
          repository(owner:$owner,name:$name){ref(qualifiedName:$ref){target{
            __typename oid ... on Tag {target{__typename oid ... on Tag {target{__typename oid}}}}
          }}}
        }'''
        owner, repository = REPOSITORY.split('/')
        result = json.loads(command(['gh', 'api', '--hostname', 'github.com', 'graphql', '-f', 'query=' + query,
                                     '-f', 'owner=' + owner, '-f', 'name=' + repository,
                                     '-f', 'ref=' + name]))
        if result.get('errors'):
            raise ValueError('Could not establish candidate source reference')
        return result['data']['repository']['ref']

    def find(self, tag):
        for page in range(1, 6):
            items = self.api('repos/' + REPOSITORY + '/releases?per_page=100&page=' + str(page))
            if not isinstance(items, list) or len(items) > 100 or any(not isinstance(item, dict) for item in items):
                raise ValueError('Release inventory is invalid')
            matches = [item for item in items if item.get('tag_name') == tag]
            if len(matches) > 1:
                raise ValueError('Candidate release tag is ambiguous')
            if matches:
                identity = matches[0].get('id')
                if type(identity) is not int or identity <= 0:
                    raise ValueError('Candidate release ID is invalid')
                result = self.release(identity)
                if result.get('id') != identity or result.get('tag_name') != tag:
                    raise ValueError('Candidate release identity changed during lookup')
                return result
            if len(items) < 100:
                return None
        raise ValueError('Release lookup exceeds capacity; no mutation attempted')

    def release(self, identity):
        return self.api('repos/' + REPOSITORY + '/releases/' + str(identity))

    def latest(self):
        try:
            result = self.api('repos/' + REPOSITORY + '/releases/latest')
        except ValueError as error:
            if '; HTTP 404)' in str(error):
                return None
            raise
        if type(result.get('id')) is not int or not isinstance(result.get('tag_name'), str):
            raise ValueError('Latest-release readback is invalid')
        return result['id'], result['tag_name']

    def create(self, tag, source):
        response = command([
            'gh', 'api', '--hostname', 'github.com', '--method', 'POST',
            'repos/' + REPOSITORY + '/releases',
            '-f', 'tag_name=' + tag, '-f', 'target_commitish=' + source,
            '-F', 'draft=true', '-F', 'prerelease=true', '-f', 'make_latest=false',
            '-f', 'name=Immutable catalog candidate',
            '-f', 'body=Immutable package-owned store overlay for checksum-pinned consumers. '
            'Only the explicitly qualified platforms are included. No application or production-channel promotion.',
        ])
        return json.loads(response)

    def upload(self, tag, path):
        # Deliberately no overwrite switch. A concurrent asset upload fails and
        # the next explicit resume must inventory and verify the server bytes.
        command(['gh', 'release', 'upload', tag, str(path), '--repo', 'https://github.com/' + REPOSITORY])

    def publish(self, tag):
        command(['gh', 'release', 'edit', tag, '--repo', 'https://github.com/' + REPOSITORY,
                 '--draft=false', '--prerelease', '--latest=false'])

    def public_digest(self, url, expected_size):
        parsed = urllib.parse.urlsplit(url)
        if (parsed.scheme != 'https' or parsed.netloc != 'github.com'
                or parsed.query or parsed.fragment or not parsed.path.startswith('/' + REPOSITORY + '/releases/download/')):
            raise ValueError('Public readback must use the exact owning GitHub release URL')
        environment = {key: value for key, value in os.environ.items()
                       if key not in {'GH_TOKEN', 'GITHUB_TOKEN', 'GH_ENTERPRISE_TOKEN', 'GITHUB_ENTERPRISE_TOKEN'}}
        with tempfile.TemporaryDirectory(prefix='catalog-public-readback-') as temporary:
            path = Path(temporary) / 'asset'
            final_url = command(['curl', '--disable', '--silent', '--show-error', '--fail', '--location',
                                 '--proto', '=https', '--proto-redir', '=https', '--max-redirs', '3',
                                 '--connect-timeout', '15', '--max-time', '300',
                                 '--max-filesize', str(expected_size), '--output', str(path),
                                 '--write-out', '%{url_effective}', url], environment)
            final = urllib.parse.urlsplit(final_url.strip())
            if final.scheme != 'https' or final.hostname not in {
                    'github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com'}:
                raise ValueError('Public readback redirected outside allowed HTTPS asset hosts')
            if not path.is_file() or path.stat().st_size != expected_size:
                raise ValueError('Public readback size differs from candidate')
            return native_store.sha256(path)


def accepted_push(environment):
    source = environment.get('GITHUB_SHA', '')
    if (environment.get('GITHUB_REPOSITORY') != REPOSITORY
            or environment.get('GITHUB_REF') != 'refs/heads/dev'
            or environment.get('GITHUB_EVENT_NAME') != 'push'
            or not re.fullmatch('[0-9a-f]{40}', source)):
        raise ValueError('Publication requires an exact accepted owning-repository dev push')
    return source


def publish(candidate_paths, lock_path, output, client=None, environment=None):
    source = accepted_push(os.environ if environment is None else environment)
    plan = publication_plan.plan(candidate_paths, qualification='native')
    document = native_store.read_json(lock_path)
    accepted = document.get('accepted_platforms')
    if (plan['source_commit'] != source or not isinstance(accepted, list)
            or set(plan['artifacts']) != set(accepted) or plan['owned'] != document.get('owned')):
        raise ValueError('Actual candidates differ from the accepted source/platform/ownership set')
    lock_hash = native_store.sha256(lock_path)
    by_platform = {}
    toolchain_preparation = {}
    for path in candidate_paths:
        descriptor = native_store.read_json(path)
        target = descriptor['platform']
        lock, profile = native_store.load_lock(lock_path, target)
        if (lock['source_commit'] != source or descriptor.get('inputs_lock_sha256') != lock_hash
                or descriptor.get('source_state') != 'committed-snapshot'
                or descriptor.get('source_archive_sha256') != profile['inputs']['source']['sha256']
                or descriptor.get('checks') != CHECKS):
            raise ValueError('Candidate lacks its exact committed-source/native-build input binding')
        by_platform[target] = path.parent / descriptor['asset']
        if 'toolchain_recipe' in profile:
            receipt_path = path.parent / ('toolchain-receipt-' + target + '.json')
            if (receipt_path.is_symlink() or not receipt_path.is_file() or receipt_path.stat().st_size > 32 << 20
                    or native_store.sha256(receipt_path) != descriptor.get('toolchain_receipt_sha256')):
                raise ValueError('Native candidate lacks its bound toolchain preparation receipt')
            receipt = json.loads(receipt_path.read_bytes(), object_pairs_hook=toolchain_inputs.native_inputs.unique_fields)
            toolchain_inputs.validate_receipt(receipt, target, profile['toolchain_recipe'])
            by_platform[target + '/toolchain'] = receipt_path
            toolchain_preparation[target] = {'sha256': native_store.sha256(receipt_path),
                'size_bytes': receipt_path.stat().st_size, 'profile': profile['toolchain_recipe'],
                'url': 'https://github.com/' + REPOSITORY + '/releases/download/' + plan['tag'] + '/' + receipt_path.name}
    release_document = {
        'schema': 1, 'product': 'DockpipeCatalogCandidate', 'artifact_class': 'native-store-overlay',
        'publication_mode': 'native', 'native_build': True,
        'source_repository': REPOSITORY, 'source_commit': source, 'source_state': 'committed-snapshot',
        'tag': plan['tag'], 'owned': plan['owned'], 'platforms': sorted(accepted),
        'artifacts': plan['artifacts'], 'inputs_lock_sha256': lock_hash,
    }
    if toolchain_preparation:
        release_document['toolchain_preparation'] = toolchain_preparation
    return catalog_transaction.publish_assets(release_document, by_platform, output, METADATA_NAME, client or GitHub(), qualification='native')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, action='append')
    parser.add_argument('--candidate-dir', type=Path)
    parser.add_argument('--lock', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    arguments = parser.parse_args()
    try:
        if bool(arguments.candidate) == bool(arguments.candidate_dir):
            raise ValueError('Choose explicit candidates or one downloaded candidate directory')
        candidates = arguments.candidate or sorted(arguments.candidate_dir.glob('*/candidate.json'))
        publish(candidates, arguments.lock, arguments.output)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        parser.exit(1, 'Native catalog publication failed: ' + str(error) + '\n')
