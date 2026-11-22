#include "sound.h"
#include "ext-idle-notify-client.h"

#include <KIdleTime>
#include <LayerShellQt/Window>
#include <QtDBus/QtDBus>
#include <QtWidgets/QtWidgets>
#include <QLocalServer>
#include <QLocalSocket>
#include <wayland-client.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace {
constexpr double pi = std::numbers::pi;
constexpr double intro = 0.8;
constexpr double outro = 1.9;
constexpr double collapseDuration = 0.66;
constexpr int artworkWidth = 760;
constexpr int artworkHeight = 450;
const QPointF pivot(380, 139);
const QColor red(187, 0, 0);
const QColor hot(191, 0, 0);
const QColor white(224, 219, 216);

double ease(double p) {
    p = std::clamp(p, 0.0, 1.0);
    return p * p * (3 - 2 * p);
}

QColor opacity(QColor color, double alpha) {
    color.setAlphaF(std::clamp(alpha, 0.0, 1.0));
    return color;
}

void pen(QPainter& painter, QColor color, double alpha, double width = 1) {
    painter.setPen(QPen(opacity(color, alpha), width, Qt::SolidLine, Qt::FlatCap));
    painter.setBrush(Qt::NoBrush);
}

QPointF radial(double radius, double angle) {
    return pivot + QPointF(radius * std::cos(angle), radius * std::sin(angle));
}

double artworkScale(const QSize& size) {
    return std::min({1.55, size.width() / 760.0, size.height() / 450.0});
}

QTransform artTransform(const QSize& size) {
    const double scale = artworkScale(size);
    QTransform transform;
    transform.translate((size.width() - artworkWidth * scale) / 2,
                        (size.height() - artworkHeight * scale) / 2 - 12 * scale);
    transform.scale(scale, scale);
    return transform;
}

QRect motionBounds(const QSize& size) {
    return artTransform(size).mapRect(QRectF(108, -82, 544, 517)).toAlignedRect()
               .adjusted(-2, -2, 2, 2).intersected(QRect(QPoint(), size));
}

QRect centeredArtworkBounds(const QSize& size) {
    const auto artwork = artTransform(size).mapRect(QRectF(76, -88, 608, 529)).toAlignedRect();
    const int horizontal = std::max(size.width() / 2 - artwork.left(), artwork.right() - size.width() / 2) + 3;
    const int vertical = std::max(size.height() / 2 - artwork.top(), artwork.bottom() - size.height() / 2) + 3;
    const QSize region(std::min(2 * horizontal, size.width()), std::min(2 * vertical, size.height()));
    return QRect(QPoint((size.width() - region.width()) / 2, (size.height() - region.height()) / 2), region);
}

QImage makeShade(const QSize& size) {
    QImage shade(size, QImage::Format_ARGB32_Premultiplied);
    const double scale = artworkScale(size);
    const double centerX = size.width() / 2.0;
    const double centerY = size.height() / 2.0 + (207 - 225 - 12) * scale;
    const double inverseX = 1 / (465 * scale);
    const double inverseY = 1 / (278 * scale);
    for (int y = 0; y < size.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(shade.scanLine(y));
        const double dy = (y + 0.5 - centerY) * inverseY;
        for (int x = 0; x < size.width(); ++x) {
            const double dx = (x + 0.5 - centerX) * inverseX;
            const double squared = dx * dx + dy * dy;
            if (squared >= 1) {
                line[x] = qRgba(6, 2, 1, 92);
                continue;
            }
            const double radius = std::sqrt(squared);
            const double weight = 0.86 * (1 - ease(radius));
            const double base = 0.36 * (1 - weight);
            const uint32_t seed = (uint32_t(x) * 0x9e3779b1u) ^ (uint32_t(y) * 0x85ebca77u);
            const double noise = ((seed ^ (seed >> 15)) * 0xc2b2ae3du >> 24) / 256.0;
            line[x] = qRgba(int(weight * std::lerp(21.0, 28.0, radius) + base * 16 + noise),
                            int(weight * 7 + base * 5 + noise),
                            int(weight * 4 + base * 3 + noise),
                            int((weight + base) * 255 + noise));
        }
    }
    return shade;
}

void arc(QPainter& painter, double radius, double from, double span, double width, double alpha) {
    pen(painter, red, alpha, width);
    painter.drawArc(QRectF(pivot.x() - radius, pivot.y() - radius, radius * 2, radius * 2),
                    qRound(-from * 2880 / pi), qRound(-span * 2880 / pi));
}

void dot(QPainter& painter, QPointF point, double radius, const QColor& color, double alpha) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(opacity(color, alpha));
    painter.drawEllipse(point, radius, radius);
}

QIcon makeIcon() {
    QPixmap icon(64, 64);
    icon.fill(Qt::transparent);
    QPainter painter(&icon);
    painter.setRenderHint(QPainter::Antialiasing);
    pen(painter, red, 1, 3);
    painter.drawEllipse(QPointF(32, 32), 28, 28);
    painter.drawPolygon(QPolygonF{QPointF(32, 13), QPointF(51, 48), QPointF(13, 48)});
    pen(painter, white, 1, 4);
    painter.drawLine(QPointF(32, 25), QPointF(32, 35));
    painter.drawLine(QPointF(32, 40), QPointF(32, 43));
    return QIcon(icon);
}

class WarningArt {
public:
    struct Backdrop {
        QImage shade;
        QImage grid;
        QRect gridArea;
    };

    WarningArt() {
        QPainterPath path;
        const double row = 17 * std::sqrt(3.0);
        for (int column = -17; column <= 17; ++column) {
            for (int line = -9; line <= 9; ++line) {
                const QPointF c(380 + column * 25.5, 207 + row * (line + (column % 2 ? 0.5 : 0)));
                path.moveTo(c + QPointF(17, 0));
                path.lineTo(c + QPointF(8.5, row / 2));
                path.lineTo(c + QPointF(-8.5, row / 2));
                path.lineTo(c + QPointF(-17, 0));
            }
        }
        QRadialGradient color(QPointF(380, 207), 400);
        color.setColorAt(0, opacity(red, 0.20));
        color.setColorAt(0.55, opacity(red, 0.12));
        color.setColorAt(1, opacity(red, 0));
        grid = QImage(gridBounds.size(), QImage::Format_ARGB32_Premultiplied);
        grid.fill(Qt::transparent);
        QPainter painter(&grid);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(-gridBounds.topLeft());
        painter.setPen(QPen(QBrush(color), 0.9));
        painter.drawPath(path);
    }

    Backdrop makeBackdrop(const QSize& size) const {
        Backdrop result;
        result.shade = makeShade(size);
        const auto transform = artTransform(size);
        result.gridArea = transform.mapRect(gridBounds).intersected(QRect(QPoint(), size));
        result.grid = QImage(result.gridArea.size(), QImage::Format_ARGB32_Premultiplied);
        result.grid.fill(Qt::transparent);
        if (!result.gridArea.isEmpty()) {
            QPainter painter(&result.grid);
            painter.translate(-result.gridArea.topLeft());
            painter.setWorldTransform(transform, true);
            painter.drawImage(gridBounds.topLeft(), grid);
        }
        return result;
    }

    void paintBackdrop(QPainter& painter, const Backdrop& backdrop, const QRect& damage,
                       double time, int duration, bool darken) const {
        const double elapsed = std::max(0.0, time - intro);
        const double ending = std::max(0.0, elapsed - duration);
        const double fade = 1 - ease((ending - 1.30) / (outro - 1.30));
        const double backgroundOpacity = fade * ease(time / intro);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        if (backgroundOpacity < 1) {
            painter.fillRect(damage, Qt::transparent);
        }
        painter.setOpacity(backgroundOpacity);
        painter.drawImage(damage.topLeft(), backdrop.shade, damage);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        const QRect gridDamage = damage.intersected(backdrop.gridArea);
        if (!gridDamage.isEmpty()) {
            painter.setOpacity(backgroundOpacity * (1 - ease(ending / collapseDuration)));
            painter.drawImage(gridDamage.topLeft(), backdrop.grid, gridDamage.translated(-backdrop.gridArea.topLeft()));
        }
        painter.setOpacity(fade);
        if (darken) {
            painter.fillRect(damage, opacity(QColor(8, 1, 2), 0.94 * ease((ending - 0.36) / 0.64)));
        }
    }

    void paint(QPainter& painter, const QSize& size, const QRect& damage, QPoint origin,
               const Backdrop* backdrop, double time, int duration, bool force) const {
        const double elapsed = std::max(0.0, time - intro);
        const double remaining = std::max(0.0, duration - elapsed);
        const double ending = std::max(0.0, elapsed - duration);
        const int seconds = static_cast<int>(std::ceil(remaining));
        const double appear = ease(time / intro);
        const double collapse = ease(ending / collapseDuration);
        const double scene = 1 - collapse;
        const double fade = 1 - ease((ending - 1.30) / (outro - 1.30));
        if (backdrop) {
            paintBackdrop(painter, *backdrop, damage, time, duration, false);
        } else {
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.fillRect(damage, Qt::transparent);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }
        painter.save();
        painter.translate(-origin);
        painter.setWorldTransform(artTransform(size), true);
        painter.setOpacity(fade);
        painter.setRenderHint(QPainter::Antialiasing);

        drawBackground(painter, time, appear * scene);
        drawSignals(painter, time, ease((time - 0.15) / 0.58) * scene, remaining);
        drawOrbits(painter, time, ease(time / 0.48) * scene, remaining, collapse);
        if (seconds > 0 && seconds <= 3) {
            const double age = ease((seconds - remaining) / 0.48);
            pen(painter, red, 0.77 * (1 - age), 2.4);
            painter.drawEllipse(pivot, 80 + 82 * age, 80 + 82 * age);
        }
        painter.save();
        painter.translate(pivot);
        painter.scale(1 - 0.78 * collapse, 1 - 0.78 * collapse);
        painter.translate(-pivot);
        drawDial(painter, time, appear * scene, remaining, duration);
        painter.restore();
        drawText(painter, seconds, remaining / duration, ease((time - 0.22) / 0.52) * scene, force);

        if (ending > 0) {
            if (backdrop) {
                painter.save();
                painter.resetTransform();
                painter.fillRect(damage, opacity(QColor(8, 1, 2), 0.94 * ease((ending - 0.36) / 0.64)));
                painter.restore();
            }
            drawEnding(painter, ending, collapse);
        }
        painter.restore();
    }

private:
    static void drawBackground(QPainter& painter, double time, double appear) {
        for (const double offset : {0.0, 0.5}) {
            const double phase = std::fmod(time * 0.64 + offset, 1.0);
            const double radius = 91 + phase * 124;
            pen(painter, red, 0.31 * appear * ease(phase / 0.14) * (1 - ease(phase)), 1.4);
            painter.drawEllipse(pivot, radius, radius);
        }
    }

    static void drawOrbits(QPainter& painter, double time, double reveal, double remaining, double collapse) {
        const double urgency = std::max(0.0, 4 - remaining);
        const double width = 131 * (1 - 0.88 * collapse);
        const double height = 36 * (1 - 0.88 * collapse);
        for (int side : {-1, 1}) {
            painter.save();
            painter.translate(pivot);
            painter.rotate(side * (31 + 22 * collapse));
            pen(painter, red, 0.34 * reveal, 1.1);
            painter.drawEllipse(QPointF(), width, height);
            const double phase = side * (time * 1.8 + urgency * urgency * 0.15 + 2.6 * collapse)
                               + (side < 0 ? pi : 0);
            for (int i = 5; i >= 0; --i) {
                const double angle = phase - side * i * 0.095;
                const double weight = 1 - i / 6.0;
                const QPointF p(width * std::cos(angle), height * std::sin(angle));
                if (i == 0) {
                    dot(painter, p, 7, red, 0.24 * reveal);
                }
                dot(painter, p, 0.8 + 2.8 * weight, hot, weight * reveal);
            }
            painter.restore();
        }
    }

    static void drawSignals(QPainter& painter, double time, double reveal, double remaining) {
        const double urgency = std::max(0.0, 4 - remaining);
        const double phase = time * 5.2 + urgency * urgency * 0.65;
        for (int side : {-1, 1}) {
            const double start = pivot.x() + side * 151;
            const double y = pivot.y() + side * 8;
            const double track = side * 112 * reveal;
            pen(painter, red, 0.45 * reveal, 1);
            painter.drawLine(QPointF(start, y), QPointF(start + track, y));
            for (double x : {start, start + track}) {
                painter.drawLine(QPointF(x, y - 10), QPointF(x, y + 10));
            }
            QPainterPath trace(QPointF(start, y));
            for (int i = 1; i <= 42; ++i) {
                const double u = i / 42.0;
                const double signal = std::sin(phase * side - 19 * u) + 0.30 * std::sin(phase * 1.55 - 42 * u);
                trace.lineTo(start + track * u, y + 14 * reveal * std::sin(pi * u) * signal);
            }
            pen(painter, red, 0.77 * reveal, 1.4);
            painter.drawPath(trace);
        }
    }

    static void drawDial(QPainter& painter, double time, double appear, double remaining, int duration) {
        const double radius = 70 + 7 * appear;
        const double urgency = std::max(0.0, 3 - remaining);
        const double pulse = 0.5 + 0.5 * std::sin(2 * pi * (0.6 * time + 0.19 * urgency * urgency));
        const double glow = appear * (0.55 + 0.44 * pulse);
        pen(painter, red, 0.18 * glow, 19);
        painter.drawEllipse(pivot, radius, radius);
        pen(painter, red, 0.46 * glow, 10);
        painter.drawEllipse(pivot, radius, radius);
        pen(painter, red, 0.49 * appear, 1.5);
        painter.drawEllipse(pivot, radius, radius);
        pen(painter, red, 0.27 * appear);
        painter.drawEllipse(pivot, radius - 16, radius - 16);
        const double sweep = 2 * pi * remaining / duration * appear;
        arc(painter, radius, -pi / 2, sweep, 5.4, appear);
        for (int i = 0; i < 48; ++i) {
            const bool major = i % 4 == 0;
            const double angle = -pi / 2 + i * pi / 24;
            const double reveal = ease(1.5 * appear - i / 90.0);
            pen(painter, red, (major ? 0.68 : 0.30) * reveal, major ? 1.6 : 1);
            painter.drawLine(radial(radius + 8, angle), radial(radius + (major ? 16 : 12), angle));
            if (major) {
                painter.drawLine(radial(radius - 16, angle), radial(radius - 11, angle));
            }
        }
        const double rotation = -pi / 2 + 0.68 * time + 0.28 * urgency * urgency;
        arc(painter, radius + 22, rotation, 0.68, 2.8, glow);
        arc(painter, radius + 22, rotation + pi, 0.68, 2.8, glow);
        dot(painter, radial(radius, sweep - pi / 2), 3, hot, appear * ease(remaining));

        const QPolygonF triangle{pivot + QPointF(0, -40), pivot + QPointF(45, 34), pivot + QPointF(-45, 34)};
        pen(painter, red, 0.30 * glow, 11);
        painter.drawPolygon(triangle);
        pen(painter, red, appear, 4.2);
        painter.drawPolygon(triangle);
        pen(painter, hot, appear, 6);
        painter.drawLine(pivot + QPointF(0, -9), pivot + QPointF(0, 12));
        painter.drawLine(pivot + QPointF(0, 17), pivot + QPointF(0, 23));

        const double scan = std::clamp((time - 0.12) / 0.67, 0.0, 1.0);
        const double y = std::lerp(-70.0, 70.0, ease(scan));
        const double reach = std::sqrt(4900 - y * y);
        pen(painter, red, 0.9 * std::sin(pi * scan), 1.6);
        painter.drawLine(pivot + QPointF(-reach, y), pivot + QPointF(reach, y));
    }

    static void label(QPainter& painter, const QString& value, double y, double h, int size,
                      QColor color, double alpha, bool mono) {
        QFont font(mono ? QStringLiteral("monospace") : QStringLiteral("sans-serif"));
        font.setPixelSize(size);
        font.setWeight(mono ? QFont::DemiBold : QFont::Black);
        painter.setFont(font);
        painter.setPen(opacity(color, alpha));
        painter.drawText(QRectF(0, y, artworkWidth, h), Qt::AlignCenter, value);
    }

    static void drawText(QPainter& painter, int seconds, double fraction, double reveal, bool force) {
        const double rise = 9 * (1 - reveal);
        for (int side : {-1, 1}) {
            for (int index = 0; index < 3; ++index) {
                const double x = pivot.x() + side * (199 + 11 * index + 14 * (1 - reveal));
                const double y = 259 + rise;
                pen(painter, red, (0.85 - 0.21 * index) * reveal, 2.2);
                painter.drawPolyline(QPolygonF{QPointF(x + side * 8, y - 10), QPointF(x, y),
                                                QPointF(x + side * 8, y + 10)});
            }
        }
        label(painter, QStringLiteral("WARNING"), 224 + rise, 67, 65, red, reveal, false);
        label(painter, QStringLiteral("SHUTDOWN IMMINENT"), 295 + rise, 19, 16, white, reveal, true);
        const auto digits = QStringLiteral("%1:%2").arg(seconds / 60, 2, 10, QLatin1Char('0'))
                                                 .arg(seconds % 60, 2, 10, QLatin1Char('0'));
        label(painter, digits, 320, 63, 62, seconds <= 3 ? hot : white, reveal, true);
        for (int index = 0; index < 12; ++index) {
            const QRectF bar(pivot.x() - 88 + 15 * index, 388 + rise, 11, 3);
            const double fill = std::clamp(fraction * 12 - index, 0.0, 1.0);
            painter.fillRect(bar, opacity(red, 0.28 * reveal));
            painter.fillRect(QRectF(bar.topLeft(), QSizeF(bar.width() * fill, bar.height())), opacity(red, reveal));
        }
        label(painter, seconds == 0 && force ? QStringLiteral("POWERING OFF") : QStringLiteral("ESC TO CANCEL"),
              405 + rise, 20, 15, white, 0.88 * reveal, true);
    }

    static void drawEnding(QPainter& painter, double age, double collapse) {
        const double sweep = ease((age - 0.54) / 0.29);
        const double close = ease((age - 0.88) / 0.35);
        const double beam = sweep * (1 - close);
        const QPointF center(pivot.x(), std::lerp(pivot.y(), artworkHeight / 2.0 + 12, sweep));
        const double radius = std::lerp(25.0, 4.5, collapse) * (1 - 0.70 * close);
        const double core = ease(age / 0.21) * (1 - close);
        dot(painter, center, 2.4 * radius, red, 0.13 * core);
        pen(painter, red, 0.76 * core, 1.4);
        painter.drawEllipse(center, radius, radius);
        dot(painter, center, 3.2, white, 0.82 * core);
        const QPointF reach(296 * beam, 0);
        pen(painter, red, 0.21 * beam, 13);
        painter.drawLine(center - reach, center + reach);
        pen(painter, hot, beam, 2.2);
        painter.drawLine(center - reach, center + reach);
        dot(painter, center - reach, 2.5, white, 0.66 * beam);
        dot(painter, center + reach, 2.5, white, 0.66 * beam);
    }

    const QRect gridBounds{-38, -46, 836, 506};
    QImage grid;
};

bool hasLayerShell() {
    wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        return false;
    }
    bool found = false;
    const wl_registry_listener listener{
        [](void* data, wl_registry*, uint32_t, const char* interface, uint32_t) {
            if (qstrcmp(interface, "zwlr_layer_shell_v1") == 0) {
                *static_cast<bool*>(data) = true;
            }
        },
        [](void*, wl_registry*, uint32_t) {},
    };
    auto* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &listener, &found);
    const bool available = wl_display_roundtrip(display) >= 0 && found;
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return available;
}

class WaylandInputIdle {
public:
    WaylandInputIdle(std::function<void()> idle, std::function<void()> resumed)
        : idle(std::move(idle)), resumed(std::move(resumed)) {
        display = wl_display_connect(nullptr);
        if (!display) {
            return;
        }
        registry = wl_display_get_registry(display);
        static constexpr wl_registry_listener listener{
            [](void* data, wl_registry* globals, uint32_t name, const char* interface, uint32_t version) {
                auto& self = *static_cast<WaylandInputIdle*>(data);
                if (!self.manager && version >= 2 && qstrcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
                    self.manager = static_cast<ext_idle_notifier_v1*>(
                        wl_registry_bind(globals, name, &ext_idle_notifier_v1_interface, 2));
                } else if (!self.seat && qstrcmp(interface, wl_seat_interface.name) == 0) {
                    self.seat = static_cast<wl_seat*>(wl_registry_bind(globals, name, &wl_seat_interface, 1));
                }
            },
            [](void*, wl_registry*, uint32_t) {},
        };
        wl_registry_add_listener(registry, &listener, this);
        if (wl_display_roundtrip(display) < 0 || !manager || !seat) {
            return;
        }
        reader = std::make_unique<QSocketNotifier>(wl_display_get_fd(display), QSocketNotifier::Read);
        QObject::connect(reader.get(), &QSocketNotifier::activated, [this] {
            if (wl_display_dispatch(display) < 0) {
                reader->setEnabled(false);
                this->resumed();
            }
        });
    }

    ~WaylandInputIdle() {
        reader.reset();
        if (notification) ext_idle_notification_v1_destroy(notification);
        if (manager) ext_idle_notifier_v1_destroy(manager);
        if (seat) wl_seat_destroy(seat);
        if (registry) wl_registry_destroy(registry);
        if (display) wl_display_disconnect(display);
    }

    bool available() const {
        return reader && reader->isEnabled();
    }

    void arm(uint32_t delay) {
        if (notification) {
            ext_idle_notification_v1_destroy(notification);
            notification = nullptr;
        }
        if (delay != 0) {
            notification = ext_idle_notifier_v1_get_input_idle_notification(manager, delay, seat);
            static constexpr ext_idle_notification_v1_listener listener{
                [](void* data, ext_idle_notification_v1*) { static_cast<WaylandInputIdle*>(data)->idle(); },
                [](void* data, ext_idle_notification_v1*) { static_cast<WaylandInputIdle*>(data)->resumed(); },
            };
            ext_idle_notification_v1_add_listener(notification, &listener, this);
        }
        wl_display_flush(display);
    }

private:
    std::function<void()> idle;
    std::function<void()> resumed;
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_seat* seat = nullptr;
    ext_idle_notifier_v1* manager = nullptr;
    ext_idle_notification_v1* notification = nullptr;
    std::unique_ptr<QSocketNotifier> reader;
};

class Overlay final : public QWidget {
public:
    Overlay(const WarningArt& art, int duration, bool force, std::function<void()> cancel)
        : art(art), duration(duration), force(force), cancel(std::move(cancel)) {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        setWindowTitle(QStringLiteral("Shutdown Warning Overlay"));
        setAttribute(Qt::WA_TranslucentBackground);
        setMouseTracking(true);
    }

    void showOn(QScreen* screen, bool layerShell, bool acceptsKeyboard) {
        screenSize = screen->size();
        if (layerShell) {
            artworkRegion = centeredArtworkBounds(screenSize);
        }
        if (!acceptsKeyboard) {
            setWindowFlag(Qt::WindowDoesNotAcceptFocus);
            setWindowFlag(Qt::WindowTransparentForInput);
        }
        winId();
        auto* window = windowHandle();
        window->setScreen(screen);
        if (layerShell) {
            auto* layer = LayerShellQt::Window::get(window);
            using Layer = LayerShellQt::Window;
            layer->setLayer(Layer::LayerOverlay);
            layer->setScreen(screen);
            layer->setScope(QStringLiteral("shutdown-warning"));
            layer->setAnchors(Layer::AnchorNone);
            layer->setDesiredSize(artworkRegion.size());
            layer->setExclusiveZone(-1);
            layer->setKeyboardInteractivity(acceptsKeyboard ? Layer::KeyboardInteractivityExclusive
                                                           : Layer::KeyboardInteractivityNone);
            setFixedSize(artworkRegion.size());
            show();
        } else {
            setGeometry(screen->geometry());
            showFullScreen();
            if (acceptsKeyboard) {
                activateWindow();
            }
        }
    }

    void advance(double time) {
        const bool full = this->time < intro || time >= intro + duration;
        this->time = time;
        if (full || !artworkRegion.isEmpty()) {
            update();
        } else {
            // The shade and grid outside the animated artwork are stable during the countdown.
            update(motionBounds(size()));
        }
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        const bool split = !artworkRegion.isEmpty();
        if (!split && backdrop.shade.size() != size()) {
            backdrop = art.makeBackdrop(size());
        }
        QPainter painter(this);
        art.paint(painter, split ? screenSize : size(), event->rect(), artworkRegion.topLeft(),
                  split ? nullptr : &backdrop, time, duration, force);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) {
            cancel();
        } else {
            QWidget::keyPressEvent(event);
        }
    }

    void closeEvent(QCloseEvent* event) override {
        event->ignore();
        cancel();
    }

private:
    const WarningArt& art;
    const int duration;
    const bool force;
    const std::function<void()> cancel;
    QSize screenSize;
    QRect artworkRegion;
    WarningArt::Backdrop backdrop;
    double time = 0;
};

class ShadeLayer final : public QWidget {
public:
    ShadeLayer(const WarningArt& art, QScreen* screen, int duration) : art(art), duration(duration) {
        setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowTransparentForInput |
                       Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        winId();
        auto* window = windowHandle();
        window->setScreen(screen);
        auto* layer = LayerShellQt::Window::get(window);
        using Layer = LayerShellQt::Window;
        layer->setLayer(Layer::LayerTop);
        layer->setScreen(screen);
        layer->setScope(QStringLiteral("shutdown-warning-shade"));
        layer->setAnchors(Layer::Anchors{Layer::AnchorTop} | Layer::AnchorBottom |
                          Layer::AnchorLeft | Layer::AnchorRight);
        layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(Layer::KeyboardInteractivityNone);
        setGeometry(screen->geometry());
        show();
    }

    void advance(double now) {
        // Full-screen buffers are only sent during the dim and blackout transitions.
        const int step = now < intro ? int(now * 10)
                       : now < intro + duration ? 8
                       : 9 + int((now - intro - duration) * 10);
        if (step != previousStep) {
            previousStep = step;
            time = now;
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        if (backdrop.shade.size() != size()) {
            backdrop = art.makeBackdrop(size());
        }
        QPainter painter(this);
        art.paintBackdrop(painter, backdrop, event->rect(), time, duration, true);
    }

private:
    const WarningArt& art;
    const int duration;
    WarningArt::Backdrop backdrop;
    double time = 0;
    int previousStep = -1;
};

struct Settings {
    int seconds = 12;
    int idleMinutes = 10;
    bool automatic = true;
    bool sounds = true;
    bool force = false;

    static Settings load() {
        QSettings store;
        return {
            std::clamp(store.value("countdown", 12).toInt(), 5, 120),
            std::clamp(store.value("idleMinutes", 10).toInt(), 1, 240),
            store.value("automatic", true).toBool(),
            store.value("sounds", true).toBool(),
            store.value("force", false).toBool(),
        };
    }

    void save() const {
        QSettings store;
        store.setValue("countdown", seconds);
        store.setValue("idleMinutes", idleMinutes);
        store.setValue("automatic", automatic);
        store.setValue("sounds", sounds);
        store.setValue("force", force);
    }
};

QString autostartPath() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("autostart/shutdown-warning.desktop"));
}

bool xdgStartsAtLogin() {
    const QString path = autostartPath();
    if (!QFileInfo::exists(path)) {
        return false;
    }
    QSettings entry(path, QSettings::IniFormat);
    entry.beginGroup(QStringLiteral("Desktop Entry"));
    return entry.value(QStringLiteral("Type")).toString() == QStringLiteral("Application") &&
           !entry.value(QStringLiteral("Exec")).toString().isEmpty() &&
           !entry.value(QStringLiteral("Hidden"), false).toBool() &&
           entry.value(QStringLiteral("X-GNOME-Autostart-enabled"), true).toBool();
}

bool setXdgStartsAtLogin(bool enabled) {
    const QString path = autostartPath();
    if (!enabled) {
        return !QFileInfo::exists(path) || QFile::remove(path);
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QSaveFile file(path);
    const QByteArray entry =
        "[Desktop Entry]\nType=Application\nName=Shutdown Warning\n"
        "Exec=shutdown-warning --background\nTryExec=shutdown-warning\nIcon=shutdown-warning\n";
    return file.open(QIODevice::WriteOnly) && file.write(entry) == entry.size() && file.commit();
}

bool isHyprlandSession() {
    return qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE") ||
           qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':').contains(QStringLiteral("Hyprland"), Qt::CaseInsensitive);
}

bool systemdStartsAtLogin() {
    const QDir config(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
    const QFileInfo mask(config.filePath(QStringLiteral("systemd/user/shutdown-warning.service")));
    return mask.symLinkTarget() != QStringLiteral("/dev/null") &&
           QFileInfo::exists(config.filePath(QStringLiteral(
               "systemd/user/graphical-session.target.wants/shutdown-warning.service")));
}

bool setSystemdStartsAtLogin(bool enabled) {
    QProcess command;
    command.start(QStringLiteral("/usr/bin/systemctl"),
                  {QStringLiteral("--user"), enabled ? QStringLiteral("enable") : QStringLiteral("disable"),
                   QStringLiteral("shutdown-warning.service")});
    if (!command.waitForFinished(3000)) {
        if (command.state() != QProcess::NotRunning) {
            command.kill();
            command.waitForFinished(250);
        }
        return false;
    }
    return command.exitStatus() == QProcess::NormalExit && command.exitCode() == 0;
}

bool startsAtLogin() {
    return systemdStartsAtLogin() || (!isHyprlandSession() && xdgStartsAtLogin());
}

bool setStartsAtLogin(bool enabled) {
    if (enabled && isHyprlandSession()) {
        const bool alreadyEnabled = systemdStartsAtLogin();
        if (!alreadyEnabled && !setSystemdStartsAtLogin(true)) {
            return false;
        }
        if (!setXdgStartsAtLogin(false)) {
            if (!alreadyEnabled) {
                setSystemdStartsAtLogin(false);
            }
            return false;
        }
        return true;
    }
    if (!enabled && systemdStartsAtLogin() && !setSystemdStartsAtLogin(false)) {
        return false;
    }
    return setXdgStartsAtLogin(enabled);
}

class Audio {
public:
    ~Audio() {
        if (process.state() != QProcess::NotRunning) {
            stop();
            process.waitForFinished(100);
        }
    }

    void begin() {
        if (!prepared) {
            prepared = true;
            if (directory.isValid()) {
                siren = save(QStringLiteral("siren.wav"), sound::warningSound());
                impact = save(QStringLiteral("impact.wav"), sound::countdownSound());
            }
        }
        play(siren);
    }
    void beat() { play(impact); }
    void stop() {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
        }
    }

private:
    QString save(const QString& name, const std::vector<char>& audio) {
        const auto path = directory.filePath(name);
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(audio.data(), audio.size()) == qint64(audio.size())
                   ? path : QString{};
    }

    void play(const QString& path) {
        if (path.isEmpty() || process.state() != QProcess::NotRunning) {
            return;
        }
        process.setStandardErrorFile(QProcess::nullDevice());
        process.start(QStringLiteral("/usr/bin/paplay"), {path}, QIODevice::ReadOnly);
    }

    QTemporaryDir directory;
    QProcess process;
    QString siren;
    QString impact;
    bool prepared = false;
};

class NumericField final : public QSpinBox {
public:
    NumericField(int minimum, int maximum) {
        setRange(minimum, maximum);
        setAttribute(Qt::WA_InputMethodEnabled, false);
        lineEdit()->setAttribute(Qt::WA_InputMethodEnabled, false);
    }
};

class Application final : public QObject {
public:
    explicit Application(bool previewOnly)
        : settings(Settings::load()), icon(makeIcon()), previewOnly(previewOnly),
          layerShell(QGuiApplication::platformName().startsWith("wayland") && hasLayerShell()) {
        timer.setTimerType(Qt::PreciseTimer);
        timer.setInterval(33);
        connect(&timer, &QTimer::timeout, this, [this] { advance(); });
        connect(qApp, &QGuiApplication::screenAdded, this, [this] { cancel(); });
        connect(qApp, &QGuiApplication::screenRemoved, this, [this] { cancel(); });
    }

    void start(bool background) {
        if (previewOnly) {
            begin(settings, false);
            return;
        }
        if (isHyprlandSession() && xdgStartsAtLogin() && !setStartsAtLogin(true)) {
            qWarning("Could not migrate login startup to the graphical session service.");
        }
        if (QGuiApplication::platformName().startsWith("wayland")) {
            waylandIdle = std::make_unique<WaylandInputIdle>(
                [this] { begin(settings, true); },
                [this] { if (automatic) cancel(); });
        }
        auto* idle = KIdleTime::instance();
        connect(idle, qOverload<int, int>(&KIdleTime::timeoutReached), this, [this, idle](int id, int) {
            if (id == idleId && id > 0) {
                idle->catchNextResumeEvent();
                begin(settings, true);
            }
        });
        connect(idle, &KIdleTime::resumingFromIdle, this, [this] {
            if (automatic) {
                cancel();
            }
        });
        armIdle();
        buildSettings();
        setupTray();
        trayAvailable = QSystemTrayIcon::isSystemTrayAvailable();
        QApplication::setQuitOnLastWindowClosed(!trayAvailable);
        trayNote->setVisible(!trayAvailable);
        if (!background) {
            showSettings();
        } else if (!trayAvailable) {
            scheduleTrayFallback();
        }
    }

    void openSettings() {
        showSettings();
    }

private:
    void setupTray() {
        tray.setIcon(icon);
        tray.setToolTip(QStringLiteral("Shutdown Warning"));
        auto* menu = new QMenu(&dialog);
        menu->addAction(QStringLiteral("Settings"), this, [this] { showSettings(); });
        menu->addAction(QStringLiteral("Preview"), this, [this] { begin(settings, false); });
        menu->addSeparator();
        menu->addAction(QStringLiteral("Exit"), this, [this] {
            if (!committed()) {
                qApp->quit();
            }
        });
        tray.setContextMenu(menu);
        connect(&tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
                showSettings();
            }
        });
        // Qt will register a visible icon when a tray host arrives, even if none exists yet.
        tray.show();
    }

    void scheduleTrayFallback() {
        QTimer::singleShot(2000, this, [this] {
            if (!trayAvailable && !dialog.isVisible() && !elapsed.isValid()) {
                showSettings();
            }
        });
    }

    void refreshTrayAvailability() {
        const bool available = QSystemTrayIcon::isSystemTrayAvailable();
        if (available == trayAvailable) {
            return;
        }
        trayAvailable = available;
        QApplication::setQuitOnLastWindowClosed(!available);
        trayNote->setVisible(!available);
        if (!available) {
            scheduleTrayFallback();
        }
    }

    void armIdle() {
        auto* idle = KIdleTime::instance();
        if (idleId > 0) {
            idle->removeIdleTimeout(idleId);
            idleId = -1;
        }
        if (waylandIdle && waylandIdle->available()) {
            waylandIdle->arm(settings.automatic ? settings.idleMinutes * 60000 : 0);
            return;
        }
        if (settings.automatic) {
            idleId = idle->addIdleTimeout(settings.idleMinutes * 60000);
        }
    }

    void buildSettings() {
        dialog.setWindowTitle(QStringLiteral("Shutdown Warning Settings"));
        dialog.setWindowIcon(icon);
        dialog.setMinimumWidth(440);
        dialog.setStyleSheet(QStringLiteral(R"(
            QDialog { background: #13090a; color: #e7ddda; }
            QLabel, QCheckBox { color: #e7ddda; }
            QLabel#heading { font-size: 22px; font-weight: 650; }
            QLabel#note { color: #b29798; }
            QFrame#rule { background: #692429; max-height: 1px; }
            QSpinBox, QPushButton { background: #251113; color: #e7ddda; border: 1px solid #692429; padding: 6px 10px; }
            QSpinBox:focus, QPushButton:focus { border-color: #da797d; }
            QSpinBox:disabled { color: #9c7777; }
            QPushButton#save { background: #670a12; border-color: #bb0000; }
            QPushButton:hover { background: #401a1d; }
            QCheckBox::indicator { width: 16px; height: 15px; background: #251113; border: 1px solid #692429; }
            QCheckBox::indicator:checked { background: #910b15; border: 1px solid #da797d; }
        )"));
        auto* layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(22, 20, 22, 20);
        layout->setSpacing(12);
        auto* header = new QHBoxLayout;
        auto* title = new QLabel(QStringLiteral("Automatic warning"));
        title->setObjectName(QStringLiteral("heading"));
        header->addWidget(title);
        header->addStretch();
        auto* glyph = new QLabel;
        glyph->setPixmap(icon.pixmap(48, 48));
        header->addWidget(glyph);
        layout->addLayout(header);
        auto* note = new QLabel(QStringLiteral("Keyboard or mouse activity cancels it before zero."));
        note->setObjectName(QStringLiteral("note"));
        layout->addWidget(note);

        const auto divider = [&] {
            auto* rule = new QFrame;
            rule->setObjectName(QStringLiteral("rule"));
            rule->setFixedHeight(1);
            layout->addWidget(rule);
        };
        divider();
        autoToggle = new QCheckBox(QStringLiteral("Start after inactivity"));
        layout->addWidget(autoToggle);
        auto* fields = new QFormLayout;
        fields->setSpacing(12);
        idleField = new NumericField(1, 240);
        fields->addRow(QStringLiteral("Inactivity (minutes)"), idleField);
        secondsField = new NumericField(5, 120);
        fields->addRow(QStringLiteral("Countdown (seconds)"), secondsField);
        layout->addLayout(fields);
        connect(autoToggle, &QCheckBox::toggled, idleField, &QWidget::setEnabled);
        sounds = new QCheckBox(QStringLiteral("Play warning sounds"));
        layout->addWidget(sounds);
        startAtLogin = new QCheckBox(QStringLiteral("Start at login"));
        layout->addWidget(startAtLogin);
        desktopStatus.setInterval(500);
        connect(&desktopStatus, &QTimer::timeout, this, [this] {
            refreshTrayAvailability();
            if (dialog.isVisible()) {
                const bool enabled = startsAtLogin();
                if (enabled != savedStartAtLogin) {
                    savedStartAtLogin = enabled;
                    startAtLogin->setChecked(enabled);
                }
            }
        });
        desktopStatus.start();
        divider();
        force = new QCheckBox(QStringLiteral("Force shutdown after countdown"));
        layout->addWidget(force);
        auto* forceNote = new QLabel(QStringLiteral(
            "Automatic warnings only. If force is denied, a normal shutdown is tried. Unsaved work can be lost."));
        forceNote->setObjectName(QStringLiteral("note"));
        forceNote->setWordWrap(true);
        layout->addWidget(forceNote);
        supportNote = new QLabel;
        supportNote->setObjectName(QStringLiteral("note"));
        supportNote->setWordWrap(true);
        layout->addWidget(supportNote);
        trayNote = new QLabel(QStringLiteral("No system tray detected. Keep this window open for automatic warnings."));
        trayNote->setObjectName(QStringLiteral("note"));
        trayNote->setWordWrap(true);
        layout->addWidget(trayNote);
        divider();
        auto* buttons = new QHBoxLayout;
        auto* preview = new QPushButton(QStringLiteral("Preview"));
        auto* cancelButton = new QPushButton(QStringLiteral("Cancel"));
        auto* save = new QPushButton(QStringLiteral("Save"));
        save->setObjectName(QStringLiteral("save"));
        save->setDefault(true);
        buttons->addWidget(preview);
        buttons->addStretch();
        buttons->addWidget(cancelButton);
        buttons->addWidget(save);
        layout->addLayout(buttons);
        connect(preview, &QPushButton::clicked, this, [this] { begin(form(), false); });
        connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
        connect(save, &QPushButton::clicked, this, [this] {
            if (startAtLogin->isChecked() != savedStartAtLogin && !setStartsAtLogin(startAtLogin->isChecked())) {
                QMessageBox::warning(&dialog, QStringLiteral("Shutdown Warning"),
                                     QStringLiteral("Could not update the login startup entry."));
                return;
            }
            savedStartAtLogin = startsAtLogin();
            settings = form();
            settings.save();
            armIdle();
            if (trayAvailable) {
                dialog.accept();
            } else {
                updateSupportNote();
            }
        });
    }

    Settings form() const {
        return {secondsField->value(), idleField->value(), autoToggle->isChecked(), sounds->isChecked(), force->isChecked()};
    }

    void updateSupportNote() {
        const bool unsupported = settings.automatic && idleId <= 0 &&
                                 (!waylandIdle || !waylandIdle->available());
        supportNote->setText(QStringLiteral("Global idle detection is unavailable in this desktop session."));
        supportNote->setVisible(unsupported);
    }

    void showSettings() {
        if (!dialog.isVisible()) {
            autoToggle->setChecked(settings.automatic);
            secondsField->setValue(settings.seconds);
            idleField->setValue(settings.idleMinutes);
            idleField->setEnabled(settings.automatic);
            sounds->setChecked(settings.sounds);
            savedStartAtLogin = startsAtLogin();
            startAtLogin->setChecked(savedStartAtLogin);
            force->setChecked(settings.force);
            updateSupportNote();
        }
        dialog.show();
        dialog.raise();
        dialog.activateWindow();
    }

    void begin(const Settings& choices, bool fromIdle) {
        if (elapsed.isValid()) {
            return;
        }
        session = choices;
        automatic = fromIdle;
        lastSecond = choices.seconds;
        QScreen* keyboardScreen = nullptr;
        if (!fromIdle) {
            keyboardScreen = QGuiApplication::screenAt(QCursor::pos());
            if (!keyboardScreen) {
                keyboardScreen = QGuiApplication::primaryScreen();
            }
        }
        if (layerShell) {
            for (auto* screen : QGuiApplication::screens()) {
                shades.push_back(std::make_unique<ShadeLayer>(art, screen, choices.seconds));
            }
        }
        for (auto* screen : QGuiApplication::screens()) {
            auto view = std::make_unique<Overlay>(art, choices.seconds, fromIdle && choices.force,
                                                 [this] { QTimer::singleShot(0, this, [this] { cancel(); }); });
            view->showOn(screen, layerShell, screen == keyboardScreen);
            views.push_back(std::move(view));
        }
        restoreSettings = !fromIdle && dialog.isVisible();
        if (restoreSettings) {
            dialog.hide();
        }
        elapsed.start();
        if (choices.sounds) {
            audio.begin();
        }
        timer.start();
    }

    bool committed() const {
        return elapsed.isValid() && automatic && session.force &&
               elapsed.elapsed() >= qRound64((intro + session.seconds) * 1000);
    }

    void cancel() {
        if (elapsed.isValid() && !committed()) {
            finish(false);
        }
    }

    void advance() {
        const double time = elapsed.elapsed() / 1000.0;
        if (time >= intro + session.seconds + outro) {
            finish(committed());
            return;
        }
        const int seconds = static_cast<int>(std::ceil(std::max(0.0, session.seconds - std::max(0.0, time - intro))));
        if (session.sounds && seconds > 0 && seconds <= 3 && seconds != lastSecond) {
            audio.beat();
        }
        lastSecond = seconds;
        for (auto& shade : shades) {
            shade->advance(time);
        }
        for (auto& view : views) {
            view->advance(time);
        }
    }

    void finish(bool powerOff) {
        timer.stop();
        audio.stop();
        elapsed.invalidate();
        automatic = false;
        if (restoreSettings) {
            restoreSettings = false;
            dialog.show();
        }
        views.clear();
        shades.clear();
        if (!previewOnly && !trayAvailable && !dialog.isVisible() && !powerOff) {
            showSettings();
        }
        if (powerOff) {
            requestPowerOff();
        }
        if (previewOnly) {
            qApp->quit();
        }
    }

    void requestPowerOff(bool force = true) {
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.login1"),
            QStringLiteral("/org/freedesktop/login1"), QStringLiteral("org.freedesktop.login1.Manager"),
            force ? QStringLiteral("PowerOffWithFlags") : QStringLiteral("PowerOff"));
        if (force) {
            constexpr qulonglong skipBlockInhibitors = 0x10;
            message << QVariant::fromValue(skipBlockInhibitors);
        } else {
            message << false;
        }
        auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, force](QDBusPendingCallWatcher* done) {
            const auto result = done->reply();
            done->deleteLater();
            if (result.type() == QDBusMessage::ErrorMessage) {
                const auto name = result.errorName();
                if (force && (name.endsWith(QStringLiteral(".AccessDenied")) ||
                              name.endsWith(QStringLiteral(".InteractiveAuthorizationRequired")) ||
                              name.endsWith(QStringLiteral(".UnknownMethod")) ||
                              name.endsWith(QStringLiteral(".InvalidArgs")) ||
                              name.endsWith(QStringLiteral(".NotSupported")))) {
                    requestPowerOff(false);
                } else {
                    QMessageBox::warning(&dialog, QStringLiteral("Shutdown Warning"),
                        QStringLiteral("Linux could not power off this PC.\n%1").arg(result.errorMessage()));
                }
            }
        });
    }

    Settings settings;
    Settings session;
    QIcon icon;
    const bool previewOnly;
    const bool layerShell;
    WarningArt art;
    Audio audio;
    QDialog dialog;
    QSystemTrayIcon tray;
    QCheckBox* autoToggle = nullptr;
    QCheckBox* sounds = nullptr;
    QCheckBox* force = nullptr;
    QCheckBox* startAtLogin = nullptr;
    QSpinBox* idleField = nullptr;
    QSpinBox* secondsField = nullptr;
    QLabel* supportNote = nullptr;
    QLabel* trayNote = nullptr;
    QTimer timer;
    QTimer desktopStatus;
    std::unique_ptr<WaylandInputIdle> waylandIdle;
    QElapsedTimer elapsed;
    std::vector<std::unique_ptr<Overlay>> views;
    std::vector<std::unique_ptr<ShadeLayer>> shades;
    int idleId = -1;
    int lastSecond = 0;
    bool automatic = false;
    bool restoreSettings = false;
    bool savedStartAtLogin = false;
    bool trayAvailable = false;
};

int renderTest() {
    WarningArt art;
    QImage image(1280, 720, QImage::Format_ARGB32_Premultiplied);
    const auto backdrop = art.makeBackdrop(image.size());
    size_t redPixels = 0;
    {
        QPainter painter(&image);
        art.paint(painter, image.size(), image.rect(), {}, &backdrop, 1.1, 12, false);
    }
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            redPixels += qRed(line[x]) > 100 && qGreen(line[x]) < 24;
        }
    }
    if (redPixels < 1000 || image.pixelColor(0, 0).alpha() < 80 || image.pixelColor(0, 0).alpha() > 100) {
        std::fprintf(stderr, "Unexpected countdown frame: %zu red pixels, corner alpha %d\n",
                     redPixels, image.pixelColor(0, 0).alpha());
        return 1;
    }
    QImage full(image.size(), image.format());
    {
        QPainter painter(&full);
        art.paint(painter, full.size(), full.rect(), {}, &backdrop, 6.25, 12, false);
    }
    {
        const QRect damage = motionBounds(image.size());
        QPainter painter(&image);
        painter.setClipRect(damage);
        art.paint(painter, image.size(), damage, {}, &backdrop, 6.25, 12, false);
    }
    if (full != image) {
        int mismatches = 0;
        QRect bounds;
        for (int y = 0; y < full.height(); ++y) {
            for (int x = 0; x < full.width(); ++x) {
                if (full.pixel(x, y) != image.pixel(x, y)) {
                    ++mismatches;
                    bounds |= QRect(x, y, 1, 1);
                }
            }
        }
        std::fprintf(stderr, "Partial repaint mismatch: %d pixels in (%d,%d) %dx%d\n",
                     mismatches, bounds.x(), bounds.y(), bounds.width(), bounds.height());
        return 1;
    }
    {
        QPainter painter(&image);
        art.paint(painter, image.size(), image.rect(), {}, &backdrop, 13.65, 12, false);
    }
    const QColor beam = image.pixelColor(image.width() / 2 + 240, image.height() / 2);
    if (beam.red() <= 90 || beam.green() >= 25) {
        std::fprintf(stderr, "Unexpected shutdown beam: %d,%d,%d\n", beam.red(), beam.green(), beam.blue());
        return 1;
    }
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    QStringList launchArgs;
    for (int index = 1; index < argc; ++index) {
        launchArgs.append(QString::fromLocal8Bit(argv[index]));
    }
    if (!launchArgs.contains(QStringLiteral("--foreground")) &&
        !launchArgs.contains(QStringLiteral("--preview")) && !launchArgs.contains(QStringLiteral("--self-test")) &&
        !launchArgs.contains(QStringLiteral("--help")) && !launchArgs.contains(QStringLiteral("-h")) &&
        !launchArgs.contains(QStringLiteral("--help-all")) &&
        !launchArgs.contains(QStringLiteral("--version")) && !launchArgs.contains(QStringLiteral("-v"))) {
        QCoreApplication launcher(argc, argv);
        if (launchArgs.contains(QStringLiteral("--autostart")) && !startsAtLogin() && !xdgStartsAtLogin()) {
            return 0;
        }
        QProcess child;
        child.setProgram(QCoreApplication::applicationFilePath());
        launchArgs.prepend(QStringLiteral("--foreground"));
        child.setArguments(launchArgs);
        child.setStandardInputFile(QProcess::nullDevice());
        child.setStandardOutputFile(QProcess::nullDevice());
        child.setStandardErrorFile(QProcess::nullDevice());
        return child.startDetached() ? 0 : 1;
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Shutdown Warning"));
    app.setApplicationVersion(QStringLiteral("1.0.5"));
    app.setOrganizationName(QStringLiteral("ShutdownWarning"));
    app.setDesktopFileName(QStringLiteral("shutdown-warning"));
    app.setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption preview(QStringLiteral("preview"), QStringLiteral("Play the simulation and exit."));
    const QCommandLineOption test(QStringLiteral("self-test"), QStringLiteral("Verify rendering without a desktop."));
    const QCommandLineOption background(QStringLiteral("background"), QStringLiteral("Start in the system tray."));
    const QCommandLineOption foreground(QStringLiteral("foreground"), QStringLiteral("Stay attached to the terminal."));
    const QCommandLineOption settings(QStringLiteral("settings"), QStringLiteral("Open settings."));
    const QCommandLineOption autostart(QStringLiteral("autostart"), QStringLiteral("Start only when login startup is enabled."));
    parser.addOptions({preview, test, background, foreground, settings, autostart});
    parser.process(app);
    if (parser.isSet(test)) {
        return renderTest();
    }
    if (parser.isSet(autostart) && !startsAtLogin() && !xdgStartsAtLogin()) {
        return 0;
    }
    const auto runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const auto socketName = QDir(runtime).filePath(QStringLiteral("shutdown-warning-settings"));
    QLockFile singleton(QDir(runtime).filePath(QStringLiteral("shutdown-warning.lock")));
    if (!parser.isSet(preview) && !singleton.tryLock()) {
        if (parser.isSet(settings)) {
            QLocalSocket request;
            request.connectToServer(socketName);
            return request.waitForConnected(500) ? 0 : 1;
        }
        return 0;
    }
    Application controller(parser.isSet(preview));
    QLocalServer settingsServer;
    if (!parser.isSet(preview)) {
        QLocalServer::removeServer(socketName);
        settingsServer.setSocketOptions(QLocalServer::UserAccessOption);
        if (settingsServer.listen(socketName)) {
            QObject::connect(&settingsServer, &QLocalServer::newConnection, &controller, [&] {
                while (auto* request = settingsServer.nextPendingConnection()) {
                    request->deleteLater();
                    controller.openSettings();
                }
            });
        }
    }
    controller.start(!parser.isSet(settings));
    return app.exec();
}
