#pragma once

#include <QString>

struct ApplicationOptions {
    QString interactiveUser;
    QString runtimeHelper;
    bool startup = false;
    bool repairElevated = false;
};
