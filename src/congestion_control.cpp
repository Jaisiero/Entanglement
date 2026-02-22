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

        // Multiplicative decrease
        m_ssthresh = std::max(m_cwnd / 2, MIN_CWND);
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
