#pragma once
#include <QSettings>

namespace ProcessSettings {
// Tests only: a lower minimum for the poll intervals (1 s, and 5 s in the
// background), so suites need not wait them out; 0 restores them.
inline double testPollFloor = 0;
inline void setPollFloorForTests(double seconds){testPollFloor=seconds;}
inline double pollFloor(double minimum){return testPollFloor>0?qMin(minimum,testPollFloor):minimum;}
inline bool enabled(){return QSettings().value("processes/enabled",false).toBool();}
inline int milliseconds(const char *key,double fallback,double minimum,double maximum) {
    return qRound(qBound(minimum,QSettings().value(QStringLiteral("processes/")+QLatin1String(key),fallback).toDouble(),maximum)*1000);
}
inline int activeMs(){return milliseconds("activeSeconds",2.5,pollFloor(1),60);}
inline int backgroundMs(){return milliseconds("backgroundSeconds",30,pollFloor(5),600);}
inline int unknownMs(){return milliseconds("unknownSeconds",10,pollFloor(1),600);}
inline int loadingMs(){return milliseconds("loadingSeconds",0.5,0,5);}
}
