#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <dispatch/dispatch.h>
#include <unistd.h>

using namespace std;

struct Config {
	int durationSeconds = 5;
	size_t messageSize = 256;
	long maxInFlight = 1024;
	int producers = 1;
	int consumers = 1;
	bool randomPayload = false;
	size_t latencySample = 100000;
	int printIntervalSeconds = 1;
	string csvPath = "";
};

static atomic<bool> stopFlag{false};

static inline uint64_t nowNs() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

static void onSignal(int) {
	stopFlag.store(true, memory_order_relaxed);
}

struct MsgHeader {
	uint64_t sequence;
	uint64_t sendTimeNs;
};

struct Stats {
	atomic<uint64_t> sentMessages{0};
	atomic<uint64_t> sentBytes{0};
	atomic<uint64_t> recvMessages{0};
	atomic<uint64_t> recvBytes{0};
};

struct LatencyRecorder {
	vector<uint64_t> samplesNs;
	size_t capacity;
	mutex mtx;
	mt19937_64 rng;
	atomic<uint64_t> seen{0};

	explicit LatencyRecorder(size_t cap)
	    : capacity(cap), rng(random_device{}()) {
		samplesNs.reserve(capacity);
	}

	void record(uint64_t valueNs) {
		uint64_t index = seen.fetch_add(1, memory_order_relaxed);
		if (capacity == 0) return;
		if (index < capacity) {
			lock_guard<mutex> lock(mtx);
			if (samplesNs.size() < capacity) {
				samplesNs.push_back(valueNs);
			}
			return;
		}
		uniform_int_distribution<uint64_t> dist(0, index);
		uint64_t pos = dist(rng);
		if (pos < capacity) {
			lock_guard<mutex> lock(mtx);
			if (pos < samplesNs.size()) {
				samplesNs[pos] = valueNs;
			}
		}
	}
};

static void computePercentiles(vector<uint64_t>& dataNs, vector<pair<double, double>>& outPctToUs) {
	if (dataNs.empty()) return;
	sort(dataNs.begin(), dataNs.end());
	auto at = [&](double pct) -> double {
		if (dataNs.empty()) return 0.0;
		double pos = pct * (dataNs.size() - 1);
		size_t idx = static_cast<size_t>(pos);
		size_t idx2 = min(idx + 1, dataNs.size() - 1);
		double frac = pos - idx;
		double vNs = static_cast<double>(dataNs[idx]) * (1.0 - frac) + static_cast<double>(dataNs[idx2]) * frac;
		return vNs / 1000.0;
	};
	const double pcts[] = {0.5, 0.90, 0.95, 0.99, 0.999};
	for (double p : pcts) {
		outPctToUs.push_back({p, at(p)});
	}
}

static bool parseBool(const string& s) {
	return s == "1" || s == "true" || s == "True" || s == "TRUE" || s == "yes" || s == "on";
}

static void usage(const char* argv0) {
	cerr << "Usage: " << argv0 << " [options]\n";
	cerr << "Options:\n";
	cerr << "  --duration-seconds N       Default 5\n";
	cerr << "  --message-size N           Default 256\n";
	cerr << "  --max-inflight N           Default 1024 (bounded in-flight operations)\n";
	cerr << "  --producers N              Default 1\n";
	cerr << "  --consumers N              Default 1 (parallel serial queues)\n";
	cerr << "  --random-payload true|false Default false\n";
	cerr << "  --latency-sample N         Default 100000\n";
	cerr << "  --print-interval N         Default 1 (seconds)\n";
	cerr << "  --csv PATH                 Append CSV results to PATH\n";
}

static Config parseArgs(int argc, char** argv) {
	Config cfg;
	for (int i = 1; i < argc; ++i) {
		string arg = argv[i];
		auto need = [&](const string& opt) {
			if (i + 1 >= argc) {
				cerr << "Missing value for " << opt << "\n";
				usage(argv[0]);
				exit(1);
			}
		};
		if (arg == "--duration-seconds") { need(arg); cfg.durationSeconds = stoi(argv[++i]); }
		else if (arg == "--message-size") { need(arg); cfg.messageSize = static_cast<size_t>(stoll(argv[++i])); }
		else if (arg == "--max-inflight") { need(arg); cfg.maxInFlight = stol(argv[++i]); }
		else if (arg == "--producers") { need(arg); cfg.producers = stoi(argv[++i]); }
		else if (arg == "--consumers") { need(arg); cfg.consumers = stoi(argv[++i]); }
		else if (arg == "--random-payload") { need(arg); cfg.randomPayload = parseBool(argv[++i]); }
		else if (arg == "--latency-sample") { need(arg); cfg.latencySample = static_cast<size_t>(stoll(argv[++i])); }
		else if (arg == "--print-interval") { need(arg); cfg.printIntervalSeconds = stoi(argv[++i]); }
		else if (arg == "--csv") { need(arg); cfg.csvPath = argv[++i]; }
		else if (arg == "--help" || arg == "-h") { usage(argv[0]); exit(0); }
		else {
			cerr << "Unknown option: " << arg << "\n";
			usage(argv[0]);
			exit(1);
		}
	}
	return cfg;
}

int main(int argc, char** argv) {
	signal(SIGINT, onSignal);
	signal(SIGTERM, onSignal);

	Config cfg = parseArgs(argc, argv);

	if (cfg.messageSize == 0 || cfg.producers <= 0 || cfg.consumers <= 0 ||
	    cfg.durationSeconds <= 0 || cfg.maxInFlight <= 0) {
		cerr << "Invalid config\n";
		return 1;
	}

	Stats stats;
	LatencyRecorder latRecorder(cfg.latencySample);

	dispatch_semaphore_t spaceSem = dispatch_semaphore_create(cfg.maxInFlight);
	vector<dispatch_queue_t> workerQueues;
	workerQueues.reserve(static_cast<size_t>(cfg.consumers));
	for (int i = 0; i < cfg.consumers; ++i) {
		string label = "gcd.worker." + to_string(i);
		dispatch_queue_t q = dispatch_queue_create(label.c_str(), DISPATCH_QUEUE_SERIAL);
		workerQueues.push_back(q);
	}
	atomic<uint64_t> rr{0};

	auto produceFunc = [&](int producerId) {
		vector<uint8_t> buffer(cfg.messageSize, 0);
		MsgHeader* header = nullptr;
		if (cfg.messageSize >= sizeof(MsgHeader)) {
			header = reinterpret_cast<MsgHeader*>(buffer.data());
		}
		mt19937_64 rng(static_cast<uint64_t>(producerId) ^ 0x9e3779b97f4a7c15ull);
		uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFFu);
		uint64_t seq = 0;
		while (!stopFlag.load(memory_order_relaxed)) {
			if (header) {
				header->sequence = seq++;
				header->sendTimeNs = nowNs();
			}
		if (cfg.randomPayload) {
			size_t start = header ? sizeof(MsgHeader) : 0;
			for (size_t i = start; i + sizeof(uint32_t) <= cfg.messageSize; i += sizeof(uint32_t)) {
				uint32_t r = dist(rng);
				memcpy(buffer.data() + i, &r, sizeof(uint32_t));
			}
		}
			dispatch_semaphore_wait(spaceSem, DISPATCH_TIME_FOREVER);

			uint64_t idx = rr.fetch_add(1, memory_order_relaxed);
			dispatch_queue_t q = workerQueues[static_cast<size_t>(idx % workerQueues.size())];

			vector<uint8_t> payload = buffer;
			dispatch_async(q, ^{
				stats.recvMessages.fetch_add(1, memory_order_relaxed);
				stats.recvBytes.fetch_add(payload.size(), memory_order_relaxed);
				if (payload.size() >= sizeof(MsgHeader)) {
					const MsgHeader* h = reinterpret_cast<const MsgHeader*>(payload.data());
					uint64_t recvNs = nowNs();
					uint64_t sendNs = h->sendTimeNs;
					if (recvNs >= sendNs) {
						latRecorder.record(recvNs - sendNs);
					}
				}
				dispatch_semaphore_signal(spaceSem);
			});

			stats.sentMessages.fetch_add(1, memory_order_relaxed);
			stats.sentBytes.fetch_add(cfg.messageSize, memory_order_relaxed);
		}
	};

	vector<thread> producers;
	for (int i = 0; i < cfg.producers; ++i) {
		producers.emplace_back(produceFunc, i);
	}

	const auto start = chrono::steady_clock::now();
	const auto endTime = start + chrono::seconds(cfg.durationSeconds);
	while (chrono::steady_clock::now() < endTime && !stopFlag.load(memory_order_relaxed)) {
		this_thread::sleep_for(chrono::seconds(cfg.printIntervalSeconds));
		uint64_t sent = stats.sentMessages.load(memory_order_relaxed);
		uint64_t recv = stats.recvMessages.load(memory_order_relaxed);
		uint64_t sbytes = stats.sentBytes.load(memory_order_relaxed);
		uint64_t rbytes = stats.recvBytes.load(memory_order_relaxed);
		cout << "GCD Progress: sent=" << sent << " recv=" << recv
		     << " sentMiB=" << fixed << setprecision(2) << (double)sbytes / (1024.0 * 1024.0)
		     << " recvMiB=" << fixed << setprecision(2) << (double)rbytes / (1024.0 * 1024.0)
		     << "\n";
		cout.flush();
	}
	stopFlag.store(true, memory_order_relaxed);
	for (auto& t : producers) t.join();

	for (long i = 0; i < cfg.maxInFlight; ++i) dispatch_semaphore_signal(spaceSem);
	this_thread::sleep_for(chrono::milliseconds(200));

	const auto end = chrono::steady_clock::now();
	double elapsedSec = chrono::duration<double>(end - start).count();
	if (elapsedSec <= 0) elapsedSec = static_cast<double>(cfg.durationSeconds);
	uint64_t sent = stats.sentMessages.load(memory_order_relaxed);
	uint64_t recv = stats.recvMessages.load(memory_order_relaxed);
	uint64_t sbytes = stats.sentBytes.load(memory_order_relaxed);
	uint64_t rbytes = stats.recvBytes.load(memory_order_relaxed);
	double recvMsgPerSec = recv / elapsedSec;
	double recvMBps = (rbytes / (1024.0 * 1024.0)) / elapsedSec;

	vector<uint64_t> latCopyNs;
	{
		lock_guard<mutex> lock(latRecorder.mtx);
		latCopyNs = latRecorder.samplesNs;
	}
	vector<pair<double, double>> pctUs;
	computePercentiles(latCopyNs, pctUs);

	cout << "\nGCD Summary:\n";
	cout << "  elapsed-sec:         " << fixed << setprecision(3) << elapsedSec << "\n";
	cout << "  messages-sent:       " << sent << "\n";
	cout << "  messages-recv:       " << recv << "\n";
	cout << "  bytes-sent:          " << sbytes << "\n";
	cout << "  bytes-recv:          " << rbytes << "\n";
	cout << "  throughput-msg/s:    " << fixed << setprecision(2) << recvMsgPerSec << "\n";
	cout << "  throughput-MiB/s:    " << fixed << setprecision(2) << recvMBps << "\n";
	if (!pctUs.empty()) {
		cout << "  latency-us (p50,p90,p95,p99,p99.9):";
		for (auto& p : pctUs) {
			cout << " p" << fixed << setprecision(3) << (p.first * 100.0) << "=" << fixed << setprecision(2) << p.second;
		}
		cout << "\n";
	} else {
		cout << "  latency-us:          not available (message-size < header)\n";
	}

	if (!cfg.csvPath.empty()) {
		FILE* f = fopen(cfg.csvPath.c_str(), "a");
		if (!f) {
			perror("fopen csv");
		} else {
			double p50 = NAN, p90 = NAN, p95 = NAN, p99 = NAN, p999 = NAN;
			if (pctUs.size() >= 5) {
				p50 = pctUs[0].second; p90 = pctUs[1].second; p95 = pctUs[2].second; p99 = pctUs[3].second; p999 = pctUs[4].second;
			}
			fprintf(f,
			        "%s,%s,%d,%zu,%ld,%d,%d,%d,%d,%zu,%.6f,%llu,%llu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
			        "gcd",
			        "gcd_queue",
			        cfg.durationSeconds,
			        cfg.messageSize,
			        cfg.maxInFlight,
			        cfg.producers,
			        cfg.consumers,
			        0,
			        cfg.randomPayload ? 1 : 0,
			        cfg.latencySample,
			        elapsedSec,
			        static_cast<unsigned long long>(recv),
			        static_cast<unsigned long long>(rbytes),
			        recvMsgPerSec,
			        recvMBps,
			        p50, p90, p95, p99, p999);
			fclose(f);
		}
	}

	return 0;
}


