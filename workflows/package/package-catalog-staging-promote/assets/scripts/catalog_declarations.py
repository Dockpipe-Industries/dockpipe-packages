"""Read only hash-admitted compiled declarations and resolve their package closure."""
import tarfile

from catalog_plan import identity
from catalog_s3 import file_digest
from catalog_transaction import PublicationStopped


def read_declarations(archive, kind, name, expected):
    import yaml
    digest, size = file_digest(archive, expected['size_bytes'])
    if digest != expected['sha256'] or size != expected['size_bytes']:
        raise PublicationStopped('Compiled declaration archive differs from public manifest')
    prefix = kind + 's/' + name + '/'
    selected = {prefix + 'package.yml': None, prefix + 'config.yml': None}
    with tarfile.open(archive, 'r:gz') as stream:
        expanded = 0
        for index, member in enumerate(stream):
            expanded += member.size
            if index >= 100000 or expanded > 2 * 1024 * 1024 * 1024:
                raise PublicationStopped('Compiled declaration archive exceeds admission bounds')
            if member.name not in selected:
                continue
            if not member.isfile() or member.size > 1048576 or selected[member.name] is not None:
                raise PublicationStopped('Compiled declaration member is duplicate or unsafe')
            with stream.extractfile(member) as content:
                selected[member.name] = yaml.safe_load(content.read(1048577))
    package = selected[prefix + 'package.yml']
    config = selected[prefix + 'config.yml']
    if not isinstance(package, dict) or package.get('name') != name or package.get('version') != expected['version']:
        raise PublicationStopped('Compiled package declaration identity differs')
    if config is not None and not isinstance(config, dict):
        raise PublicationStopped('Compiled workflow declaration is invalid')
    result = {'depends': package.get('depends', []),
              'requires_resolvers': package.get('requires_resolvers', []),
              'package_platforms': package.get('platforms', [])}
    if kind == 'workflow':
        if config is None:
            raise PublicationStopped('Compiled workflow declaration is absent')
        result['workflow_platforms'] = config.get('platforms', [])
        result['resolver'] = config.get('resolver')
    for field in ['depends', 'requires_resolvers', 'package_platforms', 'workflow_platforms']:
        values = result.get(field, [])
        if (not isinstance(values, list) or len(values) > 64
                or any(not isinstance(value, str) or not value.strip() for value in values)):
            raise PublicationStopped('Compiled declaration list is invalid or unbounded')
    return result


def dependency_closure(requested, available, metadata):
    active = set()
    complete = set()

    def visit(key, depth):
        if depth > 32 or len(active) + len(complete) >= 64:
            raise PublicationStopped('Selected dependency closure exceeds bounds')
        if key in active:
            raise PublicationStopped('Selected dependency closure has a cycle')
        if key in complete:
            return
        if key not in available or key not in metadata:
            raise PublicationStopped('Selected dependency lacks hash-verified compiled declarations')
        active.add(key)
        declaration = metadata[key]
        resolvers = set(declaration.get('requires_resolvers', []))
        names = set(declaration.get('depends', [])) | resolvers
        for name in names:
            if name in resolvers:
                matches = [candidate for candidate in available if candidate == identity('resolver', name)]
            else:
                matches = [candidate for candidate in available if candidate.split('/', 1)[1] == name]
            if len(matches) != 1:
                raise PublicationStopped('Selected dependency is missing or ambiguous: ' + name)
            visit(matches[0], depth + 1)
        active.remove(key)
        complete.add(key)

    visit(requested, 0)
    return sorted(complete)
