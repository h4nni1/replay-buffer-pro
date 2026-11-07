#include "utils/save-progress.hpp"
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QTimer>
#include <QFileInfo>
#include <mutex>
#include <chrono>
namespace ReplayBufferPro {
namespace {
struct State {
 QString path, message;
 int seconds = 0;
 uint64_t done = 0, total = 0;
 bool active = false, memory = false, verify = false, success = false;
 std::chrono::steady_clock::time_point start;
};
std::mutex stateMutex;
State state;
}
void SaveProgress::begin(const QString &path, int seconds, bool memory) {
 std::lock_guard<std::mutex> lock(stateMutex);
 state = State{}; state.path = path; state.seconds = seconds;
 state.memory = memory; state.active = true; state.start = std::chrono::steady_clock::now();
}
void SaveProgress::packets(uint64_t done, uint64_t total) {
 std::lock_guard<std::mutex> lock(stateMutex); state.done = done; state.total = total;
}
void SaveProgress::verifying() {
 std::lock_guard<std::mutex> lock(stateMutex); state.verify = true;
}
void SaveProgress::finish(bool success, const QString &message) {
 std::lock_guard<std::mutex> lock(stateMutex);
 state.active = false; state.success = success; state.message = message;
}
QWidget *SaveProgress::createWidget(QWidget *parent) {
 auto *box = new QWidget(parent);
 auto *layout = new QVBoxLayout(box); layout->setContentsMargins(6,6,6,6);
 auto *label = new QLabel(QString::fromUtf8("Сохранение клипов: готов к работе"), box);
 label->setWordWrap(true); label->setTextFormat(Qt::PlainText);
 auto *bar = new QProgressBar(box); bar->setRange(0,100); bar->setValue(0);
 bar->setFormat(QString::fromUtf8("Запись пакетов: %p%")); bar->hide();
 layout->addWidget(label); layout->addWidget(bar);
 auto *timer = new QTimer(box); timer->setInterval(200);
 QObject::connect(timer,&QTimer::timeout,box,[label,bar]() {
  State s; { std::lock_guard<std::mutex> lock(stateMutex); s = state; }
  if (!s.active) {
   bar->hide();
   if (!s.message.isEmpty()) label->setText(s.message);
   return;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-s.start).count();
  const double mb = static_cast<double>(QFileInfo(s.path).size()) / (1024.0*1024.0);
  const QString stage = s.verify ? QString::fromUtf8("Проверка и завершение файла") :
      (s.memory ? QString::fromUtf8("Запись клипа из памяти") : QString::fromUtf8("Обрезка сохранённого буфера"));
  label->setText(QString::fromUtf8("%1 · %2 с клипа\nФайл: %3 МиБ · прошло %4 с")
      .arg(stage).arg(s.seconds).arg(mb,0,'f',1).arg(static_cast<qlonglong>(elapsed)));
  bar->setVisible(s.memory && !s.verify);
  if (s.total) { bar->setRange(0,100); bar->setValue(static_cast<int>((100.0*s.done)/s.total)); }
  else { bar->setRange(0,0); }
 });
 timer->start(); return box;
}
}
