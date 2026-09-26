// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/debugfs.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/seq_file.h>

#include "morse.h"
#include "pageset.h"
#include "resilient_stats.h"
#include "skb_header.h"

static void atomic64_update_max(atomic64_t *value, u64 candidate)
{
	u64 observed = atomic64_read(value);

	while (candidate > observed) {
		u64 previous = atomic64_cmpxchg(value, observed, candidate);

		if (previous == observed)
			break;
		observed = previous;
	}
}

static unsigned int channel_index(u8 channel)
{
	switch (channel) {
	case MORSE_SKB_CHAN_COMMAND:
		return 0;
	case MORSE_SKB_CHAN_BEACON:
		return 1;
	case MORSE_SKB_CHAN_MGMT:
		return 2;
	default:
		return 3;
	}
}

static unsigned int spi_size_bucket(u32 bytes)
{
	if (bytes <= 64)
		return 0;
	if (bytes <= 256)
		return 1;
	if (bytes <= 512)
		return 2;
	if (bytes <= 2048)
		return 3;
	if (bytes <= 8192)
		return 4;
	return 5;
}

static unsigned int queue_time_bucket(u64 duration_ns)
{
	if (duration_ns <= NSEC_PER_MSEC)
		return 0;
	if (duration_ns <= 5 * NSEC_PER_MSEC)
		return 1;
	if (duration_ns <= 20 * NSEC_PER_MSEC)
		return 2;
	if (duration_ns <= 100 * NSEC_PER_MSEC)
		return 3;
	return 4;
}

static unsigned int ampdu_len_bucket(u8 len)
{
	if (len <= 1)
		return 0;
	if (len <= 2)
		return 1;
	if (len <= 4)
		return 2;
	if (len <= 8)
		return 3;
	if (len <= 16)
		return 4;
	return 5;
}

static u64 bytes_per_second(u64 bytes, u64 elapsed_ns)
{
	u64 elapsed_ms = div_u64(elapsed_ns, NSEC_PER_MSEC);

	if (!elapsed_ms)
		return 0;
	return div64_u64(bytes * MSEC_PER_SEC, elapsed_ms);
}

static int read_resilient_performance(struct seq_file *file, void *data)
{
	static const char * const channel_names[] = { "command", "beacon", "management", "data" };
	static const char * const drop_names[] = {
		"no_page", "oversize", "tailroom", "page_write", "aged"
	};
	struct morse *mors = dev_get_drvdata(file->private);
	struct morse_resilient_stats *stats = &mors->resilient_stats;
	u64 now_ns = ktime_get_mono_fast_ns();
	u64 elapsed_ns = now_ns > stats->started_ns ? now_ns - stats->started_ns : 0;
	u64 spi_wire_bytes = atomic64_read(&stats->spi_wire_bytes);
	u64 spi_payload_bytes = atomic64_read(&stats->spi_payload_read_bytes) +
		atomic64_read(&stats->spi_payload_write_bytes);
	unsigned int mcs;
	unsigned int bw;
	unsigned int index;

	seq_puts(file, "schema=resilient-morse-performance-v1\n");
	seq_printf(file, "uptime_ms=%llu\n", div_u64(elapsed_ns, NSEC_PER_MSEC));
	seq_printf(file, "channel_frequency_hz=%u\n",
		   mors->custom_configs.channel_info.op_chan_freq_hz);
	seq_printf(file, "channel_operating_bw_mhz=%u\n",
		   mors->custom_configs.channel_info.op_bw_mhz);
	seq_printf(file, "channel_primary_bw_mhz=%u\n",
		   mors->custom_configs.channel_info.pri_bw_mhz);
	seq_printf(file, "channel_primary_1mhz_index=%u\n",
		   mors->custom_configs.channel_info.pri_1mhz_chan_idx);
	seq_printf(file, "tx_page_size_bytes=%u\n", morse_pageset_tx_page_size(mors));
	seq_printf(file, "tx_cached_pages=%u\n", morse_pageset_tx_cached_pages(mors));
	seq_printf(file, "tx_reserved_pages=%u\n", morse_pageset_tx_reserved_pages(mors));
	seq_printf(file, "tx_stop_pages=%u\n", PAGESET_TX_STOP_PAGES);
	seq_printf(file, "tx_wake_pages=%u\n", PAGESET_TX_WAKE_PAGES);

	seq_printf(file, "spi_clock_hz=%llu\n", atomic64_read(&stats->spi_clock_hz));
	seq_printf(file, "spi_xfers=%llu\n", atomic64_read(&stats->spi_xfers));
	seq_printf(file, "spi_errors=%llu\n", atomic64_read(&stats->spi_errors));
	seq_printf(file, "spi_wire_bytes=%llu\n", spi_wire_bytes);
	seq_printf(file, "spi_control_wire_bytes=%llu\n",
		   atomic64_read(&stats->spi_control_wire_bytes));
	seq_printf(file, "spi_read_wire_bytes=%llu\n",
		   atomic64_read(&stats->spi_read_wire_bytes));
	seq_printf(file, "spi_write_wire_bytes=%llu\n",
		   atomic64_read(&stats->spi_write_wire_bytes));
	seq_printf(file, "spi_payload_read_bytes=%llu\n",
		   atomic64_read(&stats->spi_payload_read_bytes));
	seq_printf(file, "spi_payload_write_bytes=%llu\n",
		   atomic64_read(&stats->spi_payload_write_bytes));
	seq_printf(file, "spi_wire_bytes_per_second=%llu\n",
		   bytes_per_second(spi_wire_bytes, elapsed_ns));
	seq_printf(file, "spi_payload_bytes_per_second=%llu\n",
		   bytes_per_second(spi_payload_bytes, elapsed_ns));
	seq_printf(file, "spi_duration_ns=%llu\n", atomic64_read(&stats->spi_duration_ns));
	seq_printf(file, "spi_duration_max_ns=%llu\n",
		   atomic64_read(&stats->spi_duration_max_ns));
	seq_printf(file, "spi_dma_eligible_xfers=%llu\n",
		   atomic64_read(&stats->spi_dma_eligible_xfers));
	seq_printf(file, "spi_pio_only_xfers=%llu\n",
		   atomic64_read(&stats->spi_pio_only_xfers));
	seq_printf(file, "spi_fallbacks=%llu\n", atomic64_read(&stats->spi_fallbacks));
	seq_printf(file, "spi_fallback_from_hz=%llu\n",
		   atomic64_read(&stats->spi_fallback_from_hz));
	seq_printf(file, "spi_fallback_to_hz=%llu\n",
		   atomic64_read(&stats->spi_fallback_to_hz));
	for (index = 0; index < MORSE_RES_SPI_HIST_COUNT; index++)
		seq_printf(file, "spi_size_hist_%u=%llu\n", index,
			   atomic64_read(&stats->spi_size_hist[index]));

	seq_printf(file, "irq_wakeups=%llu\n", atomic64_read(&stats->irq_wakeups));
	seq_printf(file, "irq_latency_ns=%llu\n", atomic64_read(&stats->irq_latency_ns));
	seq_printf(file, "irq_latency_max_ns=%llu\n",
		   atomic64_read(&stats->irq_latency_max_ns));
	seq_printf(file, "irq_service_ns=%llu\n", atomic64_read(&stats->irq_service_ns));
	seq_printf(file, "irq_service_max_ns=%llu\n",
		   atomic64_read(&stats->irq_service_max_ns));

	seq_printf(file, "queue_enqueues=%llu\n", atomic64_read(&stats->queue_enqueues));
	seq_printf(file, "queue_depth_max=%llu\n", atomic64_read(&stats->queue_depth_max));
	seq_printf(file, "queue_bytes_max=%llu\n", atomic64_read(&stats->queue_bytes_max));
	seq_printf(file, "queue_residence_samples=%llu\n",
		   atomic64_read(&stats->queue_residence_samples));
	seq_printf(file, "queue_residence_ns=%llu\n",
		   atomic64_read(&stats->queue_residence_ns));
	seq_printf(file, "queue_residence_max_ns=%llu\n",
		   atomic64_read(&stats->queue_residence_max_ns));
	for (index = 0; index < MORSE_RES_QUEUE_HIST_COUNT; index++)
		seq_printf(file, "queue_residence_hist_%u=%llu\n", index,
			   atomic64_read(&stats->queue_residence_hist[index]));

	for (index = 0; index < MORSE_RES_CHANNEL_COUNT; index++) {
		seq_printf(file, "page_starvation_%s_events=%llu\n", channel_names[index],
			   atomic64_read(&stats->page_starvation_events[index]));
		seq_printf(file, "page_starvation_%s_ns=%llu\n", channel_names[index],
			   atomic64_read(&stats->page_starvation_ns[index]));
		seq_printf(file, "page_starvation_%s_max_ns=%llu\n", channel_names[index],
			   atomic64_read(&stats->page_starvation_max_ns[index]));
	}
	seq_printf(file, "page_restore_events=%llu\n",
		   atomic64_read(&stats->page_restore_events));
	seq_printf(file, "page_restore_failures=%llu\n",
		   atomic64_read(&stats->page_restore_failures));
	seq_printf(file, "page_restore_ns=%llu\n", atomic64_read(&stats->page_restore_ns));
	seq_printf(file, "page_restore_max_ns=%llu\n",
		   atomic64_read(&stats->page_restore_max_ns));
	for (index = 0; index < MORSE_RES_CHANNEL_COUNT; index++) {
		unsigned int reason;

		for (reason = 0; reason < MORSE_RES_DROP_REASON_COUNT; reason++)
			seq_printf(file, "drop_%s_%s=%llu\n", drop_names[reason],
				   channel_names[index],
				   atomic64_read(&stats->drops[reason][index]));
	}

	seq_printf(file, "driver_tx_payload_bytes=%llu\n",
		   atomic64_read(&stats->driver_tx_payload_bytes));
	seq_printf(file, "driver_rx_payload_bytes=%llu\n",
		   atomic64_read(&stats->driver_rx_payload_bytes));
	seq_printf(file, "driver_tx_payload_bytes_per_second=%llu\n",
		   bytes_per_second(atomic64_read(&stats->driver_tx_payload_bytes), elapsed_ns));
	seq_printf(file, "driver_rx_payload_bytes_per_second=%llu\n",
		   bytes_per_second(atomic64_read(&stats->driver_rx_payload_bytes), elapsed_ns));
	for (index = 0; index < MORSE_RES_AMPDU_HIST_COUNT; index++)
		seq_printf(file, "ampdu_len_hist_%u=%llu\n", index,
			   atomic64_read(&stats->ampdu_len_hist[index]));

	for (mcs = 0; mcs < MORSE_RES_MCS_COUNT; mcs++) {
		for (bw = 0; bw < MORSE_RES_BW_COUNT; bw++) {
			struct morse_resilient_rate_stat *tx = &stats->tx_rate[mcs][bw];
			struct morse_resilient_rate_stat *rx = &stats->rx_rate[mcs][bw];

			if (atomic64_read(&tx->attempts) || atomic64_read(&rx->attempts)) {
				seq_printf(file,
					   "rate_mcs=%u bw_mhz=%u tx_attempts=%llu tx_successes=%llu "
					   "tx_failures=%llu tx_aggregated=%llu tx_rssi_sum=%lld "
					   "tx_rssi_samples=%llu rx_packets=%llu rx_rssi_sum=%lld "
					   "rx_rssi_samples=%llu\n",
					   mcs, 1U << bw,
					   atomic64_read(&tx->attempts),
					   atomic64_read(&tx->successes),
					   atomic64_read(&tx->failures),
					   atomic64_read(&tx->aggregated),
					   (s64)atomic64_read(&tx->rssi_sum),
					   atomic64_read(&tx->rssi_samples),
					   atomic64_read(&rx->attempts),
					   (s64)atomic64_read(&rx->rssi_sum),
					   atomic64_read(&rx->rssi_samples));
			}
		}
	}

	return 0;
}

int morse_resilient_stats_init(struct morse *mors)
{
	if (!READ_ONCE(mors->resilient_stats.started_ns))
		WRITE_ONCE(mors->resilient_stats.started_ns, ktime_get_mono_fast_ns());
	debugfs_create_devm_seqfile(mors->dev, "resilient_performance",
				    mors->debug.debugfs_phy, read_resilient_performance);
	return 0;
}

void morse_resilient_spi_xfer(struct morse *mors,
			      enum morse_resilient_spi_direction direction,
			      u32 clock_hz, u32 wire_bytes, u32 payload_bytes,
			      u64 duration_ns, bool dma_eligible, int ret)
{
	struct morse_resilient_stats *stats;

	if (!mors)
		return;
	stats = &mors->resilient_stats;
	if (!READ_ONCE(stats->started_ns))
		WRITE_ONCE(stats->started_ns, ktime_get_mono_fast_ns());
	atomic64_set(&stats->spi_clock_hz, clock_hz);
	atomic64_inc(&stats->spi_xfers);
	atomic64_add(wire_bytes, &stats->spi_wire_bytes);
	atomic64_add(duration_ns, &stats->spi_duration_ns);
	atomic64_update_max(&stats->spi_duration_max_ns, duration_ns);
	atomic64_inc(&stats->spi_size_hist[spi_size_bucket(wire_bytes)]);
	if (dma_eligible)
		atomic64_inc(&stats->spi_dma_eligible_xfers);
	else
		atomic64_inc(&stats->spi_pio_only_xfers);
	if (ret)
		atomic64_inc(&stats->spi_errors);

	switch (direction) {
	case MORSE_RES_SPI_READ:
		atomic64_add(wire_bytes, &stats->spi_read_wire_bytes);
		if (!ret)
			atomic64_add(payload_bytes, &stats->spi_payload_read_bytes);
		break;
	case MORSE_RES_SPI_WRITE:
		atomic64_add(wire_bytes, &stats->spi_write_wire_bytes);
		if (!ret)
			atomic64_add(payload_bytes, &stats->spi_payload_write_bytes);
		break;
	default:
		atomic64_add(wire_bytes, &stats->spi_control_wire_bytes);
		break;
	}
}

void morse_resilient_spi_fallback(struct morse *mors, u32 from_hz, u32 to_hz)
{
	if (!mors)
		return;
	atomic64_inc(&mors->resilient_stats.spi_fallbacks);
	atomic64_set(&mors->resilient_stats.spi_fallback_from_hz, from_hz);
	atomic64_set(&mors->resilient_stats.spi_fallback_to_hz, to_hz);
}

void morse_resilient_irq(struct morse *mors, u64 latency_ns, u64 service_ns)
{
	struct morse_resilient_stats *stats;

	if (!mors)
		return;
	stats = &mors->resilient_stats;
	atomic64_inc(&stats->irq_wakeups);
	atomic64_add(latency_ns, &stats->irq_latency_ns);
	atomic64_add(service_ns, &stats->irq_service_ns);
	atomic64_update_max(&stats->irq_latency_max_ns, latency_ns);
	atomic64_update_max(&stats->irq_service_max_ns, service_ns);
}

void morse_resilient_queue_enqueue(struct morse *mors, u32 depth, u32 bytes)
{
	if (!mors)
		return;
	atomic64_inc(&mors->resilient_stats.queue_enqueues);
	atomic64_update_max(&mors->resilient_stats.queue_depth_max, depth);
	atomic64_update_max(&mors->resilient_stats.queue_bytes_max, bytes);
}

void morse_resilient_queue_residence(struct morse *mors, u8 channel, u64 duration_ns)
{
	struct morse_resilient_stats *stats;

	if (!mors || channel_index(channel) >= MORSE_RES_CHANNEL_COUNT)
		return;
	stats = &mors->resilient_stats;
	atomic64_inc(&stats->queue_residence_samples);
	atomic64_add(duration_ns, &stats->queue_residence_ns);
	atomic64_update_max(&stats->queue_residence_max_ns, duration_ns);
	atomic64_inc(&stats->queue_residence_hist[queue_time_bucket(duration_ns)]);
}

void morse_resilient_page_starvation_begin(struct morse *mors, u8 channel)
{
	unsigned int index;
	u64 now_ns;

	if (!mors)
		return;
	index = channel_index(channel);
	now_ns = ktime_get_mono_fast_ns();
	if (atomic64_cmpxchg(&mors->resilient_stats.page_starvation_start_ns[index],
			     0, now_ns) == 0)
		atomic64_inc(&mors->resilient_stats.page_starvation_events[index]);
}

void morse_resilient_page_starvation_end(struct morse *mors, u8 channel)
{
	unsigned int index;
	u64 start_ns;
	u64 duration_ns;

	if (!mors)
		return;
	index = channel_index(channel);
	start_ns = atomic64_xchg(&mors->resilient_stats.page_starvation_start_ns[index], 0);
	if (!start_ns)
		return;
	duration_ns = ktime_get_mono_fast_ns() - start_ns;
	atomic64_add(duration_ns, &mors->resilient_stats.page_starvation_ns[index]);
	atomic64_update_max(&mors->resilient_stats.page_starvation_max_ns[index], duration_ns);
}

void morse_resilient_page_restore(struct morse *mors, u64 duration_ns, bool success)
{
	if (!mors)
		return;
	atomic64_inc(&mors->resilient_stats.page_restore_events);
	if (!success)
		atomic64_inc(&mors->resilient_stats.page_restore_failures);
	atomic64_add(duration_ns, &mors->resilient_stats.page_restore_ns);
	atomic64_update_max(&mors->resilient_stats.page_restore_max_ns, duration_ns);
}

void morse_resilient_drop(struct morse *mors, u8 channel,
			  enum morse_resilient_drop_reason reason)
{
	if (!mors || reason >= MORSE_RES_DROP_REASON_COUNT)
		return;
	atomic64_inc(&mors->resilient_stats.drops[reason][channel_index(channel)]);
}

void morse_resilient_driver_tx_payload(struct morse *mors, u32 bytes)
{
	if (mors)
		atomic64_add(bytes, &mors->resilient_stats.driver_tx_payload_bytes);
}

void morse_resilient_driver_rx_payload(struct morse *mors, u32 bytes)
{
	if (mors)
		atomic64_add(bytes, &mors->resilient_stats.driver_rx_payload_bytes);
}

void morse_resilient_tx_rate(struct morse *mors, u8 mcs, u8 bw_index,
			     u8 attempts, bool success, bool aggregated,
			     u8 ampdu_len, s16 rssi)
{
	struct morse_resilient_rate_stat *rate;

	if (!mors || mcs >= MORSE_RES_MCS_COUNT || bw_index >= MORSE_RES_BW_COUNT || !attempts)
		return;
	rate = &mors->resilient_stats.tx_rate[mcs][bw_index];
	atomic64_add(attempts, &rate->attempts);
	if (success)
		atomic64_inc(&rate->successes);
	if (attempts > (success ? 1 : 0))
		atomic64_add(attempts - (success ? 1 : 0), &rate->failures);
	if (aggregated)
		atomic64_inc(&rate->aggregated);
	atomic64_add(rssi, &rate->rssi_sum);
	atomic64_inc(&rate->rssi_samples);
	if (ampdu_len)
		atomic64_inc(&mors->resilient_stats.ampdu_len_hist[ampdu_len_bucket(ampdu_len)]);
}

void morse_resilient_rx_rate(struct morse *mors, u8 mcs, u8 bw_index, s16 rssi)
{
	struct morse_resilient_rate_stat *rate;

	if (!mors || mcs >= MORSE_RES_MCS_COUNT || bw_index >= MORSE_RES_BW_COUNT)
		return;
	rate = &mors->resilient_stats.rx_rate[mcs][bw_index];
	atomic64_inc(&rate->attempts);
	atomic64_add(rssi, &rate->rssi_sum);
	atomic64_inc(&rate->rssi_samples);
}
