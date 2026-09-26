#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "../protocol/Protocol.h"

namespace dd::input
{

enum class ContactState : uint8_t
{
    Down,   // new contact
    Update, // still touching, maybe moved
    Up,     // lifted
    Cancel, // lifted, gesture cancelled
};

struct ContactEvent
{
    uint8_t id = 0;
    ContactState state = ContactState::Update;
    uint16_t x = 0; // normalized 0..65535, as on the wire
    uint16_t y = 0;
    uint16_t pressure = 0;
};

// Turns client touch frames into the transitions Windows touch injection expects: Down only for
// a new contact, Update while it stays down, Up exactly once. Contacts the client stops
// reporting are lifted, so a lost message can't leave a finger stuck on the desktop.
class TouchTracker
{
  public:
    std::vector<ContactEvent> Apply(const protocol::TouchFrame& frame);

    // Lifts every active contact (session end).
    std::vector<ContactEvent> ReleaseAll();

  private:
    struct Slot
    {
        bool active = false;
        uint16_t x = 0;
        uint16_t y = 0;
    };
    std::array<Slot, protocol::kMaxTouchContacts> m_slots{};
};

} // namespace dd::input
