// SPDX-License-Identifier: Apache-2.0
#include "victus/platform/platform.hpp"

#include "doctest.h"

using namespace victus;
using platform::InputState;
using platform::Key;
using platform::MouseButton;

TEST_CASE("input state: held keys report one press edge and one release edge") {
    InputState in;
    in.begin_frame();
    in.set_key(Key::F5, true);
    CHECK(in.key_down(Key::F5));
    CHECK(in.key_pressed(Key::F5));
    CHECK_FALSE(in.key_released(Key::F5));
    CHECK_FALSE(in.key_was_down(Key::F5));

    in.begin_frame();
    in.set_key(Key::F5, true); // key repeat is not a new press
    CHECK(in.key_down(Key::F5));
    CHECK_FALSE(in.key_pressed(Key::F5));
    CHECK(in.key_was_down(Key::F5));

    in.begin_frame();
    in.set_key(Key::F5, false);
    CHECK_FALSE(in.key_down(Key::F5));
    CHECK(in.key_released(Key::F5));
    CHECK_FALSE(in.key_pressed(Key::F5));

    in.begin_frame();
    CHECK_FALSE(in.key_released(Key::F5));
}

// Regression: at a low frame rate (software rendering, slow phones) a tap's down and up events both
// arrive before the next frame. The edges used to be derived from the end-of-frame state only, so the
// tap vanished: F5 did not save and quick clicks on buttons were ignored.
TEST_CASE("input state: a key or click shorter than one frame is not lost") {
    InputState in;
    in.begin_frame();
    in.set_key(Key::F5, true);
    in.set_key(Key::F5, false);
    CHECK_FALSE(in.key_down(Key::F5));
    CHECK(in.key_pressed(Key::F5));
    CHECK(in.key_released(Key::F5));

    in.set_mouse_button(MouseButton::Left, true);
    in.set_mouse_button(MouseButton::Left, false);
    CHECK_FALSE(in.mouse_down(MouseButton::Left));
    CHECK(in.mouse_pressed(MouseButton::Left));
    CHECK(in.mouse_released(MouseButton::Left));
    CHECK_FALSE(in.mouse_was_down(MouseButton::Left));

    in.begin_frame(); // the edges last exactly one frame
    CHECK_FALSE(in.key_pressed(Key::F5));
    CHECK_FALSE(in.key_released(Key::F5));
    CHECK_FALSE(in.mouse_pressed(MouseButton::Left));
    CHECK_FALSE(in.mouse_released(MouseButton::Left));
}

TEST_CASE("input state: unmapped keys never register") {
    InputState in;
    in.begin_frame();
    in.set_key(Key::Unknown, true);
    CHECK_FALSE(in.key_down(Key::Unknown));
    CHECK_FALSE(in.key_pressed(Key::Unknown));
}
