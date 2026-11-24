/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dmesgnet-phase2.cpp
 * @brief Exercise DMesgNet validation, buffering, and endpoint behavior.
 */

#include "dmn-dmesgnet.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class TestIo final : public dmn::Dmn_Io<std::string> {
public:
  auto read() -> std::optional<std::string> override {
    std::unique_lock lock{m_mutex};
    m_changed.wait(lock, [this] { return m_stopped || !m_input.empty(); });
    if (m_input.empty()) {
      return std::nullopt;
    }

    auto value{std::move(m_input.front())};
    m_input.pop_front();

    return value;
  }

  void write(const std::string &value) override {
    std::lock_guard lock{m_mutex};
    dmn::DMesgPb message;
    if (m_fail_destroyed_write && message.ParseFromString(value) &&
        message.type() == dmn::DMesgTypePb::sys && message.has_body() &&
        message.body().has_sys() && message.body().sys().has_self() &&
        message.body().sys().self().state() == dmn::DMesgStatePb::Destroyed) {
      throw std::runtime_error("injected final heartbeat failure");
    }

    if (m_fail_application_writes && message.ParseFromString(value) &&
        message.type() == dmn::DMesgTypePb::message) {
      if (m_on_application_failure) {
        m_on_application_failure();
      }

      throw std::runtime_error("injected output failure");
    }

    if (m_fail_after && message.ParseFromString(value) &&
        message.type() == dmn::DMesgTypePb::message) {
      if (*m_fail_after == 0) {
        throw std::runtime_error("injected output failure");
      }

      --*m_fail_after;
    }

    m_output.push_back(value);
    m_changed.notify_all();
  }

  void write(std::string &&value) override { write(value); }

  void shutdown() override {
    std::lock_guard lock{m_mutex};
    m_stopped = true;
    m_changed.notify_all();
  }

  void deliver(std::string value) {
    std::lock_guard lock{m_mutex};
    m_input.push_back(std::move(value));
    m_changed.notify_all();
  }

  void failApplicationWrites() {
    std::lock_guard lock{m_mutex};
    m_fail_application_writes = true;
  }

  void onApplicationFailure(std::function<void()> callback) {
    std::lock_guard lock{m_mutex};
    m_on_application_failure = std::move(callback);
  }

  void failDestroyedWrite() {
    std::lock_guard lock{m_mutex};
    m_fail_destroyed_write = true;
  }

  void failAfterApplications(std::size_t count) {
    std::lock_guard lock{m_mutex};
    m_fail_after = count;
  }

  auto applicationMessages() -> std::vector<dmn::DMesgPb> {
    std::lock_guard lock{m_mutex};
    return applicationMessagesInternal();
  }

  auto
  waitForApplicationMessages(std::size_t count) -> std::vector<dmn::DMesgPb> {
    std::unique_lock lock{m_mutex};
    m_changed.wait_for(lock, std::chrono::seconds{3}, [&] {
      return applicationMessagesInternal().size() >= count;
    });

    return applicationMessagesInternal();
  }

  auto waitForSystemState(dmn::DMesgStatePb state) -> bool {
    std::unique_lock lock{m_mutex};
    return m_changed.wait_for(lock, std::chrono::seconds{3}, [&] {
      for (const auto &payload : m_output) {
        dmn::DMesgPb message;
        if (message.ParseFromString(payload) &&
            message.type() == dmn::DMesgTypePb::sys && message.has_body() &&
            message.body().has_sys() && message.body().sys().has_self() &&
            message.body().sys().self().state() == state) {
          return true;
        }
      }

      return false;
    });
  }

  auto pendingBeforeReady() -> std::size_t {
    std::lock_guard lock{m_mutex};
    std::size_t pending{};
    for (const auto &payload : m_output) {
      dmn::DMesgPb message;
      if (message.ParseFromString(payload) &&
          message.type() == dmn::DMesgTypePb::sys && message.has_body() &&
          message.body().has_sys() && message.body().sys().has_self()) {
        const auto state{message.body().sys().self().state()};
        if (state == dmn::DMesgStatePb::Ready) {
          break;
        }
        if (state == dmn::DMesgStatePb::MasterPending) {
          ++pending;
        }
      }
    }

    return pending;
  }

private:
  auto applicationMessagesInternal() const -> std::vector<dmn::DMesgPb> {
    std::vector<dmn::DMesgPb> result;
    for (const auto &payload : m_output) {
      dmn::DMesgPb message;
      if (message.ParseFromString(payload) &&
          message.type() == dmn::DMesgTypePb::message) {
        result.push_back(message);
      }
    }

    return result;
  }

  std::mutex m_mutex;
  std::condition_variable m_changed;
  std::deque<std::string> m_input;
  std::vector<std::string> m_output;
  bool m_stopped{};
  bool m_fail_application_writes{};
  bool m_fail_destroyed_write{};
  std::optional<std::size_t> m_fail_after{};
  std::function<void()> m_on_application_failure{};
};

template <typename Predicate> auto waitUntil(Predicate predicate) -> bool {
  const auto deadline{std::chrono::steady_clock::now() +
                      std::chrono::seconds{3}};
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }

    std::this_thread::yield();
  }

  return predicate();
}

auto serialize(const dmn::DMesgPb &message) -> std::string {
  std::string payload;
  EXPECT_TRUE(message.SerializeToString(&payload));

  return payload;
}

auto applicationMessage(const std::string &topic) -> dmn::DMesgPb {
  dmn::DMesgPb message;
  message.set_type(dmn::DMesgTypePb::message);
  message.set_topic(topic);
  message.set_sourceidentifier("sender");

  return message;
}

auto readyMessage() -> dmn::DMesgPb {
  dmn::DMesgPb message;
  message.set_type(dmn::DMesgTypePb::sys);
  message.set_topic(dmn::kDMesgSysIdentifier);
  auto *self{message.mutable_body()->mutable_sys()->mutable_self()};
  self->set_identifier("peer");
  self->set_state(dmn::DMesgStatePb::Ready);
  self->set_masteridentifier("peer");
  self->mutable_initializedtimestamp()->set_seconds(1);
  self->mutable_updatedtimestamp()->set_seconds(1);

  return message;
}

class ManualHeartbeat final : public dmn::Dmn_DMesgNet::Heartbeat {
public:
  explicit ManualHeartbeat(std::function<void()> tick)
      : m_tick{std::move(tick)} {}

  void tick() { m_tick(); }

private:
  std::function<void()> m_tick;
};

auto manualOptions() -> dmn::Dmn_DMesgNet::Options {
  dmn::Dmn_DMesgNet::Options options;
  options.m_heartbeat_factory = [](std::function<void()> tick) {
    return std::make_unique<ManualHeartbeat>(std::move(tick));
  };

  return options;
}

class ImmediateHeartbeat final : public dmn::Dmn_DMesgNet::Heartbeat {
public:
  explicit ImmediateHeartbeat(std::function<void()> tick)
      : m_thread{[tick = std::move(tick)] {
          for (int i = 0; i < DMN_DMESGNET_MASTERPENDING_MAX_COUNTER; ++i) {
            tick();
          }
        }} {}

  ~ImmediateHeartbeat() override { m_thread.join(); }

private:
  std::thread m_thread;
};

TEST(DmnDmesgNetPhase2Test, InitialHeartbeatPrecedesEarlyTimerTicks) {
  auto input{std::make_shared<TestIo>()};
  auto output{std::make_shared<TestIo>()};
  dmn::Dmn_DMesgNet::Options options;
  options.m_heartbeat_factory = [output](std::function<void()> tick) {
    auto timer{std::make_unique<ImmediateHeartbeat>(std::move(tick))};
    EXPECT_TRUE(output->waitForSystemState(dmn::DMesgStatePb::Ready));
    return timer;
  };
  dmn::Dmn_DMesgNet node{"early-tick", input, output, options};
  EXPECT_EQ(output->pendingBeforeReady(),
            DMN_DMESGNET_MASTERPENDING_MAX_COUNTER);
}

TEST(DmnDmesgNetPhase2Test, ManualHeartbeatsDriveElectionAndTimeout) {
  auto input{std::make_shared<TestIo>()};
  auto output{std::make_shared<TestIo>()};
  ManualHeartbeat *clock{};
  dmn::Dmn_DMesgNet::Options options;
  options.m_heartbeat_factory = [&clock](std::function<void()> tick) {
    auto timer{std::make_unique<ManualHeartbeat>(std::move(tick))};
    clock = timer.get();
    return timer;
  };
  dmn::Dmn_DMesgNet node{"manual-heartbeat", input, output, options};
  ASSERT_NE(clock, nullptr);
  EXPECT_FALSE(node.getStatus().m_ready);
  input->deliver(serialize(readyMessage()));
  ASSERT_TRUE(waitUntil([&] { return node.getStatus().m_ready; }));
  for (int i = 0; i < DMN_DMESGNET_MASTERSYNC_MAX_COUNTER; ++i) {
    clock->tick();
    node.addExecTaskWithWait([] {})->wait();
  }
  EXPECT_FALSE(node.getStatus().m_ready);
  for (int i = 0; i < DMN_DMESGNET_MASTERPENDING_MAX_COUNTER; ++i) {
    clock->tick();
    node.addExecTaskWithWait([] {})->wait();
  }
  EXPECT_TRUE(node.getStatus().m_ready);
}

TEST(DmnDmesgNetPhase2Test, RejectsNullHeartbeatFactoryResult) {
  auto input{std::make_shared<TestIo>()};
  auto output{std::make_shared<TestIo>()};
  dmn::Dmn_DMesgNet::Options options;
  options.m_heartbeat_factory = [](std::function<void()>) {
    return std::unique_ptr<dmn::Dmn_DMesgNet::Heartbeat>{};
  };
  EXPECT_THROW((dmn::Dmn_DMesgNet{"bad-timer", input, output, options}),
               std::invalid_argument);
}

TEST(DmnDmesgNetPhase2Test, RejectsMalformedInvalidAndOversizedInput) {
  auto input{std::make_shared<TestIo>()};
  dmn::Dmn_DMesgNet::Options options;
  options.m_max_frame_bytes = 128;
  dmn::Dmn_DMesgNet node{"validation", input, nullptr, options};

  input->deliver(std::string{"\x80", 1});
  dmn::DMesgPb invalid;
  invalid.set_type(dmn::DMesgTypePb::sys);
  input->deliver(serialize(invalid));
  auto missingMaster{readyMessage()};
  missingMaster.mutable_body()
      ->mutable_sys()
      ->mutable_self()
      ->clear_masteridentifier();
  input->deliver(serialize(missingMaster));
  auto invalidState{readyMessage()};
  invalidState.mutable_body()->mutable_sys()->mutable_self()->set_state(
      static_cast<dmn::DMesgStatePb>(99));
  input->deliver(serialize(invalidState));
  auto missingTimestamp{readyMessage()};
  missingTimestamp.mutable_body()
      ->mutable_sys()
      ->mutable_self()
      ->clear_initializedtimestamp();
  input->deliver(serialize(missingTimestamp));
  auto invalidTimestamp{readyMessage()};
  invalidTimestamp.mutable_body()
      ->mutable_sys()
      ->mutable_self()
      ->mutable_updatedtimestamp()
      ->set_nanos(-1);
  input->deliver(serialize(invalidTimestamp));
  auto reservedTopic{applicationMessage(dmn::kDMesgSysIdentifier)};
  input->deliver(serialize(reservedTopic));
  input->deliver(std::string(129, 'x'));

  ASSERT_TRUE(waitUntil([&] {
    const auto status{node.getStatus()};
    return status.m_malformed_input == 1 && status.m_invalid_input == 6 &&
           status.m_oversized_input == 1;
  }));
}

TEST(DmnDmesgNetPhase2Test, RejectsZeroResourceLimits) {
  dmn::Dmn_DMesgNet::Options options;
  options.m_max_pending_messages = 0;
  EXPECT_THROW((dmn::Dmn_DMesgNet{"bad-limit", nullptr, nullptr, options}),
               std::invalid_argument);
}

TEST(DmnDmesgNetPhase2Test, ReportsConflictWithoutCachedMessage) {
  auto input{std::make_shared<TestIo>()};
  dmn::Dmn_DMesgNet node{"repair", input};
  auto conflict{applicationMessage("missing")};
  conflict.set_conflict(true);
  input->deliver(serialize(conflict));

  ASSERT_TRUE(waitUntil([&] { return node.getStatus().m_repair_unavailable; }));
}

TEST(DmnDmesgNetPhase2Test, ReportsConflictWhenOutputIsAbsent) {
  auto input{std::make_shared<TestIo>()};
  dmn::Dmn_DMesgNet node{"input-only-repair", input};
  input->deliver(serialize(applicationMessage("cached")));
  ASSERT_TRUE(waitUntil(
      [&] { return node.getTopicLastMessage("cached").has_value(); }));

  auto conflict{applicationMessage("cached")};
  conflict.set_conflict(true);
  input->deliver(serialize(conflict));
  ASSERT_TRUE(waitUntil([&] { return node.getStatus().m_repair_unavailable; }));
}

TEST(DmnDmesgNetPhase2Test, InvalidSystemMessageDoesNotChangeReadiness) {
  auto input{std::make_shared<TestIo>()};
  auto output{std::make_shared<TestIo>()};
  dmn::Dmn_DMesgNet node{"invalid-system", input, output, manualOptions()};
  auto invalid{readyMessage()};
  invalid.mutable_body()
      ->mutable_sys()
      ->mutable_self()
      ->clear_masteridentifier();
  input->deliver(serialize(invalid));
  ASSERT_TRUE(waitUntil([&] { return node.getStatus().m_invalid_input == 1; }));
  EXPECT_FALSE(node.getStatus().m_ready);

  input->deliver(serialize(readyMessage()));
  ASSERT_TRUE(waitUntil([&] { return node.getStatus().m_ready; }));
}

TEST(DmnDmesgNetPhase2Test, OutputOnlySendsApplicationMessages) {
  auto output{std::make_shared<TestIo>()};
  dmn::Dmn_DMesgNet node{"output-only", nullptr, output};
  auto writer{node.openHandler("writer")};
  writer->write(applicationMessage("out"));

  const auto messages{output->waitForApplicationMessages(1)};
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages.front().topic(), "out");
}

TEST(DmnDmesgNetPhase2Test, BoundsPendingMessagesAndFlushesInFifoOrder) {
  auto input{std::make_shared<TestIo>()};
  auto output{std::make_shared<TestIo>()};
  auto options{manualOptions()};
  options.m_max_pending_messages = 2;
  dmn::Dmn_DMesgNet node{"pending", input, output, options};
  auto writer{node.openHandler("writer")};
  writer->write(applicationMessage("first"));
  writer->write(applicationMessage("second"));
  writer->write(applicationMessage("overflow"));

  ASSERT_TRUE(
      waitUntil([&] { return node.getStatus().m_pending_overflow == 1; }));
  input->deliver(serialize(readyMessage()));

  const auto messages{output->waitForApplicationMessages(2)};
  ASSERT_EQ(messages.size(), 2U);
  EXPECT_EQ(messages[0].topic(), "first");
  EXPECT_EQ(messages[1].topic(), "second");
}

TEST(DmnDmesgNetPhase2Test, BoundsPendingBytes) {
  auto input{std::make_shared<TestIo>()};
  auto output{std::make_shared<TestIo>()};
  auto options{manualOptions()};
  options.m_max_pending_bytes = 1;
  dmn::Dmn_DMesgNet node{"byte-bound", input, output, options};
  auto writer{node.openHandler("writer")};
  writer->write(applicationMessage("too-large"));

  ASSERT_TRUE(
      waitUntil([&] { return node.getStatus().m_pending_overflow == 1; }));
}

TEST(DmnDmesgNetPhase2Test, EndpointModesReportLocalReadiness) {
  dmn::Dmn_DMesgNet local{"local"};
  auto input{std::make_shared<TestIo>()};
  dmn::Dmn_DMesgNet inputOnly{"input-only", input};
  auto bothInput{std::make_shared<TestIo>()};
  auto output{std::make_shared<TestIo>()};
  dmn::Dmn_DMesgNet both{"both", bothInput, output, manualOptions()};

  EXPECT_TRUE(local.getStatus().m_ready);
  EXPECT_TRUE(inputOnly.getStatus().m_ready);
  EXPECT_FALSE(both.getStatus().m_ready);
}

TEST(DmnDmesgNetPhase2Test, ReplacingFailedOutputRetriesPendingMessages) {
  auto input{std::make_shared<TestIo>()};
  auto output{std::make_shared<TestIo>()};
  output->failApplicationWrites();
  auto options{manualOptions()};
  options.m_max_pending_messages = 1;
  dmn::Dmn_DMesgNet node{"retry", input, output, options};
  auto writer{node.openHandler("writer")};
  writer->write(applicationMessage("retained"));
  writer->write(applicationMessage("overflow"));
  ASSERT_TRUE(
      waitUntil([&] { return node.getStatus().m_pending_overflow == 1; }));
  input->deliver(serialize(readyMessage()));
  ASSERT_TRUE(
      waitUntil([&] { return node.getStatus().m_output_failures == 1; }));

  auto replacement{std::make_shared<TestIo>()};
  ASSERT_TRUE(node.setOutputHandler(replacement));
  auto barrier{node.addExecTaskWithWait([] {})};
  barrier->wait();
  const auto messages{replacement->applicationMessages()};
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages.front().topic(), "retained");
}

TEST(DmnDmesgNetPhase2Test, ReportsMessagesDroppedAfterOutputFailure) {
  auto output{std::make_shared<TestIo>()};
  output->failApplicationWrites();
  dmn::Dmn_DMesgNet node{"output-failure", nullptr, output};
  auto writer{node.openHandler("writer")};
  writer->write(applicationMessage("first"));
  ASSERT_TRUE(waitUntil([&] {
    const auto status{node.getStatus()};
    return status.m_output_failures == 1 && status.m_output_dropped == 1;
  }));
  writer->write(applicationMessage("second"));
  ASSERT_TRUE(
      waitUntil([&] { return node.getStatus().m_output_dropped == 2; }));
}

TEST(DmnDmesgNetPhase2Test, RetryDoesNotRepeatSuccessfullyFlushedMessages) {
  auto input{std::make_shared<TestIo>()};
  auto output{std::make_shared<TestIo>()};
  output->failAfterApplications(1);
  auto options{manualOptions()};
  options.m_max_pending_messages = 2;
  dmn::Dmn_DMesgNet node{"partial-flush", input, output, options};
  auto writer{node.openHandler("writer")};
  writer->write(applicationMessage("first"));
  writer->write(applicationMessage("second"));
  writer->write(applicationMessage("overflow"));
  ASSERT_TRUE(
      waitUntil([&] { return node.getStatus().m_pending_overflow == 1; }));
  input->deliver(serialize(readyMessage()));
  ASSERT_TRUE(
      waitUntil([&] { return node.getStatus().m_output_failures == 1; }));
  const auto sent{output->applicationMessages()};
  ASSERT_EQ(sent.size(), 1U);
  EXPECT_EQ(sent.front().topic(), "first");

  auto replacement{std::make_shared<TestIo>()};
  ASSERT_TRUE(node.setOutputHandler(replacement));
  auto barrier{node.addExecTaskWithWait([] {})};
  barrier->wait();
  const auto retried{replacement->applicationMessages()};
  ASSERT_EQ(retried.size(), 1U);
  EXPECT_EQ(retried.front().topic(), "second");
}

TEST(DmnDmesgNetPhase2Test, OutputReplacementDuringFailedFlushKeepsOrder) {
  auto input{std::make_shared<TestIo>()};
  auto output{std::make_shared<TestIo>()};
  auto replacement{std::make_shared<TestIo>()};
  output->failApplicationWrites();
  auto options{manualOptions()};
  options.m_max_pending_messages = 1;
  dmn::Dmn_DMesgNet node{"replace-in-flight", input, output, options};
  output->onApplicationFailure(
      [&] { EXPECT_TRUE(node.setOutputHandler(replacement)); });
  auto writer{node.openHandler("writer")};
  writer->write(applicationMessage("first"));
  writer->write(applicationMessage("overflow"));
  ASSERT_TRUE(
      waitUntil([&] { return node.getStatus().m_pending_overflow == 1; }));
  input->deliver(serialize(readyMessage()));
  ASSERT_EQ(replacement->waitForApplicationMessages(1).size(), 1U);
  writer->write(applicationMessage("second"));
  const auto messages{replacement->waitForApplicationMessages(2)};
  ASSERT_EQ(messages.size(), 2U);
  EXPECT_EQ(messages[0].topic(), "first");
  EXPECT_EQ(messages[1].topic(), "second");
}

TEST(DmnDmesgNetPhase2Test, ShutdownReportsDiscardedPendingMessages) {
  auto input{std::make_shared<TestIo>()};
  auto output{std::make_shared<TestIo>()};
  auto options{manualOptions()};
  options.m_max_pending_messages = 1;
  dmn::Dmn_DMesgNet node{"shutdown", input, output, options};
  auto writer{node.openHandler("writer")};
  writer->write(applicationMessage("pending"));
  writer->write(applicationMessage("overflow"));
  ASSERT_TRUE(
      waitUntil([&] { return node.getStatus().m_pending_overflow == 1; }));

  node.shutdown();
  const auto status{node.getStatus()};
  EXPECT_TRUE(status.m_shutdown_started);
  EXPECT_TRUE(status.m_shutdown_complete);
  EXPECT_FALSE(status.m_ready);
  EXPECT_EQ(status.m_shutdown_failures, 0U);
  EXPECT_EQ(status.m_pending_discarded, 1U);
  EXPECT_FALSE(node.setOutputHandler(output));
  node.shutdown();
  EXPECT_EQ(node.getStatus().m_pending_discarded, 1U);
}

TEST(DmnDmesgNetPhase2Test, ShutdownReportsFinalHeartbeatFailure) {
  auto output{std::make_shared<TestIo>()};
  output->failDestroyedWrite();
  dmn::Dmn_DMesgNet node{"shutdown-failure", nullptr, output};
  node.shutdown();
  const auto status{node.getStatus()};
  EXPECT_TRUE(status.m_shutdown_complete);
  EXPECT_EQ(status.m_shutdown_failures, 1U);
  EXPECT_EQ(status.m_output_failures, 1U);
}

TEST(DmnDmesgNetPhase2Test, ConcurrentShutdownIsIdempotent) {
  auto input{std::make_shared<TestIo>()};
  auto output{std::make_shared<TestIo>()};
  dmn::Dmn_DMesgNet node{"concurrent-shutdown", input, output, manualOptions()};
  std::thread first{[&node] { node.shutdown(); }};
  std::thread second{[&node] { node.shutdown(); }};
  first.join();
  second.join();
  EXPECT_TRUE(node.getStatus().m_shutdown_complete);
  EXPECT_EQ(node.getStatus().m_shutdown_failures, 0U);
}

} // namespace
