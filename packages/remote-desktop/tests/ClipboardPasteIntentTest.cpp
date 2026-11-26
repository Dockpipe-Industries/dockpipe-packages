#include "ClipboardPasteIntent.h"

#include <cassert>
#include <iostream>

using namespace dockpipe::desktop;

namespace {

ClipboardPasteBinding binding()
{
    const QJsonObject target{{"node", "worker-one"}, {"fingerprint", QString(64, 'a')},
        {"generation", QString(64, 'b')}};
    const QJsonObject reference{{"schema", "dockpipe.remote.desktop/v1"}, {"id", QString(64, 'c')},
        {"epoch", QString(64, 'd')}, {"target", target}};
    return {reference, 4, 7, false};
}

const ClipboardPasteChord ControlV{{224}, 25};
const ClipboardPasteChord CommandV{{227}, 25};

void bothCompletionOrderings()
{
    const auto current = binding();
    ClipboardPasteIntent intent;
    intent.request(current, ControlV);
    intent.outgoingCommitted(current);
    assert(!intent.take(current, true, false));
    assert(intent.pending());
    assert(intent.take(current, true, true) == ControlV);
    assert(!intent.pending());

    intent.request(current, CommandV);
    assert(!intent.take(current, true, true));
    intent.outgoingCommitted(current);
    assert(intent.take(current, true, true) == CommandV);
}

void completionCannotCreateOrRepeatGesture()
{
    const auto current = binding();
    ClipboardPasteIntent intent;
    intent.outgoingCommitted(current);
    assert(!intent.pending());
    intent.request(current, ControlV);
    intent.outgoingCommitted(current);
    intent.outgoingCommitted(current);
    assert(intent.take(current, true, true) == ControlV);
    intent.outgoingCommitted(current);
    assert(!intent.take(current, true, true));
}

void everyBindingDimensionFencesPaste()
{
    const auto current = binding();
    QList<ClipboardPasteBinding> stale;
    auto changed = current;
    changed.reference.insert("id", "session-two");
    stale.append(changed);
    changed = current;
    changed.reference.insert("target", QJsonObject{{"node", "worker-two"}});
    stale.append(changed);
    changed = current;
    ++changed.generation;
    stale.append(changed);
    changed = current;
    ++changed.epoch;
    stale.append(changed);
    changed = current;
    changed.swapControlCommand = true;
    stale.append(changed);

    for (const auto &other : stale) {
        ClipboardPasteIntent intent;
        intent.request(current, ControlV);
        intent.outgoingCommitted(other);
        assert(!intent.take(current, true, true));
        intent.outgoingCommitted(current);
        assert(!intent.take(other, true, true));
        assert(!intent.pending());
    }
}

void cancellationAndEligibilityCannotRevive()
{
    const auto current = binding();
    ClipboardPasteIntent intent;
    intent.request(current, CommandV, true);
    assert(!intent.take(current, false, false));
    assert(!intent.pending());
    assert(!intent.take(current, true, true));
    intent.request(current, CommandV);
    intent.cancel();
    intent.outgoingCommitted(current);
    assert(!intent.take(current, true, true));
}

void latestGestureReplacesOldCommitAndChord()
{
    const auto current = binding();
    auto next = current;
    ++next.generation;
    ClipboardPasteIntent intent;
    intent.request(current, ControlV, true);
    intent.request(next, CommandV);
    intent.outgoingCommitted(current);
    assert(!intent.take(next, true, true));
    intent.outgoingCommitted(next);
    assert(intent.take(next, true, true) == CommandV);

    intent.request(current, ControlV, true);
    intent.request(current, CommandV);
    assert(!intent.take(current, true, true));
    intent.outgoingCommitted(current);
    assert(intent.take(current, true, true) == CommandV);
}

void invalidRequestsCancelAtAdmission()
{
    const auto current = binding();
    ClipboardPasteIntent intent;
    const QList<ClipboardPasteChord> invalidChords{
        {{224}, 4}, {{223}, 25}, {{232}, 25}, {{224, 224}, 25}, {{224, 225, 226, 227}, 25}};
    for (const auto &chord : invalidChords) {
        assert(intent.request(current, CommandV, true));
        assert(!intent.request(current, chord, true));
        assert(!intent.pending());
    }
    auto invalid = current;
    invalid.generation = -1;
    assert(!intent.request(invalid, CommandV));
    invalid = current;
    invalid.epoch = 0;
    assert(!intent.request(invalid, CommandV));
    invalid = current;
    invalid.reference = {};
    assert(!intent.request(invalid, CommandV));
    invalid = current;
    invalid.reference.insert("id", QString(64, 'X'));
    assert(!intent.request(invalid, CommandV));
    assert(intent.request(current, {{225}, 73}, true));
    assert(intent.take(current, true, true) == (ClipboardPasteChord{{225}, 73}));
}

void dropUsesExplicitMappedCommandChord()
{
    auto current = binding();
    current.swapControlCommand = true;
    ClipboardPasteIntent intent;
    intent.request(current, CommandV, true);
    assert(!intent.take(current, true, false));
    const auto chord = intent.take(current, true, true);
    assert(chord && chord->modifiers == QList<quint16>{227} && chord->key == 25);
    assert(!intent.take(current, true, true));
}

} // namespace

int main()
{
    bothCompletionOrderings();
    completionCannotCreateOrRepeatGesture();
    everyBindingDimensionFencesPaste();
    cancellationAndEligibilityCannotRevive();
    latestGestureReplacesOldCommitAndChord();
    invalidRequestsCancelAtAdmission();
    dropUsesExplicitMappedCommandChord();
    std::cout << "PASS paste intent: ordering, once-only commit, exact binding, cancellation, latest gesture and drop chord\n";
}
