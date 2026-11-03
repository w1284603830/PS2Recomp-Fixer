#include "ps2_gs_pacing.h"
#include "dod_host_pacer.h"
#include "GS/HostTuning.h"

#include <cassert>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::string mode(argv[1]);
    if (mode == "wait")
    {
        ps2host::GsWaitTime measured;
        std::condition_variable cv;
        std::mutex mutex;
        std::unique_lock lock(mutex);
        measured.wait(cv, lock, [] { return true; });
        assert(measured.take() == 0);
        bool ready = false;
        // The worker cannot acquire this lock until the producer actually waits.
        std::thread worker([&] {
            { std::lock_guard guard(mutex); ready = true; }
            cv.notify_one();
        });
        measured.wait(cv, lock, [&] { return ready; });
        assert(measured.take() > 0);
        assert(measured.take() == 0);
        lock.unlock();
        worker.join();
    }
    else if (mode == "deadlines")
    {
        struct Event { uint64_t deadlineCycle; ps2host::Clock::time_point hostDeadline; int id; };
        using namespace std::chrono;
        const auto now = ps2host::Clock::now();
        std::vector<Event> events{{100, now, 3}, {200, now + milliseconds(17), 4}};
        ps2host::shiftHostDeadlines(events, 12000000, 0);
        assert(events[0].hostDeadline == now + milliseconds(12));
        assert(events[1].hostDeadline == now + milliseconds(29));
        assert(events[0].deadlineCycle == 100 && events[1].deadlineCycle == 200);
        assert(events[0].id == 3 && events[1].id == 4);
        ps2host::shiftHostDeadlines(events, 0, 500);
        assert(events[0].hostDeadline == now + microseconds(12500));
        ps2host::shiftHostDeadlines(events, 0, 0);
        assert(events[1].hostDeadline == now + microseconds(29500));
    }
    else if (mode == "overflow")
    {
        struct Event { ps2host::Clock::time_point hostDeadline; };
        std::vector<Event> events{{ps2host::Clock::time_point::max() - std::chrono::nanoseconds(1)}};
        ps2host::shiftHostDeadlines(events, UINT64_MAX, UINT32_MAX);
        assert(events[0].hostDeadline == ps2host::Clock::time_point::max());
        events.clear();
        ps2host::shiftHostDeadlines(events, UINT64_MAX, 2000);
    }
    else if (mode == "options")
    {
        for (const char* text : {"-1", "+1", "1x", " 1", "1 ", "99999999999999999999999", "2001"})
            assert(ps2host::boundedUnsigned(text, 2000) == 0);
        assert(ps2host::boundedUnsigned(nullptr, 2000) == 0);
        assert(ps2host::boundedUnsigned("", 2000) == 0);
        assert(ps2host::boundedUnsigned("500", 2000) == 500);
        assert(ps2host::boundedUnsigned("2000", 2000) == 2000);
        assert(ps2host::boundedUnsigned("16667", 1000000) == 16667);
    }
    else if (mode == "pacer")
    {
        g_dodLimitHost60 = false;
        dodhost::Pacer async(true);
        const auto start = dodhost::Clock::now();
        for (int i = 0; i < 8; ++i) async.wait();
        assert(dodhost::Clock::now() - start >= std::chrono::milliseconds(8));
        dodhost::Pacer sync(false);
        sync.wait(); // No asynchronous-pump delay is selected for a synchronous renderer.
        const auto epoch = dodhost::Clock::time_point{};
        dodhost::Schedule schedule(epoch);
        assert(schedule.advance(epoch) == epoch + dodhost::Period);
        const auto late = epoch + dodhost::Period * 10;
        assert(schedule.advance(late) == late);
        assert(schedule.advance(late) == late + dodhost::Period);
    }
    else if (mode == "latency")
    {
        for (const char* text : {"", "default", "-1", "+1", "17", "1x", " 1", "999999999999999999"})
            assert(!GSHostTuning::FrameLatency(text).has_value());
        assert(!GSHostTuning::FrameLatency(nullptr).has_value());
        for (uint32_t i = 0; i <= 16; ++i)
            assert(GSHostTuning::FrameLatency(std::to_string(i).c_str()) == i);
    }
    else if (mode == "manifest")
    {
        const auto list = GSHostTuning::ParseUiPreloadList(
            "\xef\xbb\xbf # comment\r\n \t1234-abcd-00000000\t\r\n\n1234-abcd-00000000\n1-r16x32-00000000\n"
            "../outside\nC:\\asset\nfile.png\n1 bad\n");
        assert(list.names.size() == 2 && list.rejected == 4 && !list.truncated);
        assert(list.names[0] == "1234-abcd-00000000");
        assert(list.names[1] == "1-r16x32-00000000");
        assert(GSHostTuning::ParseUiPreloadList(" \t\r\n#nothing\n").names.empty());
    }
    else if (mode == "manifest-limits")
    {
        assert(GSHostTuning::ParseUiPreloadList(std::string(129, 'a')).rejected == 1);
        const auto oversized = GSHostTuning::ParseUiPreloadList(std::string(1024 * 1024 + 1, 'a'));
        assert(oversized.names.empty() && oversized.truncated);
        std::string text;
        for (int i = 0; i < 1030; ++i) text += std::to_string(i) + "-00000000\n";
        const auto list = GSHostTuning::ParseUiPreloadList(text);
        assert(list.names.size() == 1024 && list.truncated);
    }
    else return 2;
}
