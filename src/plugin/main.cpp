// OBS lifecycle and native-controls integration for the Hanni build.
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QDockWidget>
#include <QFrame>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include "plugin/plugin.hpp"
#include "utils/logger.hpp"
#include "utils/sound-notifier.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("replay-buffer-pro", "en-US")
namespace {
QPointer<ReplayBufferPro::Plugin> pluginInstance;
QPointer<QWidget> compactPanel;
bool exiting = false;

void hideOriginalDock()
{
    if (!pluginInstance) return;
    for (QWidget *p = pluginInstance->parentWidget(); p; p = p->parentWidget()) {
        if (auto *dock = qobject_cast<QDockWidget *>(p)) { dock->hide(); return; }
    }
}
void tryEmbed()
{
    if (exiting || !pluginInstance) return;
    if (compactPanel) { hideOriginalDock(); return; }
    auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (!main) return;
    auto *dock = main->findChild<QDockWidget *>(QStringLiteral("controlsDock"));
    auto *frame = dock ? dock->findChild<QFrame *>(QStringLiteral("controlsFrame")) : nullptr;
    auto *layout = frame ? qobject_cast<QVBoxLayout *>(frame->layout()) : nullptr;
    auto *replay = frame ? frame->findChild<QPushButton *>(QStringLiteral("replayBufferButton")) : nullptr;
    if (!frame || !layout || !replay) {
        ReplayBufferPro::Logger::warning("Hanni native controls unavailable; original replay dock retained");
        return;
    }
    int insertAt = -1;
    for (int i = 0; i < layout->count(); ++i) {
        auto *child = layout->itemAt(i)->layout();
        if (child && child->objectName() == QStringLiteral("replayBufferLayout")) {
            insertAt = i + 1;
            break;
        }
    }
    if (insertAt < 0) {
        ReplayBufferPro::Logger::warning("Hanni replay layout not found; original replay dock retained");
        return;
    }
    compactPanel = pluginInstance->createCompactPanel(frame);
    layout->insertWidget(insertAt, compactPanel.data());
    const QPointer<QWidget> lifetimeView = compactPanel;
    QObject::connect(pluginInstance.data(), &QObject::destroyed, compactPanel.data(), [lifetimeView]() {
        delete lifetimeView.data();
    });
    auto *timer = new QTimer(compactPanel.data());
    const QPointer<QWidget> view = compactPanel;
    const QPointer<QPushButton> button = replay;
    const auto sync = [view, button]() {
        if (view) view->setVisible(button && !button->isHidden());
    };
    QObject::connect(timer, &QTimer::timeout, compactPanel.data(), sync);
    timer->start(500);
    sync();
    hideOriginalDock();
    ReplayBufferPro::Logger::info("Hanni compact 1/5/10 minute controls embedded below replayBufferLayout");
}
void frontendEvent(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_EXIT) { exiting = true; return; }
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING && pluginInstance && !exiting)
        QTimer::singleShot(0, pluginInstance.data(), [] { tryEmbed(); });
}
}
void obs_module_post_load(void)
{
    exiting = false;
    ReplayBufferPro::SoundNotifier::initialize();
    pluginInstance = new ReplayBufferPro::Plugin();
    obs_frontend_add_dock_by_id("replay-buffer-pro", "Replay Buffer Pro", pluginInstance.data());
    obs_frontend_add_event_callback(frontendEvent, nullptr);
    // Context-bound timer is cancelled if OBS destroys the plugin widget.
    QTimer::singleShot(0, pluginInstance.data(), [] { tryEmbed(); });
}
bool obs_module_load(void)
{
    ReplayBufferPro::Logger::info("Plugin loaded: Hanni revision 3 (native 1/5/10 controls, shared recording folder)");
    return true;
}
void obs_module_unload(void)
{
    exiting = true;
    obs_frontend_remove_event_callback(frontendEvent, nullptr);
    // The compact view is outside the original dock's QObject ownership tree.
    // Remove its callbacks/timers while this module's code is still present.
    delete compactPanel.data();
    compactPanel.clear();
    ReplayBufferPro::SoundNotifier::shutdown();
    // OBS retains ownership of the original dock and its Plugin object.
    pluginInstance.clear();
    ReplayBufferPro::Logger::info("Plugin unloaded");
}
