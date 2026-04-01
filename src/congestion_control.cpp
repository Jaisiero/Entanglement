#include "congestion_control.h"
#include <algorithm>
#include <cmath>

namespace entanglement
{

    void congestion_control::reset()
    {
        m_cwnd = INITIAL_CWND;
        m_ssthresh = INITIAL_SSTHRESH;
        m_in_flight = 0;
        m_pacing_interval_us = 0;
        m_cwnd_accumulator = 0.0;
        m_srtt_us = 0.0;
        m_last_cwnd_reduction = {};
        m_loss_rate = 0.0; // Optimistic start — loss tolerance active immediately
    }

    // --- Events ---

    void congestion_control::on_packet_sent()
    {
        ++m_in_flight;
    }

    void congestion_control::on_packet_acked()
    {
        if (m_in_flight > 0)
        {
            --m_in_flight;
        }

        // Update EWMA loss rate: ACK = no loss event (0)
        m_loss_rate *= (1.0 - CC_LOSS_ALPHA);

        if (in_slow_start())
        {
            // Slow start: increase cwnd by 1 per ACK (doubles each RTT)
            if (m_cwnd < MAX_CWND)
            {
                ++m_cwnd;
            }
        }
        else
        {
            // Congestion avoidance: increase cwnd by 1/cwnd per ACK (≈ +1 per RTT)
            m_cwnd_accumulator += 1.0 / static_cast<double>(m_cwnd);
            if (m_cwnd_accumulator >= 1.0)
            {
                if (m_cwnd < MAX_CWND)
                {
                    ++m_cwnd;
                }
                m_cwnd_accumulator -= 1.0;
            }
        }
    }

    void congestion_control::on_packet_lost()
    {
        if (m_in_flight > 0)
        {
            --m_in_flight;
        }

        // Update EWMA loss rate: loss event (1)
        m_loss_rate = m_loss_rate * (1.0 - CC_LOSS_ALPHA) + CC_LOSS_ALPHA;

        // Loss tolerance: if the smoothed loss rate is below the threshold,
        // this is likely random/wireless loss — don't apply multiplicative decrease.
        // The connection keeps its throughput and relies on retransmissions instead.
        if (m_loss_rate < CC_LOSS_TOLERANCE)
            return;

        // Loss-event coalescing: only reduce cwnd once per RTT.
        // Multiple losses within the same congestion window are a single event.
        auto now = std::chrono::steady_clock::now();
        auto since_last = std::chrono::duration_cast<std::chrono::microseconds>(now - m_last_cwnd_reduction).count();

        int64_t guard = (m_srtt_us > 0.0) ? static_cast<int64_t>(m_srtt_us) : MIN_RTO_US;
        if (since_last < guard)
            return; // Already reduced cwnd for this loss event

        // Multiplicative decrease (beta = 0.7, CUBIC-style — gentler than classic 0.5)
        m_last_cwnd_reduction = now;
        m_ssthresh = std::max(static_cast<uint32_t>(m_cwnd * CC_BETA), MIN_CWND);
        m_cwnd = m_ssthresh;
        m_cwnd_accumulator = 0.0;
    }

    void congestion_control::on_packet_expired()
    {
        // Reclaim in_flight for timed-out unreliable packets.
        if (m_in_flight > 0)
        {
            --m_in_flight;
        }

        // Update EWMA loss rate: expired = lost for congestion purposes
        m_loss_rate = m_loss_rate * (1.0 - CC_LOSS_ALPHA) + CC_LOSS_ALPHA;

        // Apply congestion response when loss rate exceeds tolerance.
        // Without this, unreliable channels never reduce cwnd and can
        // saturate the link because only ACKs grow the window.
        if (m_loss_rate < CC_LOSS_TOLERANCE)
            return;

        auto now = std::chrono::steady_clock::now();
        auto since_last = std::chrono::duration_cast<std::chrono::microseconds>(now - m_last_cwnd_reduction).count();
        int64_t guard = (m_srtt_us > 0.0) ? static_cast<int64_t>(m_srtt_us) : MIN_RTO_US;
        if (since_last < guard)
            return;

        m_last_cwnd_reduction = now;
        m_ssthresh = std::max(static_cast<uint32_t>(m_cwnd * CC_BETA), MIN_CWND);
        m_cwnd = m_ssthresh;
        m_cwnd_accumulator = 0.0;
    }

    // --- Queries ---

    congestion_info congestion_control::info() const
    {
        congestion_info ci;
        ci.cwnd = m_cwnd;
        ci.in_flight = m_in_flight;
        ci.ssthresh = m_ssthresh;
        ci.pacing_interval_us = m_pacing_interval_us;
        ci.in_slow_start = in_slow_start();
        return ci;
    }

    void congestion_control::update_pacing(double srtt_us)
    {
        m_srtt_us = srtt_us;
        if (m_cwnd > 0 && srtt_us > 0.0)
        {
            // Distribute sends evenly across one RTT window
            m_pacing_interval_us = static_cast<int64_t>(srtt_us / static_cast<double>(m_cwnd));
        }
        else
        {
            m_pacing_interval_us = 0;
        }
    }

} // namespace entanglement
