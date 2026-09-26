#include "TouchTracker.h"

namespace dd::input
{

std::vector<ContactEvent> TouchTracker::Apply(const protocol::TouchFrame& frame)
{
    std::vector<ContactEvent> events;
    std::array<bool, protocol::kMaxTouchContacts> seen{};
    for (const auto& c : frame.contacts)
    {
        if (c.id >= m_slots.size() || seen[c.id])
        {
            continue;
        }
        seen[c.id] = true;
        Slot& slot = m_slots[c.id];
        ContactEvent e{c.id, ContactState::Update, c.x, c.y, c.pressure};
        switch (c.action)
        {
        case protocol::TouchAction::Down:
        case protocol::TouchAction::Move:
            e.state = slot.active ? ContactState::Update : ContactState::Down;
            slot = {true, c.x, c.y};
            break;
        case protocol::TouchAction::Up:
        case protocol::TouchAction::Cancel:
            if (!slot.active)
            {
                continue; // never went down here: nothing to lift
            }
            e.state = c.action == protocol::TouchAction::Up ? ContactState::Up : ContactState::Cancel;
            slot.active = false;
            break;
        }
        events.push_back(e);
    }
    for (uint8_t id = 0; id < m_slots.size(); ++id)
    {
        Slot& slot = m_slots[id];
        if (slot.active && !seen[id])
        {
            events.push_back({id, ContactState::Up, slot.x, slot.y, 0});
            slot.active = false;
        }
    }
    return events;
}

std::vector<ContactEvent> TouchTracker::ReleaseAll()
{
    return Apply({});
}

} // namespace dd::input
