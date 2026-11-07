#pragma once
#include <QWidget>
#include <QString>
#include <cstdint>
namespace ReplayBufferPro {
// Thread-safe state; a GUI-owned timer samples it at 5 Hz.
class SaveProgress {
public:
 static QWidget *createWidget(QWidget *parent);
 static void begin(const QString &partialPath, int seconds, bool memory);
 static void packets(uint64_t done, uint64_t total);
 static void verifying();
 static void finish(bool success, const QString &message);
};
}
