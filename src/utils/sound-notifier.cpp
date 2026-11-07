// Windows waveOut backend. No OBS source/monitoring output and no Qt Multimedia.
#include "utils/sound-notifier.hpp"
#include "utils/logger.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <mmddk.h>
#endif

namespace ReplayBufferPro {
namespace {
std::mutex instanceMutex;
SoundNotifier *instance = nullptr;
constexpr int maxVolume = 50;

// obs_module_config_path follows OBS portable mode and does not alter profile config.
QString configPath()
{
    char *path = obs_module_config_path("completion-sound.json");
    const QString result = path ? QString::fromUtf8(path) : QString();
    bfree(path);
    return result;
}

#ifdef _WIN32
// Query the persistent interface path, not the truncated WinMM display name.
// Failure is fail-closed: this output is not offered/used, never replaced by mapper.
QString interfaceId(HWAVEOUT device)
{
    DWORD bytes = 0;
    if (waveOutMessage(device, DRV_QUERYDEVICEINTERFACESIZE,
                       reinterpret_cast<DWORD_PTR>(&bytes), 0) != MMSYSERR_NOERROR ||
        bytes < sizeof(wchar_t) || bytes > 65536 || bytes % sizeof(wchar_t) != 0)
        return {};
    std::vector<wchar_t> path(bytes / sizeof(wchar_t) + 1, L'\0');
    if (waveOutMessage(device, DRV_QUERYDEVICEINTERFACE,
                       reinterpret_cast<DWORD_PTR>(path.data()), bytes) != MMSYSERR_NOERROR)
        return {};
    return QString::fromWCharArray(path.data()).toCaseFolded();
}

QVector<SoundNotifier::Device> enumerateOutputs()
{
    QVector<SoundNotifier::Device> result;
    const UINT count = waveOutGetNumDevs();
    for (UINT index = 0; index < count; ++index) {
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(index, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
            continue;
        const auto device = reinterpret_cast<HWAVEOUT>(static_cast<UINT_PTR>(index));
        const QString id = interfaceId(device);
        if (id.isEmpty())
            continue;
        // Ambiguous IDs are rejected at playback rather than selecting arbitrarily.
        result.push_back({id, QString::fromWCharArray(caps.szPname)});
    }
    return result;
}

// Entire lifetime stays on the audio worker. CALLBACK_EVENT has no plugin callback
// address that could run after unload. If a driver violates reset/unprepare's
// contract, retain its buffers/handles (and disable playback) rather than UAF.
struct Playback {
    HWAVEOUT device = nullptr;
    HANDLE done = nullptr;
    WAVEHDR header{};
    std::vector<int16_t> samples;
    bool prepared = false;

    bool release()
    {
        if (device) {
            const MMRESULT reset = waveOutReset(device);
            if (prepared && waveOutUnprepareHeader(device, &header, sizeof(header)) != MMSYSERR_NOERROR) {
                Logger::warning("Completion sound: cannot unprepare audio after reset (%u); retaining driver buffers", reset);
                return false;
            }
            prepared = false;
            if (waveOutClose(device) != MMSYSERR_NOERROR) {
                Logger::warning("Completion sound: cannot close output; retaining driver state");
                return false;
            }
            device = nullptr;
        }
        if (done) {
            CloseHandle(done);
            done = nullptr;
        }
        return true;
    }
};
struct PlaybackDeleter {
    void operator()(Playback *playback) const noexcept
    {
        if (playback->release()) delete playback;
        // On driver failure keep the allocation alive, including header and event.
    }
};
#endif
} // namespace

struct SoundNotifier::Worker {
    std::mutex mutex;
    std::condition_variable ready;
    std::thread thread;
    std::atomic<bool> stopping{false};
    bool busy = false;
    bool pending = false;
    bool backendHealthy = true;
    QString selected;
    int gain = 0;
#ifdef _WIN32
    HANDLE stopEvent = nullptr;
#endif

    Worker()
    {
#ifdef _WIN32
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stopEvent) {
            backendHealthy = false;
            return;
        }
        try {
            thread = std::thread([this]() { run(); });
        } catch (const std::exception &error) {
            Logger::warning("Completion sound worker unavailable: %s", error.what());
            backendHealthy = false;
        }
#else
        backendHealthy = false;
#endif
    }
    ~Worker()
    {
        // Synchronize the predicate update with ready.wait() to avoid a lost
        // shutdown wakeup between its predicate check and entering the wait.
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping.store(true);
        }
#ifdef _WIN32
        if (stopEvent) SetEvent(stopEvent);
#endif
        ready.notify_one();
        if (thread.joinable()) thread.join();
#ifdef _WIN32
        if (stopEvent) CloseHandle(stopEvent);
#endif
    }
    void submit(const QString &id, int volume)
    {
        if (id.isEmpty() || volume <= 0 || stopping.load()) return;
        std::lock_guard<std::mutex> lock(mutex);
        // No overlapping sounds and no unbounded queue/stale tones after a burst.
        if (busy || !backendHealthy) return;
        selected = id;
        gain = std::clamp(volume, 0, maxVolume);
        busy = pending = true;
        ready.notify_one();
    }
    void run()
    {
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopping.load()) {
            ready.wait(lock, [this]() { return pending || stopping.load(); });
            if (stopping.load()) break;
            const QString id = selected;
            const int volume = gain;
            pending = false;
            lock.unlock();
            bool healthy = true;
            try {
                healthy = play(id, volume);
            } catch (const std::exception &error) {
                Logger::warning("Completion sound failed: %s", error.what());
            }
            lock.lock();
            backendHealthy = backendHealthy && healthy;
            busy = false;
        }
    }
    bool play(const QString &id, int volume)
    {
#ifdef _WIN32
        if (stopping.load()) return true;
        UINT chosen = 0;
        int matches = 0;
        const UINT count = waveOutGetNumDevs();
        for (UINT i = 0; i < count; ++i) {
            if (interfaceId(reinterpret_cast<HWAVEOUT>(static_cast<UINT_PTR>(i))) == id) {
                chosen = i;
                ++matches;
            }
        }
        if (matches != 1) {
            Logger::warning("Completion sound skipped: selected output missing or ambiguous; no default fallback");
            return true;
        }
        std::unique_ptr<Playback, PlaybackDeleter> playback(new Playback);
        // Allocate/generate before opening a device so allocation failure cannot
        // strand a live waveOut handle. Two soft sine notes, ~365 ms total.
        constexpr int rate = 48000;
        constexpr double pi = 3.14159265358979323846;
        const int frames = rate * 365 / 1000;
        playback->samples.resize(static_cast<size_t>(frames) * 2);
        for (int i = 0; i < frames; ++i) {
            const double time = static_cast<double>(i) / rate;
            const bool second = time >= 0.185;
            const double local = second ? time - 0.185 : time;
            const double duration = second ? 0.180 : 0.160;
            double value = 0.0;
            if (local >= 0.0 && local < duration) {
                const double envelope = std::sin(pi * local / duration);
                value = 0.20 * (std::clamp(volume, 0, maxVolume) / 100.0) *
                        envelope * envelope * std::sin(2.0 * pi * (second ? 880.0 : 660.0) * local);
            }
            const auto sample = static_cast<int16_t>(std::lround(value * 32767.0));
            playback->samples[static_cast<size_t>(i) * 2] = sample;
            playback->samples[static_cast<size_t>(i) * 2 + 1] = sample;
        }
        playback->done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!playback->done) return true;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = rate * format.nBlockAlign;
        const MMRESULT opened = waveOutOpen(&playback->device, chosen, &format,
            reinterpret_cast<DWORD_PTR>(playback->done), 0, CALLBACK_EVENT | WAVE_FORMAT_DIRECT);
        if (opened != MMSYSERR_NOERROR) {
            Logger::warning("Completion sound skipped: selected output cannot open (%u); no default fallback", opened);
        } else if (interfaceId(playback->device) != id) {
            // Recheck the opened handle too: hotplug can reorder WinMM indices.
            Logger::warning("Completion sound skipped: output identity changed while opening");
        } else if (!stopping.load()) {
            playback->header.lpData = reinterpret_cast<LPSTR>(playback->samples.data());
            playback->header.dwBufferLength = static_cast<DWORD>(playback->samples.size() * sizeof(int16_t));
            MMRESULT code = waveOutPrepareHeader(playback->device, &playback->header, sizeof(WAVEHDR));
            if (code == MMSYSERR_NOERROR) {
                playback->prepared = true;
                // Opening also signals this event; don't confuse WOM_OPEN with DONE.
                ResetEvent(playback->done);
                code = waveOutWrite(playback->device, &playback->header, sizeof(WAVEHDR));
                if (code == MMSYSERR_NOERROR) {
                    HANDLE events[] = {stopEvent, playback->done};
                    // No audio waits on OBS/Qt's main or clip-writing thread.
                    const ULONGLONG deadline = GetTickCount64() + 2000;
                    while (!(playback->header.dwFlags & WHDR_DONE)) {
                        const ULONGLONG now = GetTickCount64();
                        const DWORD remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
                        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, remaining);
                        if (wait == WAIT_OBJECT_0) break; // Shutdown: reset below.
                        if (wait != WAIT_OBJECT_0 + 1) {
                            Logger::warning("Completion sound: audio wait failed/timed out; resetting output");
                            break;
                        }
                        // Ignore a late WOM_OPEN signal; only WHDR_DONE is completion.
                    }
                }
            }
            if (code != MMSYSERR_NOERROR)
                Logger::warning("Completion sound skipped: audio buffer error (%u)", code);
        }
        if (!playback->release()) {
            // A broken driver may still own the header/event. Never free its memory.
            (void)playback.release();
            return false;
        }
#else
        (void)id;
        (void)volume;
#endif
        return true;
    }
};

SoundNotifier::SoundNotifier() : worker(std::make_unique<Worker>())
{
    const QString path = configPath();
    if (!path.isEmpty()) {
        obs_data_t *data = obs_data_create_from_json_file_safe(path.toUtf8().constData(), "bak");
        if (data) {
            current.enabled = obs_data_get_bool(data, "enabled");
            current.deviceId = QString::fromUtf8(obs_data_get_string(data, "device_id")).toCaseFolded();
            current.volume = obs_data_has_user_value(data, "volume")
                ? static_cast<int>(std::clamp<int64_t>(obs_data_get_int(data, "volume"), 0, maxVolume)) : 20;
            obs_data_release(data);
        }
    }
}
SoundNotifier::~SoundNotifier() = default;

void SoundNotifier::initialize()
{
    Q_ASSERT(QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread());
    std::lock_guard<std::mutex> lock(instanceMutex);
    if (!instance) instance = new SoundNotifier();
}
void SoundNotifier::shutdown()
{
    Q_ASSERT(QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread());
    SoundNotifier *old = nullptr;
    {
        std::lock_guard<std::mutex> lock(instanceMutex);
        old = instance;
        instance = nullptr; // Concurrent producers can no longer post to this object.
    }
    delete old; // QObject removes pending metacalls; worker cancels/reset/joins.
}
void SoundNotifier::notifySuccess()
{
    std::lock_guard<std::mutex> lock(instanceMutex);
    auto *target = instance;
    if (!target) return;
    QMetaObject::invokeMethod(target, [target]() {
        if (target->current.enabled)
            target->enqueue(target->current.deviceId, target->current.volume);
    }, Qt::QueuedConnection);
}
SoundNotifier::Settings SoundNotifier::settings()
{
    return instance ? instance->current : Settings{};
}
QVector<SoundNotifier::Device> SoundNotifier::devices()
{
#ifdef _WIN32
    return enumerateOutputs();
#else
    return {};
#endif
}
bool SoundNotifier::setSettings(Settings value)
{
    if (!instance) return false;
    value.volume = std::clamp(value.volume, 0, maxVolume);
    value.deviceId = value.deviceId.toCaseFolded();
    const QString path = configPath();
    if (path.isEmpty()) return false;
    const auto slash = std::max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    if (slash < 0) return false;
    os_mkdirs(path.left(slash).toUtf8().constData());
    obs_data_t *data = obs_data_create();
    if (!data) return false;
    obs_data_set_bool(data, "enabled", value.enabled);
    obs_data_set_string(data, "device_id", value.deviceId.toUtf8().constData());
    obs_data_set_int(data, "volume", value.volume);
    const bool saved = obs_data_save_json_safe(data, path.toUtf8().constData(), "tmp", "bak");
    obs_data_release(data);
    if (saved) instance->current = value;
    return saved;
}
void SoundNotifier::enqueue(const QString &id, int volume)
{
    worker->submit(id, volume);
}
void SoundNotifier::preview(const QString &id, int volume)
{
    if (instance) instance->enqueue(id, volume); // Explicit test ignores enabled, never persists edits.
}

CompletionSoundSettings::CompletionSoundSettings(QWidget *parent)
    : QGroupBox(QString::fromUtf8(u8"Звук готового клипа"), parent)
{
    const auto values = SoundNotifier::settings();
    enabled = new QCheckBox(QString::fromUtf8(u8"Подавать звук после успешного сохранения и проверки клипа"), this);
    enabled->setChecked(values.enabled);
    output = new QComboBox(this);
    output->setMinimumContentsLength(24);
    output->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    output->setProperty("savedDeviceId", values.deviceId);
    volume = new QSpinBox(this);
    volume->setRange(0, maxVolume);
    volume->setSuffix(QStringLiteral(" %"));
    volume->setValue(values.volume);
    status = new QLabel(this);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    auto *refresh = new QPushButton(QString::fromUtf8(u8"Обновить выходы"), this);
    auto *test = new QPushButton(QString::fromUtf8(u8"Проверить звук"), this);
    connect(refresh, &QPushButton::clicked, this, [this]() { refreshDevices(); });
    connect(test, &QPushButton::clicked, this, [this]() {
        if (output->currentData().toString().isEmpty() || volume->value() == 0) {
            status->setText(QString::fromUtf8(u8"Выберите доступный выход и громкость больше нуля."));
            return;
        }
        SoundNotifier::preview(output->currentData().toString(), volume->value());
        status->setText(QString::fromUtf8(u8"Тест запрошен (даже если флажок выключен). Если звука нет — проверьте выход и журнал OBS. Настройки ещё не сохранены."));
    });
    auto *form = new QFormLayout();
    form->addRow(QString::fromUtf8(u8"Только этот выход:"), output);
    form->addRow(QString::fromUtf8(u8"Громкость (макс. 50 %):"), volume);
    auto *buttons = new QHBoxLayout();
    buttons->addWidget(refresh);
    buttons->addWidget(test);
    buttons->addStretch();
    auto *warning = new QLabel(QString::fromUtf8(u8"По умолчанию звук выключен. Выход по умолчанию Windows не используется: при исчезновении выбранного устройства звук пропускается.\n\n"
        u8"ВАЖНО: выбранный выход нужно отдельно исключить из маршрутизации GoXLR и захвата/мониторинга OBS, включая захват звука приложения OBS. Плагин сам по себе НЕ гарантирует, что сигнал не попадёт в запись или стрим. Проверьте тестовой записью."), this);
    warning->setTextFormat(Qt::PlainText);
    warning->setWordWrap(true);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(enabled);
    layout->addLayout(form);
    layout->addLayout(buttons);
    layout->addWidget(status);
    layout->addWidget(warning);
    refreshDevices();
#ifndef _WIN32
    enabled->setEnabled(false);
    test->setEnabled(false);
    status->setText(QString::fromUtf8(u8"Звук уведомления доступен только в Windows."));
#endif
}
void CompletionSoundSettings::refreshDevices()
{
    const QString selected = output->count() ? output->currentData().toString()
        : output->property("savedDeviceId").toString();
    output->clear();
    output->addItem(QString::fromUtf8(u8"— Не выбран: без звука —"), QString());
    int selectedIndex = 0;
    const auto available = SoundNotifier::devices();
    for (const auto &device : available) {
        // WinMM names can be truncated; show the full stable path as a tooltip.
        output->addItem(device.name, device.id);
        output->setItemData(output->count() - 1, device.id, Qt::ToolTipRole);
        if (device.id == selected) selectedIndex = output->count() - 1;
    }
    if (!selected.isEmpty() && selectedIndex == 0) {
        output->addItem(QString::fromUtf8(u8"[Недоступен] ранее выбранный выход — без звука"), selected);
        selectedIndex = output->count() - 1;
    }
    output->setCurrentIndex(selectedIndex);
    status->setText(QString::fromUtf8(u8"Тест использует текущий выбор. Применить/ОК сохраняет настройки; Отмена не сохраняет."));
}
bool CompletionSoundSettings::apply()
{
    SoundNotifier::Settings values;
    values.enabled = enabled->isChecked();
    values.deviceId = output->currentData().toString();
    values.volume = volume->value();
    if (!SoundNotifier::setSettings(values)) {
        status->setText(QString::fromUtf8(u8"Не удалось сохранить completion-sound.json. Проверьте права на папку настроек плагина."));
        return false;
    }
    return true;
}
} // namespace ReplayBufferPro
