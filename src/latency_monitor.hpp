#pragma once

#include "latency_measurement.hpp"
#include "timing.hpp"
#include "ring_buffer.hpp"
#include "hdr_histogram.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <iostream>

// Central latency monitoring system
class LatencyMonitor {
public:
    // Configuration
    struct Config {
        bool enabled;
        size_t sample_buffer_size;
        size_t warmup_samples;
        uint64_t report_interval_ms;
        bool enable_csv_export;
        std::string csv_filename;

        Config()
            : enabled(true)
            , sample_buffer_size(1048576)
            , warmup_samples(1000)
            , report_interval_ms(5000)
            , enable_csv_export(false)
            , csv_filename("latency_samples.csv") {}
    };

    explicit LatencyMonitor(const Config& config = Config())
        : config_(config)
        , sample_buffer_(nullptr)
        , next_message_id_(0)
        , samples_collected_(0)
        , running_(false)
        , monitor_thread_()
        , active_measurements_() {

        if (config_.enabled) {
            // Initialize sample buffer (must be power of 2)
            size_t buffer_size = config_.sample_buffer_size;
            if ((buffer_size & (buffer_size - 1)) != 0) {
                // Round up to next power of 2
                buffer_size--;
                buffer_size |= buffer_size >> 1;
                buffer_size |= buffer_size >> 2;
                buffer_size |= buffer_size >> 4;
                buffer_size |= buffer_size >> 8;
                buffer_size |= buffer_size >> 16;
                buffer_size |= buffer_size >> 32;
                buffer_size++;
            }

            sample_buffer_ = std::make_unique<BlockingSPSCRingBuffer<LatencyMeasurement, 1048576>>();

            // Initialize histograms for each metric
            for (int i = 0; i < static_cast<int>(LatencyMetric::NUM_METRICS); ++i) {
                histograms_[static_cast<LatencyMetric>(i)] = std::make_unique<HDRHistogram>(10000000, 3);
            }

            // Measure rdtsc overhead
            rdtsc_overhead_ns_ = timing::measure_rdtsc_overhead();
        }
    }

    ~LatencyMonitor() {
        stop();
    }

    // Start the monitoring thread
    void start() {
        if (!config_.enabled || running_) {
            return;
        }

        running_ = true;
        monitor_thread_ = std::thread(&LatencyMonitor::monitoring_loop, this);
        std::cout << "[LatencyMonitor] Started (rdtsc overhead: " << rdtsc_overhead_ns_ << "ns)" << std::endl;
    }

    // Stop the monitoring thread
    void stop() {
        if (!running_) {
            return;
        }

        running_ = false;
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }

        // Final report
        print_report();
    }

    // Begin tracking a new message
    uint64_t start_measurement(const std::string& symbol, const std::string& exchange) {
        if (!config_.enabled) {
            return 0;
        }

        uint64_t msg_id = next_message_id_.fetch_add(1, std::memory_order_relaxed);

        LatencyMeasurement measurement;
        measurement.message_id = msg_id;
        measurement.symbol = symbol;
        measurement.exchange = exchange;

        // Store in active measurements map
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_measurements_[msg_id] = measurement;
        }

        return msg_id;
    }

    // Record a timestamp for a specific message at a measurement point
    void record_timestamp(uint64_t message_id, MeasurementPoint point) {
        if (!config_.enabled || message_id == 0) {
            return;
        }

        uint64_t timestamp = timing::rdtsc();

        std::lock_guard<std::mutex> lock(active_mutex_);
        auto it = active_measurements_.find(message_id);
        if (it != active_measurements_.end()) {
            it->second.record(point, timestamp);
        }
    }

    // Complete a measurement and submit for analysis
    void complete_measurement(uint64_t message_id) {
        if (!config_.enabled || message_id == 0) {
            return;
        }

        LatencyMeasurement measurement;
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            auto it = active_measurements_.find(message_id);
            if (it == active_measurements_.end()) {
                return;
            }
            measurement = it->second;
            active_measurements_.erase(it);
        }

        // Submit to ring buffer for processing
        if (sample_buffer_) {
            sample_buffer_->push(std::move(measurement));
        }
    }

    // Get current statistics snapshot
    struct StatsSnapshot {
        std::unordered_map<LatencyMetric, HDRHistogram::Percentiles> metrics;
        uint64_t total_samples;
        uint64_t dropped_samples;
        uint64_t rdtsc_overhead_ns;
    };

    StatsSnapshot get_stats() const {
        StatsSnapshot snapshot;
        snapshot.rdtsc_overhead_ns = rdtsc_overhead_ns_;
        snapshot.total_samples = samples_collected_.load(std::memory_order_relaxed);
        snapshot.dropped_samples = sample_buffer_ ? sample_buffer_->get_dropped_count() : 0;

        std::lock_guard<std::mutex> lock(histogram_mutex_);
        for (const auto& [metric, histogram] : histograms_) {
            snapshot.metrics[metric] = histogram->get_common_percentiles();
        }

        return snapshot;
    }

    // Export raw samples to CSV
    void export_to_csv(const std::string& filename) const {
        std::ofstream file(filename);
        if (!file.is_open()) {
            std::cerr << "[LatencyMonitor] Failed to open CSV file: " << filename << std::endl;
            return;
        }

        // Header
        file << "message_id,symbol,exchange,";
        file << "recv_ns,parsed_ns,enqueued_ns,dequeued_ns,engine_ns,calculated_ns,dashboard_ns,";
        file << "parse_latency_ns,queue_latency_ns,engine_latency_ns,end_to_end_ns\n";

        // No data export in this simple version - would need to store all samples
        // In production, you'd process samples as they come or store them

        file.close();
        std::cout << "[LatencyMonitor] CSV export complete: " << filename << std::endl;
    }

    // Print a report to stdout
    void print_report() const {
        auto stats = get_stats();

        std::cout << "\n========== Latency Report ==========\n";
        std::cout << "Total Samples: " << stats.total_samples << "\n";
        std::cout << "Dropped Samples: " << stats.dropped_samples << "\n";
        std::cout << "RDTSC Overhead: " << stats.rdtsc_overhead_ns << " ns\n";
        std::cout << "====================================\n\n";

        for (int i = 0; i < static_cast<int>(LatencyMetric::NUM_METRICS); ++i) {
            LatencyMetric metric = static_cast<LatencyMetric>(i);
            auto it = stats.metrics.find(metric);
            if (it == stats.metrics.end()) continue;

            const auto& p = it->second;
            std::cout << metric_name(metric) << " Latency:\n";
            std::cout << "  Min:    " << std::setw(10) << p.min << " ns\n";
            std::cout << "  Mean:   " << std::setw(10) << static_cast<uint64_t>(p.mean) << " ns\n";
            std::cout << "  P50:    " << std::setw(10) << p.p50 << " ns\n";
            std::cout << "  P90:    " << std::setw(10) << p.p90 << " ns\n";
            std::cout << "  P95:    " << std::setw(10) << p.p95 << " ns\n";
            std::cout << "  P99:    " << std::setw(10) << p.p99 << " ns\n";
            std::cout << "  P99.9:  " << std::setw(10) << p.p999 << " ns\n";
            std::cout << "  P99.99: " << std::setw(10) << p.p9999 << " ns\n";
            std::cout << "  Max:    " << std::setw(10) << p.max << " ns\n";
            std::cout << "  StdDev: " << std::setw(10) << static_cast<uint64_t>(p.std_dev) << " ns\n";
            std::cout << "\n";
        }

        std::cout << "====================================\n\n";
    }

private:
    Config config_;
    std::unique_ptr<BlockingSPSCRingBuffer<LatencyMeasurement, 1048576>> sample_buffer_;
    std::atomic<uint64_t> next_message_id_;
    std::atomic<uint64_t> samples_collected_;
    std::atomic<bool> running_;
    std::thread monitor_thread_;
    uint64_t rdtsc_overhead_ns_;

    // Active measurements being tracked
    std::unordered_map<uint64_t, LatencyMeasurement> active_measurements_;
    mutable std::mutex active_mutex_;

    // Histograms per metric
    std::unordered_map<LatencyMetric, std::unique_ptr<HDRHistogram>> histograms_;
    mutable std::mutex histogram_mutex_;

    // Background monitoring loop
    void monitoring_loop() {
        auto last_report = std::chrono::steady_clock::now();
        LatencyMeasurement sample;

        while (running_) {
            // Process samples from buffer
            bool processed_any = false;
            while (sample_buffer_->try_pop(sample)) {
                process_sample(sample);
                processed_any = true;
            }

            // Check if it's time to report
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report);

            if (elapsed.count() >= static_cast<long>(config_.report_interval_ms)) {
                print_report();
                last_report = now;
            }

            // Sleep a bit if no samples processed
            if (!processed_any) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        // Process remaining samples
        while (sample_buffer_->try_pop(sample)) {
            process_sample(sample);
        }
    }

    void process_sample(const LatencyMeasurement& sample) {
        uint64_t sample_count = samples_collected_.fetch_add(1, std::memory_order_relaxed);

        // Skip warmup samples
        if (sample_count < config_.warmup_samples) {
            return;
        }

        // Convert cycles to nanoseconds and record in histograms
        auto& calibrator = timing::get_calibrator();

        std::lock_guard<std::mutex> lock(histogram_mutex_);
        for (int i = 0; i < static_cast<int>(LatencyMetric::NUM_METRICS); ++i) {
            LatencyMetric metric = static_cast<LatencyMetric>(i);
            uint64_t cycles = calculate_metric_cycles(sample, metric);

            if (cycles > 0) {
                uint64_t ns = calibrator.cycles_to_ns(cycles);
                // Subtract measurement overhead
                if (ns > rdtsc_overhead_ns_) {
                    ns -= rdtsc_overhead_ns_;
                }
                histograms_[metric]->record(ns);
            }
        }
    }
};

// Global singleton instance
inline LatencyMonitor& get_latency_monitor() {
    static LatencyMonitor monitor;
    return monitor;
}
