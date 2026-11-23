#pragma once

#include <functional>
#include <memory>

class QObject;

std::unique_ptr<QObject> watchX11Input(std::function<void()> onActivity);
