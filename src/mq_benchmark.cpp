#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <mqueue.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <sstream>

using namespace std;

struct Config {
	string queueName = "/mq_bench";
	int durationSeconds = 5;
	size_t messageSize = 256;
	long maxMessages = 1024;
	int producers = 1;
	int consumers = 1;
	bool unlinkAtStart = true;
	bool unlinkAtEnd = true;
	bool nonBlocking = false;
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
	atomic<uint64_t> sendErrors{0};
	atomic<uint64_t> recvErrors{0};
	atomic<uint64_t> sendEagain{0};
	atomic<uint64_t> recvEagain{0};
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
			} else {
				uniform_int_distribution<uint64_t> dist(0, index);
				uint64_t pos = dist(rng);
				if (pos < capacity) {
					samplesNs[pos] = valueNs;
				}
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

static void printConfig(const Config& cfg) {
	cout << "Configuration:\n";
	cout << "  queue-name:           " << cfg.queueName << "\n";
	cout << "  duration-seconds:     " << cfg.durationSeconds << "\n";
	cout << "  message-size:         " << cfg.messageSize << "\n";
	cout << "  max-messages:         " << cfg.maxMessages << "\n";
	cout << "  producers:            " << cfg.producers << "\n";
	cout << "  consumers:            " << cfg.consumers << "\n";
	cout << "  unlink-at-start:      " << (cfg.unlinkAtStart ? "true" : "false") << "\n";
	cout << "  unlink-at-end:        " << (cfg.unlinkAtEnd ? "true" : "false") << "\n";
	cout << "  non-blocking:         " << (cfg.nonBlocking ? "true" : "false") << "\n";
	cout << "  random-payload:       " << (cfg.randomPayload ? "true" : "false") << "\n";
	cout << "  latency-sample:       " << cfg.latencySample << "\n";
	cout << "  print-interval-s:     " << cfg.printIntervalSeconds << "\n";
	if (!cfg.csvPath.empty()) {
		cout << "  csv-path:             " << cfg.csvPath << "\n";
	}
	cout.flush();
}

static void usage(const char* argv0) {
	cerr << "Usage: " << argv0 << " [options]\n";
	cerr << "Options:\n";
	cerr << "  --queue-name NAME          Default /mq_bench\n";
	cerr << "  --duration-seconds N       Default 5\n";
	cerr << "  --message-size N           Default 256 (<= system msgsize_max)\n";
	cerr << "  --max-messages N           Default 1024 (<= system msg_max)\n";
	cerr << "  --producers N              Default 1\n";
	cerr << "  --consumers N              Default 1\n";
	cerr << "  --unlink-start true|false  Default true\n";
	cerr << "  --unlink-end true|false    Default true\n";
	cerr << "  --nonblocking true|false   Default false\n";
	cerr << "  --random-payload true|false Default false\n";
	cerr << "  --latency-sample N         Default 100000\n";
	cerr << "  --print-interval N         Default 1 (seconds)\n";
	cerr << "  --csv PATH                 Append CSV results to PATH\n";
}

static bool parseBool(const string& s) {
	return s == "1" || s == "true" || s == "True" || s == "TRUE" || s == "yes" || s == "on";
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
		if (arg == "--queue-name") { need(arg); cfg.queueName = argv[++i]; }
		else if (arg == "--duration-seconds") { need(arg); cfg.durationSeconds = stoi(argv[++i]); }
		else if (arg == "--message-size") { need(arg); cfg.messageSize = static_cast<size_t>(stoll(argv[++i])); }
		else if (arg == "--max-messages") { need(arg); cfg.maxMessages = stol(argv[++i]); }
		else if (arg == "--producers") { need(arg); cfg.producers = stoi(argv[++i]); }
		else if (arg == "--consumers") { need(arg); cfg.consumers = stoi(argv[++i]); }
		else if (arg == "--unlink-start") { need(arg); cfg.unlinkAtStart = parseBool(argv[++i]); }
		else if (arg == "--unlink-end") { need(arg); cfg.unlinkAtEnd = parseBool(argv[++i]); }
		else if (arg == "--nonblocking") { need(arg); cfg.nonBlocking = parseBool(argv[++i]); }
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

static long readLongFromFile(const char* path, long fallback) {
	ifstream in(path);
	if (!in.good()) return fallback;
	long v = fallback;
	in >> v;
	return in.fail() ? fallback : v;
}

static void producerThread(mqd_t mq, const Config& cfg, Stats& stats, int producerId) {
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
		timespec ts{};
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 100 * 1000 * 1000; 
		if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
		int ret = mq_timedsend(mq, reinterpret_cast<const char*>(buffer.data()),
		                       static_cast<unsigned>(cfg.messageSize), 0, &ts);
		if (ret == 0) {
			stats.sentMessages.fetch_add(1, memory_order_relaxed);
			stats.sentBytes.fetch_add(cfg.messageSize, memory_order_relaxed);
		} else {
			if (errno == EAGAIN || errno == ETIMEDOUT) {
				stats.sendEagain.fetch_add(1, memory_order_relaxed);
				this_thread::sleep_for(chrono::microseconds(50));
			} else {
				stats.sendErrors.fetch_add(1, memory_order_relaxed);
				this_thread::sleep_for(chrono::microseconds(100));
			}
		}
	}
}

static void consumerThread(mqd_t mq, const Config& cfg, Stats& stats, LatencyRecorder& latRecorder) {
	vector<uint8_t> buffer(cfg.messageSize, 0);
	MsgHeader* header = nullptr;
	if (cfg.messageSize >= sizeof(MsgHeader)) {
		header = reinterpret_cast<MsgHeader*>(buffer.data());
	}
	while (!stopFlag.load(memory_order_relaxed)) {
		unsigned int prio = 0;
		timespec ts{};
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 100 * 1000 * 1000;
		if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
		ssize_t n = mq_timedreceive(mq, reinterpret_cast<char*>(buffer.data()),
		                            static_cast<unsigned>(buffer.size()), &prio, &ts);
		if (n >= 0) {
			stats.recvMessages.fetch_add(1, memory_order_relaxed);
			stats.recvBytes.fetch_add(static_cast<size_t>(n), memory_order_relaxed);
			if (header && static_cast<size_t>(n) >= sizeof(MsgHeader)) {
				uint64_t recvNs = nowNs();
				uint64_t sendNs = header->sendTimeNs;
				if (recvNs >= sendNs) {
					latRecorder.record(recvNs - sendNs);
				}
			}
		} else {
			if (errno == EAGAIN || errno == ETIMEDOUT) {
				stats.recvEagain.fetch_add(1, memory_order_relaxed);
				this_thread::sleep_for(chrono::microseconds(50));
			} else {
				stats.recvErrors.fetch_add(1, memory_order_relaxed);
				this_thread::sleep_for(chrono::microseconds(100));
			}
		}
	}
}

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

int main(int argc, char** argv) {
	signal(SIGINT, onSignal);
	signal(SIGTERM, onSignal);

	Config cfg = parseArgs(argc, argv);
	printConfig(cfg);

	if (cfg.messageSize == 0) {
		cerr << "message-size must be > 0\n";
		return 1;
	}
	if (cfg.producers <= 0 || cfg.consumers <= 0) {
		cerr << "producers and consumers must be >= 1\n";
		return 1;
	}
	if (cfg.durationSeconds <= 0) {
		cerr << "duration-seconds must be >= 1\n";
		return 1;
	}
	if (cfg.maxMessages <= 0) {
		cerr << "max-messages must be >= 1\n";
		return 1;
	}

	if (cfg.unlinkAtStart) {
		mq_unlink(cfg.queueName.c_str());
	}

	mq_attr attr{};
	attr.mq_flags = cfg.nonBlocking ? O_NONBLOCK : 0;
#ifdef __linux__
	long sys_maxmsg = readLongFromFile("/proc/sys/fs/mqueue/msg_max", 10);
	long sys_msgsize = readLongFromFile("/proc/sys/fs/mqueue/msgsize_max", 8192);
	long requested_maxmsg = cfg.maxMessages;
	long requested_msgsize = static_cast<long>(cfg.messageSize);
	if (requested_maxmsg > sys_maxmsg) {
		cerr << "Note: requested max-messages=" << requested_maxmsg
		     << " exceeds system msg_max=" << sys_maxmsg << ", capping.\n";
		requested_maxmsg = sys_maxmsg;
	}
	if (requested_msgsize > sys_msgsize) {
		cerr << "Note: requested message-size=" << requested_msgsize
		     << " exceeds system msgsize_max=" << sys_msgsize << ", capping.\n";
		requested_msgsize = sys_msgsize;
	}
	if (requested_maxmsg < 1) requested_maxmsg = 1;
	if (requested_msgsize < 1) requested_msgsize = 1;
	attr.mq_maxmsg = requested_maxmsg;
	attr.mq_msgsize = requested_msgsize;
#else
	attr.mq_maxmsg = cfg.maxMessages;
	attr.mq_msgsize = static_cast<long>(cfg.messageSize);
#endif

	int oflags = O_CREAT | O_RDWR;
	if (cfg.nonBlocking) oflags |= O_NONBLOCK;

	mqd_t mq = mq_open(cfg.queueName.c_str(), oflags, 0600, &attr);
	if (mq == (mqd_t)-1) {
		perror("mq_open");
		cerr << "Failed to open queue " << cfg.queueName << ". On Linux, you may need to adjust /proc/sys/fs/mqueue/msg_max or msgsize_max.\n";
		return 2;
	}

	mq_attr actual{};
	if (mq_getattr(mq, &actual) == -1) {
		perror("mq_getattr");
		mq_close(mq);
		if (cfg.unlinkAtEnd) mq_unlink(cfg.queueName.c_str());
		return 2;
	}

	cout << "Effective mq attributes:\n";
	cout << "  mq_flags:    " << actual.mq_flags << "\n";
	cout << "  mq_maxmsg:   " << actual.mq_maxmsg << "\n";
	cout << "  mq_msgsize:  " << actual.mq_msgsize << "\n";
	cout.flush();

	Stats stats;
	LatencyRecorder latRecorder(cfg.latencySample);

	vector<thread> threads;
	threads.reserve(static_cast<size_t>(cfg.producers + cfg.consumers));
	for (int i = 0; i < cfg.consumers; ++i) {
		threads.emplace_back(consumerThread, mq, cref(cfg), ref(stats), ref(latRecorder));
	}
	for (int i = 0; i < cfg.producers; ++i) {
		threads.emplace_back(producerThread, mq, cref(cfg), ref(stats), i);
	}

	const auto start = chrono::steady_clock::now();
	const auto endTime = start + chrono::seconds(cfg.durationSeconds);
	while (chrono::steady_clock::now() < endTime && !stopFlag.load(memory_order_relaxed)) {
		this_thread::sleep_for(chrono::seconds(cfg.printIntervalSeconds));
		uint64_t sent = stats.sentMessages.load(memory_order_relaxed);
		uint64_t recv = stats.recvMessages.load(memory_order_relaxed);
		uint64_t sbytes = stats.sentBytes.load(memory_order_relaxed);
		uint64_t rbytes = stats.recvBytes.load(memory_order_relaxed);
		cout << "Progress: sent=" << sent << " recv=" << recv
		     << " sentMiB=" << fixed << setprecision(2) << (double)sbytes / (1024.0 * 1024.0)
		     << " recvMiB=" << fixed << setprecision(2) << (double)rbytes / (1024.0 * 1024.0)
		     << "\n";
		cout.flush();
	}
	stopFlag.store(true, memory_order_relaxed);

	for (auto& t : threads) t.join();

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

	cout << "\nSummary:\n";
	cout << "  elapsed-sec:         " << fixed << setprecision(3) << elapsedSec << "\n";
	cout << "  messages-sent:       " << sent << "\n";
	cout << "  messages-recv:       " << recv << "\n";
	cout << "  bytes-sent:          " << sbytes << "\n";
	cout << "  bytes-recv:          " << rbytes << "\n";
	cout << "  throughput-msg/s:    " << fixed << setprecision(2) << recvMsgPerSec << "\n";
	cout << "  throughput-MiB/s:    " << fixed << setprecision(2) << recvMBps << "\n";
	cout << "  send-errors:         " << stats.sendErrors.load() << " (EAGAIN " << stats.sendEagain.load() << ")\n";
	cout << "  recv-errors:         " << stats.recvErrors.load() << " (EAGAIN " << stats.recvEagain.load() << ")\n";
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
			        "mqueue",
			        cfg.queueName.c_str(),
			        cfg.durationSeconds,
			        cfg.messageSize,
			        cfg.maxMessages,
			        cfg.producers,
			        cfg.consumers,
			        cfg.nonBlocking ? 1 : 0,
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

	if (cfg.unlinkAtEnd) {
		mq_unlink(cfg.queueName.c_str());
	}
	mq_close(mq);
	return 0;
}


