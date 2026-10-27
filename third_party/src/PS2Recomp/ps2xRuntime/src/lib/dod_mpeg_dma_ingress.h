#pragma once
// Private DoD native CH4 -> HLE MPEG ownership bridge. No guest state or callbacks.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace dod_mpeg_dma
{
    inline constexpr size_t kCapacityBytes = 2u * 1024u * 1024u;
    struct Session
    {
        uint32_t mpegAddr = 0;
        uint64_t generation = 0;
        explicit operator bool() const { return mpegAddr != 0; }
    };
    enum class SubmitResult { Accepted, Backpressure, Stale, Invalid, TooLarge };

    class Ingress
    {
    public:
        explicit Ingress(size_t capacity = kCapacityBytes) : capacity_(capacity) {}
        Session begin(uint32_t mpegAddr)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            clearLocked(); session_.mpegAddr = mpegAddr; ++session_.generation;
            return session_;
        }
        void end(uint32_t mpegAddr)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_.mpegAddr == mpegAddr)
            { clearLocked(); session_.mpegAddr = 0; ++session_.generation; }
        }
        void endAll()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            clearLocked(); session_.mpegAddr = 0; ++session_.generation;
        }
        void reset(uint32_t mpegAddr)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_.mpegAddr == mpegAddr && mpegAddr != 0)
            { clearLocked(); ++session_.generation; }
        }
        Session activeSession() const
        { std::lock_guard<std::mutex> lock(mutex_); return session_; }
        size_t queuedBytes(uint32_t mpegAddr) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return session_.mpegAddr == mpegAddr && mpegAddr != 0 ? queued_ : 0;
        }
        // Success transfers the complete byte vector. On every other result the
        // caller still owns all bytes, and no byte or queue state has changed.
        SubmitResult submit(Session expected, std::vector<uint8_t> &bytes)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!expected || expected.mpegAddr != session_.mpegAddr || expected.generation != session_.generation)
                return SubmitResult::Stale;
            if ((bytes.size() & 15u) != 0u) return SubmitResult::Invalid;
            if (bytes.size() > capacity_) return SubmitResult::TooLarge;
            if (bytes.size() > capacity_ - queued_) return SubmitResult::Backpressure;
            if (!bytes.empty())
            {
                const size_t count = bytes.size();
                chunks_.push_back(std::move(bytes)); queued_ += count;
            }
            return SubmitResult::Accepted;
        }
        // MPEG owns the returned bytes. Partial takes preserve the exact suffix
        // and order; taking input never calls the decoder or resumes guest code.
        std::vector<uint8_t> take(uint32_t mpegAddr, size_t maxBytes = 64u * 1024u)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::vector<uint8_t> result;
            if (mpegAddr == 0 || session_.mpegAddr != mpegAddr || maxBytes == 0) return result;
            const size_t count = std::min(maxBytes, queued_);
            result.reserve(count);
            while (result.size() < count)
            {
                const auto &front = chunks_.front();
                const size_t part = std::min(count - result.size(), front.size() - frontOffset_);
                result.insert(result.end(), front.begin() + static_cast<std::ptrdiff_t>(frontOffset_),
                              front.begin() + static_cast<std::ptrdiff_t>(frontOffset_ + part));
                frontOffset_ += part;
                if (frontOffset_ == front.size()) { chunks_.pop_front(); frontOffset_ = 0; }
            }
            queued_ -= count;
            return result;
        }
    private:
        void clearLocked() { chunks_.clear(); frontOffset_ = 0; queued_ = 0; }
        mutable std::mutex mutex_;
        const size_t capacity_;
        Session session_;
        std::deque<std::vector<uint8_t>> chunks_;
        size_t frontOffset_ = 0, queued_ = 0;
    };

    inline Ingress &ingress() { static Ingress value; return value; }
    inline Session begin(uint32_t mpegAddr) { return ingress().begin(mpegAddr); }
    inline void reset(uint32_t mpegAddr) { ingress().reset(mpegAddr); }
    inline void end(uint32_t mpegAddr) { ingress().end(mpegAddr); }
    inline void endAll() { ingress().endAll(); }
    inline Session activeSession() { return ingress().activeSession(); }
    inline size_t queuedBytes(uint32_t mpegAddr) { return ingress().queuedBytes(mpegAddr); }
    inline std::vector<uint8_t> take(uint32_t mpegAddr, size_t maxBytes = 64u * 1024u)
    { return ingress().take(mpegAddr, maxBytes); }
}
