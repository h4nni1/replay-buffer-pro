/**
 * @file plugin.cpp
 * @brief Implementation of the Replay Buffer Pro plugin
 *
 * Provides enhanced replay buffer controls for OBS Studio including:
 * - Configurable buffer length adjustment
 * - Segment-based replay saving
 * - Automatic replay trimming
 */

// OBS includes
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>

// Qt includes
#include <QAction>
#include <QMainWindow>
#include <QMessageBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <initializer_list>
#include <exception>
#include <QSpinBox>

// STL includes
#include <vector>

// Local includes
#include "utils/obs-utils.hpp"
#include "plugin/plugin.hpp"
#include "plugin/websocket-command.hpp"
#include "config/config.hpp"
#include "utils/logger.hpp"
#include "utils/save-progress.hpp"
#include "ui/recording-folder.hpp"

namespace ReplayBufferPro
{
	//=============================================================================
	// CONSTRUCTORS & DESTRUCTOR
	//=============================================================================

	Plugin::Plugin(QWidget *parent)
			: QWidget(parent), 
			  lastKnownBufferLength(0)
	{
		// Create component instances
		replayManager = new ReplayBufferManager(this);
		settingsManager = new SettingsManager();
		pluginProperties = new PluginProperties();
		pluginProperties->load();
		replayManager->setInMemoryClips(pluginProperties->getInMemoryClips());
		
		// Create UI components with callbacks
		ui = new UIComponents(this, 
			[this](int duration) { handleSaveSegment(duration); },
			[this]() { handleSaveFullBuffer(); },
			[this]() { openProperties(PropertiesDialog::Page::SaveButtons); }
		);
		ui->setSaveButtons(pluginProperties->getButtons());
		
		// Mount the UI into this widget
		{
			QWidget *root = ui->createUI();
			auto *layout = new QVBoxLayout(this);
			layout->setContentsMargins(0, 0, 0, 0);
			layout->setSpacing(0);
			layout->addWidget(root);
            layout->addWidget(SaveProgress::createWidget(this));
            layout->addWidget(RecordingFolder::createWidget(this));
			setLayout(layout);
		}
		
		// Initialize signals and load settings
		initSignals();
		loadBufferLength();

		// Register OBS event callback
		obs_frontend_add_event_callback(handleOBSEvent, this);

		// Create and register hotkeys
		hotkeyManager = new HotkeyManager(
			this,
			[this](int duration) { handleSaveSegment(duration); },
			pluginProperties->getButtons()
		);
		hotkeyManager->registerHotkeys();

		// Tools > Replay Buffer Pro Properties
		auto *propertiesAction = static_cast<QAction *>(
			obs_frontend_add_tools_menu_qaction(obs_module_text("PropertiesMenuItem")));
		connect(propertiesAction, &QAction::triggered, this,
				[this]() { openProperties(PropertiesDialog::Page::SaveButtons); });

		// Setup settings monitoring
		settingsMonitorTimer = new QTimer(this);
		settingsMonitorTimer->setInterval(Config::SETTINGS_MONITOR_INTERVAL);
		connect(settingsMonitorTimer, &QTimer::timeout, this, &Plugin::loadBufferLength);
		settingsMonitorTimer->start();

		// This constructor runs from obs_module_post_load, after all modules load.
		registerSaveClipCommand(replayManager);
	}

	// Removed QMainWindow-based constructor; OBS wraps QWidget into a dock

	Plugin::~Plugin()
	{
		unregisterSaveClipCommand();
		// Stop the settings monitor timer first
		if (settingsMonitorTimer) {
			settingsMonitorTimer->stop();
		}
		
		// Stop saved-signal delivery before anything else goes away (no-op if
		// OBS_FRONTEND_EVENT_EXIT already did it)
		replayManager->shutdown();

		// Remove OBS callbacks before destroying components
		obs_frontend_remove_event_callback(handleOBSEvent, this);
		
		// Clean up managers that were allocated with new
		delete hotkeyManager;
		delete pluginProperties;
		delete settingsManager;
		
		// Qt parent-child relationship will handle cleanup for other components
	}

	//=============================================================================
	// INITIALIZATION
	//=============================================================================

	void Plugin::initSignals()
	{
		connect(ui->getSecondsEdit(), QOverload<int>::of(&QSpinBox::valueChanged),
				this, &Plugin::handleBufferLengthChanged);

		connect(ui->getBufferLengthDebounceTimer(), &QTimer::timeout, this, &Plugin::handleBufferLengthFinished);
	}

	//=============================================================================
	// EVENT HANDLERS
	//=============================================================================

	void Plugin::handleOBSEvent(enum obs_frontend_event event, void *ptr)
	{
		auto plugin = static_cast<Plugin *>(ptr);

		switch (event)
		{
		case OBS_FRONTEND_EVENT_EXIT:
			unregisterSaveClipCommand();
			// Disconnect from the replay buffer output while the frontend API is
			// still usable; OBS tears its callbacks down right after this event.
			if (plugin->replayManager) {
				plugin->replayManager->shutdown();
			}
			if (plugin->settingsMonitorTimer) {
				plugin->settingsMonitorTimer->stop();
			}
			if (plugin->hotkeyManager) {
				plugin->hotkeyManager->saveHotkeySettings();
			}
			break;
		case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTING:
			plugin->settingsMonitorTimer->stop();
			QMetaObject::invokeMethod(plugin, "updateBufferLengthUIState", Qt::QueuedConnection);
			break;
		case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
			plugin->settingsMonitorTimer->start();
			QMetaObject::invokeMethod(plugin, "updateBufferLengthUIState", Qt::QueuedConnection);
			QMetaObject::invokeMethod(plugin, "loadBufferLength", Qt::QueuedConnection);
			plugin->replayManager->handleFrontendEvent(event);
			break;
		case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
			plugin->replayManager->handleFrontendEvent(event);
			break;
		// Saves are not handled here. OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED is
		// suppressed when the buffer stopped mid-write or during scene
		// collection and profile switches; the manager listens to the replay
		// buffer output's own "saved" signal instead. Handling both would trim
		// every file twice.
		default:
			break;
		}
	}

	void Plugin::handleBufferLengthChanged(int value)
	{
		// Only update the UI components, don't trigger settings update yet
		ui->updateBufferLengthValue(value);

		// Restart the debounce timer
		ui->getBufferLengthDebounceTimer()->start();
	}

	void Plugin::handleBufferLengthFinished()
	{
		// Get the final spinbox value
		int value = ui->getSecondsEdit()->value();

		// Only update settings if the value has actually changed
		if (value != lastKnownBufferLength)
		{
			try {
				settingsManager->updateBufferLengthSettings(value);
				lastKnownBufferLength = value;
			} catch (const std::exception &e) {
				QMessageBox::warning(this, obs_module_text("Error"),
									QString(obs_module_text("FailedToUpdateLength")).arg(e.what()));
			}
		}
	}

	void Plugin::handleSaveFullBuffer()
	{
		replayManager->saveFullBuffer(this);
	}

    QWidget *Plugin::createCompactPanel(QWidget *parent)
    {
        auto *panel = new QWidget(parent);
        panel->setObjectName("hanniReplayCompactPanel");
        auto *layout = new QVBoxLayout(panel);
        layout->setContentsMargins(0, 4, 0, 4);
        layout->setSpacing(3);

        // Keep the row in its own host so applying settings can replace its
        // contents without disturbing the progress or recording-folder widgets.
        auto *buttonHost = new QWidget(panel);
        buttonHost->setObjectName("hanniReplayCompactButtonHost");
        auto *row = new QHBoxLayout(buttonHost);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(3);
        layout->addWidget(buttonHost);
        layout->addWidget(SaveProgress::createWidget(panel, true));
        layout->addWidget(RecordingFolder::createWidget(panel, true));
        auto *properties = new QPushButton(QString::fromUtf8("Настройки клипов и звука…"), panel);
        connect(properties, &QPushButton::clicked, this,
                [this]() { openProperties(PropertiesDialog::Page::Experimental); });
        layout->addWidget(properties);

        compactPanel = panel;
        compactButtonHost = buttonHost;

        // The timer belongs to the panel, while the connection context belongs
        // to the plugin.  Thus deleting either side cannot leave a callback
        // using a stale plugin or a stale button row.
        auto *timer = new QTimer(panel);
        timer->setInterval(500);
        connect(timer, &QTimer::timeout, this, [this]() { updateCompactButtonStates(); });
        timer->start();

        rebuildCompactButtons();
        return panel;
    }

    void Plugin::rebuildCompactButtons()
    {
        if (!compactButtonHost || !pluginProperties)
            return;

        auto *row = qobject_cast<QHBoxLayout *>(compactButtonHost->layout());
        if (!row)
            return;

        // This runs on the Qt GUI thread from PropertiesDialog::applied().
        // Remove the old buttons and their signal connections before adding the
        // new captured durations, so no button can retain an old setting.
        while (QLayoutItem *item = row->takeAt(0)) {
            if (QWidget *widget = item->widget())
                delete widget;
            delete item;
        }

        for (const SaveButton &saveButton : pluginProperties->getButtons()) {
            const int duration = saveButton.seconds;
            const QString durationText = duration % 60 == 0
                ? QString::fromUtf8("%1 мин").arg(duration / 60)
                : QString::fromUtf8("%1 с").arg(duration);
            const QString buttonText = saveButton.label.empty()
                ? durationText
                : QString::fromStdString(saveButton.label);
            auto *button = new QPushButton(buttonText, compactButtonHost);
            button->setProperty("hanniDuration", duration);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            button->setToolTip(QString::fromUtf8("Сохранить последние %1 из доступной истории буфера")
                                   .arg(durationText));
            connect(button, &QPushButton::clicked, this,
                    [this, duration]() { handleSaveSegment(duration); });
            row->addWidget(button);
        }

        updateCompactButtonStates();
    }

    void Plugin::updateCompactButtonStates()
    {
        if (!compactButtonHost || !settingsManager || !replayManager)
            return;

        auto *row = qobject_cast<QHBoxLayout *>(compactButtonHost->layout());
        if (!row)
            return;

        const bool active = obs_frontend_replay_buffer_active();
        int length = 0;
        try {
            length = settingsManager->getCurrentBufferLength();
        }
        catch (const std::exception &) {
            for (int i = 0; i < row->count(); ++i) {
                if (auto *button = qobject_cast<QPushButton *>(row->itemAt(i)->widget()))
                    button->setEnabled(false);
            }
            return;
        }

        const bool memory = replayManager->memoryClipsEnabled();
        const double available = replayManager->availableSeconds();
        for (int i = 0; i < row->count(); ++i) {
            auto *button = qobject_cast<QPushButton *>(row->itemAt(i)->widget());
            if (!button)
                continue;

            const int duration = button->property("hanniDuration").toInt();
            button->setEnabled(active && memory && length >= duration && available >= duration);
            button->setToolTip(!memory
                ? QString::fromUtf8("Включите сохранение клипов из памяти в настройках плагина")
                : QString::fromUtf8("Сохранить последние %1 с. Накоплено истории: %2 с. После запуска дождитесь заполнения.")
                    .arg(duration).arg(static_cast<int>(available)));
        }
    }

	void Plugin::handleSaveSegment(int duration)
	{
		replayManager->saveSegment(duration, this);
	}

	void Plugin::openProperties(PropertiesDialog::Page page)
	{
		// Every slot, including buttons not in use, then OBS's Save Replay hotkey
		std::vector<obs_key_combination_t> hotkeys;
		for (const auto &bindings : hotkeyManager->getAllBindings())
		{
			// One binding per hotkey here; more can be added in OBS Settings > Hotkeys
			hotkeys.push_back(bindings.empty() ? obs_key_combination_t{0, OBS_KEY_NONE} : bindings.front());
		}

		auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		PropertiesDialog dialog(pluginProperties->getButtons(), hotkeys,
							  pluginProperties->getInMemoryClips(), page,
							  mainWindow ? static_cast<QWidget *>(mainWindow) : this);
		dialog.restoreSize(pluginProperties->getWindowWidth(), pluginProperties->getWindowHeight());
		connect(&dialog, &PropertiesDialog::applied, this, [this, &dialog]() { applyProperties(dialog); });
		dialog.exec();

		// Remember a size the user dragged it to, however the window was closed;
		// unapplied edits never reach pluginProperties, so saving here keeps them out
		QSize size = dialog.sizeToRemember();
		if (size.width() != pluginProperties->getWindowWidth() || size.height() != pluginProperties->getWindowHeight())
		{
			pluginProperties->setWindowSize(size.width(), size.height());
			if (!pluginProperties->save())
			{
				Logger::warning("Failed to save the properties window size");
			}
		}
	}

	// The dialog's hotkey indexes are passed to the hotkey manager as they are
	static_assert(PropertiesDialog::FULL_BUFFER_HOTKEY == HotkeyManager::FULL_BUFFER);

	void Plugin::applyProperties(const PropertiesDialog &dialog)
	{
		pluginProperties->setButtons(dialog.buttons());
		pluginProperties->setInMemoryClips(dialog.inMemoryClips());
		replayManager->setInMemoryClips(dialog.inMemoryClips());
		if (!pluginProperties->save())
		{
			Logger::warning("Failed to save properties");
		}

		ui->setSaveButtons(pluginProperties->getButtons());
		rebuildCompactButtons();
		// Registers hotkeys for added buttons before their bindings are set below
		hotkeyManager->setButtons(pluginProperties->getButtons());

		auto changedHotkeys = dialog.changedHotkeys();
		for (size_t index : changedHotkeys)
		{
			obs_key_combination_t combination = dialog.hotkey(index);
			hotkeyManager->setBinding(index, obs_key_combination_is_empty(combination) ? nullptr : &combination);
		}
		if (!changedHotkeys.empty())
		{
			hotkeyManager->saveHotkeySettings();
		}
	}

	//=============================================================================
	// UI STATE MANAGEMENT
	//=============================================================================

	void Plugin::updateBufferLengthUIState()
	{
		bool isActive = obs_frontend_replay_buffer_active();
		ui->updateBufferLengthState(isActive);
	}

	//=============================================================================
	// SETTINGS MANAGEMENT
	//=============================================================================

	void Plugin::loadBufferLength()
	{
		int bufferLength = settingsManager->getCurrentBufferLength();
		if (bufferLength > 0 && bufferLength != lastKnownBufferLength)
		{
			lastKnownBufferLength = bufferLength;
			ui->updateBufferLengthValue(bufferLength);
		}
	}

} // namespace ReplayBufferPro
