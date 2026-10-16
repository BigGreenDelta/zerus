#pragma once
#include <QSettings>

// Banners for new optional (async, skippable) questions are off by default;
// Settings → Sessions turns them on for this Zerus.
namespace QuestionNotificationSettings {
inline bool enabled(){return QSettings().value("notifications/optionalQuestions",false).toBool();}
inline void setEnabled(bool on){QSettings().setValue("notifications/optionalQuestions",on);}
}
