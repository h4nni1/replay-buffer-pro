#include "recording-folder.hpp"
#include "utils/save-progress.hpp"

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QByteArray>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSizePolicy>
#include <QTemporaryFile>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace ReplayBufferPro
{
namespace
{
QString text(const char *value) { return QString::fromUtf8(value ? value : ""); }
bool outputsActive()
{
  return SaveProgress::isBusy() || obs_frontend_recording_active() || obs_frontend_replay_buffer_active() ||
         obs_frontend_streaming_active();
}

class FolderWidget final : public QWidget
{
public:
  explicit FolderWidget(QWidget *parent, bool compact) : QWidget(parent), compactView(compact)
  {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *title = new QLabel(text("Общая папка записи OBS (текущий профиль)"), this);
    title->setTextFormat(Qt::PlainText);
    layout->addWidget(title);
    title->setVisible(!compactView);
    path = new QLabel(this);
    path->setTextFormat(Qt::PlainText);
    path->setWordWrap(true);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    path->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(path);
    path->setVisible(!compactView);
    browse = new QPushButton(text("Папка сохранения…"), this);
    layout->addWidget(browse);
    status = new QLabel(this);
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    status->setVisible(!compactView);
    QObject::connect(browse, &QPushButton::clicked, this, [this] { choose(); });
    obs_frontend_add_event_callback(onEvent, this);
    auto *timer = new QTimer(this);
    QObject::connect(timer, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); });
    timer->start(500);
    refresh();
  }

  ~FolderWidget() override { obs_frontend_remove_event_callback(onEvent, this); }

private:
  bool compactView = false;
  QPushButton *browse = nullptr;
  QLabel *path = nullptr;
  QLabel *status = nullptr;
  std::atomic<uint64_t> generation{0};
  std::atomic<bool> switching{false};
  uint64_t shownGeneration = 0;
  config_t *shownConfig = nullptr;

  static void onEvent(enum obs_frontend_event event, void *data)
  {
    auto *self = static_cast<FolderWidget *>(data);
    if (event == OBS_FRONTEND_EVENT_PROFILE_CHANGING ||
        event == OBS_FRONTEND_EVENT_PROFILE_CHANGED) {
      self->switching.store(event == OBS_FRONTEND_EVENT_PROFILE_CHANGING);
      ++self->generation; // Detect even switching away and back inside the native dialog.
    }
  }

  static QString currentPath(config_t *config)
  {
    if (!config)
      return {};
    const char *mode = config_get_string(config, "Output", "Mode");
    if (!mode || std::strcmp(mode, "Advanced") != 0)
      return text(config_get_string(config, "SimpleOutput", "FilePath"));
    const char *type = config_get_string(config, "AdvOut", "RecType");
    const bool ffmpeg = type && std::strcmp(type, "FFmpeg") == 0;
    return text(config_get_string(config, "AdvOut", ffmpeg ? "FFFilePath" : "RecFilePath"));
  }

  void refresh()
  {
    config_t *config = obs_frontend_get_profile_config();
    const auto currentGeneration = generation.load();
    if (shownConfig != config || shownGeneration != currentGeneration) {
      status->clear();
      shownConfig = config;
      shownGeneration = currentGeneration;
    }
    const QString value = switching.load() ? QString() : currentPath(config);
    const QString shown = value.isEmpty() ? text("Папка не задана или профиль недоступен")
                                          : QDir::toNativeSeparators(value);
    if (path->text() != shown) path->setText(shown);
    const QString tip = text("Текущая общая папка записи OBS: ") + shown;
    if (browse->toolTip() != tip) browse->setToolTip(tip);
  }

  void warn(const QString &message)
  {
    QMessageBox::warning(this, text("Папка записи OBS"), message);
  }

  bool canApply(config_t *expected, uint64_t expectedGeneration)
  {
    if (outputsActive()) {
      warn(text("Сначала остановите запись, буфер повтора и трансляцию, затем дождитесь сохранения клипов. Папка не изменена."));
      return false;
    }
    if (!expected || switching.load() || generation.load() != expectedGeneration ||
        obs_frontend_get_profile_config() != expected) {
      warn(text("Профиль OBS изменился или недоступен. Папка не изменена. Выберите её заново."));
      return false;
    }
    return true;
  }

  void choose()
  {
    config_t *config = obs_frontend_get_profile_config();
    const auto originalGeneration = generation.load();
    if (!canApply(config, originalGeneration))
      return;
    // The native dialog pumps events: outputs/profile/widget lifetime can change.
    const QPointer<FolderWidget> alive(this);
    const QString selected = QFileDialog::getExistingDirectory(
      this, text("Общая папка записи и буфера повтора OBS"), currentPath(config),
      QFileDialog::ShowDirsOnly);
    if (!alive || selected.isEmpty())
      return;
    if (!canApply(config, originalGeneration))
      return;
    const QFileInfo info(selected);
    if (!info.isAbsolute() || !info.exists() || !info.isDir()) {
      warn(text("Выберите существующую папку. Новые папки автоматически не создаются."));
      return;
    }
    const QString destination = QDir::cleanPath(info.absoluteFilePath());
    QTemporaryFile probe(QDir(destination).filePath(QStringLiteral(".replay-buffer-pro-XXXXXX")));
    // Use Qt's Unicode file APIs; never trust permissions flags alone (ACLs/network shares).
    if (!probe.open()) {
      warn(text("Нет доступа для записи в выбранную папку: ") + probe.errorString());
      return;
    }
    probe.close();
    if (!probe.remove()) {
      warn(text("Не удалось удалить проверочный файл. Папка не изменена: ") + probe.fileName());
      return;
    }
    // Final check immediately before the synchronous transaction; no event loop in between.
    if (!canApply(config, originalGeneration))
      return;

    struct Previous {
      const char *section;
      const char *key;
      bool user = false;
      std::string value;
    } previous[] = {{"SimpleOutput", "FilePath"}, {"AdvOut", "RecFilePath"},
                    {"AdvOut", "FFFilePath"}};
    for (auto &old : previous) {
      old.user = config_has_user_value(config, old.section, old.key);
      const char *value = config_get_string(config, old.section, old.key);
      old.value = value ? value : "";
    }
    const QByteArray utf8 = QDir::fromNativeSeparators(destination).toUtf8();
    for (const auto &old : previous)
      config_set_string(config, old.section, old.key, utf8.constData());
    if (config_save_safe(config, "tmp", "bak") != CONFIG_SUCCESS) {
      // Restore explicit empty strings as well as absence of user overrides.
      for (const auto &old : previous) {
        if (old.user)
          config_set_string(config, old.section, old.key, old.value.c_str());
        else
          config_remove_value(config, old.section, old.key);
      }
      const bool restored = config_save_safe(config, "tmp", "bak") == CONFIG_SUCCESS;
      warn(restored ? text("Не удалось сохранить папку. Прежние настройки восстановлены.")
                    : text("Ошибка сохранения. Прежние значения восстановлены в памяти, но запись "
                           "отката на диск не удалась. Проверьте права и свободное место; "
                           "настройки профиля на диске не гарантированы."));
      return;
    }
    refresh(); // Do not show the new path/success until config_save_safe succeeds.
    status->setText(text("Общая папка записи OBS сохранена. Запустите буфер повтора заново, "
                         "чтобы применить путь. Режим и кодирование не изменены."));
  }
};
} // namespace

QWidget *RecordingFolder::createWidget(QWidget *parent, bool compact)
{
  return new FolderWidget(parent, compact);
}
} // namespace ReplayBufferPro
