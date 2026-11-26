#include "ClipboardPasteIntent.h"

#include <utility>

namespace dockpipe::desktop {
namespace {

bool digest(const QJsonValue &value)
{
    if (!value.isString())
        return false;
    const auto text = value.toString();
    if (text.size() != 64)
        return false;
    for (const auto character : text) {
        if (!((character >= u'0' && character <= u'9') || (character >= u'a' && character <= u'f')))
            return false;
    }
    return true;
}

bool validBinding(const ClipboardPasteBinding &binding)
{
    const auto &reference = binding.reference;
    const auto target = reference.value("target").toObject();
    const auto node = target.value("node");
    return binding.generation >= 0 && binding.epoch > 0 && reference.size() == 4
        && reference.value("schema") == "dockpipe.remote.desktop/v1"
        && digest(reference.value("id")) && digest(reference.value("epoch"))
        && target.size() == 3 && node.isString() && !node.toString().isEmpty()
        && digest(target.value("fingerprint")) && digest(target.value("generation"));
}

bool validChord(const ClipboardPasteChord &chord)
{
    if ((chord.key != 25 && chord.key != 73) || chord.modifiers.size() > 3)
        return false;
    QList<quint16> observed;
    for (const auto modifier : chord.modifiers) {
        if (modifier < 224 || modifier > 231 || observed.contains(modifier))
            return false;
        observed.append(modifier);
    }
    return true;
}

} // namespace

bool ClipboardPasteIntent::request(ClipboardPasteBinding binding, ClipboardPasteChord chord, bool committed)
{
    if (!validBinding(binding) || !validChord(chord)) {
        cancel();
        return false;
    }
    m_request = Request{std::move(binding), std::move(chord), committed};
    return true;
}

void ClipboardPasteIntent::outgoingCommitted(const ClipboardPasteBinding &binding)
{
    if (m_request && m_request->binding == binding)
        m_request->committed = true;
}

std::optional<ClipboardPasteChord> ClipboardPasteIntent::take(const ClipboardPasteBinding &binding,
    bool eligible, bool physicalReleased)
{
    if (!m_request)
        return std::nullopt;
    if (!eligible || m_request->binding != binding) {
        cancel();
        return std::nullopt;
    }
    if (!m_request->committed || !physicalReleased)
        return std::nullopt;

    auto chord = std::move(m_request->chord);
    cancel();
    return chord;
}

void ClipboardPasteIntent::cancel()
{
    m_request.reset();
}

bool ClipboardPasteIntent::pending() const
{
    return m_request.has_value();
}

} // namespace dockpipe::desktop
