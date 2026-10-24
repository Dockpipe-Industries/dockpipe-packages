"""Fresh readback of the exact immutable product candidate before promotion."""
import re

from catalog_transaction import PublicationStopped

REPOSITORY = 'Dockpipe-Industries/dockpipe-packages'
PLATFORMS = {'linux-amd64', 'windows-amd64', 'darwin-arm64'}
JOBS = {'inputs', 'publish', 'native (linux-amd64, ubuntu-22.04)',
        'native (windows-amd64, windows-2025-vs2026)', 'native (darwin-arm64, macos-15)'}


def admit_candidate(pin, api):
    source = pin['source_sha']
    if not re.fullmatch('[0-9a-f]{40}', source) or pin['tag'] != 'catalog-native-candidate-' + source:
        raise PublicationStopped('Product requires an exact native candidate tag/source')
    prefix = 'repos/' + REPOSITORY + '/'
    run = api.get(prefix + 'actions/runs/' + str(pin['run_id']))
    if (run.get('id') != pin['run_id'] or run.get('run_attempt') != pin['run_attempt']
            or run.get('head_sha') != source or run.get('head_branch') != 'dev'
            or run.get('event') != 'push' or run.get('status') != 'completed'
            or run.get('conclusion') != 'success'
            or run.get('path') != '.github/workflows/native-store-candidate.yml'):
        raise PublicationStopped('Actual owning product native run is not qualified')
    jobs = api.get(prefix + 'actions/runs/' + str(pin['run_id']) + '/attempts/'
                   + str(pin['run_attempt']) + '/jobs?per_page=100')
    if (jobs.get('total_count') != len(JOBS) or len(jobs.get('jobs', [])) != len(JOBS)
            or {job.get('name') for job in jobs['jobs']} != JOBS
            or any(job.get('status') != 'completed' or job.get('conclusion') != 'success'
                   for job in jobs['jobs'])):
        raise PublicationStopped('Actual product native jobs are incomplete or unsuccessful')
    reference = api.get(prefix + 'git/ref/tags/' + pin['tag'])
    if reference.get('object') != {'sha': source, 'type': 'commit',
                                    'url': 'https://api.github.com/repos/' + REPOSITORY + '/git/commits/' + source}:
        raise PublicationStopped('Actual immutable lightweight product tag differs')
    release = api.get(prefix + 'releases/' + str(pin['release_id']))
    if (release.get('id') != pin['release_id'] or release.get('tag_name') != pin['tag']
            or release.get('draft') is not False or release.get('prerelease') is not True
            or release.get('target_commitish') != source):
        raise PublicationStopped('Actual immutable product prerelease differs')
    expected_names = {'native-store-release.json'} | {
        prefix + platform + suffix for platform in PLATFORMS
        for prefix, suffix in [('dockpipe-catalog-', '.tar.gz'), ('toolchain-receipt-', '.json')]}
    if set(pin['assets']) != expected_names or len(release.get('assets', [])) != 7:
        raise PublicationStopped('Product requires the exact seven native assets')
    observed = {asset['name']: asset for asset in release['assets']}
    if set(observed) != expected_names:
        raise PublicationStopped('Actual product asset set differs')
    for name, expected in pin['assets'].items():
        actual = observed[name]
        url = 'https://github.com/' + REPOSITORY + '/releases/download/' + pin['tag'] + '/' + name
        if (expected['url'] != url or actual.get('browser_download_url') != url
                or actual.get('size') != expected['size_bytes']
                or actual.get('digest') != 'sha256:' + expected['sha256']
                or actual.get('state') != 'uploaded' or actual.get('id') != expected['asset_id']):
            raise PublicationStopped('Actual immutable product asset identity differs: ' + name)
    return {'source_sha': source, 'tag': pin['tag'], 'release_id': pin['release_id'],
            'run_id': pin['run_id'], 'run_attempt': pin['run_attempt'],
            'asset_identity': {name: {'id': observed[name]['id'], 'sha256': pin['assets'][name]['sha256'],
                                      'size_bytes': pin['assets'][name]['size_bytes']} for name in sorted(observed)}}
