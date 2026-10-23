"""Compile one source-only compatibility overlay with released runtime inputs."""
import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

import catalog_transaction
import native_store
import prepare_ci
import publication_plan
import publish_candidate

OWNED = [['workflow', 'dockpipe.terraform.core'], ['workflow', 'dockpipe.cloudflare.r2infra'],
         ['workflow', 'secretstore-r2-publish-test']]
CHECKS = ['public-locked-inputs', 'released-cli-workflow-compile', 'installed-package-closure',
          'default-mode-and-failure-execution',
          'public-core-consumer-recompose', 'flatpak-dependency-preflight',
          'exact-noncore-ownership', 'flat-store-integrity']
ROLES = {'source', 'cli', 'runtime_core', 'baseline', 'dorkpipe', 'verifier_source'}


def module(path, name):
    specification = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(result)
    return result


def validate_store_pin(pin, role):
    if (not isinstance(pin, dict) or set(pin) != {'url', 'sha256', 'size_bytes', 'manifest_sha256'}
            or type(pin['size_bytes']) is not int or not 0 < pin['size_bytes'] <= 512 << 20
            or any(not isinstance(pin[field], str) or not re.fullmatch('[0-9a-f]{64}', pin[field])
                   for field in ('sha256', 'manifest_sha256'))):
        raise ValueError('Source dependency store requires exact bounded transport/manifest identities')
    pattern = (r'https://github.com/Dockpipe-Industries/dockpipe/releases/download/'
               r'v[0-9]+\.[0-9]+\.[0-9]+-staging\.[0-9]+\.[0-9]+\.[0-9a-f]{12}/'
               r'dockpipe-packages_[0-9]+\.[0-9]+\.[0-9]+_linux-amd64\.tar\.gz'
               if role == 'baseline' else
               r'https://packages\.staging\.dockpipe\.com/packages/standalone/dorkpipe/candidates/'
               r'0\.6\.5-staging\.[0-9a-f]{12}/dorkpipe-packages-0\.6\.5-linux-amd64\.tar\.gz')
    if not isinstance(pin['url'], str) or not re.fullmatch(pattern, pin['url']):
        raise ValueError('Source dependency must use its exact immutable public owner URL')


def load_lock(path, template=False):
    lock = native_store.read_json(path)
    if (set(lock) != {'schema', 'source_commit', 'owned', 'cli_version', 'workflows', 'inputs', 'previous_artifacts'}
            or type(lock['schema']) is not int or lock['schema'] != 1 or lock['owned'] != OWNED or lock['cli_version'] != '0.6.25'
            or set(lock['inputs']) != ROLES):
        raise ValueError('Source-only overlay requires its exact ownership and released CLI')
    if not template and not re.fullmatch('[0-9a-f]{40}', lock['source_commit'] or ''):
        raise ValueError('Committed source identity is required')
    workflows = lock['workflows']
    if not isinstance(workflows, list) or len(workflows) != 3:
        raise ValueError('The default-mode repair owns exactly three source workflows')
    for workflow, identity, version in zip(workflows, OWNED, ['0.6.3', '0.6.4', '0.6.2']):
        if (set(workflow) != {'path', 'name', 'version'} or workflow['version'] != version
                or workflow['name'] != identity[1]
                or native_store.relative(workflow['path']) != workflow['path']):
            raise ValueError('Select only the reviewed source workflow repairs and new versions')
    for role in ROLES - {'baseline', 'dorkpipe', 'verifier_source'}:
        if template and role == 'source' and lock['inputs'][role] is None:
            continue
        native_store.validate_asset(lock['inputs'][role])
    for role in ('baseline', 'dorkpipe'):
        validate_store_pin(lock['inputs'][role], role)
    verifier = lock['inputs']['verifier_source']
    native_store.validate_asset(dict(verifier, url=verifier['url'].replace(
        'https://codeload.github.com/Dockpipe-Industries/dockpipe/tar.gz/',
        'https://github.com/Dockpipe-Industries/dockpipe/archive/') + '.tar.gz'))
    if not re.fullmatch(r'https://codeload.github.com/Dockpipe-Industries/dockpipe/tar.gz/[0-9a-f]{40}', verifier['url']):
        raise ValueError('Verifier must be exact public core source, never a runtime replacement')
    if not template and lock['inputs']['source']['url'] != ('https://github.com/' + native_store.REPOSITORY + '/archive/' + lock['source_commit'] + '.tar.gz'):
        raise ValueError('Source archive must name the same owning committed snapshot')
    previous = lock['previous_artifacts']
    if (not isinstance(previous, dict) or not previous
            or any('/' in name or not re.fullmatch('[0-9a-f]{64}', digest) for name, digest in previous.items())):
        raise ValueError('Retained filename identities are required')
    return lock


def prepare(template, output, environment=None, downloader=prepare_ci.download_source):
    environment = os.environ if environment is None else environment
    commit = environment.get('GITHUB_SHA', '')
    if environment.get('GITHUB_REPOSITORY') != native_store.REPOSITORY or not re.fullmatch('[0-9a-f]{40}', commit):
        raise ValueError('Source export requires the accepted owning commit')
    lock = copy.deepcopy(load_lock(template, template=True))
    if output.exists():
        raise ValueError('Use fresh source preparation output')
    output.mkdir(parents=True)
    url = 'https://github.com/' + native_store.REPOSITORY + '/archive/' + commit + '.tar.gz'
    archive = output / 'source-archive.tar.gz'
    downloader(url, archive)
    lock['source_commit'] = commit
    lock['inputs']['source'] = {'url': url, 'sha256': native_store.sha256(archive),
        'size_bytes': archive.stat().st_size, 'format': 'tar.gz', 'root': 'dockpipe-packages-' + commit}
    target = output / 'effective-lock.json'
    target.write_text(json.dumps(lock, sort_keys=True, indent=2) + '\n')
    load_lock(target)
    return target


def verifier_input(asset, cache, fetch):
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / asset['sha256']
    if not path.exists():
        if not fetch:
            raise ValueError('Missing pinned public verifier source')
        prepare_ci.download_source(asset['url'], path)
    if path.is_symlink() or path.stat().st_size != asset['size_bytes'] or native_store.sha256(path) != asset['sha256']:
        raise ValueError('Public verifier source differs from its pin')
    return path


def consumer_check(verifier, baseline, baseline_root, dorkpipe, dorkpipe_root, flat, work, artifact):
    consumer = module(verifier / 'release/packaging/ingest-pinned-stores.py', 'source_overlay_consumer')
    preflight = module(verifier / 'release/packaging/desktop/flatpak/build-catalog.py', 'source_overlay_flatpak')
    entry = next(item for item in dorkpipe['packages']['resolvers'] if item['name'] == 'dorkpipe')
    dependency = {'schema': 1, 'platform': 'linux-amd64', 'packages': {'resolvers': [entry], 'workflows': []}}
    seeded = work / 'consumer-dependencies'
    seeded_manifest = consumer.composer.assemble(baseline, dependency, [['resolver', 'dorkpipe']],
        baseline_root, dorkpipe_root, seeded, consumer.MAX_EXPANDED_BYTES)
    candidate = native_store.read_json(flat / 'packages-store-manifest.json')
    pin = {'url': 'https://github.com/' + native_store.REPOSITORY + '/releases/download/catalog-candidate-'
           + work.name + '/dockpipe-catalog-linux-amd64.tar.gz',
           'sha256': native_store.sha256(artifact), 'size_bytes': artifact.stat().st_size,
           'manifest_sha256': native_store.sha256(flat / 'packages-store-manifest.json')}
    catalog = {'source_commit': work.name, 'owned': OWNED, 'artifacts': {'linux-amd64': pin}}
    consumer.validate_catalog(catalog, {('resolver', 'dorkpipe')})
    imported = work / 'consumer-imported'
    imported_manifest = consumer.unpack_bundle(artifact, pin, imported, 'linux-amd64')
    combined = work / 'consumer-composed'
    composed = consumer.compose_catalog(catalog, 'linux-amd64', seeded_manifest, seeded, imported_manifest, imported, combined)
    core = work / 'consumer-core'
    core.mkdir()
    core_entry = composed['packages']['core']
    shutil.copyfile(combined / core_entry['tarball'], core / core_entry['tarball'])
    archives = [combined / item['tarball'] for kind in ('resolvers', 'workflows')
                for item in composed['packages'][kind]]
    preflight.validate_package_dependencies(archives, core)
    return {'consumer_source': 'public-source-archive', 'optional_archives': len(archives),
            'strict_import_recompose': True, 'flatpak_dependency_preflight': True}


def build(lock_path, cache, work, output, fetch=False, development_source=None):
    lock = load_lock(lock_path)
    if work.exists() or output.exists():
        raise ValueError('Use fresh isolated build/output directories')
    if os.name != 'posix' or os.uname().sysname != 'Linux' or os.uname().machine != 'x86_64':
        raise ValueError('This source-only compatibility lane is qualified only for Linux amd64')
    work.mkdir(parents=True)
    output.mkdir(parents=True)
    inputs = lock['inputs']
    verifier = native_store.extract(inputs['verifier_source'], verifier_input(inputs['verifier_source'], cache, fetch), work / 'verifier')
    consumer = module(verifier / 'release/packaging/ingest-pinned-stores.py', 'source_overlay_inputs')
    baseline_path = cache / inputs['baseline']['sha256']
    dorkpipe_path = cache / inputs['dorkpipe']['sha256']
    for role, path in (('baseline', baseline_path), ('dorkpipe', dorkpipe_path)):
        if not path.exists():
            if not fetch:
                raise ValueError('Missing immutable ' + role + ' dependency input')
            arguments = {'baseline_platform': 'linux-amd64'} if role == 'baseline' else {'candidate_name': 'dorkpipe', 'candidate_version': '0.6.5'}
            consumer.download(inputs[role], path, **arguments)
        consumer.verify_bundle(path, inputs[role])
    baseline_root = work / 'baseline'
    baseline = consumer.unpack_bundle(baseline_path, inputs['baseline'], baseline_root, 'linux-amd64')
    dorkpipe_root = work / 'dorkpipe'
    dorkpipe = consumer.unpack_bundle(dorkpipe_path, inputs['dorkpipe'], dorkpipe_root, 'linux-amd64', candidate=True)
    if development_source is None:
        source = native_store.extract(inputs['source'], native_store.acquire(inputs['source'], cache, fetch), work / 'source')
    else:
        source = Path(development_source)
        if not source.is_absolute() or not source.is_dir() or source.is_symlink():
            raise ValueError('Development source requires an explicit absolute task directory')
    cli_root = native_store.extract(inputs['cli'], native_store.acquire(inputs['cli'], cache, fetch), work / 'cli')
    cli = cli_root / 'dockpipe'
    cli.chmod(0o755)
    global_root = work / 'global'
    core = global_root / 'packages/core'
    core.mkdir(parents=True)
    runtime = native_store.acquire(inputs['runtime_core'], cache, fetch)
    shutil.copyfile(runtime, core / 'dockpipe-core-0.6.25.tar.gz')
    dependencies = global_root / 'packages/resolvers'
    dependencies.mkdir()
    for manifest, directory, kind, name in ((baseline, baseline_root, 'resolver', 'onepassword'),
            (dorkpipe, dorkpipe_root, 'resolver', 'dorkpipe')):
        matches = [entry for entry in manifest['packages'][kind + 's'] if entry['name'] == name]
        if len(matches) != 1:
            raise ValueError('Dependency must identify one real public resolver')
        entry = matches[0]
        source_archive = directory / entry['tarball']
        if source_archive.stat().st_size != entry['size_bytes'] or native_store.sha256(source_archive) != entry['sha256']:
            raise ValueError('Public resolver differs from its pinned catalog')
        shutil.copyfile(source_archive, dependencies / entry['tarball'])
    project = work / lock['source_commit']
    project.mkdir()
    selected_sources = []
    for workflow in lock['workflows']:
        selected = project / workflow['name']
        shutil.copytree(source / workflow['path'], selected)
        manifest = __import__('yaml').safe_load((selected / 'package.yml').read_text())
        expected_dependencies = {'dockpipe.terraform.core': [],
            'dockpipe.cloudflare.r2infra': ['dockpipe.terraform.core'],
            'secretstore-r2-publish-test': ['dorkpipe', 'onepassword', 'dockpipe.cloudflare.r2infra']}[workflow['name']]
        if (manifest.get('name') != workflow['name'] or manifest.get('kind') != 'workflow'
                or manifest.get('version') != workflow['version'] or manifest.get('depends', []) != expected_dependencies):
            raise ValueError('Source does not contain the exact reviewed dependency repair')
        if workflow['name'] == 'secretstore-r2-publish-test' and manifest.get('requires_resolvers') != ['onepassword', 'dorkpipe']:
            raise ValueError('The repaired example requires its real installed resolver dependencies')
        config = __import__('yaml').safe_load((selected / 'config.yml').read_text())
        if config.get('compile_hooks'):
            raise ValueError('Source-only compatibility workflows cannot run native build hooks')
        for path in selected.rglob('*'):
            if path.is_file():
                with path.open('rb') as incoming:
                    if incoming.read(4).startswith((b'MZ', b'\x7fELF')):
                        raise ValueError('Source-only overlay cannot carry native executable payloads')
        selected_sources.append(selected)
    (project / 'dockpipe.config.json').write_text(json.dumps({'schema': 1, 'packages': {'namespace': 'dockpipeproject'}}))
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(('DOCKPIPE_', 'LD_', 'DYLD_'))
                   and key not in {'GH_TOKEN', 'GITHUB_TOKEN', 'GH_ENTERPRISE_TOKEN', 'GITHUB_ENTERPRISE_TOKEN'}}
    environment.update(DOCKPIPE_BIN=str(cli), DOCKPIPE_GLOBAL_ROOT=str(global_root),
                       XDG_CONFIG_HOME=str(work / 'config'), XDG_CACHE_HOME=str(work / 'cache'),
                       XDG_DATA_HOME=str(work / 'data'), XDG_STATE_HOME=str(work / 'state'), HOME=str(work / 'home'))
    version = subprocess.check_output([str(cli), '--version'], text=True, env=environment, timeout=15).strip()
    if version != '0.6.25':
        raise ValueError('Source-only overlay must use the pinned released runtime')
    for selected in selected_sources:
        native_store.run([cli, 'package', 'compile', 'workflow', '--workdir', project, '--from', selected, '--force'], project, environment)
    store = work / 'workflow-store'
    native_store.run([cli, 'package', 'build', 'store', '--workdir', project, '--out', store, '--only', 'workflows'], project, environment)
    flat = work / 'flat'
    native_store.combine({'workflow': store}, OWNED, 'linux-amd64', flat, lock['previous_artifacts'])
    installed_workflows = global_root / 'packages/workflows'
    installed_workflows.mkdir()
    for entry in native_store.read_json(flat / 'packages-store-manifest.json')['packages']['workflows']:
        shutil.copyfile(flat / entry['tarball'], installed_workflows / entry['tarball'])
    execution_log = output / 'default-execution-tests.log'
    with execution_log.open('x') as log:
        for tests in ('packages/cloud/storage/tests', 'packages/terraform/resolvers/terraform-core/tests'):
            subprocess.run([os.sys.executable, '-B', '-m', 'unittest', 'discover', '-s', str(source / tests), '-v'],
                cwd=source, env=environment, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
            if execution_log.stat().st_size > 2 << 20:
                raise ValueError('Source execution test output exceeds its bound')
    artifact = output / 'dockpipe-catalog-linux-amd64.tar.gz'
    native_store.archive_flat(flat, artifact)
    proof = consumer_check(verifier, baseline, baseline_root, dorkpipe, dorkpipe_root, flat, project, artifact)
    proof.update(default_execution_tests=True, default_execution_tests_sha256=native_store.sha256(execution_log))
    (output / 'consumer-proof.json').write_text(json.dumps(proof, indent=2) + '\n')
    descriptor = {'schema': 1, 'repository': native_store.REPOSITORY, 'source_commit': lock['source_commit'],
        'source_state': 'committed-snapshot' if development_source is None else 'development',
        'source_archive_sha256': inputs['source']['sha256'] if development_source is None else None,
        'tag': 'catalog-candidate-' + lock['source_commit'], 'owned': OWNED, 'platform': 'linux-amd64',
        'native_build': False, 'source_only': True, 'published': False, 'checks': CHECKS,
        'inputs_lock_sha256': native_store.sha256(lock_path), 'asset': artifact.name,
        'sha256': native_store.sha256(artifact), 'size_bytes': artifact.stat().st_size,
        'manifest_sha256': native_store.sha256(flat / 'packages-store-manifest.json'),
        'consumer_proof_sha256': native_store.sha256(output / 'consumer-proof.json')}
    (output / 'candidate.json').write_text(json.dumps(descriptor, indent=2) + '\n')
    return descriptor


def verify_attestation(path, source):
    publish_candidate.command(['gh', 'attestation', 'verify', str(path), '--hostname', 'github.com',
        '--repo', native_store.REPOSITORY, '--signer-workflow',
        native_store.REPOSITORY + '/.github/workflows/source-store-candidate.yml',
        '--source-digest', source, '--source-ref', 'refs/heads/dev', '--deny-self-hosted-runners'])


def publish(candidate, lock_path, output, client=None, environment=None, attestor=verify_attestation):
    source = publish_candidate.accepted_push(os.environ if environment is None else environment)
    lock = load_lock(lock_path)
    descriptor = native_store.read_json(candidate)
    plan = publication_plan.plan([candidate], qualification='source-only')
    if (source != lock['source_commit'] or source != plan['source_commit'] or descriptor.get('checks') != CHECKS
            or descriptor.get('source_state') != 'committed-snapshot'
            or descriptor.get('inputs_lock_sha256') != native_store.sha256(lock_path)
            or descriptor.get('source_archive_sha256') != lock['inputs']['source']['sha256']
            or descriptor.get('consumer_proof_sha256') != native_store.sha256(candidate.parent / 'consumer-proof.json')):
        raise ValueError('Source-only publication lacks exact source/input/consumer binding')
    proof = native_store.read_json(candidate.parent / 'consumer-proof.json')
    if (proof.get('strict_import_recompose') is not True or proof.get('flatpak_dependency_preflight') is not True
            or proof.get('default_execution_tests') is not True):
        raise ValueError('Actual consumer admission is required before source overlay publication')
    for path in (candidate, candidate.parent / descriptor['asset'], candidate.parent / 'consumer-proof.json'):
        attestor(path, source)
    release = {'schema': 1, 'product': 'DockpipeCatalogCandidate', 'artifact_class': 'source-only-store-overlay',
        'source_repository': native_store.REPOSITORY, 'source_commit': source, 'source_state': 'committed-snapshot',
        'source_only': True, 'native_build': False, 'tag': plan['tag'], 'owned': OWNED,
        'platforms': ['linux-amd64'], 'artifacts': plan['artifacts'],
        'inputs_lock_sha256': native_store.sha256(lock_path), 'checks': CHECKS,
        'inputs': lock['inputs'], 'cli_version': lock['cli_version'],
        'source_archive_sha256': descriptor['source_archive_sha256'],
        'consumer_proof_sha256': descriptor['consumer_proof_sha256'], 'ci_attestation_verified': True}
    return catalog_transaction.publish_assets(release, {'linux-amd64': candidate.parent / descriptor['asset']},
        output, 'source-store-release.json', client or publish_candidate.GitHub())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=['prepare', 'build', 'publish'])
    parser.add_argument('--lock', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cache', type=Path)
    parser.add_argument('--work', type=Path)
    parser.add_argument('--candidate', type=Path)
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--development-source', type=Path, help='Local producer qualification only; publication is refused')
    args = parser.parse_args()
    if args.operation == 'prepare':
        prepare(args.lock, args.output)
    elif args.operation == 'build':
        build(args.lock, args.cache, args.work, args.output, args.fetch, args.development_source)
    else:
        publish(args.candidate, args.lock, args.output)
