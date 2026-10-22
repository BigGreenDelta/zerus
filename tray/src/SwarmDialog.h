#pragma once
#include <functional>
#include <QString>
class HgsClient;
class SwarmController;
class FleetState;
class QWidget;
void showSwarmDialog(HgsClient *client, SwarmController *controller, const FleetState &fleet,
                     QWidget *parent, const std::function<void()> &manageConnections,
                     bool connectComputer = false, const QString &computer = {});
