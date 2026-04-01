#pragma once

#include "constants.h"
#include <algorithm>
#include <chrono>
#include <cstdint>

namespace entanglement
{

    // --- Snapshot exposed to the application layer for pacing decisions ---

    struct congestion_info
    {
        uint32_t cwnd = INITIAL_CWND;         // current window size (packets)
        uint32_t in_flight = 0;               // unacked packets currently in transit
        uint32_t ssthresh = INITIAL_SSTHRESH; // slow-start threshold
        int64_t pacing_interval_us = 0;       // recommended microseconds between sends
        bool in_slow_start = true;            // still doubling cwnd each RTT?
    };

    // --- Pure-algorithmic congestion controller ---
    //
    // Owns no I/O or connection state.  The owning udp_connection calls the
    // event methods; the application reads can_send() / info() to pace its sends.
    //
    // Algorithm: conservative CWND (slow-start → AIMD congestion avoidance)
    //   + send pacing (interval = srtt / cwnd).
    //
    // Slow start:  cwnd doubles each RTT  (capped by ssthresh).
    // Congestion avoidance:  cwnd += 1 / cwnd  per ACK  (≈ +1 / RTT).
    // On loss:  ssthresh = cwnd / 2,  cwnd = max(ssthresh, MIN_CWND).

    class congestion_control
    {
    public:
        congestion_control() = default;

        // Reset to initial state
        void reset();

        // --- Events (called by udp_connection) ---

        // A new packet was sent (reliable or not — counted for window)
        void on_packet_sent();

        // A previously in-flight packet was acknowledged
        void on_packet_acked();

        // A reliable packet was detected as lost (RTO expired)
        void on_packet_lost();

        // An unreliable packet expired without ACK — reclaim in_flight and
        // apply congestion response when loss rate exceeds tolerance.
        // Prevents unreliable channels from saturating the link.
        void on_packet_expired();

        // --- Queries (read by application via client/connection) ---

        // True when the window still has room for more packets.
        // Also guard against overflowing the fixed-size send buffer:
        // even if cwnd allows more, we cannot exceed SEQUENCE_BUFFER_SIZE
        // in flight or the circular buffer will silently overwrite entries.
        bool can_send() const { return m_in_flight < m_cwnd && m_in_flight < SEQUENCE_BUFFER_SIZE - 1; }

        // Full snapshot for the application
        congestion_info info() const;

        // Recalculate pacing interval from current SRTT (in microseconds).
        // Should be called whenever RTT estimates are updated.
        void update_pacing(double srtt_us);

        // Individual accessors
        uint32_t cwnd() const { return m_cwnd; }
        uint32_t in_flight() const { return m_in_flight; }
        uint32_t ssthresh() const { return m_ssthresh; }
        int64_t pacing_interval_us() const { return m_pacing_interval_us; }
        bool in_slow_start() const { return m_cwnd < m_ssthresh; }
        double loss_rate() const { return m_loss_rate; }

    private:
        uint32_t m_cwnd = INITIAL_CWND;
        uint32_t m_ssthresh = INITIAL_SSTHRESH;
        uint32_t m_in_flight = 0;
        int64_t m_pacing_interval_us = 0;

        // Fractional cwnd accumulator for congestion avoidance
        // (we add 1/cwnd per ACK, this stores the fraction)
        double m_cwnd_accumulator = 0.0;

        // Loss event coalescing: only apply multiplicative decrease once per RTT.
        // Multiple losses within the same RTT window are a single congestion event.
        double m_srtt_us = 0.0;
        std::chrono::steady_clock::time_point m_last_cwnd_reduction{};

        // EWMA loss rate estimator: separate random/wireless loss from congestion.
        // Starts at 0.0 (optimistic — random loss tolerated until rate exceeds threshold).
        double m_loss_rate = 0.0;
    };

} // namespace entanglement
