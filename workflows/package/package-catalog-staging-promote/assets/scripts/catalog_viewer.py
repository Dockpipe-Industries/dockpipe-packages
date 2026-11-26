"""Admit the exact Linux viewer candidate for the protected staging catalog."""

import json
import subprocess

from catalog_bundle import contained_file
from catalog_consumer_gate import REPOSITORY
from catalog_plan import COMMIT, SHA256, VIEWER_IDENTITY, VIEWER_PLATFORM, entries, relative_path
from catalog_s3 import file_digest
from catalog_transaction import PublicationStopped


WORKFLOW = '.github/workflows/remote-desktop-viewer-candidate.yml'
MANIFEST = 'packages-store-manifest.json'


def admit_viewer(pin, api):
    source = pin['source_sha']
    run_id = pin['run_id']
    artifact_id = pin['artifact_id']
    if (not COMMIT.fullmatch(source) or type(run_id) is not int or run_id <= 0
            or type(artifact_id) is not int or artifact_id <= 0
            or not SHA256.fullmatch(pin['artifact_sha256'])):
        raise PublicationStopped('Viewer requires exact source, workflow and artifact identities')

    prefix = 'repos/' + REPOSITORY + '/actions/'
    run = api.get(prefix + 'runs/' + str(run_id))
    if (run.get('id') != run_id or run.get('head_sha') != source or run.get('head_branch') != 'dev'
            or run.get('event') != 'push' or run.get('status') != 'completed'
            or run.get('conclusion') != 'success' or run.get('path') != WORKFLOW
            or run.get('repository', {}).get('full_name') != REPOSITORY):
        raise PublicationStopped('Viewer build was not qualified by the owning dev workflow')

    artifact = api.get(prefix + 'artifacts/' + str(artifact_id))
    expected_name = 'native-desktop-viewer-' + VIEWER_PLATFORM + '-' + source
    owning_run = artifact.get('workflow_run', {})
    if (artifact.get('id') != artifact_id or artifact.get('name') != expected_name
            or artifact.get('expired') is not False
            or artifact.get('digest') != 'sha256:' + pin['artifact_sha256']
            or type(artifact.get('size_in_bytes')) is not int
            or not 0 < artifact['size_in_bytes'] <= 128 << 20
            or owning_run.get('id') != run_id or owning_run.get('head_sha') != source):
        raise PublicationStopped('Viewer artifact differs from its exact workflow binding')

    return expected_name


def admit_distribution_review(pin, source_root):
    review = pin['distribution_review']
    path = relative_path(review['path'])
    if not path.startswith('release/catalog-qualification/'):
        raise PublicationStopped('Viewer distribution review requires a source-bound release record')
    evidence = contained_file(source_root, path)
    if file_digest(evidence, 1048576) != (review['sha256'], review['size_bytes']):
        raise PublicationStopped('Viewer distribution review differs from the reviewed bytes')

    record = json.loads(evidence.read_bytes())
    required = {'media-source-and-build', 'license-and-relinking', 'codec-distribution',
                'installed-viewer', 'paired-media-input-cleanup'}
    if (record.get('schema') != 1 or record.get('source_sha') != pin['source_sha']
            or record.get('artifact_sha256') != pin['artifact_sha256']
            or record.get('package_sha256') != pin['package_sha256']
            or record.get('platform') != VIEWER_PLATFORM or record.get('package') != VIEWER_IDENTITY
            or record.get('checks') != dict.fromkeys(required, 'passed')):
        raise PublicationStopped('Viewer distribution and installed use have not been qualified for these bytes')


def download_viewer(pin, api, source_root, output):
    admit_distribution_review(pin, source_root)
    artifact = admit_viewer(pin, api)
    try:
        subprocess.run(['gh', 'run', 'download', str(pin['run_id']), '--repo', REPOSITORY,
                        '--name', artifact, '--dir', str(output)], check=True, timeout=180,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.SubprocessError) as error:
        raise PublicationStopped('Could not acquire the qualified viewer artifact') from error

    return verify_store(pin, output)


def verify_store(pin, output):
    path = contained_file(output, MANIFEST)
    if file_digest(path, 1048576) != (pin['manifest_sha256'], pin['manifest_size_bytes']):
        raise PublicationStopped('Viewer store manifest differs from the reviewed bytes')

    manifest = json.loads(path.read_bytes())
    packages = entries(manifest)
    if (manifest.get('platform') != VIEWER_PLATFORM or set(manifest['packages']) != {'resolvers'}
            or set(packages) != {VIEWER_IDENTITY}):
        raise PublicationStopped('Viewer store must contain only the Linux desktop resolver')

    entry = packages[VIEWER_IDENTITY]
    archive = contained_file(output, entry['tarball'])
    if (entry['sha256'] != pin['package_sha256']
            or file_digest(archive, entry['size_bytes']) != (entry['sha256'], entry['size_bytes'])):
        raise PublicationStopped('Viewer package differs from the reviewed archive')

    return entry
