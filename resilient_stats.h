/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _MORSE_RESILIENT_STATS_H_
#define _MORSE_RESILIENT_STATS_H_

#include <linux/atomic.h>
#include <linux/types.h>

struct morse;

#define MORSE_RES_MCS_COUNT 11
#define MORSE_RES_BW_COUNT 4
#define MORSE_RES_CHANNEL_COUNT 4
#define MORSE_RES_QUEUE_HIST_COUNT 5
#define MORSE_RES_SPI_HIST_COUNT 6
#define MORSE_RES_AMPDU_HIST_COUNT 6

enum morse_resilient_spi_direction {
	MORSE_RES_SPI_CONTROL = 0,
	MORSE_RES_SPI_READ,
	MORSE_RES_SPI_WRITE,
};

enum morse_resilient_drop_reason {
	MORSE_RES_DROP_NO_PAGE = 0,
	MORSE_RES_DROP_OVERSIZE,
	MORSE_RES_DROP_TAILROOM,
	MORSE_RES_DROP_PAGE_WRITE,
	MORSE_RES_DROP_AGED,
	MORSE_RES_DROP_REASON_COUNT,
};

struct morse_resilient_rate_stat {
	atomic64_t attempts;
	atomic64_t successes;
	atomic64_t failures;
	atomic64_t aggregated;
	atomic64_t rssi_sum;
	atomic64_t rssi_samples;
};

struct morse_resilient_stats {
	u64 started_ns;
	atomic64_t spi_clock_hz;
	atomic64_t spi_xfers;
	atomic64_t spi_errors;
	atomic64_t spi_wire_bytes;
	atomic64_t spi_control_wire_bytes;
	atomic64_t spi_read_wire_bytes;
	atomic64_t spi_write_wire_bytes;
	atomic64_t spi_payload_read_bytes;
	atomic64_t spi_payload_write_bytes;
	atomic64_t spi_duration_ns;
	atomic64_t spi_duration_max_ns;
	atomic64_t spi_dma_eligible_xfers;
	atomic64_t spi_pio_only_xfers;
	atomic64_t spi_size_hist[MORSE_RES_SPI_HIST_COUNT];
	atomic64_t spi_fallbacks;
	atomic64_t spi_fallback_from_hz;
	atomic64_t spi_fallback_to_hz;

	atomic64_t irq_wakeups;
	atomic64_t irq_latency_ns;
	atomic64_t irq_latency_max_ns;
	atomic64_t irq_service_ns;
	atomic64_t irq_service_max_ns;

	atomic64_t queue_enqueues;
	atomic64_t queue_depth_max;
	atomic64_t queue_bytes_max;
	atomic64_t queue_residence_ns;
	atomic64_t queue_residence_samples;
	atomic64_t queue_residence_max_ns;
	atomic64_t queue_residence_hist[MORSE_RES_QUEUE_HIST_COUNT];

	atomic64_t page_starvation_events[MORSE_RES_CHANNEL_COUNT];
	atomic64_t page_starvation_start_ns[MORSE_RES_CHANNEL_COUNT];
	atomic64_t page_starvation_ns[MORSE_RES_CHANNEL_COUNT];
	atomic64_t page_starvation_max_ns[MORSE_RES_CHANNEL_COUNT];
	atomic64_t page_restore_events;
	atomic64_t page_restore_failures;
	atomic64_t page_restore_ns;
	atomic64_t page_restore_max_ns;
	atomic64_t drops[MORSE_RES_DROP_REASON_COUNT][MORSE_RES_CHANNEL_COUNT];

	atomic64_t driver_tx_payload_bytes;
	atomic64_t driver_rx_payload_bytes;
	atomic64_t ampdu_len_hist[MORSE_RES_AMPDU_HIST_COUNT];
	struct morse_resilient_rate_stat tx_rate[MORSE_RES_MCS_COUNT][MORSE_RES_BW_COUNT];
	struct morse_resilient_rate_stat rx_rate[MORSE_RES_MCS_COUNT][MORSE_RES_BW_COUNT];
};

int morse_resilient_stats_init(struct morse *mors);
void morse_resilient_spi_xfer(struct morse *mors,
			      enum morse_resilient_spi_direction direction,
			      u32 clock_hz, u32 wire_bytes, u32 payload_bytes,
			      u64 duration_ns, bool dma_eligible, int ret);
void morse_resilient_spi_fallback(struct morse *mors, u32 from_hz, u32 to_hz);
void morse_resilient_irq(struct morse *mors, u64 latency_ns, u64 service_ns);
void morse_resilient_queue_enqueue(struct morse *mors, u32 depth, u32 bytes);
void morse_resilient_queue_residence(struct morse *mors, u8 channel, u64 duration_ns);
void morse_resilient_page_starvation_begin(struct morse *mors, u8 channel);
void morse_resilient_page_starvation_end(struct morse *mors, u8 channel);
void morse_resilient_page_restore(struct morse *mors, u64 duration_ns, bool success);
void morse_resilient_drop(struct morse *mors, u8 channel,
			  enum morse_resilient_drop_reason reason);
void morse_resilient_driver_tx_payload(struct morse *mors, u32 bytes);
void morse_resilient_driver_rx_payload(struct morse *mors, u32 bytes);
void morse_resilient_tx_rate(struct morse *mors, u8 mcs, u8 bw_index,
			     u8 attempts, bool success, bool aggregated,
			     u8 ampdu_len, s16 rssi);
void morse_resilient_rx_rate(struct morse *mors, u8 mcs, u8 bw_index, s16 rssi);

#endif /* _MORSE_RESILIENT_STATS_H_ */
