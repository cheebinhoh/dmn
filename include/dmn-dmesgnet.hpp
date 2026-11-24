/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dmesgnet.hpp
 * @brief Dmn_DMesgNet — network-aware DMesg extension.
 *
 * This header defines Dmn_DMesgNet, an extension of Dmn_DMesg that serializes
 * DMesgPb messages for network I/O and deserializes inbound messages into the
 * local DMesg publish/subscribe flow.
 *
 * Responsibilities:
 * - Serialize and send DMesgPb objects to an output Dmn_Io<std::string>.
 * - Read serialized DMesgPb strings from an input Dmn_Io<std::string> and
 *   publish them locally to Dmn_DMesg subscribers.
 * - Maintain light-weight cluster membership and a cooperative master election
 *   protocol using periodic heartbeats.
 *
 * Design summary (master election and heartbeat):
 * - Each node periodically broadcasts a heartbeat that includes:
 *   - the configured node identifier and initialization/update timestamps,
 *   - the node's current master identifier (if any),
 *   - its known remote-node list (the local node is stored separately).
 *
 * - Node states (high-level):
 *   1. MasterPending: node expects a master's heartbeat; receiving one moves
 *      the node to Ready; timeout may cause self-proclamation as master.
 *   2. Ready: normal operation where a node follows the elected master. If it
 *      receives a relinquish message from the master it may participate in a
 *      re-election.
 *   3. Destroyed: final state announced on shutdown when output is available.
 *
 * - Election/reconciliation rules (summary):
 *   - A node in MasterPending self-elects after the configured number of
 *     heartbeat timer ticks.
 *   - A Ready node may follow a remote master claim from a node with an
 *     earlier initialization timestamp.
 *   - A node returns to MasterPending after enough heartbeat ticks without
 *     synchronization from its remote master.
 *
 * Examples (brief):
 * - Nodes A and B exchange system messages. A MasterPending node can
 *   self-elect after its configured timer ticks; a Ready node can later follow
 *   a remote Ready node's master claim if that node has an earlier
 *   initialization timestamp. This is cooperative election, not quorum-based
 *   agreement.
 * - When a new node C joins later, it learns the cluster through received
 *   heartbeats and follows the elected master.
 * - When the master shuts down, it sends a final Destroyed heartbeat; nodes
 *   receiving it remove that node from their known remote-node lists.
 *
 * Implementation notes:
 * - Heartbeats are periodic (see DMN_DMESGNET_HEARTBEAT_IN_NS).
 * - Remote nodes are removed from the known-node list on a Destroyed message;
 *   absent heartbeats alone do not currently remove them.
 * - Reconciliation maintains a local view of master and membership; peers
 *   can have different views.
 */

#ifndef DMN_DMESGNET_HPP_
#define DMN_DMESGNET_HPP_

#include "dmn-debug.hpp"
#include "dmn-dmesg-pb-util.hpp"
#include "dmn-dmesg.hpp"
#include "dmn-io.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/time.h>
#include <unordered_map>

namespace dmn {

/**
 * @brief Heartbeat period in nanoseconds (1 second).
 */
#define DMN_DMESGNET_HEARTBEAT_IN_NS (1000000000)

/**
 * @brief Maximum number of consecutive heartbeat cycles to wait for a master
 *         acknowledgement before declaring self as master.
 */
#define DMN_DMESGNET_MASTERPENDING_MAX_COUNTER (3)

/**
 * @brief Maximum number of consecutive heartbeat cycles to wait for a master
 *         synchronisation confirmation before triggering a re-election.
 */
#define DMN_DMESGNET_MASTERSYNC_MAX_COUNTER (5)

/**
 * @brief Best-effort bridge between local DMesg and caller-supplied I/O.
 *
 * Readiness and counters describe only local state and do not establish remote
 * delivery, consensus, or authenticated peer identity.
 */
class Dmn_DMesgNet : public Dmn_DMesg {
public:
  /**
   * @brief Owned heartbeat source used for election ticks.
   *
   * Destruction must stop and join callbacks before returning. The factory
   * must not invoke the callback synchronously and must not retain it outside
   * the returned source, including when the factory fails.
   */
  class Heartbeat {
  public:
    virtual ~Heartbeat() = default;
  };

  /**
   * @brief Resource limits and optional heartbeat source.
   *
   * Pending messages are rejected when either bound is reached; they are not
   * coalesced. A failed flush can leave messages pending after readiness.
   * Byte limits count serialized sizes, not heap usage; they do not
   * bound transport allocations, other async queues, or the topic cache.
   * The inbound limit is checked after the transport returns a frame.
   * All limits must be nonzero.
   */
  struct Options {
    std::size_t m_max_frame_bytes{1024 * 1024}; ///< Maximum inbound frame.
    std::size_t m_max_pending_messages{1024}; ///< Maximum queued message count.
    std::size_t m_max_pending_bytes{4 * 1024 * 1024}; ///< Queued wire bytes.
    /// Optional heartbeat source; empty selects the one-second real timer.
    /// The factory must return a non-null source and honor Heartbeat's
    /// contract.
    std::function<std::unique_ptr<Heartbeat>(std::function<void()>)>
        m_heartbeat_factory{};
  };

  /**
   * @brief Local observations; counters do not imply remote delivery.
   *
   * Fields are sampled independently and may reflect concurrent updates.
   */
  struct Status {
    std::uint64_t m_malformed_input{}; ///< Empty or unparseable input.
    std::uint64_t m_invalid_input{};   ///< Parseable invalid envelopes.
    std::uint64_t m_oversized_input{}; ///< Oversized frames or datagrams.
    std::uint64_t m_input_failures{}; ///< Input errors that stopped the reader.
    std::uint64_t m_pending_overflow{}; ///< Messages rejected by queue limits.
    std::uint64_t m_repair_unavailable{}; ///< Repairs that could not proceed.
    std::uint64_t m_output_failures{}; ///< Exceptions from local output writes.
    std::uint64_t m_output_dropped{};  ///< Application messages dropped after
                                       ///< an output became unavailable.
    std::uint64_t m_pending_discarded{}; ///< Pending messages lost on shutdown.
    std::uint64_t m_shutdown_failures{}; ///< Errors during shutdown.
    bool m_ready{}; ///< Local readiness, not a remote acknowledgement.
    bool m_shutdown_started{};  ///< Shutdown has been requested.
    bool m_shutdown_complete{}; ///< Network workers and handlers have stopped.
  };

  /**
   * @brief Dmn_DMesgNet constructor.
   *
   * Create a network-aware DMesg instance that can read/write serialized
   * DMesgPb messages through Dmn_Io handlers.
   *
   * @param name           Identification name for this DMesgNet instance.
   * @param input_handler  Optional input handler to receive inbound serialized
   * DMesgPb messages (std::string). If nullptr, no
   * input processing is started.
   * @param output_handler Optional output handler to send outbound serialized
   * DMesgPb messages (std::string). If nullptr, no
   * network sends are performed.
   */
  explicit Dmn_DMesgNet(
      std::string_view name,
      std::shared_ptr<Dmn_Io<std::string>> input_handler = nullptr,
      std::shared_ptr<Dmn_Io<std::string>> output_handler = nullptr);

  /**
   * @brief Construct a node with explicit options.
   *
   * @param name Node identifier.
   * @param input_handler Optional inbound endpoint.
   * @param output_handler Optional outbound endpoint.
   * @param options Resource limits and optional heartbeat factory.
   * @throws std::invalid_argument if a limit is zero or the heartbeat factory
   * returns null.
   */
  Dmn_DMesgNet(std::string_view name,
               std::shared_ptr<Dmn_Io<std::string>> input_handler,
               std::shared_ptr<Dmn_Io<std::string>> output_handler,
               Options options);

  /**
   * @brief Shut down the network bridge and release its resources.
   *
   * Callers must ensure that no other thread is using the object during
   * destruction.
   */
  virtual ~Dmn_DMesgNet() noexcept;

  Dmn_DMesgNet(const Dmn_DMesgNet &obj) = delete;
  Dmn_DMesgNet &operator=(const Dmn_DMesgNet &obj) = delete;
  Dmn_DMesgNet(Dmn_DMesgNet &&obj) = delete;
  Dmn_DMesgNet &operator=(Dmn_DMesgNet &&obj) = delete;

  /**
   * @brief Replace the outbound transport and re-enable network writes.
   *
   * A failed output is not retried until this method installs a handler.
   * Passing nullptr disables outbound writes. Any write already in progress
   * may finish on the previous handler.
   *
   * @param outputHandler New outbound transport, or nullptr to disable output.
   * @return false if shutdown has begun; otherwise true.
   */
  auto
  setOutputHandler(std::shared_ptr<Dmn_Io<std::string>> outputHandler) -> bool;

  /**
   * @brief Sample local error counters and readiness.
   *
   * A successful local write is not evidence of remote receipt or application.
   *
   * @return Thread-safe, independently sampled status fields.
   */
  [[nodiscard]] auto getStatus() const noexcept -> Status;

  /**
   * @brief Stop network workers and handlers and discard unsent pending data.
   *
   * This operation is idempotent and waits for in-flight work. Do not call it
   * from a DMesgNet handler, transport callback, or its own async context.
   * It may block if a transport callback does not return. Input shutdown and
   * final heartbeat errors are recorded in Status, even if cleanup completes.
   * Local DMesg handlers remain owned by the object until they are closed or
   * the object is destroyed.
   *
   * @throws std::exception if cleanup cannot complete; a later call may retry.
   */
  void shutdown();

protected:
  /**
   * @brief Reconcile system-level information from a remote DMesgPb message.
   *
   * This method updates local view of the cluster (master id, neighbor list,
   * timestamps, etc.) based on information contained in dmesgpb_other. It is
   * responsible for applying election/co-election logic (for example, when a
   * remote node declares itself master or relinquishes mastership) and for
   * ensuring local state stays consistent with observed remote state.
   *
   * @param dmesgpb_other DMesgPb message received from a remote node.
   */
  void reconciliateDMesgPbSys(const dmn::DMesgPb &dmesgpb_other);

  /**
   * @brief Return the internal per-topic last-published message cache.
   *
   * Overrides @c Dmn_DMesg::getLastTopicCacheInternal() to return the
   * network layer's own cache (@c m_topic_last_dmesgpb) rather than the
   * base-class cache, so that heartbeat and reconciliation logic use the
   * correct view.
   *
   * @return Reference to the network layer's topic-to-last-message map.
   */
  auto getLastTopicCacheInternal()
      -> std::unordered_map<std::string, dmn::DMesgPb> & override;

private:
  /**
   * @brief Create the input worker and handlers; the constructor starts the
   *        worker after heartbeat initialization.
   */
  void createInputHandlerProc();

  /**
   * @brief Register the internal publish-subscriber handler that forwards
   *         outbound local messages to @c m_output_handler.
   */
  void createSubscriptHandler();

  /**
   * @brief Create and start the periodic heartbeat timer thread.
   */
  void createTimerProc();

  /**
   * @brief Drain queued outbound messages in publication order.
   */
  void sendPendingOutboundQueueMessage();

  /**
   * @brief Retain an outbound message if both pending limits permit it.
   *
   * Called only in the DMesgNet async context.
   */
  void enqueuePendingOutboundMessage(const dmn::DMesgPb &message);

  /**
   * @brief Send one payload unless shutdown or an output failure has disabled
   *        outbound writes.
   */
  auto writeOutput(const std::string &payload,
                   bool duringShutdown = false) noexcept -> bool;

  [[nodiscard]] auto hasOutputHandler() const -> bool;

  /**
   * Constructor-initialized members.
   */
  std::string m_name{}; ///< Instance name.
  Options m_options{};  ///< Resource limits.
  std::shared_ptr<Dmn_Io<std::string>>
      m_input_handler{}; ///< Inbound I/O handler.
  std::shared_ptr<Dmn_Io<std::string>>
      m_output_handler{};          ///< Outbound I/O handler.
  const bool m_election_enabled{}; ///< Both endpoints existed at construction.
  mutable std::mutex m_output_mutex{};
  std::condition_variable m_output_setters_idle{};
  std::size_t m_output_setters{};
  std::uint64_t m_output_generation{};
  bool m_output_failed{};

  /**
   * Internal runtime state and helper objects.
   */
  std::unique_ptr<dmn::Dmn_Proc>
      m_input_proc{}; ///< Thread reading from @c m_input_handler.
  Dmn_DMesg::HandlerType
      m_subscript_handler{}; ///< Subscriber handler for local messages.
  Dmn_DMesg::HandlerType m_write_handler{}; ///< Handler for write operations.
  Dmn_DMesg::HandlerType
      m_sys_handler{}; ///< Handler for system (heartbeat) messages.
  std::unique_ptr<Heartbeat> m_timer_proc{}; ///< Heartbeat timer.

  dmn::DMesgPb
      m_sys{}; ///< Local system DMesgPb (node identity, neighbor list).
  long long m_master_pending_counter{}; ///< Cycles spent waiting for a master
                                        ///< acknowledgement.
  long long m_master_sync_pending_counter{}; ///< Cycles spent waiting for
                                             ///< master synchronisation.
  struct timeval m_last_remote_master_timestamp {
  }; ///< Timestamp of the last received
     ///< master heartbeat.
  std::unordered_map<std::string, dmn::DMesgPb>
      m_topic_last_dmesgpb{}; ///< Last published message per topic.
  std::deque<dmn::DMesgPb>
      m_outbound_pending_dmesgpb{}; ///< Pending messages in publication order.
  std::size_t m_pending_bytes{};    ///< Serialized size of pending messages.

  std::atomic<std::uint64_t> m_malformed_input{};
  std::atomic<std::uint64_t> m_invalid_input{};
  std::atomic<std::uint64_t> m_oversized_input{};
  std::atomic<std::uint64_t> m_input_failures{};
  std::atomic<std::uint64_t> m_pending_overflow{};
  std::atomic<std::uint64_t> m_repair_unavailable{};
  std::atomic<std::uint64_t> m_output_failures{};
  std::atomic<std::uint64_t> m_output_dropped{};
  std::atomic<std::uint64_t> m_pending_discarded{};
  std::atomic<std::uint64_t> m_shutdown_failures{};

  std::atomic_flag m_ready{}; ///< Set once master election has completed and
                              ///< this node is ready.

  bool m_is_master{}; ///< True when this node is the elected master.
  long long m_number_of_neighbor{}; ///< Number of known neighbor nodes
                                    ///< (excluding self).

  std::atomic<bool> m_shutdown{}; ///< True once shutdown has been requested.
  std::atomic<bool> m_shutdown_complete{};
  std::mutex m_shutdown_mutex{};
}; // class Dmn_DMesgNet

} // namespace dmn

#endif // DMN_DMESGNET_HPP_
