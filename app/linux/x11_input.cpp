#include "x11_input.h"
#include "mouse_motion.h"

#include <QSocketNotifier>

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#include <unordered_map>
#include <utility>

namespace {
using DisplayPtr = std::unique_ptr<Display, decltype(&XCloseDisplay)>;

class X11Input final : public QObject {
public:
    X11Input(DisplayPtr display, int opcode, std::function<void()> onActivity)
        : display(std::move(display)), socket(ConnectionNumber(this->display.get()), QSocketNotifier::Read),
          opcode(opcode), onActivity(std::move(onActivity)) {
        connect(&socket, &QSocketNotifier::activated, this, [this] { readEvents(); });
    }

private:
    bool hasRelativePointer(int source) {
        if (const auto cached = relativeDevices.find(source); cached != relativeDevices.end()) {
            return cached->second;
        }
        int count{};
        // The source may have disconnected while its motion event was queued.
        const std::unique_ptr<XIDeviceInfo, decltype(&XIFreeDeviceInfo)> devices(
            XIQueryDevice(display.get(), XIAllDevices, &count), XIFreeDeviceInfo);
        bool relative = false;
        for (int deviceIndex = 0; devices && deviceIndex < count; ++deviceIndex) {
            const auto& device = devices.get()[deviceIndex];
            if (device.deviceid != source) {
                continue;
            }
            for (int i = 0; i < device.num_classes; ++i) {
                const auto* entry = device.classes[i];
                if (entry->type != XIValuatorClass) {
                    continue;
                }
                const auto* axis = reinterpret_cast<const XIValuatorClassInfo*>(entry);
                if (axis->number <= 1 && axis->mode == XIModeRelative) {
                    relative = true;
                    break;
                }
            }
            break;
        }
        relativeDevices.emplace(source, relative);
        return relative;
    }

    bool deliberateMotion(const XIRawEvent& event) {
        if (!hasRelativePointer(event.sourceid)) {
            // Tablet and other absolute devices don't supply relative mouse counts.
            return true;
        }
        const auto& axes = event.valuators;
        const double* values = event.raw_values;
        double dx = 0, dy = 0;
        for (int axis = 0; axis < axes.mask_len * 8; ++axis) {
            if (!XIMaskIsSet(axes.mask, axis)) {
                continue;
            }
            const double value = *values++;
            if (axis == 0) {
                dx = value;
            } else if (axis == 1) {
                dy = value;
            } else if (value != 0) {
                // Smooth scrolling is reported as a separate valuator.
                return true;
            }
        }
        return motion.moved(event.sourceid, event.time, dx, dy);
    }

    void readEvents() {
        auto* connection = display.get();
        while (XPending(connection)) {
            XEvent event{};
            XNextEvent(connection, &event);
            auto& cookie = event.xcookie;
            if (event.type != GenericEvent || cookie.extension != opcode ||
                !XGetEventData(connection, &cookie)) {
                continue;
            }
            bool activity = false;
            switch (cookie.evtype) {
            case XI_RawMotion:
                activity = deliberateMotion(*static_cast<const XIRawEvent*>(cookie.data));
                break;
            case XI_RawKeyPress:
            case XI_RawKeyRelease:
            case XI_RawButtonPress:
            case XI_RawButtonRelease:
            case XI_RawTouchBegin:
            case XI_RawTouchUpdate:
            case XI_RawTouchEnd:
                activity = true;
                break;
            case XI_HierarchyChanged:
            case XI_DeviceChanged:
                relativeDevices.clear();
                motion = {};
                break;
            }
            XFreeEventData(connection, &cookie);
            if (activity) {
                motion = {};
                onActivity();
            }
        }
    }

    DisplayPtr display;
    QSocketNotifier socket;
    int opcode;
    std::function<void()> onActivity;
    std::unordered_map<int, bool> relativeDevices;
    MouseMotion motion;
};
}

std::unique_ptr<QObject> watchX11Input(std::function<void()> onActivity) {
    DisplayPtr display(XOpenDisplay(nullptr), XCloseDisplay);
    int opcode{}, event{}, error{};
    int major = 2, minor = 2;
    if (!display || !XQueryExtension(display.get(), "XInputExtension", &opcode, &event, &error) ||
        XIQueryVersion(display.get(), &major, &minor) != Success || major < 2 || (major == 2 && minor < 1)) {
        return {};
    }
    unsigned char inputMask[XIMaskLen(XI_LASTEVENT)]{};
    XISetMask(inputMask, XI_RawKeyPress);
    XISetMask(inputMask, XI_RawKeyRelease);
    XISetMask(inputMask, XI_RawButtonPress);
    XISetMask(inputMask, XI_RawButtonRelease);
    XISetMask(inputMask, XI_RawMotion);
    XISetMask(inputMask, XI_DeviceChanged);
    if (major > 2 || minor >= 2) {
        XISetMask(inputMask, XI_RawTouchBegin);
        XISetMask(inputMask, XI_RawTouchUpdate);
        XISetMask(inputMask, XI_RawTouchEnd);
    }
    unsigned char hierarchyMask[XIMaskLen(XI_HierarchyChanged)]{};
    XISetMask(hierarchyMask, XI_HierarchyChanged);
    XIEventMask masks[] = {
        {XIAllMasterDevices, sizeof(inputMask), inputMask},
        {XIAllDevices, sizeof(hierarchyMask), hierarchyMask},
    };
    for (int screen = 0; screen < ScreenCount(display.get()); ++screen) {
        if (XISelectEvents(display.get(), RootWindow(display.get(), screen), masks, 2) != Success) {
            return {};
        }
    }
    XFlush(display.get());
    return std::make_unique<X11Input>(std::move(display), opcode, std::move(onActivity));
}
