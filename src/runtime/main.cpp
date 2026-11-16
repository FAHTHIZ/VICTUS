#include "victus/core/engine_clock.hpp"
#include <chrono>
#include <iostream>
int main() {
    victus::core::EngineClock clock;
    clock.reset();
    const auto frame = clock.advance(std::chrono::steady_clock::now());
    std::cout << "VICTUS Runtime 0.1.0\n"
              << "Core initialized; simulation steps: " << frame.steps << '\n'
              << "Bootstrap runtime — not yet a full game engine.\n";
    return 0;
}
