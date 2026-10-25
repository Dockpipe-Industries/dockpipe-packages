"""Admit only three native consumer artifacts from this trusted workflow run."""
import json
import os
import re
import urllib.error
import urllib.request

from catalog_plan import entries
from catalog_transaction import PublicationStopped, fingerprint_inventory, fingerprint_receipt

REPOSITORY = 'Dockpipe-Industries/dockpipe-packages'
WORKFLOW = '.github/workflows/ci.yml'
PLATFORMS = {'linux-amd64', 'windows-amd64', 'darwin-arm64'}


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file_pointer, code, message, headers, new_url):
        raise PublicationStopped('Qualification API redirect is not admitted')


class GitHubReadOnly:
    def __init__(self):
        self.token = os.environ.get('GH_TOKEN', '')
        if len(self.token) > 4096 or any(ord(character) < 32 for character in self.token):
            raise PublicationStopped('Qualification API token reference is invalid')
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def get(self, endpoint):
        allowed = ('repos/' + REPOSITORY + '/')
        suffix = endpoint.removeprefix(allowed)
        if (not endpoint.startswith(allowed) or not any(re.fullmatch(pattern, suffix) for pattern in [
                r'actions/runs/[0-9]+', r'actions/runs/[0-9]+/attempts/[0-9]+/jobs\?per_page=100',
                r'releases/[0-9]+', r'environments/release-staging', r'git/ref/tags/catalog-native-candidate-[0-9a-f]{40}'])):
            raise PublicationStopped('Qualification API endpoint is outside authority')
        headers = {
            'User-Agent': 'Dockpipe-Catalog-Qualification/1',
            'Accept': 'application/vnd.github+json',
            'X-GitHub-Api-Version': '2022-11-28',
        }
        if self.token:
            headers['Authorization'] = 'Bearer ' + self.token
        request = urllib.request.Request('https://api.github.com/' + endpoint, headers=headers)
        try:
            with self.opener.open(request, timeout=30) as response:
                payload = response.read(1048577)
        except (urllib.error.HTTPError, urllib.error.URLError) as error:
            raise PublicationStopped('Actual qualification API read failed') from error
        if len(payload) > 1048576:
            raise PublicationStopped('Qualification API metadata exceeds capacity')
        return json.loads(payload)


def bind_native_receipt(receipt, environment):
    """Called by the actual consumer entrypoint, never from a request success field."""
    required = ['GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_SHA', 'GITHUB_REPOSITORY']
    if any(not environment.get(key) for key in required):
        raise PublicationStopped('Native consumer requires actual workflow context')
    if environment['GITHUB_REPOSITORY'] != REPOSITORY:
        raise PublicationStopped('Native consumer repository differs')
    return {**receipt, 'run_id': int(environment['GITHUB_RUN_ID']),
            'run_attempt': int(environment['GITHUB_RUN_ATTEMPT']),
            'orchestration_source_sha': environment['GITHUB_SHA'], 'repository': REPOSITORY}


def require_runtime_capability(receipt, cli_pin):
    from catalog_consumer import RUNTIME_CAPABILITY_CHECKS
    if (not isinstance(receipt, dict) or receipt.get('cli_sha256') != cli_pin['sha256']
            or receipt.get('cli_version') != cli_pin['version']
            or receipt.get('checks') != RUNTIME_CAPABILITY_CHECKS
            or receipt.get('network_denial_claimed') is not False
            or set(receipt.get('helpers', {})) != {'shell', 'powershell', 'python'}):
        raise PublicationStopped('Native source-independent runtime capability binding differs')
    for helper in receipt['helpers'].values():
        if (not re.fullmatch('[0-9a-f]{64}', helper.get('sha256', ''))
                or type(helper.get('size_bytes')) is not int or not 0 < helper['size_bytes'] <= 1048576):
            raise PublicationStopped('Native SDK capability byte inventory is incomplete')
    outputs = receipt.get('outputs')
    expected = {'init', 'execute-starter', 'sdk-shell', 'sdk-powershell', 'sdk-python', 'python-sdk-scope'}
    if (not isinstance(outputs, list) or len(outputs) != len(expected)
            or {item.get('role') for item in outputs} != expected):
        raise PublicationStopped('Native runtime capability command inventory differs')
    for output in outputs:
        if (not re.fullmatch('[0-9a-f]{64}', output.get('sha256', ''))
                or type(output.get('size_bytes')) is not int or not 0 <= output['size_bytes'] <= 1048576):
            raise PublicationStopped('Native runtime capability output provenance is incomplete')


def admit_native_receipts(plan, receipts, cli_pins, manifests, closures, environment, api):
    if set(receipts) != PLATFORMS or set(cli_pins) != PLATFORMS:
        raise PublicationStopped('All three exact native consumer platforms are required')
    run_id = int(environment['GITHUB_RUN_ID'])
    attempt = int(environment['GITHUB_RUN_ATTEMPT'])
    if (environment.get('GITHUB_REPOSITORY') != REPOSITORY
            or environment.get('GITHUB_EVENT_NAME') != 'workflow_dispatch'
            or environment.get('GITHUB_REF') != 'refs/heads/staging'
            or environment.get('GITHUB_SHA') != plan['orchestration_source_sha']):
        raise PublicationStopped('Promotion requires the exact owning staging workflow context')
    endpoint = 'repos/' + REPOSITORY + '/actions/runs/' + str(run_id)
    run = api.get(endpoint)
    if (run.get('id') != run_id or run.get('run_attempt') != attempt
            or run.get('head_sha') != plan['orchestration_source_sha']
            or run.get('head_branch') != 'staging' or run.get('event') != 'workflow_dispatch'
            or run.get('path') != WORKFLOW
            or run.get('repository', {}).get('full_name') != REPOSITORY
            or run.get('status') not in {'in_progress', 'completed'}
            or (run.get('status') == 'completed' and run.get('conclusion') != 'success')):
        raise PublicationStopped('Actual owning workflow identity differs')
    jobs = api.get(endpoint + '/attempts/' + str(attempt) + '/jobs?per_page=100')
    if (type(jobs.get('total_count')) is not int or not 3 <= jobs['total_count'] <= 100
            or len(jobs.get('jobs', [])) != jobs['total_count']):
        raise PublicationStopped('Native consumer job inventory is incomplete or unbounded')
    wrapper = next(item for item in plan['objects'] if item['role'] == 'release-manifest')
    for platform in sorted(PLATFORMS):
        matching = [job for job in jobs['jobs'] if job.get('name') == 'consumer (' + platform + ')']
        if (len(matching) != 1 or matching[0].get('status') != 'completed'
                or matching[0].get('conclusion') != 'success'
                or matching[0].get('run_id') != run_id
                or matching[0].get('head_sha') != plan['orchestration_source_sha']):
            raise PublicationStopped('Actual native consumer job did not pass: ' + platform)
        receipt = receipts[platform]
        if (receipt.get('repository') != REPOSITORY or receipt.get('run_id') != run_id
                or receipt.get('run_attempt') != attempt
                or receipt.get('orchestration_source_sha') != plan['orchestration_source_sha']
                or receipt.get('platform') != platform
                or receipt.get('cli_sha256') != cli_pins[platform]['sha256']
                or cli_pins[platform]['version'] not in receipt.get('cli_version_output', '').split()
                or receipt.get('wrapper_sha256') != wrapper['sha256']
                or receipt.get('object_inventory_sha256') != fingerprint_inventory(plan['objects'])
                or receipt.get('exit_code') != 0 or receipt.get('source_independent') is not True
                or receipt.get('VM_operations') is not False):
            raise PublicationStopped('Native consumer artifact binding differs: ' + platform)
        require_runtime_capability(receipt.get('runtime_capability'), cli_pins[platform])
        selected = entries(manifests[platform])
        expected_requests = {item['identity'] for item in plan['reports'][platform]['decisions']
                             if item['action'] == 'promote'}
        if set(closures[platform]) != expected_requests:
            raise PublicationStopped('Declared closure set differs from exact promoted ownership')
        installed = receipt.get('installed', [])
        if (not isinstance(installed, list) or len(installed) != len(expected_requests)
                or {item.get('requested') for item in installed} != expected_requests):
            raise PublicationStopped('Native consumer did not install every selected app independently')
        for item in installed:
            expected_closure = closures[platform][item['requested']]
            if item.get('closure') != sorted(expected_closure):
                raise PublicationStopped('Native installed transitive closure differs')
            expected_archives = []
            for key in expected_closure:
                entry = selected[key]
                kind = key.split('/', 1)[0]
                expected_archives.append({'relative_path': kind + 's/' + entry['tarball'],
                                          'sha256': entry['sha256'], 'size_bytes': entry['size_bytes']})
            if item.get('archives') != sorted(expected_archives, key=lambda record: record['relative_path']):
                raise PublicationStopped('Native installed archive hash inventory differs')
    return {'schema': 1, 'wrapper_sha256': wrapper['sha256'],
            'object_inventory_sha256': fingerprint_inventory(plan['objects']),
            'source_independent': True, 'exit_code': 0, 'run_id': run_id, 'run_attempt': attempt,
            'native_receipt_sha256': {platform: fingerprint_receipt(receipt)
                                     for platform, receipt in receipts.items()}}
