// Completion sound, independent of OBS's audio mix and Qt Multimedia.
#pragma once

#include <QGroupBox>
#include <QObject>
#include <QString>
#include <QVector>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;

namespace ReplayBufferPro {

class SoundNotifier final : public QObject {
public:
    struct Settings {
        bool enabled = false;
        QString deviceId; // Windows device interface path, never a transient device index
        int volume = 20;  // Digital gain percent; deliberately capped at 50
    };
    struct Device { QString id; QString name; };

    // Call initialize/shutdown on Qt's main thread at module post-load/unload.
    static void initialize();
    static void shutdown();
    // Safe from the trim/clip worker. Queue ONLY after verification AND final rename.
    // Does nothing before initialize, after shutdown, when disabled or output missing.
    static void notifySuccess();

    // The following settings/UI APIs are main-thread-only.
    static Settings settings();
    static QVector<Device> devices();
    static bool setSettings(Settings settings);
    static void preview(const QString &deviceId, int volume);

private:
    SoundNotifier();
    ~SoundNotifier() override;
    void enqueue(const QString &deviceId, int volume);
    struct Worker;
    std::unique_ptr<Worker> worker;
    Settings current;
};

// Standalone persistence; no changes to PluginProperties or locale files.
class CompletionSoundSettings final : public QGroupBox {
public:
    explicit CompletionSoundSettings(QWidget *parent = nullptr);
    bool apply(); // Save only on the dialog's Apply/OK; Cancel leaves config alone.
private:
    void refreshDevices();
    QCheckBox *enabled;
    QComboBox *output;
    QSpinBox *volume;
    QLabel *status;
};

} // namespace ReplayBufferPro
