// SPDX-License-Identifier: Apache-2.0
// Device-loss detection (rhi::DeviceHealth) with injected driver failures. Each case creates its
// own device so the shared test device is never marked lost.
#include "gpu_fixture.hpp"

#include <string>

using namespace victus;
using rhi::GpuFailure;

namespace {
std::unique_ptr<rhi::Device> fresh_device() {
    rhi::DeviceDesc dd;
    dd.headless = true;
    dd.debug = false;
    auto d = rhi::Device::create(dd);
    REQUIRE(d.ok());
    return std::move(d).value();
}
std::uint32_t run_of(const rhi::Device& dev, GpuFailure k) { return dev.health().consecutive[static_cast<int>(k)]; }
} // namespace

TEST_CASE("rhi device loss: a run of failed submissions marks the device lost; a success resets the run") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto dev = fresh_device();
    CHECK_FALSE(dev->lost());
    CHECK(dev->lost_reason().empty());

    dev->inject_failure(GpuFailure::Submit);
    for (std::uint32_t i = 0; i + 1 < rhi::kDeviceLostAfterFailures; ++i) dev->begin_commands().submit();
    CHECK_FALSE(dev->lost());
    CHECK(run_of(*dev, GpuFailure::Submit) == rhi::kDeviceLostAfterFailures - 1);

    dev->inject_failure(GpuFailure::None); // transient: the next submission works
    dev->begin_commands().submit();
    CHECK(run_of(*dev, GpuFailure::Submit) == 0);
    CHECK(dev->health().failures == rhi::kDeviceLostAfterFailures - 1);
    CHECK_FALSE(dev->lost());

    // Delayed injection starts at the given frame.
    const std::uint64_t start = dev->stats().frames_submitted + 5;
    dev->inject_failure(GpuFailure::Submit, start);
    for (int i = 0; i < 5; ++i) dev->begin_commands().submit();
    CHECK(run_of(*dev, GpuFailure::Submit) == 0);
    for (std::uint32_t i = 0; i < rhi::kDeviceLostAfterFailures; ++i) dev->begin_commands().submit();
    CHECK(dev->lost());
    CHECK(dev->health().last_failure == GpuFailure::Submit);
    const std::string reason = dev->lost_reason();
    CHECK(reason.find("GPU device lost: command submission failed 30 times in a row") != std::string::npos);
    CHECK(reason.find("injected failure") != std::string::npos);
    dev->inject_failure(GpuFailure::None);
    dev->wait_idle();
}

TEST_CASE("rhi device loss: failed command buffer acquisition is safe to use and detected") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto dev = fresh_device();
    dev->inject_failure(GpuFailure::CommandBuffer);
    for (std::uint32_t i = 0; i < rhi::kDeviceLostAfterFailures; ++i) {
        auto cmd = dev->begin_commands();
        CHECK(cmd.native() == nullptr);
        cmd.submit(); // no-op on a failed buffer
        cmd.submit_and_wait();
    }
    CHECK(dev->lost());
    CHECK(dev->health().last_failure == GpuFailure::CommandBuffer);
    CHECK(dev->lost_reason().find("command buffer acquisition") != std::string::npos);
    dev->inject_failure(GpuFailure::None);
}
