#pragma once

class QWidget;

namespace ReplayBufferPro
{
  class RecordingFolder
  {
  public:
    // Call on the OBS UI thread; the returned widget is owned by parent.
    static QWidget *createWidget(QWidget *parent, bool compact = false);
  };
}
