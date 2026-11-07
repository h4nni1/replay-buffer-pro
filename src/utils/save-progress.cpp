#include "utils/save-progress.hpp"
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <mutex>
#include <chrono>
namespace ReplayBufferPro {
namespace {
struct State {
 QString message, notice;
 int seconds = 0;
 uint64_t done = 0, total = 0, jobs = 0;
 int64_t bytes = 0;
 bool active = false, memory = false, verify = false, success = false, finished = false;
 std::chrono::steady_clock::time_point start;
};
std::mutex stateMutex;
State state;
}
void SaveProgress::begin(const QString &partialPath, int seconds, bool memory) {
 (void)partialPath;
 std::lock_guard<std::mutex> lock(stateMutex);
 const auto jobs = state.jobs;
 state = State{}; state.jobs = jobs; state.seconds = seconds;
 state.memory = memory; state.active = true; state.start = std::chrono::steady_clock::now();
}
void SaveProgress::packets(uint64_t done, uint64_t total, int64_t bytes) {
 std::lock_guard<std::mutex> lock(stateMutex);
 state.done = done; state.total = total;
 if (bytes >= 0) state.bytes = bytes;
}
void SaveProgress::containerBytes(int64_t bytes) {
 if (bytes < 0) return;
 std::lock_guard<std::mutex> lock(stateMutex); state.bytes = bytes;
}
void SaveProgress::verifying() {
 std::lock_guard<std::mutex> lock(stateMutex); state.verify = true;
}
void SaveProgress::finish(bool success, const QString &message) {
 std::lock_guard<std::mutex> lock(stateMutex);
 state.active = false; state.success = success; state.finished = true; state.message = message;
}
void SaveProgress::notice(const QString &message) {
 std::lock_guard<std::mutex> lock(stateMutex); state.notice = message;
}
bool SaveProgress::isBusy() {
 std::lock_guard<std::mutex> lock(stateMutex); return state.active || state.jobs > 0;
}
void SaveProgress::setJobs(uint64_t jobs) {
 std::lock_guard<std::mutex> lock(stateMutex); state.jobs = jobs;
}
QWidget *SaveProgress::createWidget(QWidget *parent, bool compact) {
 auto *box = new QWidget(parent);
 auto *layout = new QVBoxLayout(box); layout->setContentsMargins(6,6,6,6);
 auto *row = new QHBoxLayout();
 auto *badge = new QLabel(QString::fromUtf8("Ожидание"), box);
 badge->setTextFormat(Qt::PlainText); badge->setObjectName("hanniClipResult");
 badge->setWordWrap(true);
 row->addWidget(badge); row->addStretch(); layout->addLayout(row);
 auto *label = new QLabel(QString::fromUtf8("Сохранение клипов: готов к работе"), box);
 label->setWordWrap(true); label->setTextFormat(Qt::PlainText); label->setVisible(!compact);
 auto *note = new QLabel(box); note->setWordWrap(true); note->setTextFormat(Qt::PlainText); note->hide();
 auto *bar = new QProgressBar(box); bar->setRange(0,100); bar->setValue(0);
 bar->setFormat(QString::fromUtf8("Обработано пакетов: %p%")); bar->hide();
 layout->addWidget(label); layout->addWidget(bar); layout->addWidget(note);
 auto *timer = new QTimer(box); timer->setInterval(250);
 QObject::connect(timer,&QTimer::timeout,box,[box,label,bar,badge,note,compact]() {
  if (!box->isVisible()) return;
  State s; { std::lock_guard<std::mutex> lock(stateMutex); s = state; }
  QString status, style;
  if (s.active) {
   status = s.verify ? QString::fromUtf8("Проверка файла") : QString::fromUtf8("Сохранение…");
   style = QStringLiteral("QLabel#hanniClipResult { background:#493B16; color:#FFE6A1; border:1px solid #A38A45; border-radius:5px; padding:4px 8px; }");
  } else if (s.finished) {
   status = s.success ? QString::fromUtf8("✓ Клип сохранён") : QString::fromUtf8("Ошибка сохранения");
   style = s.success ? QStringLiteral("QLabel#hanniClipResult { background:#173C29; color:#B9F6CF; border:1px solid #4EAA72; border-radius:5px; padding:4px 8px; }") :
      QStringLiteral("QLabel#hanniClipResult { background:#481E28; color:#FFD4DF; border:1px solid #D0748D; border-radius:5px; padding:4px 8px; }");
  } else {
   status = QString::fromUtf8("Ожидание");
   style = QStringLiteral("QLabel#hanniClipResult { background:#252C33; color:#D5DDE5; border:1px solid #64717D; border-radius:5px; padding:4px 8px; }");
  }
  if (badge->text()!=status) badge->setText(status);
  if (badge->styleSheet()!=style) badge->setStyleSheet(style);
  if (note->text()!=s.notice) note->setText(s.notice);
  note->setVisible(!s.notice.isEmpty());
  if (!s.active) {
   bar->hide();
   label->setVisible(!compact || (s.finished && !s.success));
   if (!s.message.isEmpty() && label->text()!=s.message) label->setText(s.message);
   return;
  }
  label->show();
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-s.start).count();
  const double mb = static_cast<double>(s.bytes) / (1024.0*1024.0);
  const QString stage = s.verify ? QString::fromUtf8("Проверка и завершение файла") :
      (s.memory ? QString::fromUtf8("Запись клипа из памяти") : QString::fromUtf8("Обрезка сохранённого буфера"));
  const QString value = QString::fromUtf8("%1 · %2 с клипа\nВ контейнер передано: %3 МиБ · прошло %4 с")
      .arg(stage).arg(s.seconds).arg(mb,0,'f',1).arg(static_cast<qlonglong>(elapsed));
  if (label->text()!=value) label->setText(value);
  bar->setVisible(s.memory && !s.verify);
  if (s.total) { bar->setRange(0,100); bar->setValue(static_cast<int>((100.0*s.done)/s.total)); }
  else { bar->setRange(0,0); }
 });
 timer->start(); return box;
}
}
