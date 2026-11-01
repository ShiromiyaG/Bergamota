// Where a host thread spends its time, by sampling: another thread stops it
// about every millisecond and notes where it was. The game's functions are
// named by the library (0x00123456, Battle.cro+0x1234), the rest by the
// host's debug symbols. Windows only for now; elsewhere it counts nothing.

#ifndef RUNTIME_SAMPLER_H
#define RUNTIME_SAMPLER_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

namespace runtime {

class Library;

class Sampler {
public:
    Sampler();
    ~Sampler();
    // samples the thread that calls it, until stop()
    void start_here();
    void stop();
    // the functions where the samples fell, most first, as "share% name"
    // lines; at most count
    std::vector<std::string> report(const Library &library, size_t count) const;
    uint64_t samples() const { return total_; }

private:
    void run();
    void *thread_ = nullptr; // the sampled one
    std::thread sampler_;
    std::atomic<bool> stop_{false};
    std::map<uintptr_t, uint64_t> hits_;
    uint64_t total_ = 0;
};

} // namespace runtime

#endif
