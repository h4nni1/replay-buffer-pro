#pragma once
#include <QWidget>
#include <QString>
#include <cstdint>
namespace ReplayBufferPro {
// Only the background writer updates byte positions. GUI never stats the HDD.
class SaveProgress {
public:
 static QWidget *createWidget(QWidget *parent, bool compact = false);
 static void begin(const QString &partialPath, int seconds, bool memory);
 static void packets(uint64_t done, uint64_t total, int64_t containerBytes);
 static void containerBytes(int64_t bytes);
 static void verifying();
 static void finish(bool success, const QString &message);
 static void notice(const QString &message);
 static bool isBusy();
 static void setJobs(uint64_t jobs);
};
}
