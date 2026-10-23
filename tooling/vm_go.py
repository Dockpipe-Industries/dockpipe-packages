"""Explicit host matrix for VM Go tools; Linux qualification stays Linux-owned."""

WINDOWS_PACKAGES = (
    './internal/protocol',
    './internal/controller',
    './internal/manifest',
    './internal/keymaterial',
    './internal/recovery',
    './internal/guest',
    './cmd/dockpipe-guest-agent',
)


def build_arguments(host_system):
    if host_system == 'Linux':
        return ['go', 'build', './...']
    if host_system == 'Windows':
        return ['go', 'build', *WINDOWS_PACKAGES]
    raise ValueError('VM tools support Linux qualification and Windows compatibility hosts only')


def test_arguments(host_system):
    if host_system == 'Linux':
        return [['go', 'test', './...']]
    if host_system != 'Windows':
        raise ValueError('VM tools support Linux qualification and Windows compatibility hosts only')
    return [
        ['go', 'test', './internal/protocol', './internal/controller',
         './internal/manifest', './internal/guest', './cmd/dockpipe-guest-agent'],
        # Recovery state transitions are portable. Its owner-only file store
        # and directory fsync remain part of the Linux qualification lane.
        ['go', 'test', './internal/recovery', '-run',
         '^(TestPendingTicketBlocksAndRecoveryIsConsumedBeforeResult|'
         'TestRecoveryRejectsSubstitutionAndDurableWriteFailure)$'],
        # Collision rejection is portable; this is not Windows ACL validation.
        ['go', 'test', './internal/keymaterial', '-run',
         '^TestGenerateRejectsExistingPublicKeyWithoutCreatingPrivateKey$'],
    ]
