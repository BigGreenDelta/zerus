#pragma once

#include <QTimer>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

// Сторож главного потока: если тот перестал крутить цикл событий дольше порога, из
// отдельного потока зовётся onHang -- в бою это abort() (см. main.cpp), чтобы launchd
// (KeepAlive Crashed=true, см. packaging/com.hgdev.hgs-tray.plist) или systemd
// (Restart=on-failure, см. packaging/hgs-tray.service) подняли трей заново.
//
// Зачем. На macOS 26 главный поток трея может намертво повиснуть внутри AppKit при
// обновлении элемента строки меню -- см. QuietTrayIcon.h, там подробности и живой случай
// 2026-09-14: шесть дней иконка висела и не отвечала на клики, процесс при этом «работал».
// QuietTrayIcon режет поводы, но баг Apple'овский, и гарантии нет. Мёртвый трей, который
// перезапустился через полминуты, лучше мёртвого трея на неделю; а крэш-репорт от abort()
// ещё и показывает, где именно стоял главный поток (поток сторожа назван hgs-watchdog,
// чтобы в репорте было видно, кто нажал).
//
// Устройство. QTimer на главном потоке каждые threshold/4 пишет «я жив» (момент по
// steady_clock) в atomic; сторожевой std::thread каждые threshold/2 сравнивает с текущим
// моментом. steady_clock на обеих платформах не идёт во время сна машины (mach_absolute_time
// на маке, CLOCK_MONOTONIC на Linux), так что пробуждение после ночи не выглядит как
// зависание. Срабатывает не больше одного раза. Деструктор останавливает поток через
// condition_variable, а не ждёт очередной проверки.
//
// Что НЕ считается зависанием: модальные диалоги (QMessageBox::exec крутит вложенный цикл,
// таймер тикает), любая обычная работа. Порог в десятки секунд с запасом покрывает даже
// очень тормозящий бокс.
//
// A main thread starved of CPU trips it too, whatever its stack shows. On macOS
// check `launchctl print gui/<uid>/com.hgdev.hgs-tray` first: a LaunchAgent
// without ProcessType Interactive runs as a daemon clamped below ordinary load.
//
// Windows has no steady clock that stops during sleep, so nowMs() reads the
// unbiased interrupt time there. Nothing supervises the tray on Windows; main.cpp
// registers it with Windows Error Reporting (RegisterApplicationRestart) instead,
// which restarts it after the abort once it has run for at least a minute.
class MainThreadWatchdog {
public:
    MainThreadWatchdog(int thresholdMs, std::function<void()> onHang);
    ~MainThreadWatchdog();

    MainThreadWatchdog(const MainThreadWatchdog &) = delete;
    MainThreadWatchdog &operator=(const MainThreadWatchdog &) = delete;

private:
    using Clock = std::chrono::steady_clock;
    static std::int64_t nowMs();

    const int m_thresholdMs;
    std::function<void()> m_onHang;
    std::atomic<std::int64_t> m_lastBeatMs;
    QTimer m_heartbeat;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop = false;
    // Последним: стартует в конструкторе, когда всё выше уже готово.
    std::thread m_thread;
};
