"""Dockpipe source-workflow entrypoint; execution context is checked by the driver."""
import os

from catalog_driver import main
from catalog_transaction import PublicationStopped


def arguments(environment):
    stage = environment.get('DOCKPIPE_CATALOG_PROMOTION_STAGE', '')
    if stage not in {'prepare', 'upload', 'consume', 'promote'}:
        raise PublicationStopped('Explicit catalog promotion stage is required')
    result = [stage]
    fields = [('DOCKPIPE_CATALOG_PROMOTION_BUNDLE', '--bundle')]
    if stage == 'prepare':
        fields += [('DOCKPIPE_CATALOG_PROMOTION_REQUEST', '--request'),
                   ('DOCKPIPE_CATALOG_PROMOTION_REQUEST_SHA256', '--request-sha256')]
    else:
        fields += [('DOCKPIPE_CATALOG_PROMOTION_BUNDLE_SHA256', '--bundle-sha256'),
                   ('DOCKPIPE_CATALOG_PROMOTION_STATE', '--state')]
    if stage == 'consume':
        fields.append(('DOCKPIPE_CATALOG_PROMOTION_PLATFORM', '--platform'))
    if stage == 'promote':
        fields.append(('DOCKPIPE_CATALOG_PROMOTION_RECEIPTS', '--receipts'))
    for variable, flag in fields:
        value = environment.get(variable, '')
        if not value:
            raise PublicationStopped('Required promotion input reference is absent: ' + variable)
        result += [flag, value]
    if stage in {'upload', 'promote'}:
        if environment.get('DOCKPIPE_CATALOG_PROMOTION_EXECUTE') != '1':
            raise PublicationStopped('Provider stage requires explicit execution selection')
        result.append('--execute')
    return result


if __name__ == '__main__':
    main(arguments(os.environ))
