#include "dod_host_pacer.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

dodhost::Pacer::Pacer()
{
#ifdef _WIN32
    if (enabled)
    {
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002,
                                       TIMER_MODIFY_STATE | SYNCHRONIZE);
        highResolution = timer != nullptr;
        if (!timer)
            timer = CreateWaitableTimerExW(nullptr, nullptr, 0,
                                           TIMER_MODIFY_STATE | SYNCHRONIZE);
    }
#endif
}
dodhost::Pacer::~Pacer()
{
#ifdef _WIN32
    if (timer) CloseHandle(timer);
#endif
}
void dodhost::Pacer::wait()
{
    if (!enabled) return;
    const auto target = schedule.advance(Clock::now());
#ifdef _WIN32
    const auto margin = highResolution ? std::chrono::microseconds(100)
                                       : std::chrono::microseconds(1000);
    const auto remaining = target - Clock::now() - margin;
    if (remaining > Clock::duration::zero())
    {
        LARGE_INTEGER due;
        due.QuadPart = -std::max<LONGLONG>(1,
            std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer, INFINITE);
        else
            std::this_thread::sleep_until(target - margin);
    }
    while (Clock::now() < target) YieldProcessor();
#else
    std::this_thread::sleep_until(target - std::chrono::milliseconds(1));
    while (Clock::now() < target) std::this_thread::yield();
#endif
}
