#include "Check.h"

#include "../src/input/TouchTracker.h"

using namespace dd::input;
using dd::protocol::TouchAction;
using dd::protocol::TouchContact;
using dd::protocol::TouchFrame;

namespace
{

TouchFrame Frame(std::initializer_list<TouchContact> contacts)
{
    return TouchFrame{std::vector<TouchContact>(contacts)};
}

void DownMoveUp()
{
    TouchTracker t;
    auto e = t.Apply(Frame({{0, TouchAction::Down, 100, 200, 0}}));
    CHECK(e.size() == 1 && e[0].id == 0 && e[0].state == ContactState::Down && e[0].x == 100 && e[0].y == 200);

    e = t.Apply(Frame({{0, TouchAction::Move, 110, 210, 0}}));
    CHECK(e.size() == 1 && e[0].state == ContactState::Update && e[0].x == 110);

    // A repeated Down for an active contact is just an update.
    e = t.Apply(Frame({{0, TouchAction::Down, 120, 220, 0}}));
    CHECK(e.size() == 1 && e[0].state == ContactState::Update);

    e = t.Apply(Frame({{0, TouchAction::Up, 120, 220, 0}}));
    CHECK(e.size() == 1 && e[0].state == ContactState::Up);

    // Up/cancel for a contact that isn't down produces nothing.
    CHECK(t.Apply(Frame({{0, TouchAction::Up, 0, 0, 0}})).empty());
    CHECK(t.Apply(Frame({{3, TouchAction::Cancel, 0, 0, 0}})).empty());
}

void MoveWithoutDownStartsContact()
{
    TouchTracker t;
    auto e = t.Apply(Frame({{2, TouchAction::Move, 5, 6, 0}}));
    CHECK(e.size() == 1 && e[0].state == ContactState::Down);
}

void MissingContactsAreLifted()
{
    TouchTracker t;
    t.Apply(Frame({{0, TouchAction::Down, 1, 1, 0}, {1, TouchAction::Down, 50, 60, 0}}));

    // Contact 1 vanished (lost message): lifted at its last position.
    auto e = t.Apply(Frame({{0, TouchAction::Move, 2, 2, 0}}));
    CHECK(e.size() == 2);
    CHECK(e[0].id == 0 && e[0].state == ContactState::Update);
    CHECK(e[1].id == 1 && e[1].state == ContactState::Up && e[1].x == 50 && e[1].y == 60);

    e = t.ReleaseAll();
    CHECK(e.size() == 1 && e[0].id == 0 && e[0].state == ContactState::Up);
    CHECK(t.ReleaseAll().empty());
}

void CancelAndDuplicates()
{
    TouchTracker t;
    t.Apply(Frame({{4, TouchAction::Down, 1, 1, 0}}));
    auto e = t.Apply(Frame({{4, TouchAction::Cancel, 1, 1, 0}, {4, TouchAction::Move, 9, 9, 0}}));
    CHECK(e.size() == 1 && e[0].state == ContactState::Cancel); // duplicate id ignored
    CHECK(t.ReleaseAll().empty());
}

} // namespace

void RunTouchTests()
{
    DownMoveUp();
    MoveWithoutDownStartsContact();
    MissingContactsAreLifted();
    CancelAndDuplicates();
}
