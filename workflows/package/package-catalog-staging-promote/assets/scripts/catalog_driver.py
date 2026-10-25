"""Explicit prepare/upload/consume/promote stages for the registered catalog CI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

from catalog_bundle import load, seal, write_exclusive, contained_file
from catalog_candidate_gate import admit_candidate
from catalog_consumer import verify_consumer
from catalog_consumer_gate import GitHubReadOnly, admit_native_receipts, bind_native_receipt, PLATFORMS
from catalog_inputs import prepare
from catalog_plan import json_bytes, relative_path, SHA256
from catalog_public import PublicReader
from catalog_s3 import S3Provider, file_digest
from catalog_tools import owning_acquirer, extract_cli, prepare_aws
from catalog_transaction import CatalogTransaction, PublicationStopped


def request_file(path, expected):
    path = Path(path)
    digest, _ = file_digest(path, 1048576)
    if digest != expected:
        raise PublicationStopped('Request differs from exact reviewed source bytes')
    document = json.loads(path.read_bytes())
    if set(document) != {'schema', 'promotion', 'candidate', 'baseline_manifests',
                         'baseline_pointer', 'cli', 'destination', 'aws_cli_archive',
                         'named_workflow_qualification', 'runtime'} or document['schema'] != 1:
        raise PublicationStopped('Promotion request has missing or unknown fields')
    return document


def require_context(plan):
    if (os.environ.get('GITHUB_REPOSITORY') != 'Dockpipe-Industries/dockpipe-packages'
            or os.environ.get('GITHUB_EVENT_NAME') != 'workflow_dispatch'
            or os.environ.get('GITHUB_REF') != 'refs/heads/staging'
            or os.environ.get('GITHUB_SHA') != plan['orchestration_source_sha']):
        raise PublicationStopped('Execution requires the exact owning staging workflow')


def public_reader(temporary_root):
    executable = Path(shutil.which('curl') or '').resolve()
    if not executable.is_file():
        raise PublicationStopped('Declared hosted curl tool is unavailable')
    return PublicReader(executable, file_digest(executable, 64 * 1024 * 1024)[0], temporary_root)


def manifest_bundle(root):
    records = []
    total = 0
    for path in sorted(root.rglob('*')):
        if path.is_symlink():
            raise PublicationStopped('Bundle has an alias')
        if not path.is_file() or path == root / 'bundle-manifest.json':
            continue
        digest, size = file_digest(path, 512 * 1024 * 1024)
        total += size
        if len(records) >= 4096 or total > 2 * 1024 * 1024 * 1024:
            raise PublicationStopped('Public-data bundle capacity exceeded')
        records.append({'path': path.relative_to(root).as_posix(), 'sha256': digest, 'size_bytes': size})
    return {'schema': 1, 'files': records}


def admit_bundle(root, expected):
    source = contained_file(root, 'bundle-manifest.json')
    if file_digest(source, 1048576)[0] != expected:
        raise PublicationStopped('Bundle manifest differs from trusted prepare job output')
    if json.loads(source.read_bytes()) != manifest_bundle(root):
        raise PublicationStopped('Bundle file inventory differs')
    binding = json.loads(contained_file(root, 'binding.json').read_bytes())
    request = request_file(contained_file(root, 'request.json'), binding['request_sha256'])
    plan = load(root / 'plan', binding['plan_sha256'])
    require_context(plan)
    return request, plan


def native_package_stores(request, root):
    return {platform: {'directory': root / 'inputs' / ('native-' + platform),
                       'manifest_sha256': request['candidate']['artifacts'][platform]['manifest_sha256']}
            for platform in PLATFORMS}


def qualification_document(pin, source_root):
    if not isinstance(pin, dict) or set(pin) != {'path', 'sha256', 'size_bytes'}:
        raise PublicationStopped('Exact source-bound qualification receipt reference is required')
    relative = relative_path(pin['path'])
    if not relative.startswith('release/catalog-qualification/'):
        raise PublicationStopped('Qualification receipt must be an owning reviewed source projection')
    path = contained_file(source_root, relative)
    if file_digest(path, 1048576) != (pin['sha256'], pin['size_bytes']):
        raise PublicationStopped('Qualification receipt bytes differ from the reviewed provenance')
    return json.loads(path.read_bytes())


def admit_isolated_cli_qualification(pin, source_root, cli_pin, cli_source_sha):
    """Bind reviewed raw network-none capsule proof, not a caller success flag."""
    receipt = qualification_document(pin, source_root)
    checks = {'typed offline init', 'execute real host starter', 'three SDK resource languages',
              'real Python SDK explicit CLI without PATH CLI'}
    if (receipt.get('schema') != 1 or receipt.get('cli_sha256') != cli_pin['executable_sha256']
            or receipt.get('cli_version') != cli_pin['version']
            or receipt.get('cli_source_sha') != cli_source_sha
            or receipt.get('network') != 'none' or receipt.get('source_or_sibling_mounts') is not False
            or not checks.issubset(receipt.get('checks', []))):
        raise PublicationStopped('Reviewed isolated CLI acceptance does not bind the actual public executable')
    commands = qualification_document(receipt.get('command_receipt'), source_root)
    if not isinstance(commands, list) or not 1 <= len(commands) <= 128:
        raise PublicationStopped('Isolated CLI raw command inventory is missing or unbounded')
    for command in commands:
        if (not isinstance(command, dict) or type(command.get('exit')) is not int or command['exit'] != 0
                or not isinstance(command.get('argv'), list)
                or not command['argv'] or any(not isinstance(value, str) for value in command['argv'])):
            raise PublicationStopped('Isolated CLI raw commands contain an incomplete or failed execution')
    arguments = [command['argv'] for command in commands]
    if (not any(command[0] == 'init' for command in arguments)
            or not any('--workflow' in command and 'example' in command for command in arguments)
            or any(not any(command[:4] == ['sdk', 'path', '--language', language] for command in arguments)
                   for language in ('shell', 'powershell', 'python'))):
        raise PublicationStopped('Isolated CLI raw receipt omits the required actual capabilities')
    return receipt


def admit_named_qualification(request, source_root, manifests, runtime):
    """Bind a committed projection of separately reviewed raw named-workflow proof.

    This is supplementary human-reviewed evidence. Actual native producer jobs
    and the three independently executed CI consumers remain mandatory gates.
    """
    pin = request['named_workflow_qualification']
    relative = relative_path(pin['path'])
    if not relative.startswith('release/catalog-qualification/'):
        raise PublicationStopped('Named qualification must be an owning source-bound public projection')
    path = contained_file(source_root, relative)
    if file_digest(path, 1048576) != (pin['sha256'], pin['size_bytes']):
        raise PublicationStopped('Named workflow qualification projection differs')
    document = json.loads(path.read_bytes())
    if (document.get('schema') != 1 or document.get('product_source_sha') != request['candidate']['source_sha']
            or set(document.get('platforms', {})) != PLATFORMS):
        raise PublicationStopped('Named workflow qualification source/platform binding differs')
    admit_isolated_cli_qualification(document.get('isolated_cli_acceptance'), source_root,
                                     request['cli']['linux-amd64'], runtime['cli_source_sha'])
    for platform in PLATFORMS:
        proof = document['platforms'][platform]
        expected = {key: entry['sha256'] for key, entry in manifests[platform].items()}
        if (proof.get('cli_sha256') != request['cli'][platform]['executable_sha256']
                or proof.get('package_archives') != expected
                or proof.get('effective_vmimage_run_sha256') != runtime['platforms'][platform]['vmimage_run_sha256']
                or proof.get('failed_checks') != [] or proof.get('skipped_checks') != []
                or not isinstance(proof.get('raw_receipts'), list) or not 1 <= len(proof['raw_receipts']) <= 64):
            raise PublicationStopped('Named workflow raw evidence does not bind exact installed inputs')
        for receipt in proof['raw_receipts']:
            if (not isinstance(receipt.get('role'), str) or not receipt['role']
                    or not SHA256.fullmatch(receipt.get('sha256', ''))
                    or type(receipt.get('size_bytes')) is not int or receipt['size_bytes'] <= 0):
                raise PublicationStopped('Named workflow raw evidence reference is incomplete')
    return document


def admit_pipeline_environment(api):
    """Require an existing environment before dependent jobs can bind it.

    This metadata read proves existence only. Provider stages separately reject
    missing settings before any object write; secret values are never queried.
    """
    document = api.get('repos/Dockpipe-Industries/dockpipe-packages/environments/release-staging')
    if (document.get('name') != 'release-staging'
            or type(document.get('id')) is not int or document['id'] <= 0):
        raise PublicationStopped('Existing release-staging pipeline environment is required')
    return {'name': document['name'], 'id': document['id']}


def prepare_stage(arguments):
    source = Path(arguments.request).resolve()
    source_root = Path(os.environ['GITHUB_WORKSPACE']).resolve()
    if not source.is_relative_to(source_root / 'release'):
        raise PublicationStopped('Request must be an owning release source file')
    request = request_file(source, arguments.request_sha256)
    admit_aws_pin(request['aws_cli_archive'], source_root)
    promotion = request['promotion']
    if promotion.get('orchestration_source_sha') is not None:
        raise PublicationStopped('Source request uses only an intentional self-bound orchestration slot')
    promotion['orchestration_source_sha'] = os.environ['GITHUB_SHA']
    require_context(promotion)
    if (promotion['product_source_sha'] != request['candidate']['source_sha']
            or promotion['product_tag'] != request['candidate']['tag']):
        raise PublicationStopped('Product source identities differ')
    api = GitHubReadOnly()
    admit_pipeline_environment(api)
    admit_candidate(request['candidate'], api)
    root = Path(arguments.bundle).resolve()
    if root.exists():
        raise PublicationStopped('Prepare output must be fresh')
    root.mkdir(mode=0o700)
    temporary = root / 'temporary'
    temporary.mkdir(mode=0o700)
    acquire = owning_acquirer()
    reader = public_reader(temporary)
    candidate = request['candidate']
    metadata_pin = candidate['assets']['native-store-release.json']
    metadata = json.loads(acquire(metadata_pin, root / 'metadata-cache').read_bytes())
    expected_owned = {kind + '/' + name for kind, name in metadata.get('owned', [])}
    if (metadata.get('source_commit') != candidate['source_sha'] or metadata.get('tag') != candidate['tag']
            or metadata.get('publication_mode') != 'native' or metadata.get('native_build') is not True
            or metadata.get('artifact_class') != 'native-store-overlay'
            or set(metadata.get('platforms', [])) != PLATFORMS
            or set(candidate['artifacts']) != PLATFORMS
            or expected_owned != set(promotion['owned'])
            or candidate['artifacts'] != metadata['artifacts']):
        raise PublicationStopped('Requested native inputs differ from the genuine public product descriptor')
    for platform in PLATFORMS:
        for name, declared in [('dockpipe-catalog-' + platform + '.tar.gz', metadata['artifacts'][platform]),
                               ('toolchain-receipt-' + platform + '.json', metadata['toolchain_preparation'][platform])]:
            published = candidate['assets'][name]
            if any(declared[field] != published[field] for field in ['url', 'sha256', 'size_bytes']):
                raise PublicationStopped('Public native descriptor and actual release asset differ')
            if name.startswith('toolchain-receipt-'):
                acquire(published, root / 'metadata-cache')
    plan = prepare(request, root / 'inputs', acquire, reader)
    for field, path in [('source_lock', 'release/native-store.lock.json'),
                        ('source_dependencies', 'release/dependencies.lock.json')]:
        pin = request['candidate'][field]
        if pin['url'] != ('https://raw.githubusercontent.com/Dockpipe-Industries/dockpipe-packages/'
                          + request['candidate']['source_sha'] + '/' + path):
            raise PublicationStopped('Product source lock URL is not bound to the exact native source')
    source_lock = json.loads(acquire(request['candidate']['source_lock'], root / 'inputs/cache').read_bytes())
    dependencies = json.loads(acquire(request['candidate']['source_dependencies'], root / 'inputs/cache').read_bytes())
    from catalog_runtime_gate import admit_runtime
    runtime = admit_runtime(request['runtime'], source_lock, dependencies, request['cli'], reader,
                            root / 'runtime-readiness', acquire, native_package_stores(request, root))
    plan['runtime_binding'] = runtime
    from catalog_plan import entries
    candidate_manifests = {}
    for platform in PLATFORMS:
        manifest = root / 'inputs' / ('native-' + platform) / 'packages-store-manifest.json'
        candidate_manifests[platform] = entries(json.loads(manifest.read_bytes()))
    qualification = admit_named_qualification(request, source_root, candidate_manifests, runtime)
    write_exclusive(root / 'named-qualification.json', json_bytes(qualification))
    cli_plan = {}
    if set(request['cli']) != PLATFORMS:
        raise PublicationStopped('Exact three native public CLI pins are required')
    for platform, pin in request['cli'].items():
        archive = acquire(pin['archive'], root / 'inputs/cache')
        executable = extract_cli(archive, {**pin, 'platform': platform}, root / 'cli' / platform)
        cli_plan[platform] = {'sha256': pin['executable_sha256'], 'version': pin['version'],
                              'input_file': executable.relative_to(root).as_posix()}
    plan['cli'] = cli_plan
    sealed = seal(plan, root / 'plan')
    write_exclusive(root / 'request.json', source.read_bytes())
    write_exclusive(root / 'binding.json', json_bytes({'request_sha256': arguments.request_sha256,
                                                       'plan_sha256': sealed['plan_sha256']}))
    temporary.rmdir()
    encoded = json_bytes(manifest_bundle(root))
    write_exclusive(root / 'bundle-manifest.json', encoded)
    digest = hashlib.sha256(encoded).hexdigest()
    print(json.dumps({'bundle_sha256': digest, 'plan_sha256': sealed['plan_sha256']}))
    if os.environ.get('GITHUB_OUTPUT'):
        with Path(os.environ['GITHUB_OUTPUT']).open('a') as output:
            output.write('bundle_sha256=' + digest + '\n')




def admit_aws_pin(pin, source_root):
    descriptor_path = contained_file(source_root, 'release/catalog-promotion/aws-cli-2.32.9.json')
    descriptor = json.loads(descriptor_path.read_bytes())
    if (descriptor.get('schema') != 1 or descriptor.get('version') != '2.32.9'
            or descriptor.get('platform') != 'linux-amd64'
            or descriptor.get('archive') != pin
            or descriptor.get('signature', {}).get('valid') is not True
            or descriptor.get('signature', {}).get('fingerprint')
            != 'FB5DB77FD5C118B80511ADA8A6310ACC4672475C'):
        raise PublicationStopped('AWS CLI differs from the source-bound official dependency admission')
    return descriptor


def destination_from_environment(declaration):
    expected_fields = {'endpoint_reference', 'bucket_reference', 'target_sha256'}
    if (set(declaration) != expected_fields
            or declaration['endpoint_reference'] != 'R2_ENDPOINT_URL'
            or declaration['bucket_reference'] != 'DOCKPIPE_RELEASE_BUCKET'
            or not SHA256.fullmatch(declaration['target_sha256'])):
        raise PublicationStopped('Destination must use the exact reviewed staging references')
    destination = {
        'endpoint': os.environ.get(declaration['endpoint_reference'], ''),
        'bucket': os.environ.get(declaration['bucket_reference'], ''),
    }
    if (not all(destination.values())
            or hashlib.sha256(json_bytes(destination)).hexdigest() != declaration['target_sha256']):
        raise PublicationStopped('Configured staging destination differs from reviewed target digest')
    return destination


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('stage', choices=['prepare', 'upload', 'consume', 'promote'])
    parser.add_argument('--bundle', required=True)
    parser.add_argument('--bundle-sha256')
    parser.add_argument('--request')
    parser.add_argument('--request-sha256')
    parser.add_argument('--state')
    parser.add_argument('--platform')
    parser.add_argument('--receipts')
    parser.add_argument('--execute', action='store_true')
    arguments = parser.parse_args(argv)
    if arguments.stage == 'prepare':
        if not arguments.request or not arguments.request_sha256:
            parser.error('prepare requires --request and --request-sha256')
        prepare_stage(arguments)
        return
    if not arguments.bundle_sha256 or not arguments.state:
        parser.error('execution stages require --bundle-sha256 and --state')
    if arguments.stage == 'promote' and not arguments.receipts:
        parser.error('promote requires --receipts from this workflow run')
    root = Path(arguments.bundle).resolve()
    request, plan = admit_bundle(root, arguments.bundle_sha256)
    state = Path(arguments.state).resolve()
    if state.exists():
        raise PublicationStopped('Stage evidence root must be fresh')
    state.mkdir(mode=0o700)
    if arguments.stage == 'consume':
        platform = arguments.platform
        if platform not in PLATFORMS:
            raise PublicationStopped('Consumer platform is not admitted')
        pin = plan['cli'][platform]
        executable = contained_file(root, pin['input_file'])
        executable.chmod(0o755)
        source_checkout = Path(os.environ['GITHUB_WORKSPACE'])
        expected_scripts = source_checkout / 'workflows/package/package-catalog-staging-promote/assets/scripts'
        if Path(__file__).resolve().parent != expected_scripts.resolve():
            raise PublicationStopped('Consumer helpers are outside the actual sparse checkout')
        receipt = verify_consumer(executable, pin['sha256'], pin['version'], state / 'consumer', plan,
                                  platform, plan['input_manifests'][platform],
                                  plan['dependency_closures'][platform], plan['dependency_closures'][platform],
                                  source_checkout)
        bound = bind_native_receipt(receipt, os.environ)
        write_exclusive(state / 'receipt.json', json_bytes(bound))
        return
    if not arguments.execute:
        raise PublicationStopped('Provider stages require explicit execution selection')
    expected = destination_from_environment(request['destination'])
    if not os.environ.get('AWS_ACCESS_KEY_ID') or not os.environ.get('AWS_SECRET_ACCESS_KEY'):
        raise PublicationStopped('Explicit staging credential references are unresolved')
    api = GitHubReadOnly()
    candidate_before = admit_candidate(request['candidate'], api)
    executable, digest = prepare_aws(request['aws_cli_archive'], state / 'official-aws')
    temporary = state / 'temporary'
    temporary.mkdir(mode=0o700)
    provider = S3Provider(executable, digest, expected['endpoint'], expected['bucket'], temporary, plan['prefix'])
    reader = public_reader(temporary)
    def checkpoint(value):
        with (state / 'checkpoints.jsonl').open('ab') as output:
            output.write(json_bytes(value))
            output.flush()
            os.fsync(output.fileno())
    def consumers(current_plan):
        if admit_candidate(request['candidate'], api) != candidate_before:
            raise PublicationStopped('Product candidate changed before pointer commit')
        from catalog_runtime_gate import admit_runtime
        acquire = owning_acquirer()
        source_lock = json.loads(acquire(request['candidate']['source_lock'], state / 'source-cache').read_bytes())
        dependencies = json.loads(acquire(request['candidate']['source_dependencies'], state / 'source-cache').read_bytes())
        current_runtime = admit_runtime(request['runtime'], source_lock, dependencies, request['cli'],
                                        reader, state / 'runtime-readiness', acquire, native_package_stores(request, root))
        if current_runtime != current_plan['runtime_binding']:
            raise PublicationStopped('Runtime/CLI/SDK or package adapter changed before optional catalog commit')
        receipts_root = Path(arguments.receipts).resolve()
        receipts = {platform: json.loads(contained_file(receipts_root, platform + '/receipt.json').read_bytes())
                    for platform in PLATFORMS}
        return admit_native_receipts(current_plan, receipts, current_plan['cli'],
                                     current_plan['input_manifests'], current_plan['dependency_closures'],
                                     os.environ, api)
    transaction = CatalogTransaction(provider, reader, consumers, checkpoint)
    if arguments.stage == 'upload':
        def payload(item):
            if 'bytes' in item:
                return item['bytes']
            source = plan['archive_sources'][item['key']]
            if 'input_file' in source:
                return contained_file(root / 'inputs', source['input_file'])
            target = state / 'one-public-payload'
            if target.exists():
                target.unlink()
            from catalog_inputs import acquire_baseline
            acquire_baseline({**item, 'url': source['url']}, target, reader)
            return target
        result = transaction.stage_objects(plan, plan['baseline_pointer'], payload)
        write_exclusive(state / 'staged.json', json_bytes(result))
    else:
        staged = json.loads(contained_file(Path(arguments.receipts).resolve(), 'upload/staged.json').read_bytes())
        result = transaction.finish(plan, plan['baseline_pointer'], staged)
        write_exclusive(state / 'promotion.json', json_bytes(result))
    if admit_candidate(request['candidate'], api) != candidate_before:
        raise PublicationStopped('Product candidate changed during transaction')


if __name__ == '__main__':
    try:
        main()
    except (PublicationStopped, ValueError, KeyError, OSError) as error:
        print('Catalog promotion stopped: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
