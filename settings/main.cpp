// Dusk Settings: the control panel for the Dusk desktop.
//
// Every change is written to ~/.config/dusk/dusk.conf straight away and Dusk
// applies it live. Settings that don't apply right now (e.g. picture options
// while the wallpaper is a colour) are hidden, and anything changed from its
// default gets a reset button.
#include <condition_variable>
#include <csignal>
#include <ctime>
#include <execinfo.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <future>
#include <map>
#include <mutex>
#include <pwd.h>
#include <QStandardPaths>
#include <QCryptographicHash>
#include "logo_svg.hpp"
#include <QAbstractButton>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QKeyEvent>
#include <QStyledItemDelegate>
#include <QConicalGradient>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QImageReader>
#include <QRegularExpression>
#include <QAbstractItemModel>
#include <QFileSystemWatcher>
#include <fcntl.h>
#include <sys/file.h>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>
#include <QWindow>
#include <xkbcommon/xkbcommon.h>
#include <QDir>
#include <QDate>
#include <QLocale>
#include <QTime>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QShortcut>
#include <QBuffer>
#include <QInputDialog>
#include <QStorageInfo>
#include <QSysInfo>
#include <QClipboard>
#include <QRegularExpression>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRadialGradient>
#include <QProcess>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QCheckBox>
#include <QFontDatabase>
#include <QRandomGenerator>
#include <QScreen>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <QFontDatabase>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "settings_file.hpp"

namespace {

// The version, from meson.build (so it can't fall behind).
#ifdef DUSK_VERSION
const char *kVersion = DUSK_VERSION;
#else
const char *kVersion = "0.33.0";
#endif

SettingsFile *settings = nullptr;

// ============================================================================
// Look: the same neutral light/dark theme as the desktop, with its accent
// colour and its window border and shadow style.
// ============================================================================

// The interface font: yours, or the best UI font installed. Adwaita Sans
// (GNOME's, from Inter) and Inter are drawn for screens and stay sharp at
// small sizes; Google Sans (Dusk's old default) was soft at 1x.
// In Dusk, Settings draws its own header bar (like GNOME apps do) instead
// of sitting under Dusk's title bar.
bool g_csd = false;

QString ui_font() {
	const QString chosen = settings->get("font");
	if (!chosen.isEmpty() && chosen != "Google Sans") return chosen;
	static QString best;
	if (best.isEmpty()) {
		const QStringList have = QFontDatabase::families();
		for (const char *f : {"Adwaita Sans", "Inter", "Inter Variable", "Noto Sans", "Cantarell"}) {
			if (have.contains(f)) {
				best = f;
				break;
			}
		}
		if (best.isEmpty()) best = chosen.isEmpty() ? QString("Sans Serif") : chosen;
	}
	return best;
}

struct Look {
	QColor window, card, text, dim, line, field, hover, accent, on_accent, shadow;
	QColor side;         // the sidebar, a shade off the page
	int border = 2;      // 0, 1, 2 (ink)
	int shadow_kind = 1; // 0 none, 1 soft, 2 ink
	bool dark = true;
	int radius = 16;     // cards
};

Look current_look() {
	Look l;
	const bool dark = settings->get("theme", "dark") != "light";
	l.dark = dark;
	// 0.35: flat and quiet. Thin hairlines and no drop shadows on cards,
	// whatever the desktop's outline style (ink lines were heavy here); the
	// corners follow Settings › Appearance › Roundness.
	l.border = 1;
	l.shadow_kind = 0;
	// Cards sit inside the window: a little less round than the desktop's
	// panels (concentric corners), whatever Roundness says.
	l.radius = 12;   // libadwaita's boxed lists
	if (dark) {
		// libadwaita's dark greys (as GNOME apps): window #222226, sidebar
		// #2e2e32, boxed lists 8% white over the window.
		l.window = QColor("#222226");
		l.card = QColor("#343438");
		l.text = QColor("#ffffff");
		l.dim = QColor("#a0a0a4");
		l.line = QColor(255, 255, 255, 22);
		l.field = QColor("#3e3e42");
		l.hover = QColor(255, 255, 255, 16);
		l.shadow = QColor(0, 0, 0, 0);
		l.side = QColor("#2e2e32");
	} else {
		// libadwaita's light: window #fafafb, sidebar #ebebed, white cards.
		l.window = QColor("#fafafb");
		l.card = QColor("#ffffff");
		l.text = QColor("#2e2e33");
		l.dim = QColor("#6e6e73");
		l.line = QColor(0, 0, 6, 26);
		l.field = QColor("#ebebed");
		l.hover = QColor(0, 0, 6, 14);
		l.shadow = QColor(0, 0, 0, 0);
		l.side = QColor("#ebebed");
	}
	l.accent = QColor(settings->get("accent", "#4d8df6"));
	if (!l.accent.isValid()) {
		l.accent = QColor("#4d8df6");
	}
	// White or near-black, whichever has more contrast on the accent.
	auto lin = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
	const double L = 0.2126 * lin(l.accent.redF()) + 0.7152 * lin(l.accent.greenF()) + 0.0722 * lin(l.accent.blueF());
	l.on_accent = 1.05 / (L + 0.05) >= (L + 0.05) / 0.0625 ? QColor("#ffffff") : QColor("#1c1c21");
	return l;
}

QString css_rgba(const QColor &c) {
	return QString("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue())
		.arg(QString::number(c.alphaF(), 'f', 3));
}

// ---- Dusk's own icons ---------------------------------------------------------
// Drawn here, like the desktop draws its own (app icons still come from the
// icon theme). A 16 x 16 grid centred on 0,0; 1.6 px round-ended lines.

void icon_paint(QPainter &p, const QString &name, const QColor &c) {
	QPen pen(c, 1.75, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	auto solid = [&p, &c] { p.setPen(Qt::NoPen); p.setBrush(c); };
	auto stroke = [&p, &pen] { p.setPen(pen); p.setBrush(Qt::NoBrush); };
	auto rrect = [&p](double x, double y, double w, double h, double r) {
		p.drawRoundedRect(QRectF(x, y, w, h), r, r);
	};
	if (name == "appearance") {
		// A palette.
		QPainterPath path;
		path.moveTo(0, -7);
		path.cubicTo(4.5, -7, 7.4, -4, 7.4, -0.4);
		path.cubicTo(7.4, 2.6, 5.2, 3.2, 3.6, 2.8);
		path.cubicTo(2.0, 2.4, 1.0, 3.6, 2.0, 5.0);
		path.cubicTo(2.8, 6.2, 1.6, 7.2, 0, 7.2);
		path.cubicTo(-4.2, 7.2, -7.4, 4, -7.4, 0);
		path.cubicTo(-7.4, -4, -4.2, -7, 0, -7);
		p.drawPath(path);
		solid();
		for (const QPointF &dot : {QPointF(-3.8, 1.2), QPointF(-3.0, -2.8), QPointF(0.8, -4.0), QPointF(4.0, -1.8)}) {
			p.drawEllipse(dot, 1.25, 1.25);
		}
	} else if (name == "windows") {
		rrect(-7.2, -4.2, 11, 10.6, 2);
		p.drawLine(QPointF(-7.2, -1.4), QPointF(3.8, -1.4));
		p.drawPolyline(QPolygonF({QPointF(-4.2, -6.8), QPointF(5.4, -6.8), QPointF(7.2, -6.8), QPointF(7.2, 3.8)}));
	} else if (name == "panels") {
		rrect(-7.4, -6.4, 14.8, 12.8, 2.4);
		solid();
		rrect(-5.4, 1.6, 10.8, 3, 1.5);
	} else if (name == "controlcentre") {
		// Two switches.
		rrect(-7.2, -6.6, 14.4, 5.6, 2.8);
		rrect(-7.2, 1.0, 14.4, 5.6, 2.8);
		solid();
		p.drawEllipse(QPointF(3.6, -3.8), 1.7, 1.7);
		p.drawEllipse(QPointF(-3.6, 3.8), 1.7, 1.7);
	} else if (name == "notifications") {
		solid();
		QPainterPath bell;
		bell.moveTo(-6, 4.2);
		bell.cubicTo(-4.6, 2.6, -4.8, 0.5, -4.8, -1.2);
		bell.cubicTo(-4.8, -4.6, -2.6, -6.4, 0, -6.4);
		bell.cubicTo(2.6, -6.4, 4.8, -4.6, 4.8, -1.2);
		bell.cubicTo(4.8, 0.5, 4.6, 2.6, 6, 4.2);
		bell.closeSubpath();
		p.drawPath(bell);
		p.drawChord(QRectF(-1.9, 3.5, 3.8, 3.8), 180 * 16, 180 * 16);
	} else if (name == "animations") {
		// A ball with motion lines.
		solid();
		p.drawEllipse(QPointF(3.0, 0), 4.2, 4.2);
		stroke();
		p.drawLine(QPointF(-7.2, -3), QPointF(-3.4, -3));
		p.drawLine(QPointF(-7.2, 0), QPointF(-2.6, 0));
		p.drawLine(QPointF(-7.2, 3), QPointF(-3.4, 3));
	} else if (name == "osd") {
		// A speaker over a level bar, like the on-screen display itself.
		solid();
		QPainterPath spk;
		spk.moveTo(-4.6, -5.4);
		spk.lineTo(-2.8, -5.4);
		spk.lineTo(0.2, -7.6);
		spk.lineTo(0.2, 0.0);
		spk.lineTo(-2.8, -2.2);
		spk.lineTo(-4.6, -2.2);
		spk.closeSubpath();
		p.drawPath(spk);
		stroke();
		p.drawArc(QRectF(-1.6, -6.6, 5.2, 5.2), -50 * 16, 100 * 16);
		rrect(-7.2, 3.0, 14.4, 4.4, 2.2);
		solid();
		rrect(-5.6, 4.6, 7.0, 1.2, 0.6);
	} else if (name == "pointer") {
		rrect(-4.4, -7.2, 8.8, 14.4, 4.4);
		p.drawLine(QPointF(0, -7.2), QPointF(0, -2.4));
		p.drawLine(QPointF(-4.4, -2.4), QPointF(4.4, -2.4));
	} else if (name == "media") {
		// A music note.
		solid();
		p.drawEllipse(QPointF(-3.6, 4.4), 2.6, 2.1);
		p.drawEllipse(QPointF(4.4, 2.8), 2.6, 2.1);
		stroke();
		p.drawPolyline(QPolygonF({QPointF(-1.1, 4.2), QPointF(-1.1, -5.8), QPointF(6.9, -7.4), QPointF(6.9, 2.6)}));
	} else if (name == "screenshots") {
		// Corners like the maximise button, round a lens.
		QPainterPath frame;
		const double e = 6.8, len = 3.6, r = 2.0;
		for (int i = 0; i < 4; i++) {
			const double sx = i % 2 ? -1 : 1, sy = i / 2 ? -1 : 1;
			const double x = i % 2 ? e : -e, y = i / 2 ? e : -e;
			frame.moveTo(x, y + sy * len);
			frame.lineTo(x, y + sy * r);
			frame.quadTo(x, y, x + sx * r, y);
			frame.lineTo(x + sx * len, y);
		}
		p.drawPath(frame);
		p.drawEllipse(QPointF(0, 0), 2.4, 2.4);
	} else if (name == "keyboard") {
		rrect(-7.6, -4.8, 15.2, 9.6, 2.2);
		solid();
		for (int i = 0; i < 4; i++) p.drawEllipse(QPointF(-4.2 + i * 2.8, -1.8), 0.9, 0.9);
		rrect(-3.6, 1.2, 7.2, 1.6, 0.8);
	} else if (name == "displays") {
		rrect(-7.4, -6.4, 14.8, 10.2, 2);
		p.drawLine(QPointF(-3, 7), QPointF(3, 7));
		p.drawLine(QPointF(0, 3.8), QPointF(0, 7));
	} else if (name == "apps") {
		solid();
		for (int i = 0; i < 4; i++) rrect(i % 2 ? 1.2 : -6.8, i / 2 ? 1.2 : -6.8, 5.6, 5.6, 1.8);
	} else if (name == "startup") {
		// A rocket.
		QPainterPath body;
		body.moveTo(0, -7.4);
		body.cubicTo(3.2, -5.0, 3.4, -0.6, 2.4, 3.2);
		body.lineTo(-2.4, 3.2);
		body.cubicTo(-3.4, -0.6, -3.2, -5.0, 0, -7.4);
		body.closeSubpath();
		p.drawPath(body);
		p.drawLine(QPointF(-2.6, 0.6), QPointF(-5.2, 3.8));
		p.drawLine(QPointF(2.6, 0.6), QPointF(5.2, 3.8));
		p.drawLine(QPointF(0, 5.4), QPointF(0, 7.4));
		solid();
		p.drawEllipse(QPointF(0, -2.4), 1.2, 1.2);
	} else if (name == "power") {
		rrect(-7.6, -4.6, 13.6, 9.2, 2.6);
		solid();
		rrect(6.6, -2, 1.6, 4, 0.8);
		QPainterPath bolt;
		bolt.moveTo(0.6, -3.6);
		bolt.lineTo(-2.6, 0.6);
		bolt.lineTo(-0.4, 0.6);
		bolt.lineTo(-1.0, 3.6);
		bolt.lineTo(2.4, -0.8);
		bolt.lineTo(0.2, -0.8);
		bolt.closeSubpath();
		p.drawPath(bolt);
	} else if (name == "language" || name == "globe") {
		// A globe.
		p.drawEllipse(QPointF(0, 0), 7, 7);
		p.drawEllipse(QPointF(0, 0), 3, 7);
		p.drawLine(QPointF(-7, 0), QPointF(7, 0));
	} else if (name == "datetime" || name == "clock") {
		p.drawEllipse(QPointF(0, 0), 7, 7);
		p.drawPolyline(QPolygonF({QPointF(0, -4), QPointF(0, 0), QPointF(3, 2)}));
	} else if (name == "about" || name == "info") {
		p.drawEllipse(QPointF(0, 0), 7, 7);
		solid();
		p.drawEllipse(QPointF(0, -3.4), 1.1, 1.1);
		stroke();
		p.drawLine(QPointF(0, -0.6), QPointF(0, 4.0));
	} else if (name == "desktop") {
		// A folder on a screen.
		rrect(-7.4, -6.4, 14.8, 10.2, 2);
		p.drawLine(QPointF(-3, 7), QPointF(3, 7));
		solid();
		QPainterPath f;
		f.moveTo(-3.6, -3.4);
		f.lineTo(-1.2, -3.4);
		f.lineTo(-0.2, -2.4);
		f.lineTo(3.6, -2.4);
		f.lineTo(3.6, 1.6);
		f.lineTo(-3.6, 1.6);
		f.closeSubpath();
		p.drawPath(f);
	} else if (name == "reset") {
		// Undo: a hook arrow pointing back.
		QPainterPath path;
		path.moveTo(6.2, 5.8);
		path.cubicTo(6.2, 0.2, 3.4, -2.4, -1.0, -2.4);
		path.lineTo(-5.8, -2.4);
		p.drawPath(path);
		p.drawPolyline(QPolygonF({QPointF(-2.6, -5.8), QPointF(-6.0, -2.4), QPointF(-2.6, 1.0)}));
	} else if (name == "close") {
		p.drawLine(QPointF(-4.6, -4.6), QPointF(4.6, 4.6));
		p.drawLine(QPointF(4.6, -4.6), QPointF(-4.6, 4.6));
	} else if (name == "w:menu") {
		solid();
		for (int r = 0; r < 3; r++)
			for (int q = 0; q < 3; q++) p.drawEllipse(QPointF((q - 1) * 4.6, (r - 1) * 4.6), 1.6, 1.6);
	} else if (name == "w:tasks") {
		rrect(-7.4, -2.6, 6.4, 6.4, 1.6);
		rrect(1.0, -2.6, 6.4, 6.4, 1.6);
		p.drawLine(QPointF(-6, 6.4), QPointF(-2.4, 6.4));
	} else if (name == "w:pins") {
		QPainterPath pin;
		pin.moveTo(-3.6, -6.8);
		pin.lineTo(3.6, -6.8);
		pin.lineTo(2.6, -1.8);
		pin.lineTo(4.8, 1.2);
		pin.lineTo(-4.8, 1.2);
		pin.lineTo(-2.6, -1.8);
		pin.closeSubpath();
		p.drawPath(pin);
		p.drawLine(QPointF(0, 1.2), QPointF(0, 7.2));
	} else if (name == "w:spacer") {
		p.drawLine(QPointF(-6.8, 0), QPointF(6.8, 0));
		p.drawPolyline(QPolygonF({QPointF(-4, -2.8), QPointF(-6.8, 0), QPointF(-4, 2.8)}));
		p.drawPolyline(QPolygonF({QPointF(4, -2.8), QPointF(6.8, 0), QPointF(4, 2.8)}));
	} else if (name == "w:separator") {
		p.drawLine(QPointF(0, -7), QPointF(0, 7));
	} else if (name == "w:status") {
		p.drawArc(QRectF(-7.4, -1.8, 9.6, 9.6), 45 * 16, 90 * 16);
		p.drawArc(QRectF(-5.2, 0.4, 5.2, 5.2), 45 * 16, 90 * 16);
		solid();
		p.drawEllipse(QPointF(-2.6, 3.0), 1.2, 1.2);
		QPainterPath spk;
		spk.moveTo(2.0, -0.6);
		spk.lineTo(3.6, -0.6);
		spk.lineTo(6.0, -3.0);
		spk.lineTo(6.0, 5.6);
		spk.lineTo(3.6, 3.2);
		spk.lineTo(2.0, 3.2);
		spk.closeSubpath();
		p.drawPath(spk);
	} else if (name == "w:media") {
		// A note.
		solid();
		p.drawEllipse(QPointF(-3.0, 4.2), 2.6, 2.1);
		stroke();
		p.drawLine(QPointF(-0.6, 4.0), QPointF(-0.6, -6.6));
		p.drawLine(QPointF(-0.6, -6.6), QPointF(5.0, -4.6));
	} else if (name == "w:clipboard") {
		rrect(-6, -5.6, 12, 13, 2.2);
		solid();
		rrect(-3, -7.6, 6, 3.6, 1.4);
	} else if (name == "w:overview" || name == "workspaces") {
		// GNOME's workspace dots: one long, the others round.
		solid();
		p.drawRoundedRect(QRectF(-7.6, -1.6, 7.4, 3.2), 1.6, 1.6);
		p.drawEllipse(QPointF(2.6, 0), 1.6, 1.6);
		p.drawEllipse(QPointF(6.6, 0), 1.6, 1.6);
	} else if (name == "w:keyboard") {
		p.drawRoundedRect(QRectF(-7, -4.6, 14, 9.2), 2, 2);
		for (int r = 0; r < 2; r++) {
			for (int k = 0; k < 4; k++) p.drawPoint(QPointF(-4.2 + k * 2.8, -2.0 + r * 2.4));
		}
		p.drawLine(QPointF(-3, 2.6), QPointF(3, 2.6));
	} else if (name == "w:trash") {
		p.drawLine(QPointF(-6.4, -4.4), QPointF(6.4, -4.4));
		p.drawPolyline(QPolygonF({QPointF(-2.4, -4.4), QPointF(-2.4, -6.6), QPointF(2.4, -6.6), QPointF(2.4, -4.4)}));
		QPainterPath bin;
		bin.moveTo(-5, -4.4);
		bin.lineTo(-4.2, 6.6);
		bin.lineTo(4.2, 6.6);
		bin.lineTo(5, -4.4);
		p.drawPath(bin);
		p.drawLine(QPointF(-1.4, -1.4), QPointF(-1.4, 3.6));
		p.drawLine(QPointF(1.4, -1.4), QPointF(1.4, 3.6));
	} else if (name == "w:tray") {
		solid();
		p.drawEllipse(QPointF(-4.6, 0), 2, 2);
		p.drawEllipse(QPointF(0, 0), 2, 2);
		stroke();
		p.drawEllipse(QPointF(4.6, 0), 2, 2);
	} else if (name == "w:showdesktop") {
		rrect(-7.4, -6.4, 14.8, 10.2, 2);
		p.drawLine(QPointF(-3, 7), QPointF(3, 7));
		p.drawLine(QPointF(0, -3.6), QPointF(0, 1.2));
		p.drawPolyline(QPolygonF({QPointF(-2.4, -1.2), QPointF(0, 1.4), QPointF(2.4, -1.2)}));
	} else if (name == "folder") {
		solid();
		QPainterPath f;
		f.moveTo(-7.4, -4.4);
		f.cubicTo(-7.4, -5.6, -6.6, -6.4, -5.4, -6.4);
		f.lineTo(-2.2, -6.4);
		f.lineTo(-0.4, -4.6);
		f.lineTo(5.4, -4.6);
		f.cubicTo(6.6, -4.6, 7.4, -3.8, 7.4, -2.6);
		f.lineTo(7.4, 4.6);
		f.cubicTo(7.4, 5.8, 6.6, 6.6, 5.4, 6.6);
		f.lineTo(-5.4, 6.6);
		f.cubicTo(-6.6, 6.6, -7.4, 5.8, -7.4, 4.6);
		f.closeSubpath();
		p.drawPath(f);
	} else if (name == "foryou") {
		// A four-pointed sparkle and a small one.
		QPainterPath st;
		st.moveTo(-1.5, -7);
		st.cubicTo(-1, -2.5, -0.5, -2, 3.5, -1.5);
		st.cubicTo(-0.5, -1, -1, -0.5, -1.5, 4);
		st.cubicTo(-2, -0.5, -2.5, -1, -6.5, -1.5);
		st.cubicTo(-2.5, -2, -2, -2.5, -1.5, -7);
		p.drawPath(st);
		p.drawLine(QPointF(4.6, 3), QPointF(4.6, 7));
		p.drawLine(QPointF(2.6, 5), QPointF(6.6, 5));
	} else if (name == "phone") {
		// A phone: tall, rounded, a dot at the bottom.
		rrect(-4.6, -7.4, 9.2, 14.8, 2.2);
		solid();
		p.drawEllipse(QPointF(0, 4.6), 0.9, 0.9);
	} else if (name == "lockscreen") {
		// A padlock.
		QPainterPath shackle;
		shackle.moveTo(-3.8, -1.2);
		shackle.lineTo(-3.8, -3.4);
		shackle.cubicTo(-3.8, -5.6, -2.1, -7.2, 0, -7.2);
		shackle.cubicTo(2.1, -7.2, 3.8, -5.6, 3.8, -3.4);
		shackle.lineTo(3.8, -1.2);
		p.drawPath(shackle);
		rrect(-6.2, -1.2, 12.4, 8.4, 2.2);
		solid();
		p.drawEllipse(QPointF(0, 2.8), 1.2, 1.2);
	} else {
		p.drawEllipse(QPointF(0, 0), 5, 5);
	}
}

// One of Dusk's icons, `size` px, sharp on HiDPI.
QIcon dusk_icon(const QString &name, const QColor &color, int size = 18) {
	const qreal dpr = std::max<qreal>(2.0, qApp ? qApp->devicePixelRatio() : 2.0);
	QPixmap pm(QSize(size, size) * dpr);
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.translate(size / 2.0, size / 2.0);
	p.scale(size / 16.0, size / 16.0);
	icon_paint(p, name, color);
	p.end();
	return QIcon(pm);
}

// Run after the look changed (repaint previews, swatches...). A hook can
// belong to a widget: once that's gone (a rebuilt page: Panels rebuilds its
// rows whenever the settings file changes) it's dropped instead of being
// called on a deleted widget, which closed Settings, most often right after
// changing the wallpaper (that runs every hook). And hooks may add hooks
// while they run.
class Hooks {
public:
	void push_back(std::function<void()> f) { v_.push_back(H{nullptr, false, std::move(f)}); }
	void add(QObject *owner, std::function<void()> f) { v_.push_back(H{owner, true, std::move(f)}); }
	void run() {
		v_.erase(std::remove_if(v_.begin(), v_.end(), [](const H &h) { return h.owned && !h.owner; }), v_.end());
		const size_t n = v_.size();
		for (size_t i = 0; i < n && i < v_.size(); i++) {
			if (v_[i].owned && !v_[i].owner) continue;
			const std::function<void()> f = v_[i].f;   // (the list can grow while it runs)
			if (f) f();
		}
	}

private:
	struct H {
		QPointer<QObject> owner;
		bool owned;
		std::function<void()> f;
	};
	std::vector<H> v_;
};
Hooks restyle_hooks;

bool settings_translucent = false;   // opened see-through (Appearance › Settings app)

void apply_look(QApplication &app) {
	const Look l = current_look();
	// Nothing about the look changed (a slider for something else): leave
	// everything as it is. Restyling every widget and redrawing every icon
	// on each change is what made Settings lag.
	static QString last_key;
	const QString look_key = QStringList{l.window.name(), l.card.name(), l.text.name(), l.dim.name(), l.line.name(QColor::HexArgb),
		l.field.name(), l.hover.name(QColor::HexArgb), l.accent.name(), l.on_accent.name(), QString::number(l.border),
		QString::number(l.shadow_kind), QString::number(l.radius), ui_font(),
		settings->get("font_size", "11"), settings->get("settings_opacity", "100")}.join('|');
	if (look_key == last_key) return;
	last_key = look_key;
	static bool styled = false;
	if (!styled) {
		app.setStyle(QStyleFactory::create("Fusion"));
		styled = true;
	}
	QPalette p;
	p.setColor(QPalette::Window, l.window);
	p.setColor(QPalette::WindowText, l.text);
	p.setColor(QPalette::Base, l.field);
	p.setColor(QPalette::AlternateBase, l.card);
	p.setColor(QPalette::ToolTipBase, l.card);
	p.setColor(QPalette::ToolTipText, l.text);
	p.setColor(QPalette::Text, l.text);
	p.setColor(QPalette::PlaceholderText, l.dim);
	p.setColor(QPalette::Button, l.field);
	p.setColor(QPalette::ButtonText, l.text);
	p.setColor(QPalette::Highlight, l.accent);
	p.setColor(QPalette::HighlightedText, l.on_accent);
	p.setColor(QPalette::Link, l.accent);
	p.setColor(QPalette::Disabled, QPalette::Text, l.dim);
	p.setColor(QPalette::Disabled, QPalette::WindowText, l.dim);
	p.setColor(QPalette::Disabled, QPalette::ButtonText, l.dim);
	app.setPalette(p);

	// See-through (Appearance › Settings app): only when the window was opened
	// that way (Qt can't switch it on later); the cards stay more solid.
	const double see = settings_translucent
		? std::clamp(settings->get("settings_opacity", "100").toInt(), 20, 100) / 100.0 : 1.0;
	QColor wc = l.window, cc = l.card;
	wc.setAlphaF(see);
	cc.setAlphaF(std::min(1.0, see + 0.18));
	const QString window_css = see < 1 ? css_rgba(wc) : l.window.name();
	const QString card_css = see < 1 ? css_rgba(cc) : l.card.name();
	const QString card_border = l.border > 0
		? QString("%1px solid %2").arg(l.border).arg(css_rgba(l.line)) : QString("none");
	QString css = R"(
		QWidget { color: TEXT; font-family: "FONTFAMILY"; font-size: 11pt; }
		QWidget#root { background: WINDOW; }
		QScrollArea, QScrollArea > QWidget > QWidget { background: PANE; border: none; }
		QLabel#subtitle, QLabel#hint { color: DIM; font-size: 10pt; }
		QLabel#heading { font-size: 15pt; font-weight: 800; }
		QWidget#headerbar { background: transparent; }
		QLabel#headertitle { font-weight: 700; }
		QToolButton#winbutton {
			background: FIELD; border: none; border-radius: 12px;
			min-width: 24px; max-width: 24px; min-height: 24px; max-height: 24px;
		}
		QToolButton#winbutton:hover { background: HOVER2; }
		QLabel#crumb { color: DIM; font-size: 10pt; }
		QLabel#cardtitle { font-weight: 700; font-size: 11pt; padding-left: 2px; }
		QFrame#card { background: CARD; border: CARDBORDER; border-radius: RADIUSpx; }
		QFrame#divider { background: HOVER; max-height: 1px; min-height: 1px; border: none; }
		QPushButton {
			background: FIELD; border: none; border-radius: 9px; padding: 7px 16px; font-weight: 600;
			min-height: 20px;
		}
		QPushButton:hover { background: HOVER2; }
		QPushButton:checked { background: ACCENT; color: ONACCENT; font-weight: 600; }
		QPushButton#primary { background: ACCENT; color: ONACCENT; font-weight: 600; border: none; }
		QPushButton#primary:disabled { background: FIELD; color: DIM; }
		QPushButton#tile {
			background: transparent; border: 1px solid LINE; border-radius: 14px; padding: 0; text-align: left;
			min-height: 62px; max-height: 62px;
		}
		QPushButton#tile:hover { background: HOVER; }
		QLabel#tiletitle { font-weight: 600; }
		QLabel#bigname { font-size: 15pt; font-weight: 700; }
		QToolButton#reset {
			background: transparent; border: none; border-radius: 13px;
			min-width: 26px; max-width: 26px; min-height: 26px; max-height: 26px;
		}
		QToolButton#reset:hover { background: HOVER2; }
		QToolButton#cardreset {
			background: transparent; border: none; border-radius: 12px; padding: 3px 10px;
			color: DIM; font-size: 10pt; font-weight: 500;
		}
		QToolButton#cardreset:hover { background: HOVER2; }
		QLineEdit#search { border-radius: 9px; padding: 7px 12px; }
		QListWidget#results {
			background: CARD; border: 1px solid HOVER2; border-radius: 12px; padding: 4px; outline: 0;
		}
		QListWidget#results::item { padding: 6px 10px; border-radius: 8px; }
		QListWidget#results::item:selected { background: ACCENT; color: ONACCENT; }
		QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox {
			background: FIELD; border: none; border-radius: 9px; padding: 6px 12px;
			min-height: 22px; selection-background-color: ACCENT; selection-color: ONACCENT;
		}
		QComboBox:hover, QSpinBox:hover, QDoubleSpinBox:hover { background: HOVER2; }
		QComboBox { combobox-popup: 0; }
		QComboBox::drop-down { border: none; width: 24px; }
		QComboBox QAbstractItemView {
			background: CARD; border: 1px solid HOVER2; border-radius: 12px; padding: 5px;
			selection-background-color: ACCENT; selection-color: ONACCENT; outline: 0;
		}
		QComboBox QAbstractItemView::item {
			min-height: 30px; padding: 0 10px; border-radius: 8px; border: none;
		}
		QComboBox QAbstractItemView::item:hover { background: HOVER; }
		QComboBox QAbstractItemView::item:selected { background: ACCENT; color: ONACCENT; }
		QToolButton#step {
			background: FIELD; border: none; border-radius: 15px; font-size: 13pt;
			min-width: 30px; max-width: 30px; min-height: 30px; max-height: 30px;
		}
		QToolButton#step:hover { background: HOVER2; }
		QToolButton#step:disabled { color: DIM; }
		QLabel#stepvalue { background: FIELD; border-radius: 12px; padding: 6px 0; }
		QSpinBox::up-button, QSpinBox::down-button { width: 0; border: none; }
		QLabel#slidervalue {
			background: FIELD; border-radius: 10px; padding: 3px 6px; color: TEXT; font-size: 9pt;
		}
		QWidget#side { background: SIDE; border-right: 1px solid LINE; }
		QListWidget#sidebar { background: transparent; border: none; padding: 4px 10px 10px; outline: 0; }
		QListWidget#sidebar::item { background: transparent; border: none; }
		QListWidget#editor {
			background: FIELD; border: none; border-radius: 12px; padding: 4px; outline: 0;
		}
		QListWidget#editor::item { padding: 7px 8px; border-radius: 8px; }
		QListWidget#editor::item:hover:!selected { background: HOVER; }
		QListWidget#editor::item:selected { background: ACCENTSOFT; color: TEXT; }
		QFrame#phonerow { background: FIELD; border-radius: 12px; }
		QPushButton#danger { background: DANGERSOFT; color: DANGER; font-weight: 600; }
		QPushButton#danger:hover { background: DANGER; color: #ffffff; }
		QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
		QScrollBar::handle:vertical { background: HOVER2; border-radius: 3px; min-height: 30px; }
		QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
		QScrollBar::handle:horizontal { background: HOVER2; border-radius: 3px; min-width: 30px; }
		QScrollBar::handle:hover { background: DIM; }
		QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; border: none; background: none; }
		QScrollBar::add-page, QScrollBar::sub-page { background: none; }
	)";
	QColor hover2 = l.text;
	hover2.setAlphaF(0.14);
	QColor accent_soft = l.accent;
	accent_soft.setAlphaF(0.28);
	const QColor danger = settings->get("theme", "dark") != "light" ? QColor("#ff6b6b") : QColor("#c62828");
	QColor danger_soft = danger;
	danger_soft.setAlphaF(0.14);
	css.replace("ACCENTSOFT", css_rgba(accent_soft)).replace("DANGERSOFT", css_rgba(danger_soft))
		.replace("DANGER", danger.name());
	{
		// The interface font, in the style sheet itself: with a style sheet
		// set, Qt keeps each widget's family from when it was styled, so
		// changing only the app's font didn't reach this window.
		QString fam = ui_font();
		fam.remove('"').remove('\\').remove(';').remove('}');
		css.replace("FONTFAMILY", fam);
	}
	css.replace("SIDE", see < 1 ? css_rgba(QColor(l.side.red(), l.side.green(), l.side.blue(), static_cast<int>(255 * see))) : l.side.name())
		.replace("LINE", css_rgba(l.line));
	css.replace("CARDBORDER", card_border).replace("RADIUS", QString::number(l.radius))
		.replace("WINDOW", window_css).replace("PANE", see < 1 ? QString("transparent") : l.window.name())
		.replace("CARD", card_css)
		.replace("TEXT", l.text.name()).replace("DIM", l.dim.name())
		.replace("FIELD", l.field.name()).replace("ONACCENT", l.on_accent.name())
		.replace("ACCENT", l.accent.name()).replace("HOVER2", css_rgba(hover2))
		.replace("HOVER", css_rgba(l.hover));
	// The text size from Settings › Appearance › Fonts (11 pt: this app's 10).
	// Every size moves with it.
	{
		const int by = std::clamp(settings->get("font_size", "11").toInt(), 7, 20) - 11;
		static const QRegularExpression size_re("font-size: (\\d+)pt");
		QString out;
		qsizetype at = 0;
		for (auto it = size_re.globalMatch(css); it.hasNext();) {
			const QRegularExpressionMatch m = it.next();
			out += css.mid(at, m.capturedStart() - at);
			out += QString("font-size: %1pt").arg(std::max(6, m.captured(1).toInt() + by));
			at = m.capturedEnd();
		}
		css = out + css.mid(at);
	}
	// Setting the style sheet restyles every widget on every page (slow):
	// only when it really changed.
	static QString last_css;
	if (css != last_css) {
		last_css = css;
		app.setStyleSheet(css);
	}
	QFont font = app.font();
	const QString family = ui_font();
	// Smooth text: shapes kept (light vertical hinting only), always
	// antialiased, so it isn't blocky at normal viewing distance.
	if (font.family() != family || font.hintingPreference() != QFont::PreferVerticalHinting) {
		font.setFamily(family);
		font.setHintingPreference(QFont::PreferVerticalHinting);
		font.setStyleStrategy(QFont::PreferAntialias);
		app.setFont(font);
	}
	restyle_hooks.run();
}

QApplication *the_app = nullptr;

// ============================================================================
// Building blocks
// ============================================================================

// An on/off switch, drawn like the desktop's control centre switches.
class Switch : public QAbstractButton {
public:
	explicit Switch(QWidget *parent = nullptr) : QAbstractButton(parent) {
		setCheckable(true);
		setCursor(Qt::PointingHandCursor);
		setFixedSize(46, 26);
		anim_.setDuration(260);
		anim_.setEasingCurve(QEasingCurve::OutCubic);
		QObject::connect(&anim_, &QVariantAnimation::valueChanged, [this](const QVariant &v) {
			t_ = v.toDouble();
			update();
		});
		QObject::connect(this, &QAbstractButton::toggled, [this](bool on) {
			if (quiet_) {
				t_ = on ? 1.0 : 0.0;
				update();
				return;
			}
			anim_.stop();
			anim_.setStartValue(t_);
			anim_.setEndValue(on ? 1.0 : 0.0);
			anim_.start();
		});
	}
	QSize sizeHint() const override { return QSize(46, 26); }
	// Set without animating (when the value comes from the file).
	void set_quietly(bool on) {
		quiet_ = true;
		setChecked(on);
		quiet_ = false;
		t_ = on ? 1.0 : 0.0;
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override {
		const Look l = current_look();
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		QColor off = l.dim;
		off.setAlphaF(0.45);
		QColor track(
			static_cast<int>(off.red() + (l.accent.red() - off.red()) * t_),
			static_cast<int>(off.green() + (l.accent.green() - off.green()) * t_),
			static_cast<int>(off.blue() + (l.accent.blue() - off.blue()) * t_));
		track.setAlphaF(0.45 + 0.55 * t_);
		if (!isEnabled()) track.setAlphaF(0.25);
		p.setBrush(track);
		p.drawRoundedRect(QRectF(1, 1, 44, 24), 12, 12);
		// Half way, the knob stretches into a pill as if pulled across.
		const double stretch = 9 * std::sin(M_PI * t_);
		const double kx = 4 + t_ * 20 - stretch * t_;
		p.setBrush(t_ > 0.5 ? l.on_accent : QColor("#ffffff"));
		p.drawRoundedRect(QRectF(kx, 4, 18 + stretch, 18), 9, 9);
	}

private:
	QVariantAnimation anim_;
	double t_ = 0;
	bool quiet_ = false;
};

// A slider drawn like the rest of the app (and the control centre's): a
// rounded track filled with the accent up to the value, and a round knob
// with a soft shadow that grows a little when pointed at or dragged. A click
// anywhere on the track jumps there; the wheel and the arrow keys step it.
class Slider : public QSlider {
public:
	Slider() : QSlider(Qt::Horizontal) {
		setCursor(Qt::PointingHandCursor);
		setFixedHeight(30);
		setMinimumWidth(80);
		setAttribute(Qt::WA_Hover);
		setFocusPolicy(Qt::StrongFocus);
		anim_.setDuration(140);
		QObject::connect(&anim_, &QVariantAnimation::valueChanged, [this](const QVariant &v) {
			grow_ = v.toDouble();
			update();
		});
	}
	QSize sizeHint() const override { return QSize(200, 30); }
	QSize minimumSizeHint() const override { return QSize(80, 30); }

protected:
	double knob_x(int v) const {
		const double r = 11, w = width() - 2 * r - 2;
		const double f = maximum() > minimum() ? double(v - minimum()) / (maximum() - minimum()) : 0;
		return 1 + r + f * w;
	}
	int value_at(double x) const {
		const double r = 11, w = width() - 2 * r - 2;
		const double f = std::clamp((x - 1 - r) / std::max(1.0, w), 0.0, 1.0);
		return minimum() + static_cast<int>(std::lround(f * (maximum() - minimum())));
	}
	void paintEvent(QPaintEvent *) override {
		const Look l = current_look();
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		const double cy = height() / 2.0, th = 6, kx = knob_x(value());
		QColor track = l.text;
		track.setAlphaF(isEnabled() ? 0.16 : 0.08);
		p.setBrush(track);
		p.drawRoundedRect(QRectF(1, cy - th / 2, width() - 2, th), th / 2, th / 2);
		QColor fill = l.accent;
		if (!isEnabled()) fill.setAlphaF(0.4);
		p.setBrush(fill);
		p.drawRoundedRect(QRectF(1, cy - th / 2, std::max(th, kx), th), th / 2, th / 2);
		// The knob: white with a soft shadow, an accent ring when it has the keyboard.
		const double r = 8 + 2.5 * grow_;
		for (int i = 3; i >= 1; i--) {
			p.setBrush(QColor(0, 0, 0, 14 * (4 - i)));
			p.drawEllipse(QPointF(kx, cy + 1), r + i, r + i);
		}
		if (hasFocus()) {
			QColor ring = l.accent;
			ring.setAlphaF(0.35);
			p.setBrush(ring);
			p.drawEllipse(QPointF(kx, cy), r + 4, r + 4);
		}
		p.setBrush(isEnabled() ? QColor("#ffffff") : l.dim);
		p.drawEllipse(QPointF(kx, cy), r, r);
		p.setBrush(fill);
		p.drawEllipse(QPointF(kx, cy), 3, 3);
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) return QSlider::mousePressEvent(e);
		dragging_ = true;
		setSliderDown(true);
		setValue(value_at(e->position().x()));
		animate(1);
		e->accept();
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (dragging_) setValue(value_at(e->position().x()));
		e->accept();
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (dragging_) {
			dragging_ = false;
			setSliderDown(false);
			animate(underMouse() ? 0.6 : 0);
		}
		e->accept();
	}
	bool event(QEvent *e) override {
		if (e->type() == QEvent::HoverEnter) animate(dragging_ ? 1 : 0.6);
		if (e->type() == QEvent::HoverLeave && !dragging_) animate(0);
		return QSlider::event(e);
	}

private:
	void animate(double to) {
		anim_.stop();
		anim_.setStartValue(grow_);
		anim_.setEndValue(to);
		anim_.start();
	}
	QVariantAnimation anim_;
	double grow_ = 0;
	bool dragging_ = false;
};

// Chips you put in order: the panel's widgets, drawn like a panel. Drag one
// along to move it (the others make room as it goes), click one to pick it,
// the × on it takes it off; Left / Right pick, Ctrl+Left / Right move,
// Delete removes. The chips wrap onto more lines rather than scrolling.
//
// As a palette (`palette` true) the chips are what can be added: clicking
// one calls on_pick.
class ChipEditor : public QWidget {
public:
	struct Chip {
		QString id, label, icon;
		bool flexible = false;   // a flexible space: drawn as a dashed gap
	};
	std::function<void()> on_change;          // the order changed (editor)
	std::function<void(const QString &)> on_pick;   // a chip was clicked (palette)

	explicit ChipEditor(bool palette) : palette_(palette) {
		setMouseTracking(true);
		setAttribute(Qt::WA_Hover);
		setFocusPolicy(palette ? Qt::NoFocus : Qt::StrongFocus);
		QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
		sp.setHeightForWidth(true);
		setSizePolicy(sp);
		setMinimumWidth(160);
	}
	void set_chips(std::vector<Chip> chips) {
		chips_ = std::move(chips);
		if (selected_ >= static_cast<int>(chips_.size())) selected_ = -1;
		updateGeometry();
		update();
	}
	const std::vector<Chip> &chips() const { return chips_; }
	int selected() const { return selected_; }
	void set_empty_text(const QString &t) { empty_ = t; }

	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override {
		const auto r = place(w);
		double bottom = kRowH;
		for (const QRectF &q : r) bottom = std::max(bottom, q.bottom());
		return static_cast<int>(bottom + kPad) + (palette_ ? 0 : 0);
	}
	QSize sizeHint() const override { return QSize(420, heightForWidth(420)); }
	QSize minimumSizeHint() const override { return QSize(160, heightForWidth(std::max(width(), 160))); }

protected:
	static constexpr double kPad = 8, kGap = 6, kRowH = 36;

	double chip_width(const Chip &c) const {
		const QFontMetricsF fm(font());
		return 12 + 18 + 7 + fm.horizontalAdvance(c.label) + (palette_ ? 26 : 30);
	}
	std::vector<QRectF> place(int w) const {
		std::vector<QRectF> out;
		double x = kPad, y = kPad;
		const double room = w - 2 * kPad;
		for (const Chip &c : chips_) {
			const double cw = std::min(chip_width(c), room);
			if (x > kPad && x + cw > kPad + room) {
				x = kPad;
				y += kRowH + kGap;
			}
			out.emplace_back(x, y, cw, kRowH);
			x += cw + kGap;
		}
		return out;
	}
	QRectF close_rect(const QRectF &r) const { return QRectF(r.right() - 26, r.center().y() - 10, 20, 20); }
	int chip_at(const QPointF &pos) const {
		const auto r = place(width());
		for (size_t i = 0; i < r.size(); i++) {
			if (r[i].contains(pos)) return static_cast<int>(i);
		}
		return -1;
	}

	void paintEvent(QPaintEvent *) override {
		const Look l = current_look();
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		// The ground: like the panel (or a dashed tray for the palette).
		const QRectF all = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
		if (palette_) {
			QPen pen(l.dim, 1.2, Qt::DashLine);
			QColor pc = l.dim;
			pc.setAlphaF(0.45);
			pen.setColor(pc);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(all, 16, 16);
		} else {
			p.setPen(hasFocus() ? QPen(l.accent, 1.5) : Qt::NoPen);
			p.setBrush(l.field);
			p.drawRoundedRect(all, 16, 16);
		}
		const auto r = place(width());
		if (chips_.empty() && !empty_.isEmpty()) {
			p.setPen(l.dim);
			p.drawText(rect(), Qt::AlignCenter, empty_);
		}
		for (size_t i = 0; i < chips_.size(); i++) {
			if (static_cast<int>(i) == drag_ && moved_) continue;   // drawn last, under the pointer
			draw_chip(p, l, chips_[i], r[i], static_cast<int>(i));
		}
		if (drag_ >= 0 && moved_ && drag_ < static_cast<int>(chips_.size())) {
			// A gap where it will land, and the chip itself following the pointer.
			QColor gap = l.accent;
			gap.setAlphaF(0.18);
			p.setPen(QPen(l.accent, 1.2, Qt::DashLine));
			p.setBrush(gap);
			p.drawRoundedRect(r[drag_].adjusted(1, 1, -1, -1), 12, 12);
			QRectF at = r[drag_];
			at.moveTopLeft(drag_pos_ - grab_);
			for (int k = 4; k >= 1; k--) {
				p.setPen(Qt::NoPen);
				p.setBrush(QColor(0, 0, 0, 10 * (5 - k)));
				p.drawRoundedRect(at.adjusted(-k, -k + 3, k, k + 3), 12 + k, 12 + k);
			}
			draw_chip(p, l, chips_[drag_], at, drag_, true);
		}
	}

	void draw_chip(QPainter &p, const Look &l, const Chip &c, const QRectF &r, int i, bool lifted = false) {
		const bool sel = !palette_ && i == selected_;
		const bool hov = i == hover_ || lifted;
		QColor bg = sel ? l.accent : l.card;
		if (hov && !sel) bg = palette_ ? l.field : mix_colour(l.card, l.text, 0.06);
		const QColor fg = sel ? l.on_accent : l.text;
		p.setPen(Qt::NoPen);
		if (c.flexible && !sel) {
			QColor dash = l.dim;
			dash.setAlphaF(0.7);
			p.setPen(QPen(dash, 1.2, Qt::DashLine));
			p.setBrush(hov ? QBrush(bg) : QBrush(Qt::NoBrush));
		} else {
			p.setBrush(bg);
		}
		p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
		const QIcon icon = dusk_icon(c.icon, fg, 18);
		icon.paint(&p, QRect(static_cast<int>(r.x() + 12), static_cast<int>(r.center().y() - 9), 18, 18));
		p.setPen(fg);
		const QRectF tr(r.x() + 12 + 18 + 7, r.y(), r.width() - (12 + 18 + 7) - (palette_ ? 26 : 30), r.height());
		p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, QFontMetricsF(font()).elidedText(c.label, Qt::ElideRight, tr.width()));
		// × to take it off (editor) / + to add it (palette).
		const QRectF cr = close_rect(r);
		if (palette_ || hov || sel) {
			if (!palette_ && i == hover_ && hover_close_) {
				QColor h = fg;
				h.setAlphaF(0.18);
				p.setPen(Qt::NoPen);
				p.setBrush(h);
				p.drawEllipse(cr);
			}
			QColor mark = fg;
			mark.setAlphaF(palette_ ? 0.75 : 0.8);
			p.setPen(QPen(mark, 1.6, Qt::SolidLine, Qt::RoundCap));
			const QPointF m = cr.center();
			if (palette_) {
				p.drawLine(m + QPointF(-4.5, 0), m + QPointF(4.5, 0));
				p.drawLine(m + QPointF(0, -4.5), m + QPointF(0, 4.5));
			} else {
				p.drawLine(m + QPointF(-3.5, -3.5), m + QPointF(3.5, 3.5));
				p.drawLine(m + QPointF(-3.5, 3.5), m + QPointF(3.5, -3.5));
			}
		}
	}
	static QColor mix_colour(const QColor &a, const QColor &b, double t) {
		return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
			a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF());
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		const QPointF pos = e->position();
		if (drag_ >= 0 && (e->buttons() & Qt::LeftButton) && !palette_) {
			if (!moved_ && (pos - press_).manhattanLength() > 6) {
				moved_ = true;
				setCursor(Qt::ClosedHandCursor);
			}
			if (moved_) {
				drag_pos_ = pos;
				// Make room: the chip takes the place of the one nearest the pointer.
				const auto r = place(width());
				int best = drag_;
				double bd = 1e18;
				for (size_t i = 0; i < r.size(); i++) {
					const QPointF d = r[i].center() - (pos - grab_ + QPointF(r[drag_].width() / 2, r[drag_].height() / 2));
					const double dist = d.x() * d.x() + d.y() * d.y() * 4;
					if (dist < bd) {
						bd = dist;
						best = static_cast<int>(i);
					}
				}
				if (best != drag_) {
					Chip c = chips_[drag_];
					chips_.erase(chips_.begin() + drag_);
					chips_.insert(chips_.begin() + best, c);
					drag_ = best;
					selected_ = best;
					changed_ = true;
				}
				update();
				return;
			}
		}
		const int h = chip_at(pos);
		const bool hc = h >= 0 && close_rect(place(width())[h]).contains(pos);
		if (h != hover_ || hc != hover_close_) {
			hover_ = h;
			hover_close_ = hc;
			setCursor(h >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
			if (h >= 0) setToolTip(palette_ ? "Add “" + chips_[h].label + "”" : hc ? "Take it off" : "Drag to move it");
			update();
		}
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) return;
		const int i = chip_at(e->position());
		if (palette_) {
			if (i >= 0 && on_pick) on_pick(chips_[i].id);
			return;
		}
		setFocus();
		if (i >= 0 && close_rect(place(width())[i]).contains(e->position())) {
			remove(i);
			return;
		}
		selected_ = i;
		drag_ = i;
		moved_ = false;
		changed_ = false;
		press_ = e->position();
		if (i >= 0) grab_ = e->position() - place(width())[i].topLeft();
		update();
	}
	void mouseReleaseEvent(QMouseEvent *) override {
		const bool commit = drag_ >= 0 && moved_ && changed_;
		drag_ = -1;
		moved_ = false;
		setCursor(hover_ >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
		update();
		if (commit && on_change) on_change();
	}
	bool event(QEvent *e) override {
		if (e->type() == QEvent::HoverLeave) {
			hover_ = -1;
			hover_close_ = false;
			update();
		}
		return QWidget::event(e);
	}
	void focusInEvent(QFocusEvent *e) override { update(); QWidget::focusInEvent(e); }
	void focusOutEvent(QFocusEvent *e) override { update(); QWidget::focusOutEvent(e); }
	void keyPressEvent(QKeyEvent *e) override {
		if (palette_ || chips_.empty()) return QWidget::keyPressEvent(e);
		const int n = static_cast<int>(chips_.size());
		const bool move = e->modifiers() & (Qt::ControlModifier | Qt::AltModifier);
		int by = e->key() == Qt::Key_Left ? -1 : e->key() == Qt::Key_Right ? 1 : 0;
		if (by) {
			if (selected_ < 0) {
				selected_ = by > 0 ? 0 : n - 1;
			} else if (move) {
				const int to = selected_ + by;
				if (to >= 0 && to < n) {
					std::swap(chips_[selected_], chips_[to]);
					selected_ = to;
					if (on_change) on_change();
				}
			} else {
				selected_ = std::clamp(selected_ + by, 0, n - 1);
			}
			update();
			return;
		}
		if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && selected_ >= 0) {
			remove(selected_);
			return;
		}
		QWidget::keyPressEvent(e);
	}
	void resizeEvent(QResizeEvent *e) override {
		QWidget::resizeEvent(e);
		if (heightForWidth(width()) != height()) updateGeometry();
	}

private:
	void remove(int i) {
		if (i < 0 || i >= static_cast<int>(chips_.size())) return;
		chips_.erase(chips_.begin() + i);
		if (selected_ >= static_cast<int>(chips_.size())) selected_ = static_cast<int>(chips_.size()) - 1;
		if (selected_ == i && chips_.empty()) selected_ = -1;
		hover_ = -1;
		updateGeometry();
		update();
		if (on_change) on_change();
	}

	bool palette_;
	std::vector<Chip> chips_;
	QString empty_;
	int selected_ = -1, hover_ = -1, drag_ = -1;
	bool hover_close_ = false, moved_ = false, changed_ = false;
	QPointF press_, grab_, drag_pos_;
};

// A titled group of rows.
// A drop-down drawn like the rest of the app: a rounded field, the choice
// shortened with "…" if it doesn't fit, and a chevron. Its list opens
// below it as a rounded card.
// A width that gives way on a narrow window (a phone or tablet screen, or
// a big scale): up to `width`, down to 120.
void flex_width(QWidget *w, int width) {
	w->setMinimumWidth(std::min(width, 120));
	w->setMaximumWidth(width);
	QSizePolicy sp = w->sizePolicy();
	sp.setHorizontalPolicy(QSizePolicy::Preferred);
	w->setSizePolicy(sp);
}

class ElideCombo : public QComboBox {
public:
	ElideCombo() {
		setItemDelegate(new QStyledItemDelegate(this));
		setCursor(Qt::PointingHandCursor);
		setFixedHeight(34);
		setMaxVisibleItems(12);
		QWidget *popup = view()->window();
		popup->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
		popup->setAttribute(Qt::WA_TranslucentBackground);
	}

protected:
	void paintEvent(QPaintEvent *) override {
		const Look l = current_look();
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(l.field);
		const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
		p.drawRoundedRect(r, 12, 12);
		if (underMouse() && isEnabled()) {
			QColor h = l.text;
			h.setAlphaF(0.07);
			p.setBrush(h);
			p.drawRoundedRect(r, 12, 12);
		}
		int x = 12;
		const QIcon icon = itemIcon(currentIndex());
		if (!icon.isNull()) {
			icon.paint(&p, QRect(x, (height() - 16) / 2, 16, 16));
			x += 22;
		}
		const QRect tr(x, 0, width() - x - 30, height());
		p.setPen(isEnabled() ? l.text : l.dim);
		p.setFont(font());
		p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft,
			fontMetrics().elidedText(currentText(), Qt::ElideRight, tr.width()));
		QPen chevron(l.dim, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
		p.setPen(chevron);
		p.setBrush(Qt::NoBrush);
		const QPointF c(width() - 17, height() / 2.0 + 1);
		const QPointF pts[3] = {c + QPointF(-4, -2.5), c + QPointF(0, 1.5), c + QPointF(4, -2.5)};
		p.drawPolyline(pts, 3);
	}
	void enterEvent(QEnterEvent *e) override {
		QComboBox::enterEvent(e);
		update();
	}
	void leaveEvent(QEvent *e) override {
		QComboBox::leaveEvent(e);
		update();
	}
	void showPopup() override {
		QComboBox::showPopup();
		// At least as wide as the field, wider if the choices need it.
		QWidget *popup = view()->window();
		int want = width();
		for (int i = 0; i < count(); i++) {
			want = std::max(want, fontMetrics().horizontalAdvance(itemText(i)) + 48);
		}
		popup->resize(std::min(want, 420), popup->height());
	}
};

// ---- search (like Arqen's): every row and card title, by page ----
struct SearchEntry {
	QString title, subtitle, card;
	QPointer<QWidget> target;
	int page;
};
std::vector<SearchEntry> search_index;
int g_page = -1;                 // the page being built
QString g_card_title;            // the card being filled

class Card;
void reset_card(Card *card);     // "Reset" on a card's title: all its settings back to the defaults

// Holds a card and paints its shadow round it, with room on every side.
// (Qt's drop-shadow effect drew each card again off-screen on every repaint,
// which was slow, and its offset shadow was cut off in a hard line where a
// card reached the page's edge.)
class ShadowHolder : public QWidget {
public:
	QFrame *frame = nullptr;
	ShadowHolder() {
		setAttribute(Qt::WA_TranslucentBackground);
		auto *v = new QVBoxLayout(this);
		v->setContentsMargins(kSide, 1, kSide, kBottom);
		v->setSpacing(0);
	}
	static constexpr int kSide = 7, kBottom = 11;

protected:
	void paintEvent(QPaintEvent *) override {
		if (!frame) return;
		const Look l = current_look();
		if (l.shadow_kind == 0 || l.shadow.alpha() == 0) return;
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		const QRectF r = QRectF(frame->geometry());
		const double radius = l.radius;
		if (l.shadow_kind == 2) {
			// Ink: short and only a little soft, straight down a touch.
			for (int i = 3; i >= 1; i--) {
				QColor c = l.shadow;
				c.setAlphaF(l.shadow.alphaF() * 0.34);
				p.setBrush(c);
				p.drawRoundedRect(r.adjusted(-i * 0.5, i * 0.8, i * 0.5, i * 1.3), radius + i * 0.5, radius + i * 0.5);
			}
		} else {
			// Soft: a wide, faint glow, a little lower than the card.
			const int steps = 8;
			for (int i = steps; i >= 1; i--) {
				QColor c = l.shadow;
				c.setAlphaF(l.shadow.alphaF() * 0.12 * (1.0 - double(i - 1) / steps));
				p.setBrush(c);
				p.drawRoundedRect(r.adjusted(-i * 0.8, i * 0.5, i * 0.8, i * 1.2), radius + i, radius + i);
			}
		}
	}
};

class Card {
public:
	QWidget *outer;          // title + frame
	ShadowHolder *holder = nullptr;
	QFrame *frame;
	QVBoxLayout *rows;
	QToolButton *reset_all = nullptr;
	std::vector<QPointer<QWidget>> members;

	Card(QVBoxLayout *page, const QString &title, const QString &description = QString()) {
		outer = new QWidget;
		auto *v = new QVBoxLayout(outer);
		v->setContentsMargins(0, 8, 0, 0);
		v->setSpacing(6);
		g_card_title = title;
		if (!title.isEmpty()) {
			auto *head = new QHBoxLayout;
			// Lined up with the card (inset by its shadow's room).
			head->setContentsMargins(ShadowHolder::kSide, 0, ShadowHolder::kSide + 4, 0);
			auto *t = new QLabel(title);
			t->setObjectName("cardtitle");
			head->addWidget(t);
			head->addStretch(1);
			reset_all = new QToolButton;
			reset_all->setObjectName("cardreset");
			reset_all->setText("Reset");
			reset_all->setToolTip("Everything in " + title + " back to the defaults");
			reset_all->setCursor(Qt::PointingHandCursor);
			QSizePolicy sp = reset_all->sizePolicy();
			sp.setRetainSizeWhenHidden(true);
			reset_all->setSizePolicy(sp);
			reset_all->hide();
			QObject::connect(reset_all, &QToolButton::clicked, [this] { reset_card(this); });
			head->addWidget(reset_all);
			v->addLayout(head);
			search_index.push_back(SearchEntry{title, QString(), QString(), outer, g_page});
		}
		frame = new QFrame;
		frame->setObjectName("card");
		rows = new QVBoxLayout(frame);
		rows->setContentsMargins(16, 8, 12, 8);
		rows->setSpacing(0);
		holder = new ShadowHolder;
		holder->frame = frame;
		holder->layout()->addWidget(frame);
		v->addWidget(holder);
		if (!description.isEmpty()) {
			auto *d = new QLabel(description);
			d->setObjectName("hint");
			d->setWordWrap(true);
			d->setContentsMargins(4 + ShadowHolder::kSide, 0, 4 + ShadowHolder::kSide, 0);
			v->addWidget(d);
		}
		page->addWidget(outer);
		apply_shadow();
	}
	void apply_shadow() {
		frame->setGraphicsEffect(nullptr);
		if (holder) holder->update();   // painted by the holder, in the new style
	}
	void add(QWidget *w) {
		members.push_back(w);
		rows->addWidget(w);
	}
};

std::vector<Card *> all_cards;

// Everything that follows a setting: refreshes its widget from the file,
// knows when it should be shown, and when its reset button appears.
struct Binding {
	QPointer<QWidget> row;
	QPointer<QToolButton> reset;
	QString key;
	QString fallback;
	std::function<void()> refresh;     // settings -> widget
	std::function<bool()> visible;     // null = always
	std::function<bool()> is_default;  // null = compare with fallback
};
std::vector<Binding> bindings;
// Run on every change at once (before it's saved for Dusk): previews.
std::vector<std::function<void(const QString &)>> live_hooks;
void live(const QString &key) {
	for (auto &hook : live_hooks) hook(key);
}

// Rows hidden until "Show more settings" is on (Arqen's way of keeping
// pages short).
std::vector<QPointer<QWidget>> advanced_rows;
QWidget *advanced(QWidget *row) {
	advanced_rows.push_back(row);
	return row;
}

void update_rows() {
	const bool more = settings->get_bool("show_advanced", false);
	for (Binding &b : bindings) {
		if (!b.row) {
			continue;
		}
		bool shown = b.visible ? b.visible() : true;
		if (!more && std::find(advanced_rows.begin(), advanced_rows.end(), b.row) != advanced_rows.end()) {
			shown = false;
		}
		b.row->setVisible(shown);
		if (b.reset) {
			const bool def = b.is_default ? b.is_default()
				: !settings->has(b.key) || settings->get(b.key) == b.fallback;
			b.reset->setVisible(!def);
		}
	}
	for (auto &r : advanced_rows) {
		if (!r) continue;
		const bool bound = std::any_of(bindings.begin(), bindings.end(), [&r](const Binding &b) { return b.row == r; });
		if (!more) r->hide();
		else if (!bound) r->show();
	}
	// Cards with nothing to show hide too; a card with something changed
	// shows its "Reset".
	for (Card *c : all_cards) {
		bool any = false;
		for (const auto &m : c->members) {
			if (m && !m->isHidden()) {
				any = true;
			}
		}
		c->outer->setVisible(any);
		if (c->reset_all) {
			bool changed_any = false;
			for (const Binding &b : bindings) {
				if (b.row && b.reset && !b.reset->isHidden() &&
						std::find(c->members.begin(), c->members.end(), b.row) != c->members.end()) {
					changed_any = true;
				}
			}
			c->reset_all->setVisible(changed_any);
		}
	}
}

void reset_card(Card *card) {
	std::vector<QPointer<QToolButton>> resets;
	for (const Binding &b : bindings) {
		if (b.row && b.reset && !b.reset->isHidden() &&
				std::find(card->members.begin(), card->members.end(), b.row) != card->members.end()) {
			resets.push_back(b.reset);
		}
	}
	for (auto &r : resets) {
		if (r) r->click();
	}
}

void refresh_all() {
	for (Binding &b : bindings) {
		if (b.row && b.refresh) {
			b.refresh();
		}
	}
	update_rows();
}

// Keys that change the look of this app too.
QColor accent_from_wallpaper();

void changed(const QString &key) {
	live(key);
	if (key == "theme" || key == "dark_style" || key == "accent" || key == "border" || key == "shadow" ||
			key.startsWith("ui_") || key == "settings_opacity" || key == "font" || key == "font_size") {
		apply_look(*the_app);
	} else if (key == "show_advanced") {
		refresh_all();
		return;
	} else if (key.startsWith("wallpaper")) {
		// "From wallpaper" is on: Dusk picks the new accent itself once the
		// new wallpaper has come in (doing it here too made the change stutter).
		restyle_hooks.run();   // preview, "From wallpaper"
	}
	update_rows();
}

QToolButton *make_reset_button() {
	auto *b = new QToolButton;
	b->setObjectName("reset");
	b->setToolTip("Back to the default");
	b->setCursor(Qt::PointingHandCursor);
	b->setIcon(dusk_icon("reset", current_look().dim, 16));
	b->setIconSize(QSize(16, 16));
	restyle_hooks.push_back([p = QPointer<QToolButton>(b)] {
		if (p) p->setIcon(dusk_icon("reset", current_look().dim, 16));
	});
	QSizePolicy sp = b->sizePolicy();
	sp.setRetainSizeWhenHidden(true);   // rows don't jump when it appears
	b->setSizePolicy(sp);
	b->hide();
	return b;
}

// A row: title (and a line under it) on the left, the control on the right,
// then the reset button.
QWidget *make_row(Card *card, const QString &title, const QString &subtitle, QWidget *editor,
		QToolButton **reset_out, bool editor_below = false) {
	auto *row = new QWidget;
	auto *outer = new QVBoxLayout(row);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);
	if (!card->members.empty()) {
		auto *div = new QFrame;
		div->setObjectName("divider");
		outer->addWidget(div);
	}
	auto *h = new QHBoxLayout;
	h->setContentsMargins(0, 9, 0, 9);
	h->setSpacing(12);
	auto *texts = new QVBoxLayout;
	texts->setSpacing(1);
	auto *t = new QLabel(title);
	t->setWordWrap(true);   // narrow windows: wraps instead of pushing the page wider
	texts->addWidget(t);
	if (!subtitle.isEmpty()) {
		auto *s = new QLabel(subtitle);
		s->setObjectName("subtitle");
		s->setWordWrap(true);
		texts->addWidget(s);
	}
	h->addLayout(texts, 1);
	if (editor && !editor_below) {
		// A list or field that can give way shares the room with the text
		// (full width when there's room, narrower on a small screen).
		const bool flexible = editor->maximumWidth() <= 400 && editor->minimumWidth() < editor->maximumWidth();
		h->addWidget(editor, flexible ? 1 : 0, Qt::AlignVCenter);
	}
	QToolButton *reset = nullptr;
	if (reset_out) {
		reset = make_reset_button();
		h->addWidget(reset, 0, Qt::AlignVCenter);
		*reset_out = reset;
	} else if (editor && !editor_below) {
		// Room where the reset button would be, so every switch and list
		// lines up in one column.
		QToolButton *room = make_reset_button();
		room->hide();
		h->addWidget(room, 0, Qt::AlignVCenter);
	}
	outer->addLayout(h);
	if (editor && editor_below) {
		outer->addWidget(editor);
		outer->addSpacing(8);
	}
	card->add(row);
	search_index.push_back(SearchEntry{title, subtitle, g_card_title, row, g_page});
	return row;
}

using Items = std::vector<std::pair<QString, QString>>;   // label, value

QWidget *toggle(Card *card, const QString &title, const QString &subtitle, const QString &key,
		bool fallback, std::function<bool()> visible = {}) {
	auto *sw = new Switch;
	QToolButton *reset = nullptr;
	QWidget *row = make_row(card, title, subtitle, sw, &reset);
	auto refresh = [sw, key, fallback] {
		const QSignalBlocker block(sw);
		sw->set_quietly(settings->get_bool(key, fallback));
	};
	refresh();
	QObject::connect(sw, &QAbstractButton::toggled, [key](bool on) {
		settings->set_bool(key, on);
		changed(key);
	});
	QObject::connect(reset, &QToolButton::clicked, [key, refresh] {
		settings->remove(key);
		refresh();
		changed(key);
	});
	bindings.push_back(Binding{row, reset, key, fallback ? "true" : "false", refresh, visible, [key, fallback] {
		return !settings->has(key) || settings->get_bool(key, fallback) == fallback;
	}});
	return row;
}

// A switch for whether `item` is in the comma list `key` (keeps the list's order).
QWidget *list_toggle(Card *card, const QString &title, const QString &subtitle, const QString &key,
		const QString &item, const QStringList &order, const QStringList &fallback,
		std::function<bool()> visible = {}) {
	auto *sw = new Switch;
	QWidget *row = make_row(card, title, subtitle, sw, nullptr);
	auto current = [key, fallback] {
		return settings->has(key) ? settings->get(key).split(',', Qt::SkipEmptyParts) : fallback;
	};
	auto refresh = [sw, item, current] {
		const QSignalBlocker block(sw);
		sw->set_quietly(current().contains(item));
	};
	refresh();
	QObject::connect(sw, &QAbstractButton::toggled, [key, item, order, current](bool on) {
		QStringList now = current();
		now.removeAll(item);
		if (on) now.append(item);
		QStringList sorted;
		for (const QString &o : order) if (now.contains(o)) sorted.append(o);
		settings->set(key, sorted.join(','));
		changed(key);
	});
	bindings.push_back(Binding{row, nullptr, key, fallback.join(','), refresh, visible, {}});
	return row;
}

QWidget *choice(Card *card, const QString &title, const QString &subtitle, const QString &key,
		const Items &items, const QString &fallback, std::function<bool()> visible = {}) {
	auto *combo = new ElideCombo;
	flex_width(combo, 240);
	for (const auto &[label, value] : items) {
		combo->addItem(label, value);
	}
	QToolButton *reset = nullptr;
	QWidget *row = make_row(card, title, subtitle, combo, &reset);
	auto refresh = [combo, key, fallback] {
		const QSignalBlocker block(combo);
		const int i = combo->findData(settings->get(key, fallback));
		combo->setCurrentIndex(i >= 0 ? i : std::max(0, combo->findData(fallback)));
	};
	refresh();
	QObject::connect(combo, &QComboBox::currentIndexChanged, [combo, key](int) {
		settings->set(key, combo->currentData().toString());
		// After the list has closed: restyling everything (theme, font...)
		// while it was still open is what made Settings seem to freeze.
		QTimer::singleShot(0, [key] { changed(key); });
	});
	QObject::connect(reset, &QToolButton::clicked, [key, refresh] {
		settings->remove(key);
		refresh();
		changed(key);
	});
	bindings.push_back(Binding{row, reset, key, fallback, refresh, visible, {}});
	return row;
}

QWidget *number(Card *card, const QString &title, const QString &subtitle, const QString &key,
		int min, int max, int step, int fallback, const QString &suffix,
		std::function<bool()> visible = {}) {
	// − value + : no tiny spin arrows.
	auto *box = new QWidget;
	box->setFixedWidth(150);
	auto *h = new QHBoxLayout(box);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(6);
	auto *minus = new QToolButton;
	minus->setObjectName("step");
	minus->setText(QString::fromUtf8("−"));
	minus->setAutoRepeat(true);
	auto *plus = new QToolButton;
	plus->setObjectName("step");
	plus->setText("+");
	plus->setAutoRepeat(true);
	auto *value = new QLabel;
	value->setObjectName("stepvalue");
	value->setAlignment(Qt::AlignCenter);
	h->addWidget(minus);
	h->addWidget(value, 1);
	h->addWidget(plus);
	QToolButton *reset = nullptr;
	QWidget *row = make_row(card, title, subtitle, box, &reset);
	auto get = [key, fallback, min, max] {
		bool ok = false;
		const int v = settings->get(key, QString::number(fallback)).toInt(&ok);
		return std::clamp(ok ? v : fallback, min, max);
	};
	auto refresh = [value, minus, plus, get, suffix, min, max] {
		const int v = get();
		value->setText(QString::number(v) + suffix);
		minus->setEnabled(v > min);
		plus->setEnabled(v < max);
	};
	refresh();
	// Each press shows at once; the setting is saved a moment after the last
	// press (pressing + five times is one change for Dusk, not five).
	auto *later = new QTimer(box);
	later->setSingleShot(true);
	later->setInterval(350);
	QObject::connect(later, &QTimer::timeout, [key] { changed(key); });
	auto change = [key, get, refresh, step, min, max, later](int dir) {
		const int v = std::clamp(get() + dir * step, min, max);
		settings->set(key, QString::number(v));
		refresh();
		live(key);
		later->start();
	};
	QObject::connect(minus, &QToolButton::clicked, [change] { change(-1); });
	QObject::connect(plus, &QToolButton::clicked, [change] { change(1); });
	QObject::connect(reset, &QToolButton::clicked, [key, refresh] {
		settings->remove(key);
		refresh();
		changed(key);
	});
	bindings.push_back(Binding{row, reset, key, QString::number(fallback), refresh, visible,
		[key, fallback] { return !settings->has(key) || settings->get(key).toInt() == fallback; }});
	return row;
}

// A slider for a number stored as value / divisor (e.g. 0.95 with divisor 100).
QWidget *slider(Card *card, const QString &title, const QString &subtitle, const QString &key,
		int min, int max, int fallback, int divisor, std::function<QString(int)> label,
		std::function<bool()> visible = {}) {
	auto *box = new QWidget;
	auto *h = new QHBoxLayout(box);
	h->setContentsMargins(0, 0, 0, 0);
	flex_width(box, 240);
	h->setSpacing(10);
	auto *s = new Slider;
	s->setRange(min, max);
	auto *value = new QLabel;
	value->setObjectName("slidervalue");
	value->setMinimumWidth(56);
	value->setAlignment(Qt::AlignCenter);
	h->addWidget(s, 1);
	h->addWidget(value);
	QToolButton *reset = nullptr;
	QWidget *row = make_row(card, title, subtitle, box, &reset);
	auto to_int = [divisor](const QString &v, int fb) {
		bool ok = false;
		const double d = v.toDouble(&ok);
		return ok ? static_cast<int>(std::lround(d * divisor)) : fb;
	};
	auto refresh = [s, value, key, fallback, to_int, label] {
		const QSignalBlocker block(s);
		const int v = to_int(settings->get(key), fallback);
		s->setValue(v);
		value->setText(label(v));
	};
	refresh();
	// The number follows the knob at once; the setting is saved when you let
	// go (or a moment after the wheel / arrow keys stop), not at every step:
	// each save makes Dusk restyle everything.
	auto *later = new QTimer(s);
	later->setSingleShot(true);
	later->setInterval(220);
	auto commit = [s, key, divisor] {
		const int v = s->value();
		const QString text = divisor == 1 ? QString::number(v)
			: QString::number(static_cast<double>(v) / divisor, 'f', divisor >= 100 ? 2 : 1);
		if (settings->get(key) == text) return;
		settings->set(key, text);
		changed(key);
	};
	QObject::connect(later, &QTimer::timeout, commit);
	QObject::connect(s, &QSlider::valueChanged, [s, value, label, later](int v) {
		value->setText(label(v));
		if (!s->isSliderDown()) later->start();
	});
	QObject::connect(s, &QSlider::sliderReleased, [later, commit] {
		later->stop();
		commit();
	});
	QObject::connect(reset, &QToolButton::clicked, [key, refresh] {
		settings->remove(key);
		refresh();
		changed(key);
	});
	bindings.push_back(Binding{row, reset, key, QString(), refresh, visible,
		[key, fallback, to_int] {
			return !settings->has(key) || to_int(settings->get(key), fallback) == fallback;
		}});
	return row;
}

QWidget *text_field(Card *card, const QString &title, const QString &subtitle, const QString &key,
		const QString &placeholder, std::function<bool()> visible = {},
		QLineEdit **edit_out = nullptr) {
	auto *edit = new QLineEdit;
	edit->setPlaceholderText(placeholder);
	flex_width(edit, 240);
	QToolButton *reset = nullptr;
	QWidget *row = make_row(card, title, subtitle, edit, &reset);
	auto refresh = [edit, key] {
		if (edit->text() != settings->get(key)) {
			const QSignalBlocker block(edit);
			edit->setText(settings->get(key));
		}
	};
	refresh();
	QObject::connect(edit, &QLineEdit::textChanged, [key](const QString &t) {
		if (t.trimmed().isEmpty()) {
			settings->remove(key);
		} else {
			settings->set(key, t.trimmed());
		}
		changed(key);
	});
	QObject::connect(reset, &QToolButton::clicked, [key, refresh] {
		settings->remove(key);
		refresh();
		changed(key);
	});
	bindings.push_back(Binding{row, reset, key, QString(), refresh, visible,
		[key] { return settings->get(key).isEmpty(); }});
	if (edit_out) {
		*edit_out = edit;
	}
	return row;
}

QPushButton *color_button(const QString &key, const QString &fallback) {
	auto *button = new QPushButton;
	button->setFixedSize(64, 28);
	button->setCursor(Qt::PointingHandCursor);
	auto paint = [button = QPointer<QPushButton>(button), key, fallback] {
		if (!button) return;
		const QString c = settings->get(key, fallback);
		button->setStyleSheet(QString("QPushButton { background: %1; border-radius: %2px; "
			"border: 2px solid rgba(128,128,128,0.4); }").arg(QColor(c).name())
			.arg(button->height() / 2));
	};
	paint();
	restyle_hooks.add(button, paint);
	QObject::connect(button, &QPushButton::clicked, [button, key, fallback, paint] {
		const QColor picked = QColorDialog::getColor(QColor(settings->get(key, fallback)),
			button->window(), "Choose a colour");
		if (picked.isValid()) {
			settings->set(key, picked.name());
			paint();
			changed(key);
		}
	});
	return button;
}

QWidget *color_row(Card *card, const QString &title, const QString &key, const QString &fallback,
		std::function<bool()> visible = {}) {
	QPushButton *button = color_button(key, fallback);
	QToolButton *reset = nullptr;
	QWidget *row = make_row(card, title, QString(), button, &reset);
	auto refresh = [button, key, fallback] {
		button->setStyleSheet(QString("QPushButton { background: %1; border-radius: 14px; "
			"border: 2px solid rgba(128,128,128,0.4); }").arg(QColor(settings->get(key, fallback)).name()));
	};
	QObject::connect(reset, &QToolButton::clicked, [key, refresh] {
		settings->remove(key);
		refresh();
		changed(key);
	});
	bindings.push_back(Binding{row, reset, key, fallback, refresh, visible,
		[key, fallback] {
			return !settings->has(key) || QColor(settings->get(key)) == QColor(fallback);
		}});
	return row;
}


// A page: big title, a line under it, then cards, in a scroll area.
struct Page {
	QWidget *widget;
	QVBoxLayout *layout;
};

Page make_page(const QString &title, const QString &subtitle) {
	auto *inner = new QWidget;
	auto *layout = new QVBoxLayout(inner);
	layout->setContentsMargins(32, 28, 32, 32);
	layout->setSpacing(12);
	auto *heading = new QLabel(title);
	heading->setObjectName("heading");
	// (With its own header bar the page's name is up there; For you keeps its greeting.)
	heading->setVisible(!g_csd || title.startsWith("Good "));
	layout->addWidget(heading);
	if (!subtitle.isEmpty()) {
		auto *sub = new QLabel(subtitle);
		sub->setObjectName("subtitle");
		sub->setWordWrap(true);
		layout->addWidget(sub);
	}
	// A readable column (about 760 px), centred: on a wide window the
	// extra room is margin, not stretched cards.
	inner->setMaximumWidth(820);
	auto *centre = new QWidget;
	auto *ch = new QHBoxLayout(centre);
	ch->setContentsMargins(0, 0, 0, 0);
	ch->addStretch(1);
	ch->addWidget(inner, 100);
	ch->addStretch(1);
	auto *scroll = new QScrollArea;
	scroll->setWidget(centre);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	return Page{scroll, layout};
}

Card *card(Page &page, const QString &title, const QString &description = QString()) {
	auto *c = new Card(page.layout, title, description);
	all_cards.push_back(c);
	return c;
}

void finish(Page &page) {
	page.layout->addStretch(1);
}

// "Show more settings" at the bottom of a page with fine-tuning options.
void more_toggle(Page &page);

void more_toggle(Page &page) {
	// Only where there's more to show.
	const bool any = std::any_of(advanced_rows.begin(), advanced_rows.end(),
		[&page](const QPointer<QWidget> &w) { return w && page.widget->isAncestorOf(w); });
	if (!any) return;
	Card *c = card(page, "");
	toggle(c, "Show more settings", "Fine-tuning and rarely needed options", "show_advanced", false);
}

// ---- things about this computer, for hiding what doesn't apply ------------

bool has_touchpad() {
	QFile f("/proc/bus/input/devices");
	if (!f.open(QIODevice::ReadOnly)) {
		return true;   // can't tell: show it
	}
	const QString all = QString::fromUtf8(f.readAll()).toLower();
	return all.contains("touchpad") || all.contains("trackpad");
}

bool mains_online() {
	QDirIterator it("/sys/class/power_supply");
	while (it.hasNext()) {
		const QString dir = it.next();
		QFile type(dir + "/type"), online(dir + "/online");
		if (type.open(QIODevice::ReadOnly) && type.readAll().trimmed() == "Mains" &&
			online.open(QIODevice::ReadOnly) && online.readAll().trimmed() == "1") return true;
	}
	return false;
}

bool has_battery() {
	QDirIterator it("/sys/class/power_supply");
	while (it.hasNext()) {
		const QString dir = it.next();
		auto read = [&dir](const char *name) {
			QFile f(dir + "/" + name);
			return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
		};
		// Not a mouse's or headset's battery (scope "Device").
		if (read("type") == "Battery" && read("scope") != "Device" && read("present") != "0") {
			// A dead one (0% with the charger in) counts as none, as in the shell.
			if (read("capacity") == "0" && mains_online()) continue;
			return true;
		}
	}
	return false;
}

[[maybe_unused]] bool has_backlight() {
	return !QDir("/sys/class/backlight").entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
}

// ---- installed apps (for pinning) ------------------------------------------

struct App {
	QString id, name, icon;
};

std::vector<App> installed_apps() {
	std::vector<App> apps;
	QSet<QString> seen;
	QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
	dirs << "/var/lib/flatpak/exports/share/applications"
		<< QDir::homePath() + "/.local/share/flatpak/exports/share/applications";
	for (const QString &dir : dirs) {
		QDirIterator it(dir, {"*.desktop"}, QDir::Files, QDirIterator::Subdirectories);
		while (it.hasNext()) {
			const QString path = it.next();
			QString id = QDir(dir).relativeFilePath(path).replace('/', '-');
			if (seen.contains(id)) {
				continue;
			}
			seen.insert(id);
			QFile f(path);
			if (!f.open(QIODevice::ReadOnly)) {
				continue;
			}
			App app;
			app.id = id;
			bool in_entry = false, hidden = false;
			QTextStream ts(&f);
			while (!ts.atEnd()) {
				const QString line = ts.readLine().trimmed();
				if (line.startsWith('[')) {
					in_entry = line == "[Desktop Entry]";
					continue;
				}
				if (!in_entry) continue;
				if (line.startsWith("Name=") && app.name.isEmpty()) app.name = line.mid(5);
				else if (line.startsWith("Icon=")) app.icon = line.mid(5);
				else if (line == "NoDisplay=true" || line == "Hidden=true") hidden = true;
				else if (line.startsWith("Type=") && line != "Type=Application") hidden = true;
			}
			if (!hidden && !app.name.isEmpty()) {
				apps.push_back(app);
			}
		}
	}
	std::sort(apps.begin(), apps.end(), [](const App &a, const App &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	return apps;
}

QIcon app_icon(const QString &name) {
	if (name.isEmpty()) return QIcon::fromTheme("application-x-executable");
	if (name.startsWith('/')) return QIcon(name);
	QIcon icon = QIcon::fromTheme(name);
	return icon.isNull() ? QIcon::fromTheme("application-x-executable") : icon;
}

// A list the user reorders by dragging. As tall as what's in it (two rows at
// least, and it scrolls past `max_rows`), so a short list isn't a big
// empty box.
QListWidget *editor_list(int /*min_height*/ = 0, int max_rows = 8) {
	auto *list = new QListWidget;
	list->setObjectName("editor");
	list->setDragDropMode(QAbstractItemView::InternalMove);
	list->setDefaultDropAction(Qt::MoveAction);
	list->setIconSize(QSize(22, 22));
	list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	QPointer<QListWidget> guard(list);
	auto fit = [guard, max_rows] {
		if (!guard) return;
		int row_h = guard->count() > 0 ? guard->sizeHintForRow(0) : 0;
		if (row_h <= 0) row_h = std::max(32, guard->fontMetrics().height() + 16);
		const int rows = std::clamp(guard->count(), 2, max_rows);
		guard->setFixedHeight(rows * row_h + 2 * guard->frameWidth() + 10);
	};
	fit();
	QObject::connect(list->model(), &QAbstractItemModel::rowsInserted, list, [fit] { QTimer::singleShot(0, fit); });
	QObject::connect(list->model(), &QAbstractItemModel::rowsRemoved, list, [fit] { QTimer::singleShot(0, fit); });
	QObject::connect(list->model(), &QAbstractItemModel::modelReset, list, [fit] { QTimer::singleShot(0, fit); });
	return list;
}

// ============================================================================
// Pages
// ============================================================================

// ---- Appearance ---------------------------------------------------------------

struct Swatch {
	const char *name, *hex;
};
const Swatch kSwatches[] = {
	{"Blue", "#4d8df6"}, {"Navy", "#2b4f8f"}, {"Racing red", "#c8102e"},
	{"Orange", "#f27a1a"}, {"Gold", "#d6a21e"}, {"Green", "#3d8f5a"},
	{"Teal", "#2aa198"}, {"Purple", "#8a63d2"}, {"Pink", "#e05a8c"},
	{"Silver grey", "#8e9399"}, {"Cream", "#d8c7a2"},
};

// ---- Pictures, decoded off the window's thread and kept --------------------------
// (Decoding a big wallpaper while the window waited made Settings lag.)
std::map<QString, QImage> g_pictures;
std::set<QString> g_pictures_loading;

// `path` shrunk to cover `box`, or a null image while it's being read
// (`ready` runs once it is).
// Pictures for the wallpaper thumbnails and preview are read off the main
// thread by two workers at low priority (decoding dozens at once, one thread
// each, made the whole computer stutter: GNOME's .jxl wallpapers are slow to
// read), and each small copy is kept in ~/.cache/dusk/thumbnails, so Settings
// opens at once after the first time.
namespace {

struct PictureJob {
	QString path, key;
	QSize box;
	std::function<void()> ready;
};

std::mutex g_picture_mutex;
std::condition_variable g_picture_wake;
std::deque<PictureJob> g_picture_jobs;

QString thumbnail_file(const QString &key) {
	return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/dusk/thumbnails/" +
		QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex()) + ".png";
}

void picture_worker() {
	// Below everything else: the apps you're using come first.
	setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 15);
	for (;;) {
		PictureJob job;
		{
			std::unique_lock<std::mutex> lock(g_picture_mutex);
			g_picture_wake.wait(lock, [] { return !g_picture_jobs.empty(); });
			job = std::move(g_picture_jobs.front());
			g_picture_jobs.pop_front();
		}
		const QString cached = thumbnail_file(job.key);
		QImage img;
		if (QFileInfo::exists(cached)) img.load(cached, "PNG");
		if (img.isNull()) {
			QImageReader reader(job.path);
			reader.setAutoTransform(true);
			const QSize full = reader.size();
			if (full.isValid()) reader.setScaledSize(full.scaled(job.box, Qt::KeepAspectRatioByExpanding));
			img = reader.read();
			if (!img.isNull() && (img.width() > job.box.width() * 2 || img.height() > job.box.height() * 2)) {
				img = img.scaled(job.box, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
			}
			if (!img.isNull()) {
				QDir().mkpath(QFileInfo(cached).absolutePath());
				img.save(cached, "PNG");
			}
		}
		QMetaObject::invokeMethod(qApp, [key = job.key, img, ready = job.ready] {
			g_pictures[key] = img.isNull() ? QImage(1, 1, QImage::Format_ARGB32) : img;
			g_pictures_loading.erase(key);
			if (ready) ready();
		}, Qt::QueuedConnection);
	}
}

} // namespace

QImage cached_picture(const QString &path, QSize box, std::function<void()> ready) {
	if (path.isEmpty()) return QImage();
	const QFileInfo info(path);
	const QString key = path + "|" + QString::number(info.lastModified().toMSecsSinceEpoch()) + "|" +
		QString::number(box.width()) + "x" + QString::number(box.height());
	auto it = g_pictures.find(key);
	if (it != g_pictures.end()) return it->second;
	if (!g_pictures_loading.count(key)) {
		g_pictures_loading.insert(key);
		static std::once_flag started;
		std::call_once(started, [] {
			for (int i = 0; i < 2; i++) std::thread(picture_worker).detach();
		});
		{
			std::lock_guard<std::mutex> lock(g_picture_mutex);
			// The big preview first: it's what you look at.
			PictureJob job{path, key, box, std::move(ready)};
			if (box.width() >= 400) g_picture_jobs.push_front(std::move(job));
			else g_picture_jobs.push_back(std::move(job));
		}
		g_picture_wake.notify_one();
	}
	return QImage();
}

std::function<void()> g_preview_repaint;   // the wallpaper preview, when its picture is ready

// What the wallpaper will look like (roughly), with a hint of the panel.
QPixmap wallpaper_preview() {
	const QSize size(320, 180);
	QPixmap pm(size);
	pm.fill(QColor("#101114"));
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	const Look l = current_look();
	const QString type = settings->get("wallpaper_type", "dusk");
	if (type == "image") {
		const QString mode = settings->get("wallpaper_mode", "fill");
		const QString path = settings->get("wallpaper");
		const QImage pic = cached_picture(path, QSize(640, 360), [] { if (g_preview_repaint) g_preview_repaint(); });
		const QPixmap image = pic.isNull() || pic.width() <= 1 ? QPixmap() : QPixmap::fromImage(pic);
		if (pic.isNull() && !path.isEmpty()) {
			p.setPen(l.dim);
			p.drawText(pm.rect(), Qt::AlignCenter, "Loading…");
		} else if (!image.isNull()) {
			QSize s = image.size();
			if (mode == "stretch") s = size;
			else if (mode == "fit") s.scale(size, Qt::KeepAspectRatio);
			else if (mode == "center") s = image.size() * (size.width() / 1920.0);
			else s.scale(size, Qt::KeepAspectRatioByExpanding);
			p.drawPixmap(QRect(QPoint((size.width() - s.width()) / 2, (size.height() - s.height()) / 2), s), image);
			(void)0;
		} else {
			p.setPen(l.dim);
			p.drawText(pm.rect(), Qt::AlignCenter, "Choose a picture");
		}
	} else if (type == "solid") {
		p.fillRect(pm.rect(), QColor(settings->get("wallpaper_color", "#2b2d3a")));
	} else if (type == "halftone") {
		p.fillRect(pm.rect(), l.window);
		QColor dot = l.accent;
		dot.setAlphaF(0.55);
		p.setPen(Qt::NoPen);
		p.setBrush(dot);
		const double spacing = 6, reach = std::hypot(size.width(), size.height()) * 0.62;
		int row = 0;
		for (double y = spacing / 2; y < size.height(); y += spacing, row++) {
			for (double x = row % 2 ? spacing : spacing / 2; x < size.width(); x += spacing) {
				const double t = 1.0 - std::hypot(x - size.width(), y - size.height()) / reach;
				if (t > 0) p.drawEllipse(QPointF(x, y), spacing * 0.48 * t, spacing * 0.48 * t);
			}
		}
	} else {
		QLinearGradient sky(0, 0, 0, size.height());
		sky.setColorAt(0.00, QColor::fromRgbF(0.10, 0.09, 0.19));
		sky.setColorAt(0.42, QColor::fromRgbF(0.20, 0.18, 0.31));
		sky.setColorAt(0.60, QColor::fromRgbF(0.40, 0.37, 0.48));
		sky.setColorAt(0.75, QColor::fromRgbF(0.22, 0.20, 0.32));
		sky.setColorAt(1.00, QColor::fromRgbF(0.12, 0.11, 0.20));
		p.fillRect(pm.rect(), sky);
		QRadialGradient glow(size.width() * 0.5, size.height() * 0.6, size.width() * 0.55);
		QColor a = l.accent;
		a.setAlphaF(0.16);
		glow.setColorAt(0, a);
		a.setAlphaF(0);
		glow.setColorAt(1, a);
		p.fillRect(pm.rect(), glow);
	}
	// A floating panel.
	p.setPen(Qt::NoPen);
	QColor bar = l.card;
	bar.setAlphaF(0.95);
	p.setBrush(bar);
	p.drawRoundedRect(QRectF(8, size.height() - 18, size.width() - 16, 12), 6, 6);
	p.setBrush(l.accent);
	p.drawEllipse(QPointF(16, size.height() - 12), 4, 4);
	p.end();
	QPixmap rounded(size);
	rounded.fill(Qt::transparent);
	QPainter r(&rounded);
	r.setRenderHint(QPainter::Antialiasing);
	QPainterPath path;
	path.addRoundedRect(QRectF(0, 0, size.width(), size.height()), 12, 12);
	r.setClipPath(path);
	r.drawPixmap(0, 0, pm);
	return rounded;
}

// ---- Style presets ----------------------------------------------------------------

struct Preset {
	const char *name, *blurb;
	// ui_outline, ui_shadow, ui_texture, border (windows), shadow (windows)
	const char *outline, *ui_shadow, *texture, *border, *shadow;
};
const Preset kPresets[] = {
	{"Calm", "Hairlines and soft, faint shadows", "thin", "soft", "none", "thin", "soft"},
	{"Comic", "Soft ink lines and short drawn shadows", "ink", "ink", "none", "outline", "ink"},
	{"Flat", "Thin lines, no shadows", "thin", "none", "none", "thin", "none"},
};

bool preset_active(const Preset &p) {
	return settings->get("ui_outline", "thin") == p.outline &&
		settings->get("ui_shadow", "soft") == p.ui_shadow &&
		settings->get("border", "thin") == p.border && settings->get("shadow", "soft") == p.shadow;
}

void apply_preset(const Preset &p) {
	const std::pair<const char *, const char *> keys[] = {{"ui_outline", p.outline},
		{"ui_shadow", p.ui_shadow}, {"ui_texture", p.texture}, {"border", p.border},
		{"shadow", p.shadow}};
	for (const auto &[key, value] : keys) {
		settings->set(key, value);
	}
	settings->remove("ui_texture");   // screentone dots are gone
	settings->set("ui_style", QString(p.name).toLower());
	apply_look(*the_app);
	refresh_all();
}

// A little picture of a preset: a card and a button in that style.
class PresetButton : public QAbstractButton {
public:
	explicit PresetButton(const Preset &preset) : preset_(preset) {
		setCursor(Qt::PointingHandCursor);
		setFixedSize(150, 118);
		setToolTip(preset.blurb);
	}

protected:
	void paintEvent(QPaintEvent *) override {
		const Look l = current_look();
		const Preset &p = preset_;
		const bool on = preset_active(p);
		QPainter g(this);
		g.setRenderHint(QPainter::Antialiasing);
		// Frame of the choice.
		QColor ring = on ? l.accent : l.hover;
		g.setPen(QPen(ring, on ? 2.5 : 1.0));
		g.setBrush(underMouse() ? l.hover : Qt::transparent);
		g.drawRoundedRect(QRectF(1.5, 1.5, width() - 3, 84), 14, 14);

		// The sample card.
		const QRectF card(26, 16, 98, 56);
		const bool dark = settings->get("theme", "dark") != "light";
		QColor ink = dark ? QColor(9, 8, 12) : QColor(42, 36, 51);
		const QString sh = p.ui_shadow;
		if (sh == "soft") {
			for (int i = 6; i >= 1; i--) {
				QColor c(0, 0, 0, dark ? 14 : 8);
				g.setPen(Qt::NoPen);
				g.setBrush(c);
				g.drawRoundedRect(card.adjusted(-i, -i + 5, i, i + 5), 12 + i, 12 + i);
			}
		} else if (sh == "ink") {
			QColor c = ink;
			c.setAlpha(dark ? 110 : 70);
			g.setPen(Qt::NoPen);
			g.setBrush(c);
			g.drawRoundedRect(card.translated(3, 4), 12, 12);
		}
		g.setBrush(l.card);
		const QString o = p.outline;
		if (o == "ink") g.setPen(QPen(ink, 2));
		else if (o == "thin") g.setPen(QPen(l.dim, 1));
		else g.setPen(Qt::NoPen);
		g.drawRoundedRect(card, 12, 12);
		// A pill button and two text lines on it.
		g.setPen(Qt::NoPen);
		g.setBrush(l.accent);
		g.drawRoundedRect(QRectF(card.left() + 10, card.top() + 12, 30, 30), 15, 15);
		QColor line = l.text;
		line.setAlpha(150);
		g.setBrush(line);
		g.drawRoundedRect(QRectF(card.left() + 48, card.top() + 16, 38, 6), 3, 3);
		line.setAlpha(70);
		g.setBrush(line);
		g.drawRoundedRect(QRectF(card.left() + 48, card.top() + 28, 28, 6), 3, 3);

		g.setPen(on ? l.accent : l.text);
		QFont f = font();
		f.setBold(on);
		g.setFont(f);
		g.drawText(QRectF(0, 88, width(), 26), Qt::AlignCenter, p.name);
	}
	void enterEvent(QEnterEvent *) override { update(); }
	void leaveEvent(QEvent *) override { update(); }

private:
	const Preset &preset_;
};

// Icon themes installed (not the cursor-only ones), as (shown name, folder).
std::vector<std::pair<QString, QString>> icon_themes() {
	QStringList dirs = {QDir::homePath() + "/.local/share/icons", QDir::homePath() + "/.icons"};
	for (const QString &d : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
		dirs << d + "/icons";
	}
	std::vector<std::pair<QString, QString>> themes;
	QStringList seen;
	for (const QString &dir : dirs) {
		for (const QString &name : QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
			if (seen.contains(name) || name == "hicolor" || name == "default" || name == "locolor") continue;
			QFile f(dir + "/" + name + "/index.theme");
			if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
			QString shown = name, dirs_line;
			bool hidden = false;
			while (!f.atEnd()) {
				const QString line = QString::fromUtf8(f.readLine()).trimmed();
				if (line.startsWith("Name=")) shown = line.mid(5);
				else if (line.startsWith("Directories=")) dirs_line = line.mid(12);
				else if (line == "Hidden=true") hidden = true;
			}
			// Cursor themes have an index.theme too, but no icon folders.
			if (hidden || dirs_line.trimmed().isEmpty()) continue;
			seen << name;
			themes.push_back({shown, name});
		}
	}
	std::sort(themes.begin(), themes.end(), [](const auto &a, const auto &b) {
		return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
	});
	return themes;
}

QStringList cursor_themes() {
	QStringList dirs = {QDir::homePath() + "/.local/share/icons", QDir::homePath() + "/.icons"};
	for (const QString &d : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
		dirs << d + "/icons";
	}
	QStringList themes;
	for (const QString &dir : dirs) {
		for (const QString &name : QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
			if (QDir(dir + "/" + name + "/cursors").exists() && !themes.contains(name)) {
				themes << name;
			}
		}
	}
	return themes;
}

// The wallpaper's most colourful common colour, made bright enough to be
// an accent (an idea from Arqen shell). Invalid if there's no picture.
QImage cached_picture(const QString &path, QSize box, std::function<void()> ready);

QColor accent_from_image(QImage img);

QColor accent_from_wallpaper() {
	QImage img;
	const QImage have = cached_picture(settings->get("wallpaper"), QSize(640, 360), {});
	if (!have.isNull() && have.width() > 1) {
		img = have.scaled(QSize(80, 80), Qt::KeepAspectRatio, Qt::FastTransformation);
	} else {
		QImageReader reader(settings->get("wallpaper"));
		const QSize full = reader.size();
		if (full.isValid()) reader.setScaledSize(full.scaled(QSize(80, 80), Qt::KeepAspectRatio));
		img = reader.read();
	}
	return accent_from_image(img);
}

QColor accent_from_image(QImage img) {
	if (img.isNull()) return QColor();
	if (img.width() > 120 || img.height() > 120) img = img.scaled(QSize(80, 80), Qt::KeepAspectRatio, Qt::FastTransformation);
	// Votes per hue (36 buckets), weighted by how colourful each pixel is.
	double weight[36] = {}, r[36] = {}, g[36] = {}, b[36] = {};
	for (int y = 0; y < img.height(); y++) {
		for (int x = 0; x < img.width(); x++) {
			const QColor c = img.pixelColor(x, y);
			const double sat = c.hsvSaturationF(), val = c.valueF();
			if (sat < 0.18 || val < 0.18) continue;
			const int bucket = std::clamp(static_cast<int>(c.hsvHueF() * 36), 0, 35);
			const double w = sat * sat * val;
			weight[bucket] += w;
			r[bucket] += c.redF() * w;
			g[bucket] += c.greenF() * w;
			b[bucket] += c.blueF() * w;
		}
	}
	int best = -1;
	for (int i = 0; i < 36; i++) {
		// Neighbouring hues count a little, so a spread-out colour wins over a speck.
		const double score = weight[i] + 0.5 * (weight[(i + 35) % 36] + weight[(i + 1) % 36]);
		if (score > 0 && (best < 0 || score > weight[best] + 0.5 * (weight[(best + 35) % 36] +
				weight[(best + 1) % 36]))) {
			best = i;
		}
	}
	if (best < 0) return QColor("#8e9399");   // a grey picture: silver grey
	QColor c = QColor::fromRgbF(r[best] / weight[best], g[best] / weight[best], b[best] / weight[best]);
	// Readable on both light and dark: saturation and brightness in a middle band.
	float h, sat, v;
	c.getHsvF(&h, &sat, &v);
	return QColor::fromHsvF(h, std::clamp(sat, 0.45f, 0.85f), std::clamp(v, 0.62f, 0.90f));
}

// Where wallpapers are looked for: your Pictures/Wallpapers, the system's
// wallpaper folders, then folders you added (wallpaper_folders, "|"-separated).
QStringList wallpaper_folders() {
	QStringList dirs = {QDir::homePath() + "/Pictures/Wallpapers"};
	const QString pics = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	if (!pics.isEmpty()) dirs << pics + "/Wallpapers";
	dirs << QDir::homePath() + "/.local/share/backgrounds" << QDir::homePath() + "/.local/share/wallpapers";
	for (const QString &d : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
		dirs << d + "/backgrounds" << d + "/wallpapers";
	}
	dirs << settings->get("wallpaper_folders").split('|', Qt::SkipEmptyParts);
	dirs.removeDuplicates();
	return dirs;
}

// The fonts: one for everything you read (Dusk, this app and other apps),
// one for terminals and code. Each listed in its own lettering; a preview
// underneath. Google Sans and Google Sans Code by default (./build.sh
// installs them; if they're missing, a button here gets them).
bool font_installed(const QString &family) {
	return QFontDatabase::families().contains(family, Qt::CaseInsensitive);
}

QWidget *font_row(Card *card, const QString &title, const QString &subtitle, const QString &key,
		const QString &fallback, bool mono) {
	Items fonts;
	QStringList fams = QFontDatabase::families();
	fams.sort(Qt::CaseInsensitive);
	if (!fams.contains(fallback, Qt::CaseInsensitive)) fonts.push_back({fallback + " (not installed)", fallback});
	for (const QString &f : fams) {
		if (QFontDatabase::isPrivateFamily(f)) continue;
		if (mono && !QFontDatabase::isFixedPitch(f) && !f.contains("Mono", Qt::CaseInsensitive) &&
				!f.contains("Code", Qt::CaseInsensitive)) continue;
		fonts.push_back({f == fallback ? f + " (default)" : f, f});
	}
	QWidget *row = choice(card, title, subtitle, key, fonts, fallback);
	if (auto *combo = row->findChild<QComboBox *>()) {
		// (Not each name in its own lettering: that loaded every font on the
		// computer when the list opened, and Settings froze.)
		combo->setMaxVisibleItems(14);
		flex_width(combo, 260);
	}
	return row;
}

void fonts_card(Page &page) {
	Card *fonts = card(page, "Fonts",
		"What everything is written in. Apps follow when “Style other apps” is on.");
	QWidget *ui_row = font_row(fonts, "Interface", "Dusk's panels and menus, this app, and other apps' text", "font",
		"Google Sans", false);
	number(fonts, "Text size", "Dusk's panels and menus, this app and other apps", "font_size", 7, 20, 1, 11, " pt");
	QWidget *mono_row = font_row(fonts, "Terminal and code", "Terminals, code editors and anything set in a fixed width",
		"mono_font", "Google Sans Code", true);
	number(fonts, "Terminal text size", QString(), "mono_font_size", 7, 24, 1, 11, " pt");

	// What they look like: follows the choices at once, and while you go
	// down a font list, shows the font under the pointer before you pick it.
	auto *preview = new QFrame;
	preview->setObjectName("phonerow");
	auto *pv = new QVBoxLayout(preview);
	pv->setContentsMargins(16, 12, 16, 14);
	pv->setSpacing(4);
	auto caption = [](const QString &text) {
		auto *l = new QLabel(text);
		l->setObjectName("hint");
		return l;
	};
	auto *ui_title = new QLabel("Evening settles over the desktop");
	auto *ui = new QLabel("The quick brown fox jumps over the lazy dog · 0123456789\n"
		"Menus, panels, notifications and the windows of your apps look like this.");
	ui->setWordWrap(true);
	auto *code = new QLabel("$ echo \"Hello, Dusk\"  # {}[]()<>=!0Oo1lI\n"
		"for (int i = 0; i < 10; i++) print(i);");
	code->setWordWrap(true);
	pv->addWidget(caption("INTERFACE"));
	pv->addWidget(ui_title);
	pv->addWidget(ui);
	pv->addSpacing(8);
	pv->addWidget(caption("TERMINAL AND CODE"));
	pv->addWidget(code);
	fonts->add(preview);
	auto css_for = [](const QString &family, double pt, bool bold) {
		QString fam = family;
		fam.remove('"').remove(';').remove('}');
		return QString("font-family: \"%1\"; font-size: %2pt;%3").arg(fam).arg(pt)
			.arg(bold ? " font-weight: 600;" : "");
	};
	// The window's style sheet names the interface font for every widget;
	// these say their own (a style sheet, so it wins).
	auto show = [ui_title, ui, code, css_for](const QString &ui_family = QString(), const QString &mono_family = QString()) {
		const QString uf = ui_family.isEmpty() ? ui_font() : ui_family;
		const QString mf = mono_family.isEmpty() ? settings->get("mono_font", "Google Sans Code") : mono_family;
		const int us = std::clamp(settings->get("font_size", "11").toInt(), 7, 20);
		const int ms = std::clamp(settings->get("mono_font_size", "11").toInt(), 7, 24);
		ui_title->setStyleSheet(css_for(uf, us + 4, true));
		ui->setStyleSheet(css_for(uf, us, false));
		code->setStyleSheet(css_for(mf, ms, false));
	};
	show();
	live_hooks.push_back([show, p = QPointer<QFrame>(preview)](const QString &key) {
		if (p && (key == "font" || key == "font_size" || key == "mono_font" || key == "mono_font_size" || key == "*"))
			show();
	});
	bindings.push_back(Binding{preview, nullptr, "", "", [show] { show(); }, {}, {}});
	// Trying fonts in the lists.
	for (QComboBox *combo : {ui_row->findChild<QComboBox *>(), mono_row->findChild<QComboBox *>()}) {
		if (!combo) continue;
		const bool is_mono = combo == mono_row->findChild<QComboBox *>();
		QObject::connect(combo, &QComboBox::highlighted, preview, [combo, is_mono, show](int i) {
			const QString fam = combo->itemData(i).toString();
			if (fam.isEmpty()) return;
			if (is_mono) show(QString(), fam);
			else show(fam, QString());
		});
		// The list closed without a pick: back to the chosen ones.
		class Closed : public QObject {
		public:
			std::function<void()> fn;
			bool eventFilter(QObject *, QEvent *e) override {
				if (e->type() == QEvent::Hide && fn) QTimer::singleShot(0, this, fn);
				return false;
			}
		};
		auto *closed = new Closed;
		closed->setParent(combo);
		closed->fn = [show] { show(); };
		combo->view()->window()->installEventFilter(closed);
	}

	// Google's fonts missing (no internet when Dusk was installed): get them here.
	if (!font_installed("Google Sans") || !font_installed("Google Sans Code")) {
		auto *get = new QPushButton("Download Google Sans and Google Sans Code");
		get->setObjectName("primary");
		get->setCursor(Qt::PointingHandCursor);
		auto *note = new QLabel("They're free (SIL Open Font License), from Google Fonts. Until then "
			"Dusk uses a standard font in their place.");
		note->setObjectName("hint");
		note->setWordWrap(true);
		auto *box = new QWidget;
		auto *v = new QVBoxLayout(box);
		v->setContentsMargins(0, 4, 0, 4);
		v->addWidget(note);
		v->addWidget(get, 0, Qt::AlignLeft);
		fonts->add(box);
		QObject::connect(get, &QPushButton::clicked, [get, note] {
			get->setEnabled(false);
			get->setText("Downloading…");
			const QString dir = QDir::homePath() + "/.local/share/fonts/dusk";
			const QString base = "https://github.com/google/fonts/raw/main/ofl/";
			const QString q = "'" + dir + "'";
			const QString script = "set -e; mkdir -p " + q + "; cd " + q + "; "
				"curl -fsSL -o GoogleSans.ttf '" + base + "googlesans/GoogleSans%5BGRAD%2Copsz%2Cwght%5D.ttf'; "
				"curl -fsSL -o GoogleSans-Italic.ttf '" + base + "googlesans/GoogleSans-Italic%5BGRAD%2Copsz%2Cwght%5D.ttf'; "
				"rm -f GoogleSansFlex.ttf; "
				"curl -fsSL -o GoogleSansCode.ttf '" + base + "googlesanscode/GoogleSansCode%5Bwght%5D.ttf'; "
				"curl -fsSL -o GoogleSansCode-Italic.ttf '" + base +
				"googlesanscode/GoogleSansCode-Italic%5Bwght%5D.ttf'; fc-cache -f " + q;
			auto *proc = new QProcess(get);
			QObject::connect(proc, &QProcess::finished, [proc, get, note](int code, QProcess::ExitStatus) {
				proc->deleteLater();
				if (code == 0) {
					get->setText("Done");
					note->setText("Installed. Apps pick the fonts up as they open; Dusk Settings shows them "
						"in the lists the next time it opens.");
					changed("font");
				} else {
					get->setEnabled(true);
					get->setText("Try again");
					note->setText("Couldn't download them (no internet?). ./build.sh fonts does the same from a terminal.");
				}
			});
			proc->start("sh", {"-c", script});
		});
	}
}

QWidget *appearance_page() {
	Page page = make_page("Appearance", "Light or dark, your accent colour and the wallpaper.");

	Card *style = card(page, "Style");
	choice(style, "Theme", QString(), "theme", {{"Dark", "dark"}, {"Light", "light"}}, "dark");
	toggle(style, "High contrast", "Accessibility: stronger window outlines and text that keeps its "
		"contrast on any wallpaper", "high_contrast", false);
	choice(style, "Icons", "In the panels, control centre and menus. App and file icons follow your icon theme.",
		"ui_icons", {{"Dusk's own", "dusk"}, {"From the icon theme", "theme"}}, "dusk");
	toggle(style, "Style other apps", "GNOME, GTK, Qt and Chrome-type apps follow light/dark and your accent",
		"style_apps", true);

	// Look presets, then the knobs they set.
	Card *look = card(page, "Look",
		"A preset sets the lines and shadows of panels, menus and windows. "
		"Change any of them below to mix your own.");
	{
		auto *box = new QWidget;
		auto *h = new QHBoxLayout(box);
		h->setContentsMargins(0, 4, 0, 4);
		h->setSpacing(10);
		std::vector<PresetButton *> buttons;
		for (const Preset &p : kPresets) {
			auto *b = new PresetButton(p);
			buttons.push_back(b);
			h->addWidget(b);
			QObject::connect(b, &QAbstractButton::clicked, [&p] { apply_preset(p); });
		}
		h->addStretch(1);
		look->add(box);
		auto repaint = [buttons] {
			for (auto *b : buttons) b->update();
		};
		restyle_hooks.push_back(repaint);
		bindings.push_back(Binding{box, nullptr, "", "", repaint, {}, {}});
	}
	choice(look, "Lines", "Outlines around panels, menus and cards", "ui_outline",
		{{"Ink (like line art)", "ink"}, {"Thin", "thin"}, {"None", "none"}}, "ink");
	choice(look, "Shadows", "Under panels, menus and popups", "ui_shadow",
		{{"Ink (short, drawn)", "ink"}, {"Soft", "soft"}, {"None", "none"}}, "ink");
	advanced(slider(look, "Roundness", "Corners of panels, menus and cards", "ui_radius", 8, 32, 22, 1,
		[](int v) { return QString("%1 px").arg(v); }));
	// Accent swatches + custom colour.
	{
		auto *box = new QWidget;
		auto *h = new QHBoxLayout(box);
		h->setContentsMargins(0, 0, 0, 0);
		h->setSpacing(6);
		std::vector<QPushButton *> buttons;
		for (const Swatch &s : kSwatches) {
			auto *b = new QPushButton;
			b->setFixedSize(26, 26);
			b->setToolTip(s.name);
			b->setCursor(Qt::PointingHandCursor);
			b->setProperty("hex", QString(s.hex));
			buttons.push_back(b);
			h->addWidget(b);
			QObject::connect(b, &QPushButton::clicked, [s] {
				settings->remove("accent_auto");
				settings->set("accent", s.hex);
				changed("accent");
			});
		}
		// "Any colour": a rainbow ring around a plus.
		auto *custom = new QPushButton;
		custom->setFixedSize(26, 26);
		custom->setToolTip("Any colour");
		custom->setCursor(Qt::PointingHandCursor);
		{
			QPixmap pm(52, 52);
			pm.fill(Qt::transparent);
			QPainter g(&pm);
			g.setRenderHint(QPainter::Antialiasing);
			QConicalGradient cg(26, 26, 90);
			for (int i = 0; i <= 6; i++) cg.setColorAt(i / 6.0, QColor::fromHsvF(std::fmod(i / 6.0, 1.0), 0.75, 0.95));
			g.setPen(QPen(QBrush(cg), 6));
			g.setBrush(Qt::NoBrush);
			g.drawEllipse(QRectF(4, 4, 44, 44));
			g.setPen(QPen(QColor(150, 150, 150), 4, Qt::SolidLine, Qt::RoundCap));
			g.drawLine(QPointF(26, 17), QPointF(26, 35));
			g.drawLine(QPointF(17, 26), QPointF(35, 26));
			g.end();
			pm.setDevicePixelRatio(2);
			custom->setIcon(QIcon(pm));
			custom->setIconSize(QSize(26, 26));
		}
		QObject::connect(custom, &QPushButton::clicked, [custom] {
			const QColor picked = QColorDialog::getColor(QColor(settings->get("accent", "#4d8df6")),
				custom->window(), "Choose an accent colour");
			if (picked.isValid()) {
				settings->remove("accent_auto");
				settings->set("accent", picked.name());
				changed("accent");
			}
		});
		h->addSpacing(6);
		h->addWidget(custom);
		auto *from_wall = new QPushButton("From wallpaper");
		from_wall->setCheckable(true);
		from_wall->setCursor(Qt::PointingHandCursor);
		from_wall->setToolTip("Take the accent from the wallpaper, and again whenever it changes");
		h->addSpacing(6);
		h->addWidget(from_wall);
		QObject::connect(from_wall, &QPushButton::clicked, [from_wall](bool on) {
			if (!on) {
				settings->remove("accent_auto");   // keeps the colour it has now
				changed("accent");
				return;
			}
			settings->set_bool("accent_auto", true);
			const QColor c = accent_from_wallpaper();
			if (c.isValid()) {
				settings->set("accent", c.name());
			}
			changed("accent");
			(void)from_wall;
		});
		auto show_wall = [from_wall] {
			from_wall->setVisible(settings->get("wallpaper_type", "dusk") == "image" &&
				!settings->get("wallpaper").isEmpty());
			const QSignalBlocker block(from_wall);
			from_wall->setChecked(settings->get_bool("accent_auto", false));
		};
		show_wall();
		restyle_hooks.push_back(show_wall);
		auto paint = [buttons, custom] {
			const QColor now(settings->get("accent", "#4d8df6"));
			bool any = false;
			const Look l = current_look();
			for (QPushButton *b : buttons) {
				const QColor c(b->property("hex").toString());
				const bool on = c == now;
				any = any || on;
				b->setStyleSheet(QString("QPushButton { background: %1; border-radius: 13px; "
					"border: %2; padding: 0; }").arg(c.name())
					.arg(on ? QString("3px solid %1").arg(l.text.name()) : QString("1px solid rgba(128,128,128,0.35)")));
			}
			// A custom colour shows as the fill of the "any colour" circle.
			custom->setStyleSheet(QString("QPushButton { background: %1; border-radius: 13px; border: %2; padding: 0; }")
				.arg(any ? QString("transparent") : now.name())
				.arg(any ? QString("none") : QString("3px solid %1").arg(l.text.name())));
		};
		paint();
		restyle_hooks.push_back(paint);
		QToolButton *reset = nullptr;
		QWidget *row = make_row(style, "Accent colour", "Highlights, the menu button, switches",
			box, &reset, true);
		QObject::connect(reset, &QToolButton::clicked, [] {
			settings->remove("accent");
			changed("accent");
		});
		bindings.push_back(Binding{row, reset, "accent", "#4d8df6", paint, {}, [] {
			return !settings->has("accent") || QColor(settings->get("accent")) == QColor("#4d8df6");
		}});
	}

	// See-through and blur: each part on its own.
	{
		auto pct = [](int v) { return QString("%1%").arg(v); };
		auto blur_pct = [](int v) { return v == 0 ? QString("Off") : QString("%1%").arg(v); };
		Card *menus = card(page, "Menus, control centre and popups",
			"Notifications, the media card, the OSD and right-click menus too.");
		slider(menus, "Opacity", "Lower = more see-through", "ui_opacity", 0, 100, 92, 1, pct);
		slider(menus, "Blur", "How much what's behind them is blurred", "ui_blur", 0, 100, 100, 1, blur_pct);
		Card *panels = card(page, "Panels",
			"Each panel's own background opacity is in Panels (Look › Background).");
		slider(panels, "Blur", "Behind every panel", "panel_blur", 0, 100, 100, 1, blur_pct);
		Card *app = card(page, "This Settings app");
		slider(app, "Opacity", settings_translucent ? QString("100% is solid")
			: QString("Close and reopen Settings after lowering it the first time"), "settings_opacity", 20, 100, 100, 1, pct);
		slider(app, "Blur", "Behind the window when it's see-through", "settings_blur", 0, 100, 100, 1, blur_pct);
		toggle(app, "Its own header bar", "Like GNOME apps (off: under Dusk's title bar). Takes effect when "
			"Settings opens again", "settings_csd", true);
	}

	// Wallpaper
	fonts_card(page);

	Card *wall = card(page, "Wallpaper");
	{
		auto *preview = new QLabel;
		preview->setFixedSize(320, 180);
		auto paint = [preview] { preview->setPixmap(wallpaper_preview()); };
		QPointer<QLabel> weak_preview(preview);
		g_preview_repaint = [weak_preview] {
			if (weak_preview) weak_preview->setPixmap(wallpaper_preview());
		};
		paint();
		restyle_hooks.push_back(paint);
		auto *holder = new QWidget;
		auto *h = new QHBoxLayout(holder);
		h->setContentsMargins(0, 8, 0, 8);
		h->addWidget(preview);
		h->addStretch(1);
		wall->add(holder);
		bindings.push_back(Binding{holder, nullptr, "", "", paint, {}, {}});
	}
	auto is_image = [] { return settings->get("wallpaper_type", "dusk") == "image"; };
	choice(wall, "Wallpaper", QString(), "wallpaper_type",
		{{"Dusk evening", "dusk"}, {"Halftone dots", "halftone"}, {"Solid colour", "solid"},
			{"Picture", "image"}}, "dusk");
	// Pictures to pick from: your Wallpapers folder, the system's wallpapers,
	// and any folders you add.
	{
		auto *holder = new QWidget;
		auto *grid = new QGridLayout(holder);
		grid->setContentsMargins(0, 8, 0, 8);
		grid->setSpacing(8);
		auto thumbs = std::make_shared<std::vector<QPointer<QPushButton>>>();
		auto paint = [thumbs] {
			const Look l = current_look();
			const bool image = settings->get("wallpaper_type", "dusk") == "image";
			const QString now = settings->get("wallpaper");
			for (auto &b : *thumbs) {
				if (!b) continue;
				const bool on = image && b->property("path").toString() == now;
				b->setStyleSheet(QString("QPushButton { background: %1; border-radius: 10px; padding: 0; "
					"border: %2; }").arg(QString("rgba(%1,%2,%3,%4)").arg(l.hover.red()).arg(l.hover.green())
					.arg(l.hover.blue()).arg(l.hover.alphaF()))
					.arg(on ? QString("3px solid %1").arg(l.accent.name()) : QString("1px solid transparent")));
			}
		};
		auto *empty = new QLabel;
		empty->setObjectName("subtitle");
		empty->setWordWrap(true);
		auto fill = [grid, thumbs, paint, empty] {
			for (auto &b : *thumbs) {
				if (b) b->deleteLater();
			}
			thumbs->clear();
			QStringList files;
			auto collect = [&files](const QString &dir, int limit) {
				if (dir.isEmpty() || !QDir(dir).exists()) return;
				QDirIterator it(dir, {"*.jpg", "*.jpeg", "*.png", "*.webp", "*.jxl", "*.avif", "*.JPG", "*.PNG"},
					QDir::Files, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
				QStringList found;
				while (it.hasNext() && found.size() < limit) {
					const QString f = it.next();
					// Fedora ships light/dark pairs and small sizes; skip tiny ones.
					if (QFileInfo(f).size() < 40 * 1024) continue;
					found << f;
				}
				found.sort();
				files << found;
			};
			for (const QString &d : wallpaper_folders()) collect(d, 80);
			files.removeDuplicates();
			while (files.size() > 160) files.removeLast();
			empty->setVisible(files.isEmpty());
			empty->setText("No pictures yet. Put some in " + QDir::homePath() + "/Pictures/Wallpapers, or add a folder below.");
			for (int i = 0; i < files.size(); i++) {
				auto *b = new QPushButton;
				b->setFixedSize(112, 66);
				b->setIconSize(QSize(104, 58));
				b->setCursor(Qt::PointingHandCursor);
				b->setToolTip(QFileInfo(files[i]).fileName());
				b->setProperty("path", files[i]);
				grid->addWidget(b, i / 5, i % 5);
				thumbs->push_back(b);
				QObject::connect(b, &QPushButton::clicked, [path = files[i]] {
					settings->set("wallpaper_type", "image");
					settings->set("wallpaper", path);
					// "From wallpaper": the accent comes from the thumbnail right
					// away, written with the picture (Dusk would otherwise have to
					// read the whole photo first).
					if (settings->get_bool("accent_auto", false)) {
						const QImage thumb = cached_picture(path, QSize(208, 116), {});
						if (!thumb.isNull() && thumb.width() > 1) {
							const QColor c = accent_from_image(thumb);
							if (c.isValid()) {
								settings->set("accent", c.name());
								settings->set("accent_for", path);
								apply_look(*the_app);
							}
						}
					}
					changed("wallpaper");   // the preview and the highlighted picture
					refresh_all();
				});
				// Thumbnails are read in the background, so the window opens at once.
				QPointer<QPushButton> weak(b);
				const QString path = files[i];
				auto show = [weak, path] {
					if (!weak) return;
					const QSize want(208, 116);
					const QImage img = cached_picture(path, want, {});
					if (img.isNull()) return;
					if (img.width() <= 1) {
						weak->hide();   // a format Qt can't read
						return;
					}
					QPixmap pm(want);
					pm.fill(Qt::transparent);
					QPainter p(&pm);
					p.setRenderHint(QPainter::Antialiasing);
					p.setRenderHint(QPainter::SmoothPixmapTransform);
					QPainterPath clip;
					clip.addRoundedRect(QRectF(0, 0, want.width(), want.height()), 14, 14);
					p.setClipPath(clip);
					const QImage scaled = img.scaled(want, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
					p.drawImage(QPoint((want.width() - scaled.width()) / 2, (want.height() - scaled.height()) / 2), scaled);
					p.end();
					pm.setDevicePixelRatio(2);
					weak->setIcon(QIcon(pm));
				};
				if (!cached_picture(path, QSize(208, 116), show).isNull()) show();
			}
			grid->setColumnStretch(5, 1);
			paint();
		};
		fill();
		restyle_hooks.push_back(paint);
		auto *box = new QWidget;
		auto *v = new QVBoxLayout(box);
		v->setContentsMargins(0, 0, 0, 0);
		v->addWidget(empty);
		v->addWidget(holder);
		wall->add(box);
		bindings.push_back(Binding{box, nullptr, "", "", paint, {}, {}});

		// Where the pictures come from.
		auto *folders = new QLabel;
		folders->setObjectName("subtitle");
		folders->setWordWrap(true);
		folders->setTextInteractionFlags(Qt::TextSelectableByMouse);
		auto *add = new QPushButton("Add folder…");
		add->setCursor(Qt::PointingHandCursor);
		auto *remove = new QPushButton("Remove…");
		remove->setCursor(Qt::PointingHandCursor);
		auto *btns = new QWidget;
		auto *h = new QHBoxLayout(btns);
		h->setContentsMargins(0, 0, 0, 0);
		h->addWidget(add);
		h->addWidget(remove);
		auto show_folders = [folders, remove] {
			const QStringList own = settings->get("wallpaper_folders").split('|', Qt::SkipEmptyParts);
			QStringList lines = {QDir::homePath() + "/Pictures/Wallpapers", "System wallpapers (/usr/share/backgrounds, /usr/share/wallpapers)"};
			lines << own;
			folders->setText(lines.join("\n"));
			remove->setVisible(!own.isEmpty());
		};
		show_folders();
		QWidget *row = make_row(wall, "Folders", QString(), btns, nullptr);
		if (auto *outer = qobject_cast<QVBoxLayout *>(row->layout())) outer->addWidget(folders);
		QObject::connect(add, &QPushButton::clicked, [add, fill, show_folders] {
			const QString dir = QFileDialog::getExistingDirectory(add->window(), "Add a folder of wallpapers",
				QDir::homePath() + "/Pictures");
			if (dir.isEmpty()) return;
			QStringList own = settings->get("wallpaper_folders").split('|', Qt::SkipEmptyParts);
			if (!own.contains(dir)) own << dir;
			settings->set("wallpaper_folders", own.join('|'));
			show_folders();
			fill();
		});
		QObject::connect(remove, &QPushButton::clicked, [remove, fill, show_folders] {
			QStringList own = settings->get("wallpaper_folders").split('|', Qt::SkipEmptyParts);
			if (own.isEmpty()) return;
			QMenu menu(remove);
			for (const QString &d : own) {
				menu.addAction(d, [d, fill, show_folders] {
					QStringList now = settings->get("wallpaper_folders").split('|', Qt::SkipEmptyParts);
					now.removeAll(d);
					if (now.isEmpty()) settings->remove("wallpaper_folders");
					else settings->set("wallpaper_folders", now.join('|'));
					show_folders();
					fill();
				});
			}
			menu.exec(remove->mapToGlobal(QPoint(0, remove->height())));
		});
	}
	{
		auto *pick = new QPushButton("Choose…");
		auto *name = new QLabel;
		name->setObjectName("subtitle");
		auto *box = new QWidget;
		auto *h = new QHBoxLayout(box);
		h->setContentsMargins(0, 0, 0, 0);
		h->addWidget(name);
		h->addWidget(pick);
		auto refresh = [name] {
			const QString p = settings->get("wallpaper");
			name->setText(p.isEmpty() ? "None chosen" : QFileInfo(p).fileName());
		};
		refresh();
		QWidget *row = make_row(wall, "Picture", QString(), box, nullptr);
		QObject::connect(pick, &QPushButton::clicked, [pick, refresh] {
			const QString start = settings->get("wallpaper").isEmpty()
				? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
				: QFileInfo(settings->get("wallpaper")).absolutePath();
			const QString file = QFileDialog::getOpenFileName(pick->window(), "Choose a wallpaper",
				start, "Pictures (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.tif *.tiff *.svg)");
			if (!file.isEmpty()) {
				settings->set("wallpaper", file);
				refresh();
				changed("wallpaper");
				restyle_hooks.run();
			}
		});
		bindings.push_back(Binding{row, nullptr, "wallpaper", "", refresh, is_image, {}});
	}
	choice(wall, "Position", QString(), "wallpaper_mode",
		{{"Fill the screen", "fill"}, {"Fit inside", "fit"}, {"Centre", "center"},
			{"Stretch", "stretch"}}, "fill", is_image);
	color_row(wall, "Colour", "wallpaper_color", "#2b2d3a",
		[] { return settings->get("wallpaper_type", "dusk") == "solid"; });
	choice(wall, "Transition", "How a new wallpaper comes in", "wallpaper_transition",
		{{"Random", "random"}, {"Fade", "fade"}, {"Slide", "slide"}, {"Push", "push"}, {"Zoom", "zoom"},
			{"Diagonal wipe", "wipe"}, {"Grow", "grow"}, {"Circle from the middle", "circle"},
			{"Circle from the pointer", "pointer"}, {"Iris (the old one closes)", "iris"}, {"None", "none"}}, "random");
	advanced(slider(wall, "Transition length", QString(), "wallpaper_transition_ms", 150, 4000, 900, 1,
		[](int v) { return QString("%1 s").arg(v / 1000.0, 0, 'f', 1); },
		[] { return settings->get("wallpaper_transition", "random") != "none"; }));

	// Icons and cursor
	Card *cursor = card(page, "Icons and cursor");
	{
		Items icons = {{"Same as GNOME apps", ""}};
		bool papirus = false;
		for (const auto &[shown, name] : icon_themes()) {
			// Papirus picks its dark version in dark mode by itself.
			icons.push_back({name == "Papirus" ? shown + " (follows light / dark)" : shown, name});
			papirus = papirus || name == "Papirus";
		}
		choice(cursor, "Icon theme", "Apps, the panels and menus", "icon_theme", icons, papirus ? "Papirus" : "");
	}
	Items themes = {{"Same as GNOME apps", ""}};
	const QStringList cursors = cursor_themes();
	for (const QString &t : cursors) {
		themes.push_back({t, t});
	}
	choice(cursor, "Cursor theme", QString(), "cursor_theme", themes,
		cursors.contains("Bibata-Modern-Classic") ? "Bibata-Modern-Classic" : "");
	advanced(choice(cursor, "Cursor size", QString(), "cursor_size",
		{{"Same as GNOME apps", "0"}, {"24", "24"}, {"32", "32"}, {"40", "40"}, {"48", "48"},
			{"64", "64"}}, "0"));

	more_toggle(page);
	finish(page);
	return page.widget;
}

// ---- Windows --------------------------------------------------------------------

// ---- Snap layouts (tiling) -----------------------------------------------------------

const QString kDefaultLayouts = "50-50/1-1;66-34/1-1;33-34-33/1-1-1;50-50/1-2;50-50/2-2;25-50-25/1-1-1";

// "50-50/1-2" -> zones (x, y, w, h fractions), like Dusk reads it.
std::vector<QRectF> layout_zones(const QString &spec) {
	const QStringList halves = spec.split('/');
	QList<double> cols;
	for (const QString &c : halves.value(0).split('-', Qt::SkipEmptyParts)) {
		if (c.toDouble() > 0) cols << c.toDouble();
	}
	QList<int> rows;
	for (const QString &r : halves.value(1).split('-', Qt::SkipEmptyParts)) rows << std::clamp(r.toInt(), 1, 4);
	double sum = 0;
	for (double c : cols) sum += c;
	std::vector<QRectF> out;
	double x = 0;
	for (int i = 0; i < cols.size(); i++) {
		const double w = cols[i] / sum;
		const int n = i < rows.size() ? rows[i] : 1;
		for (int r = 0; r < n; r++) out.push_back(QRectF(x, double(r) / n, w, 1.0 / n));
		x += w;
	}
	return out;
}

// A little screen with the layout's zones, like the one Dusk shows.
QPixmap layout_preview(const QString &spec, int w = 84, int h = 52) {
	const Look l = current_look();
	QPixmap pm(w * 2, h * 2);
	pm.setDevicePixelRatio(2);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	QColor screen = l.text;
	screen.setAlphaF(0.08);
	p.setPen(Qt::NoPen);
	p.setBrush(screen);
	p.drawRoundedRect(QRectF(0, 0, w, h), 8, 8);
	QColor zone = l.text;
	zone.setAlphaF(0.25);
	p.setBrush(zone);
	for (const QRectF &z : layout_zones(spec)) {
		p.drawRoundedRect(QRectF(3 + z.x() * (w - 6) + 1.5, 3 + z.y() * (h - 6) + 1.5,
			z.width() * (w - 6) - 3, z.height() * (h - 6) - 3), 4, 4);
	}
	return pm;
}

void tiling_card(Page &page) {
	Card *c = card(page, "Tiling",
		"Drag a window to the top of the screen, rest on its maximise button, or press Super+Z "
		"to pick a spot from these layouts.");
	toggle(c, "Layouts bar when dragging to the top", QString(), "snap_bar", true);
	advanced(slider(c, "Gap between tiled windows", QString(), "tile_gap", 0, 24, 0, 1,
		[](int v) { return v == 0 ? QString("None") : QString("%1 px").arg(v); }));

	// The layouts: drag to reorder, remove, add your own.
	auto *box = new QWidget;
	auto *v = new QVBoxLayout(box);
	v->setContentsMargins(0, 8, 0, 10);
	v->setSpacing(8);
	auto *list = new QListWidget;
	list->setObjectName("editor");
	list->setViewMode(QListView::IconMode);
	list->setFlow(QListView::LeftToRight);
	list->setWrapping(true);
	list->setMovement(QListView::Snap);
	list->setDragDropMode(QAbstractItemView::InternalMove);
	list->setDefaultDropAction(Qt::MoveAction);
	list->setIconSize(QSize(84, 52));
	list->setGridSize(QSize(100, 68));
	list->setFixedHeight(160);
	list->setResizeMode(QListView::Adjust);
	v->addWidget(list);

	// Builder: columns, then rows in each column.
	auto *builder = new QHBoxLayout;
	builder->setSpacing(8);
	auto *cols = new ElideCombo;
	flex_width(cols, 200);
	const std::pair<const char *, const char *> kCols[] = {
		{"Halves (50 / 50)", "50-50"}, {"60 / 40", "60-40"}, {"40 / 60", "40-60"},
		{"Two thirds / one third", "66-34"}, {"One third / two thirds", "34-66"}, {"70 / 30", "70-30"},
		{"Thirds", "33-34-33"}, {"Wide middle (25 / 50 / 25)", "25-50-25"},
		{"Half and two quarters", "50-25-25"}, {"Two quarters and half", "25-25-50"},
		{"Quarters (4 columns)", "25-25-25-25"},
	};
	for (const auto &[label, value] : kCols) cols->addItem(label, value);
	builder->addWidget(cols);
	std::vector<ElideCombo *> rows;
	for (int i = 0; i < 4; i++) {
		auto *r = new ElideCombo;
		r->setFixedWidth(110);
		r->addItem("1 row", 1);
		r->addItem("2 rows", 2);
		r->addItem("3 rows", 3);
		rows.push_back(r);
		builder->addWidget(r);
	}
	auto *preview = new QLabel;
	preview->setFixedSize(84, 52);
	builder->addWidget(preview);
	auto *add = new QPushButton("Add");
	add->setObjectName("primary");
	builder->addWidget(add);
	builder->addStretch(1);
	v->addLayout(builder);
	auto *buttons = new QHBoxLayout;
	auto *remove = new QPushButton("Remove selected");
	auto *defaults = new QPushButton("Back to the defaults");
	buttons->addWidget(remove);
	buttons->addWidget(defaults);
	buttons->addStretch(1);
	v->addLayout(buttons);
	c->add(box);

	auto spec_now = [cols, rows] {
		const QString cs = cols->currentData().toString();
		const int n = cs.count('-') + 1;
		QStringList r;
		for (int i = 0; i < n; i++) r << QString::number(rows[i]->currentData().toInt());
		return cs + "/" + r.join('-');
	};
	auto update_builder = [cols, rows, preview, spec_now] {
		const int n = cols->currentData().toString().count('-') + 1;
		for (int i = 0; i < 4; i++) rows[i]->setVisible(i < n);
		preview->setPixmap(layout_preview(spec_now()));
	};
	auto fill = [list] {
		const QSignalBlocker block(list);
		list->clear();
		for (const QString &spec : settings->get("snap_layouts", kDefaultLayouts).split(';', Qt::SkipEmptyParts)) {
			auto *item = new QListWidgetItem(QIcon(layout_preview(spec.trimmed())), QString());
			item->setData(Qt::UserRole, spec.trimmed());
			item->setToolTip(spec.trimmed());
			list->addItem(item);
		}
	};
	auto store = [list] {
		QStringList specs;
		for (int i = 0; i < list->count(); i++) specs << list->item(i)->data(Qt::UserRole).toString();
		if (specs.join(';') == kDefaultLayouts) settings->remove("snap_layouts");
		else settings->set("snap_layouts", specs.join(';'));
		changed("snap_layouts");
	};
	fill();
	update_builder();
	QObject::connect(cols, &QComboBox::currentIndexChanged, update_builder);
	for (ElideCombo *r : rows) QObject::connect(r, &QComboBox::currentIndexChanged, update_builder);
	QObject::connect(add, &QPushButton::clicked, [list, spec_now, store] {
		const QString spec = spec_now();
		auto *item = new QListWidgetItem(QIcon(layout_preview(spec)), QString());
		item->setData(Qt::UserRole, spec);
		item->setToolTip(spec);
		list->addItem(item);
		store();
	});
	QObject::connect(remove, &QPushButton::clicked, [list, store] {
		if (list->count() <= 1) return;   // keep at least one
		delete list->takeItem(list->currentRow() >= 0 ? list->currentRow() : list->count() - 1);
		store();
	});
	QObject::connect(defaults, &QPushButton::clicked, [fill] {
		settings->remove("snap_layouts");
		fill();
		changed("snap_layouts");
	});
	QObject::connect(list->model(), &QAbstractItemModel::rowsMoved, [store] { store(); });
	restyle_hooks.push_back([fill, update_builder] { fill(); update_builder(); });
}

QWidget *windows_page() {
	Page page = make_page("Windows", "How windows look and behave.");

	Card *frame = card(page, "Frame");
	choice(frame, "Border", QString(), "border",
		{{"Bold outline", "outline"}, {"Thin line", "thin"}, {"None", "none"}}, "outline");
	advanced(toggle(frame, "Accent outline", "The active window's border in the accent colour",
		"accent_border", false, [] { return settings->get("border", "thin") != "none"; }));
	choice(frame, "Shadow", QString(), "shadow",
		{{"Ink", "ink"}, {"Hard", "hard"},
			{"Soft", "soft"}, {"None", "none"}}, "ink");
	slider(frame, "Corner radius", QString(), "window_radius", 0, 24, 12, 1,
		[](int v) { return v == 0 ? QString("Square") : QString("%1 px").arg(v); });

	Card *title = card(page, "Title bar");
	choice(title, "Buttons", "Round grey buttons like GNOME's, corner brackets with a red ✕, or plain", "button_style",
		{{"GNOME", "gtk"}, {"Drawn", "sketch"}, {"Plain", "plain"}}, "gtk");
	toggle(title, "Minimise button", QString(), "show_minimize", true);
	toggle(title, "Maximise button", "Double-click the title bar or drag it to the top to maximise", "show_maximize", false);
	choice(title, "Button side", QString(), "buttons_side", {{"Right", "right"}, {"Left", "left"}}, "right");
	toggle(title, "GNOME and KDE apps match",
		"Apps that draw their own title bar get these buttons too, on the same side",
		"apps_follow_buttons", true);
	toggle(title, "Show the window's name", QString(), "show_titles", false);
	advanced(choice(title, "Title", QString(), "title_align",
		{{"Centred", "center"}, {"Left, with the app icon", "left"}}, "center",
		[] { return settings->get_bool("show_titles", false); }));
	advanced(number(title, "Height", "GNOME's header bars are 46", "titlebar_height", 24, 60, 1, 36, " px"));
	advanced(choice(title, "Double-click", QString(), "double_click",
		{{"Maximise", "maximize"}, {"Minimise", "minimize"}, {"Nothing", "none"}}, "maximize"));
	advanced(choice(title, "Middle-click", QString(), "middle_click",
		{{"Minimise", "minimize"}, {"Nothing", "none"}}, "minimize"));

	Card *behaviour = card(page, "Behaviour");
	toggle(behaviour, "Games can skip vsync",
		"A full-screen game that asks for it shows each frame at once: less input lag, maybe some tearing",
		"allow_tearing", true);
	choice(behaviour, "New windows open", QString(), "placement",
		{{"In the centre", "center"}, {"Cascaded", "cascade"}, {"Under the pointer", "cursor"}}, "center");
	toggle(behaviour, "Snap to screen edges",
		"Drag to the left or right edge for half the screen, or to the top to maximise",
		"edge_snap", true);
	advanced(toggle(behaviour, "Focus follows the mouse", "Windows get the keyboard when the pointer is over them",
		"focus_follows_mouse", false));

	tiling_card(page);

	Card *screen = card(page, "Screen");
	advanced(slider(screen, "Rounded screen corners", QString(), "screen_corners", 0, 40, 14, 1,
		[](int v) { return v == 0 ? QString("Off") : QString("%1 px").arg(v); }));

	more_toggle(page);
	finish(page);
	return page.widget;
}

// ---- Workspaces & overview ----------------------------------------------------

QWidget *workspaces_page() {
	Page page = make_page("Workspaces", "Desktops side by side, and the overview that shows them (like GNOME).");
	Card *ws = card(page, "Workspaces", "Super+1…9 goes straight to one, Super+Page Up / Down or Ctrl+Alt+Left / Right "
		"switch; add Shift to take the window along. Scrolling over the overview button on a panel switches too.");
	choice(ws, "How many", "Add one with New workspace (the overview, or right-click the overview button)",
		"workspaces_dynamic", {{"The ones you make", "false"}, {"Automatic, like GNOME (one empty at the end)", "true"}},
		"false");
	number(ws, "Workspaces", QString(), "workspace_count", 1, 12, 1, 1, QString(),
		[] { return !settings->get_bool("workspaces_dynamic", false); });
	toggle(ws, "Go round", "Past the last workspace comes the first", "workspace_wrap", false,
		[] { return !settings->get_bool("workspaces_dynamic", false); });
	choice(ws, "Open apps on panels", "Clicking one on another workspace takes you there", "tasks_all_workspaces",
		{{"From every workspace", "true"}, {"Only this workspace's", "false"}}, "true");

	Card *ov = card(page, "Overview", "Your windows side by side, with the other workspaces peeking in at the "
		"sides. Type to find an open window; drag a window onto a workspace (or onto New) to move it there.");
	choice(ov, "Tapping Super opens", "Super+S always opens the overview", "super_key",
		{{"The app menu", "menu"}, {"The overview", "overview"}}, "menu");
	choice(ov, "Hot corner", "Push the pointer into this corner to open the overview", "hot_corner",
		{{"Top left", "top-left"}, {"Top right", "top-right"}, {"Bottom left", "bottom-left"},
			{"Bottom right", "bottom-right"}, {"Off", "none"}}, "top-left");
	toggle(ov, "Show minimised windows", "A little faded; click one to bring it back", "overview_minimized", true);
	choice(ov, "Workspace pictures", "Small pictures of every workspace: click one to go there, drag a window onto one "
		"to move it, + for a new one", "overview_strip",
		{{"At the top", "top"}, {"At the bottom", "bottom"}, {"On the left", "left"}, {"On the right", "right"},
			{"Off (a label instead)", "off"}}, "top");

	Card *btn = card(page, "Overview button", "On a panel (add \"Overview and workspaces\" in Panels).");
	choice(btn, "Looks like", QString(), "overview_icon",
		{{"Dots, one per workspace (GNOME)", "dots"},
			{"LED segments, one per workspace", "led"},
			{"Numbers: 1 2 3", "numbers"},
			{"Roman numerals: I II III", "roman"}, {"Letters: A B C", "letters"}, {"Short lines", "lines"},
			{"Rings", "rings"}, {"Small squares", "boxes"}, {"Which of how many: 2 / 4", "count"},
			{"Your distribution's logo", "distro"},
			{"A picture of your own…", "custom"}}, "count");
	{
		auto *box = new QWidget;
		auto *h = new QHBoxLayout(box);
		h->setContentsMargins(0, 0, 0, 0);
		h->setSpacing(8);
		auto *edit = new QLineEdit;
		edit->setPlaceholderText("Picture file or icon name");
		flex_width(edit, 200);
		auto *pick = new QPushButton("Choose…");
		pick->setCursor(Qt::PointingHandCursor);
		h->addWidget(edit);
		h->addWidget(pick);
		QWidget *row = make_row(btn, "Picture", "PNG, SVG or an icon theme name", box, nullptr);
		auto refresh = [edit] {
			if (edit->text() != settings->get("overview_icon_custom")) {
				const QSignalBlocker block(edit);
				edit->setText(settings->get("overview_icon_custom"));
			}
		};
		refresh();
		QObject::connect(edit, &QLineEdit::textChanged, [](const QString &t) {
			if (t.trimmed().isEmpty()) settings->remove("overview_icon_custom");
			else settings->set("overview_icon_custom", t.trimmed());
			changed("overview_icon_custom");
		});
		QObject::connect(pick, &QPushButton::clicked, [pick, edit] {
			const QString file = QFileDialog::getOpenFileName(pick->window(), "Choose a picture",
				QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
				"Pictures (*.png *.svg *.jpg *.jpeg *.webp)");
			if (!file.isEmpty()) edit->setText(file);
		});
		bindings.push_back(Binding{row, nullptr, "overview_icon_custom", "", refresh,
			[] { return settings->get("overview_icon", "count") == "custom"; }, {}});
	}
	finish(page);
	return page.widget;
}

// ---- Panels -----------------------------------------------------------------------

const Items kWidgets = {
	{"App menu", "menu"}, {"Overview and workspaces", "overview"}, {"Apps (pinned and open)", "tasks"},
	{"Flexible space", "spacer"}, {"Separator", "separator"}, {"Clock", "clock"},
	{"Status icons (control centre)", "status"}, {"Now playing", "media"},
	{"Clipboard", "clipboard"}, {"App status icons (tray)", "tray"}, {"Trash", "trash"},
	{"Show desktop", "showdesktop"}, {"On-screen keyboard", "keyboard"},
	{"Notifications (bell)", "notifications"},
};
const QString kDefaultWidgets = "menu,tasks,spacer,overview,spacer,media,clipboard,tray,status,notifications,clock";

QString widget_label(const QString &value) {
	for (const auto &[label, v] : kWidgets) {
		if (v == value) return label;
	}
	return value;
}

QStringList panel_ids() {
	if (!settings->has("panels")) {
		return {"main"};
	}
	QStringList ids;
	for (const QString &p : settings->get("panels").split(',', Qt::SkipEmptyParts)) {
		if (!p.trimmed().isEmpty()) ids << p.trimmed();
	}
	return ids;
}

QString panel_edge(const QString &id) {
	return settings->get("panel." + id + ".edge", id == "main" ? settings->get("panel_position", "bottom") : "bottom");
}

QString panel_name(const QString &id) {
	const QString edge = panel_edge(id);
	// "Panel 1", "Panel 2"… (or the name you gave it), and where it is now:
	// a panel keeps its name when it moves to another edge.
	const QString where = edge == "top" ? "top" : edge == "left" ? "left" : edge == "right" ? "right" : "bottom";
	const QString own = settings->get("panel." + id + ".name").trimmed();
	const int n = std::max(0, static_cast<int>(panel_ids().indexOf(id))) + 1;
	return (own.isEmpty() ? QString("Panel %1").arg(n) : own) + "  ·  " + where;
}

// Rebuilt whenever another panel is picked, or panels are added/removed.
class PanelsPage {
public:
	QWidget *widget;

	PanelsPage() {
		Page page = make_page("Panels & dock",
			"Bars along the screen edges. Add as many as you like, put them on any edge, and "
			"choose what's on them. Right-click a panel on the desktop for quick options.");
		layout_ = page.layout;

		// Picker row: which panel, add, remove.
		auto *picker = new QWidget;
		auto *h = new QHBoxLayout(picker);
		h->setContentsMargins(0, 6, 0, 0);
		combo_ = new ElideCombo;
		flex_width(combo_, 240);
		auto *add = new QPushButton("Add panel");
		add->setObjectName("primary");
		remove_ = new QPushButton("Remove");
		h->addWidget(combo_);
		h->addWidget(add);
		h->addWidget(remove_);
		h->addStretch(1);
		layout_->addWidget(picker);

		// The look: how the panels are drawn, and (picking one) a matching
		// arrangement of them, like that desktop's.
		{
			Card *look = card(page, "Look",
				"Choosing one also sets the panels up like that desktop (where they are, their size and "
				"what's on them); you can change any of it afterwards. Your pinned apps stay.");
			choice(look, "Panels look like", QString(), "panel_look",
				{{"Dusk", "dusk"}, {"Windows 11", "windows"}, {"GNOME", "gnome"}, {"macOS", "macos"}}, "dusk");
			choice(look, "App menu looks like", "Dusk: a list by the button · Windows 11: Start · "
				"GNOME: the app grid · macOS: Launchpad", "launcher_look",
				{{"Dusk", "dusk"}, {"Windows 11", "windows"}, {"GNOME", "gnome"}, {"macOS", "macos"}}, "dusk");
			toggle(look, "Magnify the dock", "macOS look, Dock style: icons grow as the pointer passes over them",
				"dock_magnify", false);
			live_hooks.push_back([this](const QString &key) {
				if (key != "panel_look" || applying_) return;
				applying_ = true;
				apply_panel_look(settings->get("panel_look", "dusk"));
				applying_ = false;
			});
		}

		holder_ = new QWidget;
		holder_layout_ = new QVBoxLayout(holder_);
		holder_layout_->setContentsMargins(0, 0, 0, 0);
		holder_layout_->setSpacing(10);
		layout_->addWidget(holder_);

		// Settings for every panel.
		Card *clock = card(page, "Clock and status icons");
		toggle(clock, "24-hour clock", QString(), "clock_24h", true);
		toggle(clock, "Show the date", QString(), "show_date", true);
		choice(clock, "Battery", QString(), "battery_style",
			{{"Battery icon", "icon"}, {"Icon and percentage (coloured)", "percent"}, {"Ring with the number", "ring"}},
			"percent", [] { return has_battery(); });
		choice(clock, "Clicking the clock", QString(), "clock_click",
			{{"Opens the calendar", "calendar"}, {"Opens the notifications", "notifications"},
			 {"Opens the control centre", "controlcentre"}},
			"calendar");
		choice(clock, "Weeks start on", QString(), "week_start",
			{{"Monday", "monday"}, {"Sunday", "sunday"}}, "sunday",
			[] { return settings->get("clock_click", "calendar") == "calendar"; });

		Card *menu = card(page, "App menu button");
		choice(menu, "Icon", QString(), "menu_icon",
			{{"Dusk's logo", "dusk"}, {"Dots", "dots"}, {"Squares", "squares"}, {"Search", "search"},
				{"Your distribution's logo", "distro"}, {"A picture of your own…", "custom"}}, "dusk");
		{
			auto *box = new QWidget;
			auto *h = new QHBoxLayout(box);
			h->setContentsMargins(0, 0, 0, 0);
			h->setSpacing(8);
			auto *edit = new QLineEdit;
			edit->setPlaceholderText("Picture file or icon name");
			flex_width(edit, 200);
			auto *pick = new QPushButton("Choose…");
			pick->setCursor(Qt::PointingHandCursor);
			h->addWidget(edit);
			h->addWidget(pick);
			QWidget *row = make_row(menu, "Picture", "PNG, SVG or an icon theme name such as start-here",
				box, nullptr);
			auto refresh = [edit] {
				if (edit->text() != settings->get("menu_icon_custom")) {
					const QSignalBlocker block(edit);
					edit->setText(settings->get("menu_icon_custom"));
				}
			};
			refresh();
			QObject::connect(edit, &QLineEdit::textChanged, [](const QString &t) {
				if (t.trimmed().isEmpty()) settings->remove("menu_icon_custom");
				else settings->set("menu_icon_custom", t.trimmed());
				changed("menu_icon_custom");
			});
			QObject::connect(pick, &QPushButton::clicked, [pick, edit] {
				const QString file = QFileDialog::getOpenFileName(pick->window(), "Choose a picture",
					QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
					"Pictures (*.png *.svg *.jpg *.jpeg *.webp)");
				if (!file.isEmpty()) edit->setText(file);
			});
			bindings.push_back(Binding{row, nullptr, "menu_icon_custom", "", refresh,
				[] { return settings->get("menu_icon", "dusk") == "custom"; }, {}});
		}
		finish(page);
		widget = page.widget;

		QObject::connect(combo_, &QComboBox::currentIndexChanged, [this](int) {
			current_ = combo_->currentData().toString();
			build();
		});
		QObject::connect(add, &QPushButton::clicked, [this] { add_panel(); });
		QObject::connect(remove_, &QPushButton::clicked, [this] { remove_panel(); });
		fill_combo();
	}

	void refresh_from_file() {
		// Only when something about the panels changed: rebuilding for every
		// change of the file (Dusk writes the accent after a new wallpaper,
		// for one) threw away rows you might be using.
		const QString sig = signature();
		if (sig == last_sig_) return;
		fill_combo();
	}

private:
	bool applying_ = false;
	QString last_sig_;

	static QString signature() {
		QString sig = settings->get("panels") + "|" + settings->get("panel_position") + "|" + settings->get("panel_style");
		for (const QString &id : panel_ids()) {
			for (const char *k : {"edge", "style", "size", "widgets", "output", "autohide", "labels", "opacity"}) {
				sig += "|" + settings->get("panel." + id + "." + k);
			}
		}
		return sig;
	}

	// Sets the panels up like that desktop. The main panel keeps its pinned
	// apps and screen; other panels are replaced.
	void apply_panel_look(const QString &look) {
		for (const QString &id : panel_ids()) {
			if (id != "main") settings->remove_prefix("panel." + id + ".");
		}
		for (const char *k : {"edge", "style", "size", "widgets", "opacity", "autohide", "labels"}) {
			settings->remove(QString("panel.main.") + k);
		}
		auto put = [](const QString &id, const QString &edge, const QString &style, int size, const QString &widgets,
				const QString &opacity) {
			settings->set("panel." + id + ".edge", edge);
			settings->set("panel." + id + ".style", style);
			settings->set("panel." + id + ".size", QString::number(size));
			settings->set("panel." + id + ".widgets", widgets);
			if (!opacity.isEmpty()) settings->set("panel." + id + ".opacity", opacity);
		};
		QStringList ids = {"main"};
		if (look == "windows") {
			// The taskbar: Start and the apps in the middle, the tray and the clock on the right.
			put("main", "bottom", "full", 48, "spacer,menu,overview,tasks,spacer,media,clipboard,tray,status,clock,notifications", "0.85");
			settings->set("clock_click", "calendar");
		} else if (look == "gnome") {
			// The top bar: Activities, the clock in the middle, the status menu on the right
			// (the apps: Super, or the app menu button).
			// The clock opens the notifications (GNOME's date menu), a bell beside it.
			put("main", "top", "full", 32, "overview,menu,spacer,clock,notifications,spacer,media,tray,status", "1");
			settings->set("clock_click", "notifications");
			settings->set("overview_icon", "dots");
		} else if (look == "macos") {
			// The menu bar along the top, and the dock.
			put("main", "bottom", "dock", 60, "menu,tasks,separator,trash", "0.6");
			// The clock opens the notification centre, as on a Mac.
			put("top", "top", "full", 28, "overview,spacer,media,clipboard,tray,status,clock", "0.7");
			settings->set("clock_click", "notifications");
			ids << "top";
		}
		write_ids(ids);
		current_ = "main";
		fill_combo();
		changed("panels");
	}

	void fill_combo() {
		last_sig_ = signature();
		const QSignalBlocker block(combo_);
		combo_->clear();
		const QStringList ids = panel_ids();
		for (const QString &id : ids) {
			combo_->addItem(panel_name(id), id);
		}
		int i = combo_->findData(current_);
		if (i < 0) i = 0;
		combo_->setCurrentIndex(i);
		current_ = combo_->currentData().toString();
		remove_->setEnabled(!ids.isEmpty());
		build();
	}

	void write_ids(const QStringList &ids) {
		settings->set("panels", ids.join(", "));
	}

	void add_panel() {
		QStringList ids = panel_ids();
		int n = 2;
		while (ids.contains("panel" + QString::number(n))) n++;
		const QString id = "panel" + QString::number(n);
		// A free edge, if there is one.
		QStringList used;
		for (const QString &p : ids) used << panel_edge(p);
		QString edge = "top";
		for (const char *e : {"top", "bottom", "left", "right"}) {
			if (!used.contains(e)) { edge = e; break; }
		}
		ids << id;
		write_ids(ids);
		settings->set("panel." + id + ".edge", edge);
		settings->set("panel." + id + ".widgets", edge == "left" || edge == "right"
			? "menu,pins,tasks,spacer,status,clock" : "spacer,clock,spacer");
		current_ = id;
		fill_combo();
		changed("panels");
	}

	void remove_panel() {
		QStringList ids = panel_ids();
		ids.removeAll(current_);
		write_ids(ids);
		settings->remove_prefix("panel." + current_ + ".");
		current_ = ids.isEmpty() ? QString() : ids.first();
		fill_combo();
		changed("panels");
	}

	void build() {
		// Throw away the old panel's rows.
		for (Card *c : cards_) {
			all_cards.erase(std::remove(all_cards.begin(), all_cards.end(), c), all_cards.end());
			delete c->outer;
			delete c;
		}
		cards_.clear();
		if (current_.isEmpty()) {
			auto *c = new Card(holder_layout_, "", "There are no panels. Add one to get the app menu and "
				"window buttons back (Super still opens the menu).");
			c->frame->hide();
			cards_.push_back(c);
			all_cards.push_back(c);
			c->add(new QWidget);
			update_rows();
			return;
		}
		const QString id = current_;
		const QString pre = "panel." + id + ".";
		const bool main = id == "main";
		// The main panel's defaults include settings from before Dusk had
		// several panels.
		const QString def_edge = main ? settings->get("panel_position", "bottom") : "bottom";
		const QString def_style = main ? settings->get("panel_style", "full") : "full";
		auto vertical = [pre, def_edge] {
			const QString e = settings->get(pre + "edge", def_edge);
			return e == "left" || e == "right";
		};
		auto widgets = [pre] {
			// (A "pins" widget from before 0.21.6 is part of "tasks" now.)
			QStringList w = settings->get(pre + "widgets", kDefaultWidgets).split(',', Qt::SkipEmptyParts);
			if (w.contains("pins")) {
				if (w.contains("tasks")) w.removeAll("pins");
				else w.replaceInStrings(QRegularExpression("^pins$"), "tasks");
			}
			return w;
		};

		Card *place = new_card("Place");
		{
			QLineEdit *name_edit = nullptr;
			const int n = std::max(0, static_cast<int>(panel_ids().indexOf(current_))) + 1;
			text_field(place, "Name", "Shown here and in the panel's menu", pre + "name", QString("Panel %1").arg(n),
				{}, &name_edit);
			if (name_edit) {
				QObject::connect(name_edit, &QLineEdit::editingFinished, [this] { fill_combo(); });
			}
		}
		choice(place, "Screen edge", QString(), pre + "edge",
			{{"Bottom", "bottom"}, {"Top", "top"}, {"Left", "left"}, {"Right", "right"}}, def_edge);
		Items outputs = {{"Every screen", "all"}, {"The main screen", "primary"}};
		for (QScreen *s : QGuiApplication::screens()) {
			if (!s->name().isEmpty()) {
				outputs.push_back({s->name() + (s->model().isEmpty() ? "" : "  " + s->model()), s->name()});
			}
		}
		if (QGuiApplication::screens().size() > 1 || settings->has(pre + "output")) {
			choice(place, "Show on", QString(), pre + "output", outputs, "all");
		}
		toggle(place, "Hide automatically", "Slides away; touch the screen edge to bring it back",
			pre + "autohide", false);

		Card *look = new_card("Look");
		choice(look, "Style", QString(), pre + "style",
			{{"Floating", "floating"}, {"Full width", "full"}, {"Dock (only as long as its contents)", "dock"}},
			def_style);
		slider(look, "Size", QString(), pre + "size", 28, 72, 44, 1,
			[](int v) { return QString("%1 px").arg(v); });
		slider(look, "Background", "0% leaves only the blur", pre + "opacity", 0, 100, 75, 100,
			[](int v) { return QString("%1%").arg(v); });
		toggle(look, "Window titles", "Next to the icons of open apps", pre + "labels",
			main ? settings->get_bool("panel_labels", false) : false,
			[vertical, widgets] { return !vertical() && widgets().contains("tasks"); });

		// Widgets, in order.
		Card *wcard = new_card("On the panel",
			"In order, from the panel's start (left, or top) to its end. Drag a widget to move it, "
			"or pick it and use Ctrl+← →; the × (or Delete) takes it off. A flexible space pushes "
			"things apart: one on each side of something puts it in the middle.");
		{
			auto *box = new QWidget;
			auto *v = new QVBoxLayout(box);
			v->setContentsMargins(0, 4, 0, 4);
			v->setSpacing(8);
			static const std::map<QString, QString> kShort = {
				{"menu", "App menu"}, {"tasks", "Apps"}, {"spacer", "Space"},
				{"separator", "Line"}, {"overview", "Workspaces"}, {"clipboard", "Clipboard"},
				{"tray", "Tray"}, {"trash", "Trash"}, {"status", "Status"}, {"clock", "Clock"},
				{"media", "Now playing"}, {"showdesktop", "Desktop"}, {"keyboard", "Keyboard"}};
			auto chip = [](const QString &w) {
				auto it = kShort.find(w);
				return ChipEditor::Chip{w, it != kShort.end() ? it->second : widget_label(w), "w:" + w, w == "spacer"};
			};
			// The panel: its widgets in order.
			auto *strip = new ChipEditor(false);
			strip->set_empty_text("Nothing on this panel: add something below");
			v->addWidget(strip);
			auto *addlabel = new QLabel("Add to the panel");
			addlabel->setObjectName("cardtitle");
			addlabel->setContentsMargins(0, 6, 0, 0);
			v->addWidget(addlabel);
			// What can go on it (each widget once; spaces and lines as often as you like).
			auto *palette = new ChipEditor(true);
			palette->set_empty_text("Everything's on the panel");
			v->addWidget(palette);
			auto fill_palette = [palette, strip, chip] {
				QSet<QString> used;
				for (const auto &c : strip->chips()) used.insert(c.id);
				std::vector<ChipEditor::Chip> all;
				for (const auto &[label, value] : kWidgets) {
					if (used.contains(value) && value != "spacer" && value != "separator") continue;
					all.push_back(chip(value));
				}
				palette->set_chips(all);
			};
			auto write = [pre, strip, fill_palette] {
				QStringList out;
				for (const auto &c : strip->chips()) out << c.id;
				const QString joined = out.join(",");
				if (joined != settings->get(pre + "widgets", kDefaultWidgets)) {
					settings->set(pre + "widgets", joined);
					changed(pre + "widgets");
				}
				fill_palette();
			};
			strip->on_change = write;
			// Added after the picked one (or at the end).
			palette->on_pick = [strip, chip, write](const QString &id) {
				std::vector<ChipEditor::Chip> now = strip->chips();
				const int sel = strip->selected();
				const int at = sel >= 0 ? sel + 1 : static_cast<int>(now.size());
				now.insert(now.begin() + at, chip(id));
				strip->set_chips(now);
				write();
			};
			auto fill = [strip, widgets, chip, fill_palette] {
				std::vector<ChipEditor::Chip> all;
				for (const QString &w : widgets()) all.push_back(chip(w));
				strip->set_chips(all);
				fill_palette();
			};
			fill();
			restyle_hooks.add(strip, [s = QPointer<QWidget>(strip), p = QPointer<QWidget>(palette)] {
				if (s) s->update();
				if (p) p->update();
			});
			QToolButton *reset = nullptr;
			QWidget *r = make_row(wcard, "Widgets", QString(), box, &reset, true);
			QObject::connect(reset, &QToolButton::clicked, [pre, fill] {
				settings->remove(pre + "widgets");
				fill();
				changed(pre + "widgets");
			});
			bindings.push_back(Binding{r, reset, pre + "widgets", kDefaultWidgets, fill, {}, {}});
		}

		// Pinned apps (only with the "Pinned apps" widget).
		Card *pins = new_card("Pinned apps", "Always on the panel with the open apps, in this order; "
			"a dot shows which are open. Right-click an open app on the panel to pin it.");
		{
			auto *box = new QWidget;
			auto *v = new QVBoxLayout(box);
			v->setContentsMargins(0, 0, 0, 0);
			QListWidget *list = editor_list(150);
			const std::vector<App> apps = installed_apps();
			auto find_app = [apps](const QString &id) -> const App * {
				for (const App &a : apps) {
					if (a.id == id || a.id == id + ".desktop") return &a;
				}
				return nullptr;
			};
			auto fill = [list, pre, find_app] {
				const QSignalBlocker block(list);
				list->clear();
				for (const QString &id : settings->get(pre + "pins").split(',', Qt::SkipEmptyParts)) {
					const App *a = find_app(id.trimmed());
					auto *item = new QListWidgetItem(app_icon(a ? a->icon : ""), a ? a->name : id);
					item->setData(Qt::UserRole, id.trimmed());
					list->addItem(item);
				}
			};
			auto write = [list, pre] {
				QStringList out;
				for (int i = 0; i < list->count(); i++) {
					out << list->item(i)->data(Qt::UserRole).toString();
				}
				if (out.isEmpty()) settings->remove(pre + "pins");
				else settings->set(pre + "pins", out.join(","));
				changed(pre + "pins");
			};
			fill();
			auto later = [write] { QTimer::singleShot(0, write); };
			QObject::connect(list->model(), &QAbstractItemModel::rowsMoved, later);
			v->addWidget(list);
			auto *row = new QHBoxLayout;
			auto *add = new QPushButton("Add an app…");
			auto *del = new QPushButton("Remove");
			row->addWidget(add);
			row->addWidget(del);
			row->addStretch(1);
			v->addLayout(row);
			QObject::connect(del, &QPushButton::clicked, [list, write] {
				delete list->currentItem();
				write();
			});
			QObject::connect(add, &QPushButton::clicked, [box, list, apps, write] {
				QDialog dialog(box->window());
				dialog.setWindowTitle("Pin an app");
				dialog.resize(380, 520);
				auto *dv = new QVBoxLayout(&dialog);
				auto *search = new QLineEdit;
				search->setPlaceholderText("Search");
				auto *choices = new QListWidget;
				choices->setObjectName("editor");
				choices->setIconSize(QSize(24, 24));
				for (const App &a : apps) {
					auto *item = new QListWidgetItem(app_icon(a.icon), a.name);
					item->setData(Qt::UserRole, a.id);
					choices->addItem(item);
				}
				QObject::connect(search, &QLineEdit::textChanged, [choices](const QString &t) {
					for (int i = 0; i < choices->count(); i++) {
						choices->item(i)->setHidden(!choices->item(i)->text().contains(t, Qt::CaseInsensitive));
					}
				});
				auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
				QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
				QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
				QObject::connect(choices, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
				dv->addWidget(search);
				dv->addWidget(choices, 1);
				dv->addWidget(buttons);
				if (dialog.exec() == QDialog::Accepted && choices->currentItem()) {
					QListWidgetItem *picked = choices->currentItem();
					auto *item = new QListWidgetItem(picked->icon(), picked->text());
					item->setData(Qt::UserRole, picked->data(Qt::UserRole));
					list->addItem(item);
					write();
				}
			});
			QToolButton *reset = nullptr;
			QWidget *r = make_row(pins, "Apps", "Click to open (or bring back) the app", box, &reset, true);
			QObject::connect(reset, &QToolButton::clicked, [pre, fill] {
				settings->remove(pre + "pins");
				fill();
				changed(pre + "pins");
			});
			bindings.push_back(Binding{r, reset, pre + "pins", "", fill,
				[widgets] { return widgets().contains("tasks"); }, {}});
		}
		update_rows();
	}

	Card *new_card(const QString &title, const QString &description = QString()) {
		auto *c = new Card(holder_layout_, title, description);
		cards_.push_back(c);
		all_cards.push_back(c);
		return c;
	}

	QVBoxLayout *layout_;
	QComboBox *combo_;
	QPushButton *remove_;
	QWidget *holder_;
	QVBoxLayout *holder_layout_;
	std::vector<Card *> cards_;
	QString current_ = "main";
};

PanelsPage *panels_page = nullptr;

// ---- Desktop & clipboard ----------------------------------------------------------

QWidget *desktop_page() {
	Page page = make_page("Desktop & clipboard", "Files on the desktop, and what you copy.");
	Card *d = card(page, "Desktop",
		"Double-click to open. Drag icons to arrange them, or onto a folder to move them in. Drag files "
		"onto the desktop from Files to put them there. Right-click the wallpaper for New folder and "
		"Paste; after clicking the desktop, Delete, F2 (rename) and Ctrl+C / X / V work too.");
	toggle(d, "Files and folders on the desktop", "What's in your Desktop folder, on the main screen",
		"desktop_icons", true);
	toggle(d, "Trash", "Double-click to open it in your file manager; drop files on it to throw them away",
		"desktop_trash", true, [] { return settings->get_bool("desktop_icons", true); });
	toggle(d, "Home folder", QString(), "desktop_home", false, [] { return settings->get_bool("desktop_icons", true); });
	{
		const QString folder = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
		auto *open = new QPushButton("Open");
		open->setCursor(Qt::PointingHandCursor);
		make_row(d, "Desktop folder", folder, open, nullptr);
		QObject::connect(open, &QPushButton::clicked, [folder] {
			QDir().mkpath(folder);
			QProcess::startDetached("xdg-open", {folder});
		});
	}
	Card *c = card(page, "Clipboard",
		"Super+V, or the clipboard on a panel, lists what you copied. Pick an entry and it's on the "
		"clipboard again: paste it anywhere with Ctrl+V or right-click › Paste.");
	toggle(c, "Keep a clipboard history",
		"Text, pictures and files; passwords from password managers are left out.", "clipboard_history", true);
	choice(c, "Remember", QString(), "clipboard_size",
		{{"10 entries", "10"}, {"25 entries", "25"}, {"50 entries", "50"}, {"100 entries", "100"}}, "25",
		[] { return settings->get_bool("clipboard_history", true); });
	toggle(c, "Keep it after a restart", "Saved (only for you) in ~/.local/state/dusk; off: forgotten when you log out",
		"clipboard_keep", true, [] { return settings->get_bool("clipboard_history", true); });
	toggle(c, "Middle-click pastes", "Select text, then middle-click to paste it somewhere else",
		"middle_click_paste", true);
	finish(page);
	return page.widget;
}

// ---- Control centre -----------------------------------------------------------

const Items kTiles = {
	{"Wi-Fi", "wifi"}, {"Bluetooth", "bluetooth"}, {"Dark mode", "dark"},
	{"Night light", "nightlight"}, {"Do not disturb", "dnd"}, {"Airplane mode", "airplane"},
	{"Caffeine (stay awake)", "caffeine"}, {"Power mode", "power"}, {"Microphone", "mic"},
};
const QString kDefaultTiles = "wifi:wide,bluetooth:wide,dark,nightlight,dnd,airplane,caffeine,power";

QWidget *control_centre_page() {
	Page page = make_page("Control centre",
		"The sidebar that opens from the status icons on a panel (or Super+A).");

	Card *tiles = card(page, "Tiles",
		"Arrange tiles in the control centre itself: press the pencil at the top, then drag tiles "
		"to move them (onto another tile to swap), pull a corner to resize, the red button hides "
		"one, and hidden tiles can be added back underneath. Right-click a tile for its options.");
	{
		auto *b = new QPushButton("Reset the layout");
		QWidget *row = make_row(tiles, "Tile layout", QString(), b, nullptr);
		auto refresh = [b] { b->setEnabled(settings->has("cc_layout")); };
		refresh();
		QObject::connect(b, &QPushButton::clicked, [refresh] {
			settings->remove("cc_layout");
			refresh();
			changed("cc_layout");
		});
		bindings.push_back(Binding{row, nullptr, "cc_layout", "", refresh, {}, {}});
	}
	Card *look = card(page, "Look", "Dusk's own (round toggles and wide bars like a Samsung phone, tiles you "
		"arrange yourself), or like the quick settings of Windows 11, GNOME or macOS. Right-click the control "
		"centre to switch quickly.");
	choice(look, "Look like", QString(), "cc_style",
		{{"Dusk", "dusk"}, {"Windows 11", "windows"}, {"GNOME", "gnome"}, {"macOS", "macos"}}, "dusk");
	toggle(look, "What's playing", "The media card (Windows, GNOME and macOS looks)", "cc_media", true,
		[] { return settings->get("cc_style", "dusk") != "dusk"; });
	Card *parts = card(page, "Sidebar");
	toggle(parts, "Notifications", "Recent notifications under the tiles", "cc_notifications", true,
		[] { return settings->get_bool("notifications", true); });
	advanced(choice(parts, "Side of the screen", QString(), "cc_side",
		{{"Next to the button", "auto"}, {"Left", "left"}, {"Right", "right"}}, "auto"));

	Card *night = card(page, "Night light", "Warmer colours for the evening. Needs a screen that supports it "
		"(nearly all built-in and external screens do).");
	toggle(night, "Night light", QString(), "night_light", false);
	slider(night, "Colour temperature", "Lower is warmer", "night_light_temp", 2500, 6500, 4300, 1,
		[](int v) { return QString("%1 K").arg(v); },
		[] { return settings->get_bool("night_light", false); });

	more_toggle(page);
	finish(page);
	return page.widget;
}

// ---- Notifications --------------------------------------------------------------

// A little screen of 3 x 3 spots: click where pop-ups should appear (like Arqen's).
class PlacePicker : public QWidget {
public:
	std::function<void(const QString &)> picked;
	QString value;
	PlacePicker() {
		setFixedSize(150, 92);
		setMouseTracking(true);
		setCursor(Qt::PointingHandCursor);
	}

protected:
	static QString name_at(int i) {
		static const char *kNames[] = {"top-left", "top", "top-right", "left", "center", "right",
			"bottom-left", "bottom", "bottom-right"};
		return kNames[i];
	}
	QRectF cell(int i) const {
		const double w = (width() - 8) / 3.0, h = (height() - 8) / 3.0;
		return QRectF((i % 3) * (w + 4), (i / 3) * (h + 4), w, h);
	}
	void paintEvent(QPaintEvent *) override {
		const Look l = current_look();
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		for (int i = 0; i < 9; i++) {
			QColor c = l.field;
			if (name_at(i) == value) c = l.accent;
			else if (i == hover_) c = l.hover;
			p.setBrush(c);
			p.drawRoundedRect(cell(i), 7, 7);
		}
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		int h = -1;
		for (int i = 0; i < 9; i++) if (cell(i).contains(e->position())) h = i;
		if (h != hover_) { hover_ = h; update(); }
	}
	void leaveEvent(QEvent *) override { hover_ = -1; update(); }
	void mousePressEvent(QMouseEvent *e) override {
		for (int i = 0; i < 9; i++) {
			if (cell(i).contains(e->position())) {
				value = name_at(i);
				update();
				if (picked) picked(value);
			}
		}
	}

private:
	int hover_ = -1;
};

// A row with the nine-place picker: click where on the screen it goes.
QWidget *place_row(Card *c, const QString &title, const QString &key, const QString &fallback,
		std::function<bool()> visible = {}) {
	auto *pick = new PlacePicker;
	QToolButton *reset = nullptr;
	QWidget *row = make_row(c, title, "Click where on the screen", pick, &reset);
	auto refresh = [pick, key, fallback] {
		QString v = settings->get(key, fallback);
		if (v == "top-center") v = "top";
		pick->value = v;
		pick->update();
	};
	refresh();
	pick->picked = [key](const QString &v) {
		settings->set(key, v);
		changed(key);
	};
	QObject::connect(reset, &QToolButton::clicked, [refresh, key] {
		settings->remove(key);
		refresh();
		changed(key);
	});
	bindings.push_back(Binding{row, reset, key, fallback, refresh, visible, {}});
	return row;
}

QWidget *notifications_page() {
	Page page = make_page("Notifications", "Messages from apps: how they pop up and where.");
	auto on = [] { return settings->get_bool("notifications", true); };
	Card *c = card(page, "Pop-ups");
	toggle(c, "Do not disturb", "No pop-ups; notifications still collect in the control centre (urgent "
		"ones still show)", "dnd", false, on);
	toggle(c, "Pop-ups over full-screen windows",
		"Off: while a game or video is full screen, notifications wait quietly in the control centre",
		"toast_over_fullscreen", false, on);
	place_row(c, "Where pop-ups appear", "notification_position", "bottom-right", on);
	slider(c, "Pop-up duration", QString(), "notification_timeout", 2000, 15000, 5000, 1,
		[](int v) { return QString("%1 s").arg(std::lround(v / 1000.0)); }, on);

	Card *list = card(page, "The notification list");
	choice(list, "Where they're kept",
		"Separate: a notification centre of its own, opened from the panel's bell (or the clock), as on "
		"Windows, GNOME and macOS", "notif_centre",
		{{"Automatic (separate for the Windows, GNOME and macOS looks)", "auto"},
		 {"In the control centre", "together"}, {"Their own notification centre", "separate"}}, "auto", on);
	toggle(list, "Show notifications in the list", QString(), "cc_notifications", true, on);
	toggle(list, "Group by app", "Notifications from the same app stack into one card; click the "
		"number to open the stack", "notif_group", false,
		[on] { return on() && settings->get_bool("cc_notifications", true); });

	toggle(list, "Keep them after a restart", "Notifications you haven't dismissed or cleared are still "
		"there when you log in again", "notification_history_keep", true,
		[on] { return on() && settings->get_bool("cc_notifications", true); });

	Card *svc = card(page, "");
	advanced(toggle(svc, "Dusk shows notifications", "Turn off if you use another notification app "
		"(Dusk restarts the service)", "notifications", true));
	more_toggle(page);
	finish(page);
	return page.widget;
}

// ---- Animations -------------------------------------------------------------------

QWidget *animations_page() {
	Page page = make_page("Animations", "Movement when things open, close and change.");
	auto on = [] { return settings->get_bool("animations", true); };
	Card *all = card(page, "");
	toggle(all, "Animations", "Off makes everything instant", "animations", true);
	toggle(all, "Reduce motion", "Accessibility: everything appears at once, with no glides or fades "
		"(also saves battery)", "reduce_motion", false);
	slider(all, "Speed", QString(), "anim_speed", 25, 300, 100, 100,
		[](int v) { return v == 100 ? QString("Normal") : QString("%1×").arg(v / 100.0, 0, 'f', 2); }, on);
	Card *which = card(page, "Animate");
	advanced(toggle(which, "Opening and closing windows", QString(), "anim_windows", true, on));
	advanced(toggle(which, "Minimising and restoring", QString(), "anim_minimize", true, on));
	advanced(toggle(which, "App menu and control centre", QString(), "anim_menu", true, on));
	advanced(toggle(which, "On-screen display and notifications", QString(), "anim_osd", true, on));
	advanced(toggle(which, "Auto-hiding panels", QString(), "anim_panel", true, on));
	advanced(toggle(which, "Switching workspaces", "The windows slide sideways", "anim_workspaces", true, on));
	advanced(toggle(which, "The overview", "Windows glide into place", "anim_overview", true, on));
	advanced(toggle(which, "Windows glide to a new size", "When maximising, snapping or going fullscreen",
		"anim_maximize", true, on));
	advanced(toggle(which, "Buttons fade and glide", "Hover highlights, selections, sliders, task buttons",
		"anim_hover", true, on));
	Card *how = card(page, "Style");
	choice(how, "Motion", QString(), "anim_curve",
		{{"Smooth", "smooth"}, {"Snappy", "snappy"}, {"Bouncy", "bouncy"}}, "smooth", on);
	choice(how, "Windows open and close", QString(), "anim_open_style",
		{{"Zoom", "zoom"}, {"Slide up", "slide"}, {"Fade", "fade"}}, "zoom",
		[on] { return on() && settings->get_bool("anim_windows", true); });
	choice(how, "Minimising", QString(), "anim_minimize_style",
		{{"Into its button on the panel", "panel"}, {"Drop and fade", "fade"}}, "panel",
		[on] { return on() && settings->get_bool("anim_minimize", true); });
	more_toggle(page);
	finish(page);
	return page.widget;
}

// ---- On-screen display ------------------------------------------------------------

QWidget *osd_page() {
	Page page = make_page("On-screen display",
		"Shown when you change the volume or brightness, or press Caps Lock.");
	auto on = [] { return settings->get_bool("osd", true); };
	Card *c = card(page, "");
	toggle(c, "Show the on-screen display", QString(), "osd", true);
	place_row(c, "Where it appears", "osd_position", "bottom", on);
	choice(c, "Orientation", "Upright suits the left and right edges", "osd_orientation",
		{{"Horizontal", "horizontal"}, {"Vertical", "vertical"}}, "horizontal", on);
	choice(c, "Level", QString(), "osd_style",
		{{"LED segments", "segments"}, {"Smooth bar", "bar"}}, "segments", on);
	slider(c, "Stay on screen", QString(), "osd_timeout", 500, 5000, 1500, 1,
		[](int v) { return QString("%1 s").arg(v / 1000.0, 0, 'f', 1); }, on);
	advanced(toggle(c, "Caps Lock and Num Lock", QString(), "osd_lock_keys", true, on));
	advanced(toggle(c, "Keyboard layout changes", QString(), "osd_layout", true, on));
	more_toggle(page);
	finish(page);
	return page.widget;
}

// ---- Mouse & touchpad -----------------------------------------------------------

QWidget *pointer_page() {
	Page page = make_page("Mouse & touchpad", QString());
	Card *general = card(page, "Pointer");
	slider(general, "Speed", QString(), "pointer_speed", -100, 100, 0, 100,
		[](int v) { return v == 0 ? QString("Normal") : v < 0 ? QString("Slower") : QString("Faster"); });
	advanced(choice(general, "Acceleration", "Adaptive: faster moves go further", "accel_profile",
		{{"Adaptive", "adaptive"}, {"Off (flat, for games)", "flat"}}, "adaptive"));
	advanced(choice(general, "Primary button", QString(), "left_handed",
		{{"Left (right-handed)", "false"}, {"Right (left-handed)", "true"}}, "false"));
	if (has_touchpad()) {
		Card *touchpad = card(page, "Touchpad");
		toggle(touchpad, "Tap to click", QString(), "tap_to_click", true);
		toggle(touchpad, "Natural scrolling", "Content follows your fingers", "natural_scroll_touchpad", true);
		toggle(touchpad, "Ignore while typing", "So your palm doesn't move the pointer",
			"disable_while_typing", true);
		choice(touchpad, "Right-click", "How a click becomes a right-click", "click_method",
			{{"Automatic", "default"}, {"Two-finger click", "fingers"},
				{"Click the bottom-right corner", "areas"}}, "default");
	}
	if (has_touchpad()) {
		Card *g = card(page, "Gestures", "Swipe with several fingers on the touchpad. Two fingers always scroll; "
			"anything not set here goes to the app (pinching to zoom, for example).");
		const Items actions = {
			{"Nothing", "none"}, {"Open the overview", "overview"}, {"Close the overview", "overview-close"},
			{"Overview (open or close)", "overview-toggle"}, {"Next workspace", "workspace-next"},
			{"Previous workspace", "workspace-prev"}, {"App menu", "app-menu"}, {"Control centre", "control-centre"},
			{"Show the desktop", "show-desktop"}, {"Clipboard history", "clipboard"}, {"Screenshot", "screenshot"},
			{"Switch to the previous window", "switch-window"}, {"Close the window", "close-window"},
			{"Maximise the window", "maximize"}, {"Minimise the window", "minimize"}, {"Full screen", "fullscreen"},
			{"Snap the window left", "snap-left"}, {"Snap the window right", "snap-right"},
		};
		{
			auto *l = new QLabel("Scroll");
			l->setObjectName("subtitle");
			make_row(g, "2 fingers", "Scrolling, in every direction", l, nullptr);
		}
		const char *dirs[][2] = {{"up", "Up"}, {"down", "Down"}, {"left", "Left"}, {"right", "Right"}};
		for (int fingers = 3; fingers <= 5; fingers++) {
			for (const auto &d : dirs) {
				const QString key = QString("gesture_%1_%2").arg(fingers).arg(d[0]);
				QString def = "none";
				if (fingers == 3) {
					def = QString(d[0]) == "up" ? "overview" : QString(d[0]) == "down" ? "overview-close"
						: QString(d[0]) == "left" ? "workspace-next" : "workspace-prev";
				}
				QWidget *row = choice(g, QString("%1 fingers %2").arg(fingers).arg(QString(d[1]).toLower()), QString(),
					key, actions, def);
				if (fingers > 3) advanced(row);
			}
		}
	}
	Card *mouse = card(page, "Mouse");
	toggle(mouse, "Natural scrolling", "Reverses the scroll wheel", "natural_scroll_mouse", false);
	advanced(toggle(mouse, "Middle-click with both buttons", "Press left and right together",
		"middle_emulation", false));
	more_toggle(page);
	finish(page);
	return page.widget;
}

// ---- Small helpers for running system tools ----------------------------------------

QString run_tool_now(const QString &program, const QStringList &args, int timeout_ms) {
	QProcess p;
	p.start(program, args);
	if (!p.waitForFinished(timeout_ms)) {
		p.kill();
		p.waitForFinished(200);
		return QString();
	}
	return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

// Pages asked the system tools (localectl, timedatectl, xdg-mime, lspci…)
// one after another as they were built, so opening some pages took a second
// or more. They're asked all at once, in the background, as Settings
// starts; a page takes the answer that's ready (each answer is used once,
// so anything asked again later is fresh).
std::mutex g_tool_mutex;
std::map<QString, std::shared_future<QString>> g_tool_cache;

QString tool_key(const QString &program, const QStringList &args) {
	return program + QChar(0x1f) + args.join(QChar(0x1f));
}

void prefetch_tool(const QString &program, const QStringList &args, int timeout_ms = 6000) {
	if (QStandardPaths::findExecutable(program).isEmpty()) return;
	const std::lock_guard<std::mutex> lock(g_tool_mutex);
	const QString key = tool_key(program, args);
	if (g_tool_cache.count(key)) return;
	g_tool_cache[key] = std::async(std::launch::async, [program, args, timeout_ms] {
		return run_tool_now(program, args, timeout_ms);
	}).share();
}

QString run_tool(const QString &program, const QStringList &args, int timeout_ms = 3000) {
	std::shared_future<QString> ready;
	{
		const std::lock_guard<std::mutex> lock(g_tool_mutex);
		auto it = g_tool_cache.find(tool_key(program, args));
		if (it != g_tool_cache.end()) {
			ready = it->second;
			g_tool_cache.erase(it);
		}
	}
	if (ready.valid()) return ready.get();
	return run_tool_now(program, args, timeout_ms);
}

bool have_program(const QString &name) {
	return !QStandardPaths::findExecutable(name).isEmpty();
}

// Desktop files of installed apps: path, and what they can open.
struct DesktopApp {
	QString id, name, icon, path, exec;
	QStringList mime;
};

std::vector<DesktopApp> desktop_apps_scan() {
	std::vector<DesktopApp> out;
	QSet<QString> seen;
	QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
	dirs << "/var/lib/flatpak/exports/share/applications"
		<< QDir::homePath() + "/.local/share/flatpak/exports/share/applications";
	for (const QString &dir : dirs) {
		QDirIterator it(dir, {"*.desktop"}, QDir::Files, QDirIterator::Subdirectories);
		while (it.hasNext()) {
			const QString path = it.next();
			QString id = QDir(dir).relativeFilePath(path).replace('/', '-');
			if (seen.contains(id)) continue;
			seen.insert(id);
			QFile f(path);
			if (!f.open(QIODevice::ReadOnly)) continue;
			DesktopApp a;
			a.id = id;
			a.path = path;
			bool in_entry = false, hidden = false;
			QTextStream ts(&f);
			while (!ts.atEnd()) {
				const QString line = ts.readLine().trimmed();
				if (line.startsWith('[')) { in_entry = line == "[Desktop Entry]"; continue; }
				if (!in_entry) continue;
				if (line.startsWith("Name=") && a.name.isEmpty()) a.name = line.mid(5);
				else if (line.startsWith("Icon=")) a.icon = line.mid(5);
				else if (line.startsWith("Exec=")) a.exec = line.mid(5);
				else if (line.startsWith("MimeType=")) a.mime = line.mid(9).split(';', Qt::SkipEmptyParts);
				else if (line == "NoDisplay=true" || line == "Hidden=true") hidden = true;
			}
			if (!hidden && !a.name.isEmpty()) out.push_back(a);
		}
	}
	std::sort(out.begin(), out.end(), [](const DesktopApp &a, const DesktopApp &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	return out;
}

// The installed apps: read in the background as Settings starts, then
// kept for half a minute (several pages list them).
std::shared_future<std::vector<DesktopApp>> g_apps_future;
std::vector<DesktopApp> g_apps;
qint64 g_apps_at = 0;

void prefetch_desktop_apps() {
	g_apps_future = std::async(std::launch::async, [] { return desktop_apps_scan(); }).share();
}

std::vector<DesktopApp> desktop_apps() {
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (g_apps_future.valid()) {
		g_apps = g_apps_future.get();
		g_apps_future = {};
		g_apps_at = now;
	} else if (g_apps_at == 0 || now - g_apps_at > 30000) {
		g_apps = desktop_apps_scan();
		g_apps_at = now;
	}
	return g_apps;
}

QString strip_field_codes(QString exec) {
	exec.replace(QRegularExpression(" ?%[fFuUdDnNickvm]"), "");
	return exec.trimmed();
}

// ---- Keyboard shortcuts --------------------------------------------------------------

// Click, then press the keys: it shows and reports them as "Super+Shift+E".
// Keys written the one way ("Super+Ctrl+Alt+Shift+key", the key lower case)
// so "shift+super+F23" and "Super+Shift+f23" are seen as the same.
QString normal_keys(const QString &keys) {
	QStringList mods, out;
	QString key;
	for (QString p : keys.split('+', Qt::SkipEmptyParts)) {
		p = p.trimmed();
		const QString l = p.toLower();
		if (l == "super" || l == "logo" || l == "meta" || l == "win") mods << "Super";
		else if (l == "ctrl" || l == "control") mods << "Ctrl";
		else if (l == "alt") mods << "Alt";
		else if (l == "shift") mods << "Shift";
		else key = l;
	}
	for (const char *m : {"Super", "Ctrl", "Alt", "Shift"}) {
		if (mods.contains(m)) out << m;
	}
	out << key;
	return out.join('+');
}

// Dusk's own shortcuts, as keys, for "already used by".
const std::vector<std::pair<QString, QString>> &dusk_shortcuts() {
	static std::vector<std::pair<QString, QString>> list;
	if (!list.empty()) return list;
	auto add = [](const QString &k, const QString &what) { list.push_back({normal_keys(k), what}); };
	add("Alt+F1", "App menu");
	add("Super+Tab", "Overview");
	add("Super+S", "Overview");
	add("Super+W", "Overview");
	for (int i = 0; i <= 9; i++) {
		add(QString("Super+%1").arg(i), QString("Go to workspace %1").arg(i == 0 ? 10 : i));
		add(QString("Super+Shift+%1").arg(i), QString("Move the window to workspace %1").arg(i == 0 ? 10 : i));
	}
	add("Super+Page_Up", "Previous workspace");
	add("Super+Page_Down", "Next workspace");
	add("Super+Shift+Page_Up", "Previous workspace, with the window");
	add("Super+Shift+Page_Down", "Next workspace, with the window");
	add("Ctrl+Alt+Left", "Previous workspace");
	add("Ctrl+Alt+Right", "Next workspace");
	add("Super+Alt+Left", "Previous workspace");
	add("Super+Alt+Right", "Next workspace");
	add("Super+Home", "First workspace");
	add("Super+End", "Last workspace");
	add("Super+A", "Control centre");
	add("Super+N", "Control centre");
	add("Super+Return", "Terminal");
	add("Super+I", "Settings");
	add("Super+E", "Your files");
	add("Super+Z", "Snap layouts");
	add("Super+D", "Show the desktop");
	add("Super+K", "On-screen keyboard");
	add("Super+L", "Lock the screen");
	add("Super+V", "Clipboard history");
	add("Super+F", "Full screen");
	add("Super+Q", "Close the window");
	add("Alt+F4", "Close the window");
	add("Alt+Tab", "Switch windows");
	add("Alt+Shift+Tab", "Switch windows (back)");
	add("Super+Up", "Maximise");
	add("Super+Down", "Restore or minimise");
	add("Super+Left", "Snap left");
	add("Super+Right", "Snap right");
	add("Super+Shift+Left", "Move the window to the next screen");
	add("Super+Shift+Right", "Move the window to the next screen");
	add("Print", "Screenshot");
	add("Shift+Print", "Screenshot of every screen");
	add("Alt+Print", "Screenshot of the window");
	add("Ctrl+Alt+Shift+R", "Record the screen");
	add("Ctrl+Alt+BackSpace", "Quit Dusk");
	for (const char *k : {"XF86AudioRaiseVolume", "XF86AudioLowerVolume", "XF86AudioMute", "XF86AudioMicMute"}) {
		add(k, "Volume");
	}
	add("XF86MonBrightnessUp", "Brightness");
	add("XF86MonBrightnessDown", "Brightness");
	add("XF86AudioPlay", "Play / pause");
	add("XF86AudioNext", "Next track");
	add("XF86AudioPrev", "Previous track");
	return list;
}

// What else uses these keys: one of Dusk's shortcuts, or another of yours.
QString keys_used_by(const QString &keys, const QString &except = QString()) {
	const QString k = normal_keys(keys);
	for (const QString &mine : settings->keys_with_prefix("shortcut.")) {
		const QString other = mine.startsWith("shortcut.") ? mine.mid(9) : mine;
		if (other == except) continue;
		if (normal_keys(other) == k) return "your shortcut for " + settings->get("shortcut." + other);
	}
	for (const auto &[dk, what] : dusk_shortcuts()) {
		if (dk == k) return "Dusk: " + what;
	}
	return QString();
}

class KeyRecorder : public QPushButton {
public:
	std::function<void(const QString &)> recorded;
	std::function<void(const QString &)> notice;   // a line for under the row (empty: none)

	explicit KeyRecorder(const QString &keys) : keys_(keys) {
		setFixedWidth(190);
		setCursor(Qt::PointingHandCursor);
		setFocusPolicy(Qt::StrongFocus);
		show_keys();
		QObject::connect(this, &QPushButton::clicked, [this] {
			if (recording_) return;
			recording_ = true;
			setText("Press a key or keys…");
			if (notice) notice("Any key works, alone or with Super, Ctrl, Alt and Shift. Esc cancels.");
			// While this note is there, Dusk lets every key through to us,
			// even ones it would use itself.
			QFile note(note_path());
			note.open(QIODevice::WriteOnly);
			setFocus();
			grabKeyboard();
		});
	}
	~KeyRecorder() override {
		if (recording_) QFile::remove(note_path());
	}
	void set_keys(const QString &keys) {
		keys_ = keys;
		show_keys();
	}

protected:
	void keyPressEvent(QKeyEvent *e) override {
		if (!recording_) {
			QPushButton::keyPressEvent(e);
			return;
		}
		const int k = e->key();
		if (k == Qt::Key_Escape && e->modifiers() == Qt::NoModifier) {
			stop();
			if (notice) notice(QString());
			return;
		}
		QStringList parts;
		const Qt::KeyboardModifiers m = e->modifiers();
		if (m & Qt::MetaModifier) parts << "Super";
		if (m & Qt::ControlModifier) parts << "Ctrl";
		if (m & Qt::AltModifier) parts << "Alt";
		if (m & Qt::ShiftModifier) parts << "Shift";
		if (k == Qt::Key_Meta || k == Qt::Key_Super_L || k == Qt::Key_Super_R || k == Qt::Key_Control ||
				k == Qt::Key_Alt || k == Qt::Key_Shift || k == Qt::Key_AltGr) {
			setText(parts.join("+") + "+…");
			return;   // wait for the real key
		}
		const QString name = key_name(k, e->nativeVirtualKey());
		if (name.isEmpty()) {
			if (notice) notice("That key doesn't send anything Dusk can see (it may be handled by the laptop itself).");
			return;
		}
		// A letter, number or symbol on its own would stop you typing it.
		const bool plain = name.size() == 1 || QStringList{"space", "Return", "Tab", "BackSpace", "comma", "period",
			"slash", "minus", "equal", "semicolon", "apostrophe", "bracketleft", "bracketright", "backslash",
			"grave"}.contains(name);
		const bool has_mod = m & (Qt::MetaModifier | Qt::ControlModifier | Qt::AltModifier);
		if (plain && !has_mod) {
			if (notice) notice("Add Super, Ctrl or Alt to " + name + ", or you couldn't type it any more.");
			setText("Press a key or keys…");
			return;
		}
		parts << name;
		keys_ = parts.join("+");
		stop();
		if (recorded) recorded(keys_);
	}
	void focusOutEvent(QFocusEvent *e) override {
		if (recording_) {
			stop();
			if (notice) notice(QString());
		}
		QPushButton::focusOutEvent(e);
	}

private:
	static QString note_path() {
		return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/dusk-recording-keys";
	}
	// Qt's key -> the xkb name Dusk understands; keys Qt has no name for
	// (a laptop's own keys, Copilot's F23, an Omen key) by their keysym.
	static QString key_name(int k, quint32 keysym) {
		if (k >= Qt::Key_A && k <= Qt::Key_Z) return QString(QChar('A' + (k - Qt::Key_A)));
		if (k >= Qt::Key_0 && k <= Qt::Key_9) return QString(QChar('0' + (k - Qt::Key_0)));
		if (k >= Qt::Key_F1 && k <= Qt::Key_F35) return QString("F%1").arg(k - Qt::Key_F1 + 1);
		static const std::map<int, const char *> kNames = {
			{Qt::Key_Space, "space"}, {Qt::Key_Return, "Return"}, {Qt::Key_Enter, "KP_Enter"},
			{Qt::Key_Tab, "Tab"}, {Qt::Key_Backtab, "Tab"}, {Qt::Key_Backspace, "BackSpace"}, {Qt::Key_Delete, "Delete"},
			{Qt::Key_Insert, "Insert"}, {Qt::Key_Home, "Home"}, {Qt::Key_End, "End"},
			{Qt::Key_PageUp, "Page_Up"}, {Qt::Key_PageDown, "Page_Down"}, {Qt::Key_Left, "Left"},
			{Qt::Key_Right, "Right"}, {Qt::Key_Up, "Up"}, {Qt::Key_Down, "Down"},
			{Qt::Key_Print, "Print"}, {Qt::Key_Pause, "Pause"}, {Qt::Key_Comma, "comma"},
			{Qt::Key_Period, "period"}, {Qt::Key_Slash, "slash"}, {Qt::Key_Minus, "minus"},
			{Qt::Key_Equal, "equal"}, {Qt::Key_Semicolon, "semicolon"}, {Qt::Key_Apostrophe, "apostrophe"},
			{Qt::Key_BracketLeft, "bracketleft"}, {Qt::Key_BracketRight, "bracketright"},
			{Qt::Key_Backslash, "backslash"}, {Qt::Key_QuoteLeft, "grave"}, {Qt::Key_Escape, "Escape"},
			{Qt::Key_MediaPlay, "XF86AudioPlay"}, {Qt::Key_MediaNext, "XF86AudioNext"},
			{Qt::Key_MediaPrevious, "XF86AudioPrev"}, {Qt::Key_Calculator, "XF86Calculator"},
		};
		auto it = kNames.find(k);
		if (it != kNames.end()) return QString(it->second);
		if (keysym != 0) {
			char buf[64] = {};
			if (xkb_keysym_get_name(keysym, buf, sizeof buf) > 0) return QString::fromLatin1(buf);
		}
		return QString();
	}
	void stop() {
		recording_ = false;
		releaseKeyboard();
		QFile::remove(note_path());
		show_keys();
	}
	void show_keys() { setText(keys_.isEmpty() ? QString("Set keys…") : keys_); }

	QString keys_;
	bool recording_ = false;
};

// Your own shortcuts ("shortcut.<keys> = command" in the file), with an app
// picker so you rarely have to type a command.
class ShortcutsCard {
public:
	explicit ShortcutsCard(Page &page) {
		card_ = card(page, "Your shortcuts",
			"Pick keys and what they open: any key on its own (F1, a laptop's extra keys, Copilot) or with "
			"Super, Ctrl, Alt and Shift. If the keys are already used, it says by what; yours win over Dusk's own.");
		list_ = new QWidget;
		rows_ = new QVBoxLayout(list_);
		rows_->setContentsMargins(0, 6, 0, 6);
		rows_->setSpacing(8);
		card_->add(list_);
		auto *add_row = new QWidget;
		auto *h = new QHBoxLayout(add_row);
		h->setContentsMargins(0, 2, 0, 10);
		auto *add = new QPushButton("Add shortcut");
		add->setObjectName("primary");
		h->addWidget(add);
		h->addStretch(1);
		card_->add(add_row);
		apps_ = desktop_apps();
		QObject::connect(add, &QPushButton::clicked, [this] { add_row_ui(QString(), QString()); });
		for (const QString &key : settings->keys_with_prefix("shortcut.")) {
			add_row_ui(key.mid(9), settings->get(key));
		}
	}

private:
	void add_row_ui(const QString &keys0, const QString &command0) {
		auto *row = new QWidget;
		auto *rv = new QVBoxLayout(row);
		rv->setContentsMargins(0, 0, 0, 0);
		rv->setSpacing(4);
		auto *line = new QWidget;
		auto *h = new QHBoxLayout(line);
		h->setContentsMargins(0, 0, 0, 0);
		h->setSpacing(8);
		rv->addWidget(line);
		auto *hint = new QLabel;
		hint->setObjectName("hint");
		hint->setWordWrap(true);
		hint->hide();
		rv->addWidget(hint);
		auto keys = std::make_shared<QString>(keys0);
		auto *rec = new KeyRecorder(keys0);
		auto *what = new ElideCombo;
		flex_width(what, 200);
		what->addItem("Choose an app…", "");
		for (const DesktopApp &a : apps_) {
			what->addItem(app_icon(a.icon), a.name, "app:" + a.id);
		}
		what->addItem("A command…", "cmd");
		auto *edit = new QLineEdit(command0);
		edit->setPlaceholderText("Command, e.g. nautilus");
		auto *del = new QToolButton;
		del->setObjectName("reset");
		del->setText(QString::fromUtf8("✕"));
		del->setToolTip("Remove this shortcut");
		h->addWidget(rec);
		h->addWidget(what);
		h->addWidget(edit, 1);
		h->addWidget(del);
		rows_->addWidget(row);

		// Show which app the command opens, if it's one we know.
		for (int i = 0; i < what->count(); i++) {
			const QString d = what->itemData(i).toString();
			if (d.startsWith("app:") && command0 == launch_command(d.mid(4))) {
				what->setCurrentIndex(i);
				edit->setVisible(false);
			}
		}
		if (what->currentIndex() == 0 && !command0.isEmpty()) {
			what->setCurrentIndex(what->count() - 1);
		}

		auto store = [keys, edit] {
			if (keys->isEmpty() || edit->text().trimmed().isEmpty()) return;
			settings->set("shortcut." + *keys, edit->text().trimmed());
			changed("shortcut");
		};
		QObject::connect(what, &QComboBox::currentIndexChanged, [this, what, edit, store](int) {
			const QString d = what->currentData().toString();
			if (d.startsWith("app:")) {
				edit->setText(launch_command(d.mid(4)));
				edit->setVisible(false);
				store();
			} else if (d == "cmd") {
				edit->setVisible(true);
				edit->setFocus();
			}
		});
		QObject::connect(edit, &QLineEdit::editingFinished, store);
		auto say = [hint](const QString &t) {
			hint->setText(t);
			hint->setVisible(!t.isEmpty());
		};
		// What else uses these keys (yours win over Dusk's own).
		auto check = [keys, say] {
			const QString other = keys->isEmpty() ? QString() : keys_used_by(*keys, *keys);
			say(other.isEmpty() ? QString() : QString::fromUtf8("⚠ Also ") + other +
				(other.startsWith("Dusk") ? " — this shortcut will be used instead" : " — only one of them will work"));
		};
		rec->notice = say;
		rec->recorded = [keys, store, check](const QString &k) {
			if (!keys->isEmpty() && *keys != k) {
				settings->remove("shortcut." + *keys);
			}
			*keys = k;
			store();
			check();
		};
		check();
		QObject::connect(del, &QToolButton::clicked, [row, keys] {
			if (!keys->isEmpty()) {
				settings->remove("shortcut." + *keys);
				changed("shortcut");
			}
			row->deleteLater();
		});
	}
	static QString launch_command(const QString &desktop_id) {
		QString id = desktop_id;
		if (id.endsWith(".desktop")) id.chop(8);
		return "gtk-launch " + id;
	}

	Card *card_;
	QWidget *list_;
	QVBoxLayout *rows_;
	std::vector<DesktopApp> apps_;
};

void shortcut_cards(Page &page) {
	new ShortcutsCard(page);
}

// ---- Screenshots ----------------------------------------------------------------------

QWidget *screenshots_page() {
	Page page = make_page("Screenshots & recording",
		"Print opens the screenshot tool (pictures and videos). Shift+Print: every screen at once. Alt+Print: the active window.");
	auto saving = [] { return settings->get_bool("screenshot_save", true); };

	Card *tool = card(page, "Taking one");
	choice(tool, "Print key", QString(), "screenshot_print",
		{{"Opens the screenshot tool", "tool"}, {"Takes the screen at once", "screen"}}, "tool");
	choice(tool, "The tool starts with", "It remembers the one you used last", "screenshot_mode",
		{{"An area you drag out", "area"}, {"A window", "window"}, {"A whole screen", "screen"}}, "area",
		[] { return settings->get("screenshot_print", "tool") == "tool"; });
	toggle(tool, "Show the pointer", "In the picture", "screenshot_pointer", false);
	choice(tool, "Delay", "Time to open a menu first", "screenshot_delay",
		{{"None", "0"}, {"3 seconds", "3"}, {"5 seconds", "5"}, {"10 seconds", "10"}}, "0");

	Card *after = card(page, "Afterwards");
	toggle(after, "Copy to the clipboard", "So you can paste it straight away", "screenshot_copy", true);
	toggle(after, "HD for sharing", "Small screenshots are enlarged to 1920 px, so WhatsApp and others send "
		"them in HD instead of a blurry copy", "screenshot_hd", true);
	toggle(after, "Save a file", "Off: only copied", "screenshot_save", true);
	{
		auto *pick = new QPushButton("Choose…");
		auto *name = new QLabel;
		name->setObjectName("subtitle");
		auto *box = new QWidget;
		auto *h = new QHBoxLayout(box);
		h->setContentsMargins(0, 0, 0, 0);
		h->addWidget(name);
		h->addWidget(pick);
		auto refresh = [name] {
			const QString p = settings->get("screenshot_dir");
			name->setText(p.isEmpty() ? QString("Pictures/Screenshots") : QFileInfo(p).fileName());
		};
		refresh();
		QToolButton *reset = nullptr;
		QWidget *row = make_row(after, "Save in", QString(), box, &reset);
		QObject::connect(pick, &QPushButton::clicked, [pick, refresh] {
			const QString dir = QFileDialog::getExistingDirectory(pick->window(), "Save screenshots in",
				settings->get("screenshot_dir", QDir::homePath() + "/Pictures"));
			if (!dir.isEmpty()) {
				settings->set("screenshot_dir", dir);
				refresh();
				changed("screenshot_dir");
			}
		});
		QObject::connect(reset, &QToolButton::clicked, [refresh] {
			settings->remove("screenshot_dir");
			refresh();
			changed("screenshot_dir");
		});
		bindings.push_back(Binding{row, reset, "screenshot_dir", "", refresh, saving,
			[] { return settings->get("screenshot_dir").isEmpty(); }});
	}
	advanced(text_field(after, "File name", "%Y year, %m month, %d day, %H-%M-%S time", "screenshot_name",
		"Screenshot from %Y-%m-%d %H-%M-%S", saving));
	toggle(after, "Notification", "With Open and Show in folder", "screenshot_notify", true);
	toggle(after, "Camera sound", QString(), "screenshot_sound", false);
	{
		Items editors = {{"No", "none"}};
		if (have_program("satty")) editors.push_back({"Yes, in Satty", "satty"});
		if (have_program("swappy")) editors.push_back({"Yes, in Swappy", "swappy"});
		choice(after, "Draw on it first", editors.size() > 1 ? QString("Opens the editor before saving")
			: QString("Install satty or swappy for this"), "screenshot_editor", editors, "none");
	}
	if (!have_program("grim")) {
		auto *note = new QLabel("Screenshots need grim: sudo dnf install grim wl-clipboard");
		note->setObjectName("subtitle");
		note->setContentsMargins(0, 4, 0, 10);
		after->add(note);
	}
	{
		Card *rec = card(page, "Screen recording",
			"In the tool, switch to video (the camera / video switch, or V), pick what to record and press the red "
			"button. Shift+Ctrl+Alt+R records the screen at once. A red pill with the time shows on the panel: "
			"click it (or press the keys again) to stop. Saved in Videos/Screencasts.");
		choice(rec, "Sound", QString(), "record_audio",
			{{"None", "none"}, {"What you hear", "system"}, {"Microphone", "mic"}}, "none");
		choice(rec, "Frames per second", QString(), "record_fps", {{"60 (smooth)", "60"}, {"30 (smaller files)", "30"}}, "60");
		choice(rec, "Quality", QString(), "record_quality", {{"High", "high"}, {"Standard (smaller files)", "standard"}}, "high");
		if (!have_program("wf-recorder")) {
			auto *note = new QLabel("Recording needs wf-recorder: sudo dnf install wf-recorder");
			note->setObjectName("subtitle");
			note->setContentsMargins(0, 4, 0, 10);
			rec->add(note);
		}
	}
	{
		Card *keys = card(page, "In the tool");
		auto *note = new QLabel("Drag to choose an area (drag it again to move it) · Enter or Space takes it · "
			"1, 2, 3 switch between area, window and screen · C a picture, V a video · P: the pointer · "
			"Esc or right-click closes.");
		note->setObjectName("subtitle");
		note->setWordWrap(true);
		note->setContentsMargins(0, 6, 0, 6);
		keys->add(note);
	}
	more_toggle(page);
	finish(page);
	return page.widget;
}

// ---- Media & lyrics -------------------------------------------------------------------

QWidget *media_page() {
	Page page = make_page("Media & lyrics",
		"What's playing, in the control centre and the panel's media card, and its lyrics on the screen.");
	Card *snd = card(page, "Sound");
	toggle(snd, "Keep the sound card awake",
		"Stops the click in headphones when sound starts after a quiet moment (uses a little more battery)",
		"audio_keep_awake", true);
	Card *card_ = card(page, "Media card");
	choice(card_, "Beat visualizer", "Behind the card, like Arqen's: it stays in place, only its height follows the music",
		"media_viz", {{"Bars", "bars"}, {"Mirror (from the middle)", "mirror"}, {"Dots", "dots"}, {"Wave (one line)", "wave"},
		{"Hills", "hills"}, {"Off", "off"}}, "bars");
	choice(card_, "Its colour", QString(), "media_viz_color",
		{{"From the album art", "art"}, {"Accent", "accent"}, {"White / text", "text"}, {"Rainbow", "rainbow"}}, "art");
	toggle(card_, "Follow the music's beat", "Listens to what's playing (off: a made-up beat)",
		"media_wave_beat", true);
	toggle(card_, "Scroll long titles", "Titles that don't fit slide along", "media_marquee", true);

	Card *ly = card(page, "Lyrics",
		"Synced lyrics come from LRCLIB (lrclib.net, free) when a song starts, and are kept for next time. "
		"The microphone button on the media card shows them on the screen and opens their timing.");
	toggle(ly, "Look up lyrics", "For the song that's playing", "lyrics", true);
	auto looking = [] { return settings->get_bool("lyrics", true); };
	toggle(ly, "Show on the screen", "While it plays", "lyrics_show", false, looking);
	auto showing = [] { return settings->get_bool("lyrics", true) && settings->get_bool("lyrics_show", false); };
	choice(ly, "Style", QString(), "lyrics_style", {
		{"Classic: the line, with the ones around it", "classic"},
		{"Karaoke: fills in as it's sung", "karaoke"},
		{"Subtitles: on a dark box", "subtitle"},
		{"Glow: neon, pulsing with the beat", "glow"},
		{"Scrolling: a column gliding up", "scroll"},
		{"Bounce: words pop in as they're sung", "bounce"},
		{"Typewriter: typed out letter by letter", "typewriter"},
		{"Comic: bold outlined letters", "comic"},
	}, "comic", showing);
	choice(ly, "Colour", "The highlights in it, the words touched with it", "lyrics_color",
		{{"Accent", "accent"}, {"From the album cover", "art"}, {"White", "white"}}, "accent", looking);
	place_row(ly, "Where they appear", "lyrics_position", "bottom", showing);
	toggle(ly, "Over the windows", "Off: on the desktop, behind the windows", "lyrics_on_top", false, showing);
	slider(ly, "Size", QString(), "lyrics_size", 60, 200, 100, 1, [](int v) { return QString("%1%").arg(v); }, showing);
	// SongRec from anywhere: a package, Flathub (any id), a snap, a menu entry.
	bool songrec = have_program("songrec") || QFileInfo::exists("/snap/bin/songrec");
	for (const QString &d : {QDir::homePath() + "/.local/share/flatpak/exports/bin", QString("/var/lib/flatpak/exports/bin"),
			QDir::homePath() + "/.local/share/applications", QString("/usr/share/applications")}) {
		if (!songrec && !QDir(d).entryList({"*songrec*", "*SongRec*"}, QDir::Files).isEmpty()) songrec = true;
	}
	toggle(ly, "Recognise songs by ear", songrec
			? QString("When a song's name finds nothing (videos), listen to it with SongRec")
			: QString("Needs SongRec: ./build.sh install adds it from Flathub"),
		"lyrics_recognize", true, looking);

	Card *tr = card(page, "Translation",
		"Lyrics in another language, translated line by line by Google Translate's free web service "
		"(the words of the song are sent to it; kept for next time).");
	toggle(tr, "Translate lyrics", "When they're in another language than the one below", "lyrics_translate", true,
		looking);
	auto translating = [looking] { return looking() && settings->get_bool("lyrics_translate", true); };
	choice(tr, "Into", QString(), "lyrics_translate_to",
		{{"Your system's language", ""}, {"English", "en"}, {"Afrikaans", "af"}, {"Arabic", "ar"},
			{"Bengali", "bn"}, {"Chichewa (Nyanja)", "ny"}, {"Chinese (simplified)", "zh-CN"},
			{"Chinese (traditional)", "zh-TW"}, {"Dutch", "nl"}, {"French", "fr"}, {"German", "de"}, {"Greek", "el"},
			{"Hausa", "ha"}, {"Hebrew", "iw"}, {"Hindi", "hi"}, {"Igbo", "ig"}, {"Indonesian", "id"},
			{"Italian", "it"}, {"Japanese", "ja"}, {"Kinyarwanda", "rw"}, {"Korean", "ko"}, {"Malay", "ms"},
			{"Persian", "fa"}, {"Polish", "pl"}, {"Portuguese", "pt"}, {"Russian", "ru"}, {"Sesotho", "st"},
			{"Shona", "sn"}, {"Spanish", "es"}, {"Swahili", "sw"}, {"Swedish", "sv"}, {"Tagalog", "tl"},
			{"Thai", "th"}, {"Turkish", "tr"}, {"Ukrainian", "uk"}, {"Urdu", "ur"},
			{"Vietnamese", "vi"}, {"Xhosa", "xh"}, {"Yoruba", "yo"}, {"Zulu", "zu"}},
		"", translating);
	choice(tr, "Show", QString(), "lyrics_translate_show",
		{{"Under each line", "under"}, {"Instead of the lyrics", "instead"}}, "under", translating);

	Card *timing = card(page, "Timing",
		"If lyrics come too early or late for every song, move them here. For one song (a sped-up or "
		"slowed version), use Earlier / Later and Slower / Faster under the panel's media card.");
	slider(timing, "Delay", "Lyrics too early? Move it right", "lyrics_delay", -3000, 3000, 0, 1,
		[](int v) { return QString("%1%2 s").arg(v > 0 ? "+" : "").arg(v / 1000.0, 0, 'f', 2); }, looking);
	slider(timing, "Speed", "Lyrics drifting ahead? Lower it", "lyrics_speed", 80, 120, 100, 1,
		[](int v) { return QString("%1%").arg(v); }, looking);
	more_toggle(page);
	finish(page);
	return page.widget;
}

// ---- Default apps ---------------------------------------------------------------------

void default_app_cards(Page &page) {
	struct Kind {
		const char *title;
		QStringList types;   // the first decides what's offered and shown
	};
	const std::vector<Kind> kinds = {
		{"Web browser", {"x-scheme-handler/https", "x-scheme-handler/http", "text/html"}},
		{"Files", {"inode/directory"}},
		{"Email", {"x-scheme-handler/mailto"}},
		{"Music", {"audio/mpeg", "audio/flac", "audio/ogg", "audio/x-vorbis+ogg", "audio/mp4", "audio/x-wav"}},
		{"Videos", {"video/mp4", "video/x-matroska", "video/webm", "video/quicktime"}},
		{"Pictures", {"image/png", "image/jpeg", "image/webp", "image/gif"}},
		{"Text files", {"text/plain"}},
		{"PDF documents", {"application/pdf"}},
	};
	const std::vector<DesktopApp> apps = desktop_apps();
	Card *c = card(page, "Open with", have_program("xdg-mime") ? QString()
		: QString("Needs xdg-utils: sudo dnf install xdg-utils"));
	for (const Kind &k : kinds) {
		auto *combo = new ElideCombo;
		flex_width(combo, 240);
		for (const DesktopApp &a : apps) {
			if (a.mime.contains(k.types.first())) {
				combo->addItem(app_icon(a.icon), a.name, a.id);
			}
		}
		if (combo->count() == 0) {
			delete combo;
			continue;
		}
		const QString current = run_tool("xdg-mime", {"query", "default", k.types.first()});
		int i = combo->findData(current);
		if (i < 0) {
			combo->insertItem(0, "Not set", "");
			i = 0;
		}
		combo->setCurrentIndex(i);
		make_row(c, k.title, QString(), combo, nullptr);
		const QStringList types = k.types;
		const bool browser = QString(k.title) == "Web browser";
		QObject::connect(combo, &QComboBox::currentIndexChanged, [combo, types, browser](int) {
			const QString id = combo->currentData().toString();
			if (id.isEmpty()) return;
			QProcess::startDetached("xdg-mime", QStringList{"default", id} + types);
			if (browser) {
				QProcess::startDetached("xdg-settings", {"set", "default-web-browser", id});
			}
		});
	}
}

// ---- Power ------------------------------------------------------------------------------

// The power mode now ("" = no power-profiles-daemon), and the ones offered.
QString power_mode_now() {
	QString out = run_tool("busctl", {"--system", "get-property", "org.freedesktop.UPower.PowerProfiles",
		"/org/freedesktop/UPower/PowerProfiles", "org.freedesktop.UPower.PowerProfiles", "ActiveProfile"});
	if (out.isEmpty()) {
		out = run_tool("busctl", {"--system", "get-property", "net.hadess.PowerProfiles",
			"/net/hadess/PowerProfiles", "net.hadess.PowerProfiles", "ActiveProfile"});
	}
	if (out.isEmpty() && have_program("powerprofilesctl")) {
		return run_tool("powerprofilesctl", {"get"});
	}
	// s "balanced"
	out = out.trimmed();
	const int q = out.indexOf('"');
	return q < 0 ? out : out.mid(q + 1).chopped(out.endsWith('"') ? 1 : 0);
}

QString power_modes() {
	QString out = run_tool("busctl", {"--system", "get-property", "org.freedesktop.UPower.PowerProfiles",
		"/org/freedesktop/UPower/PowerProfiles", "org.freedesktop.UPower.PowerProfiles", "Profiles"});
	if (out.isEmpty()) {
		out = run_tool("busctl", {"--system", "get-property", "net.hadess.PowerProfiles",
			"/net/hadess/PowerProfiles", "net.hadess.PowerProfiles", "Profiles"});
	}
	if (out.isEmpty() && have_program("powerprofilesctl")) {
		out = run_tool("powerprofilesctl", {"list"});
	}
	return out;
}

QWidget *power_page() {
	Page page = make_page("Power & battery", "When it locks, when the screen turns off and when the computer goes to sleep.");
	const Items off = {{"Never", "0"}, {"1 minute", "1"}, {"2 minutes", "2"}, {"3 minutes", "3"},
		{"5 minutes", "5"}, {"10 minutes", "10"}, {"15 minutes", "15"}, {"30 minutes", "30"},
		{"1 hour", "60"}};
	const Items sleep = {{"Never", "0"}, {"5 minutes", "5"}, {"10 minutes", "10"}, {"15 minutes", "15"},
		{"20 minutes", "20"}, {"30 minutes", "30"}, {"45 minutes", "45"}, {"1 hour", "60"},
		{"2 hours", "120"}};
	// "Lock" is an earlier lock of its own; left at "With the screen" it
	// locks when the screen turns off (Settings › Lock screen).
	Items lock_at = off;
	if (settings->get("lock_screen_off", "true") != "false") lock_at[0] = {"With the screen", "0"};
	const bool battery = has_battery();
	Card *idle = card(page, "When you're away",
		"Videos, games and calls keep the screen on, and so does Caffeine in the control centre.");
	// Each one has its own time.
	if (battery) {
		choice(idle, "Lock on battery", "Sooner than the screen turns off, or with it", "lock_after_battery", lock_at, "0");
		choice(idle, "Lock when plugged in", "Sooner than the screen turns off, or with it", "lock_after_ac", lock_at, "0");
		choice(idle, "Screen off on battery", QString(), "screen_off_battery", off, "5");
		choice(idle, "Screen off when plugged in", QString(), "screen_off_ac", off, "10");
		choice(idle, "Sleep on battery", QString(), "sleep_battery", sleep, "15");
		choice(idle, "Sleep when plugged in", QString(), "sleep_ac", sleep, "0");
	} else {
		choice(idle, "Lock", "Sooner than the screen turns off, or with it", "lock_after_ac", lock_at, "0");
		choice(idle, "Turn the screen off", QString(), "screen_off_ac", off, "10");
		choice(idle, "Sleep", QString(), "sleep_ac", sleep, "0");
	}
	toggle(idle, "Stay awake while a window is full screen", "Games and videos, even ones that don't ask",
		"idle_fullscreen", true);

	{
		Card *btn = card(page, "Power menu",
			"Opened by the round power button in the app menu, and by Power in the control centre. "
			"Switch off what you don't want in it.");
		toggle(btn, "Power button in the app menu", "The control centre always has one", "menu_power", true);
		const QStringList order = {"lock", "sleep", "hibernate", "logout", "restart", "shutdown", "firmware"};
		const QStringList def = {"lock", "sleep", "logout", "restart", "shutdown"};
		list_toggle(btn, "Lock", "Dusk's lock screen (Settings › Lock screen)",
			"power_actions", "lock", order, def);
		list_toggle(btn, "Sleep", QString(), "power_actions", "sleep", order, def);
		list_toggle(btn, "Hibernate", "Only if your system is set up for it", "power_actions", "hibernate",
			order, def);
		list_toggle(btn, "Log out", QString(), "power_actions", "logout", order, def);
		list_toggle(btn, "Restart", QString(), "power_actions", "restart", order, def);
		list_toggle(btn, "Shut down", QString(), "power_actions", "shutdown", order, def);
		advanced(list_toggle(btn, "Restart to firmware setup", "The BIOS/UEFI settings", "power_actions",
			"firmware", order, def));
		toggle(btn, "Ask first", "Before logging out, restarting and shutting down", "power_confirm", true);
		advanced(text_field(btn, "Another screen locker", "Empty = Dusk's own lock screen. Or a command, e.g. "
			"swaylock -f", "lock_command", "Dusk's own"));
	}

	if (have_program("busctl") || have_program("powerprofilesctl")) {
		const QString mode_now = power_mode_now();
		if (!mode_now.isEmpty()) {
		Card *mode = card(page, "Power mode");
		auto *combo = new ElideCombo;
		flex_width(combo, 240);
		combo->addItem("Power saver", "power-saver");
		combo->addItem("Balanced", "balanced");
		if (power_modes().contains("performance")) combo->addItem("Performance", "performance");
		const int i = combo->findData(mode_now);
		combo->setCurrentIndex(std::max(0, i));
		make_row(mode, "Mode", "Also in the control centre", combo, nullptr);
		QObject::connect(combo, &QComboBox::currentIndexChanged, [combo](int) {
			const QString p = combo->currentData().toString();
			// Over D-Bus (new name, then old), then the command line tool.
			QProcess::startDetached("sh", {"-c",
				"busctl --system set-property org.freedesktop.UPower.PowerProfiles "
				"/org/freedesktop/UPower/PowerProfiles org.freedesktop.UPower.PowerProfiles ActiveProfile s \"$0\" || "
				"busctl --system set-property net.hadess.PowerProfiles /net/hadess/PowerProfiles "
				"net.hadess.PowerProfiles ActiveProfile s \"$0\" || powerprofilesctl set \"$0\"", p});
		});
		}
	}
	if (battery) {
		Card *b = card(page, "Battery");
		toggle(b, "Low battery warnings", "At 20%, 10% and 5%", "battery_warnings", true);
	}
	finish(page);
	return page.widget;
}

// ---- Startup apps -------------------------------------------------------------------------

struct StartupEntry {
	QString file, name, icon, exec;
	QString user_path, system_path;
	bool enabled = true;
};

QString user_autostart_dir() {
	return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/autostart";
}

std::vector<StartupEntry> read_startup() {
	QStringList sys_dirs;
	for (const QString &d : qEnvironmentVariable("XDG_CONFIG_DIRS", "/etc/xdg").split(':', Qt::SkipEmptyParts)) {
		sys_dirs << d + "/autostart";
	}
	std::map<QString, StartupEntry> by_file;
	auto read = [&by_file](const QString &path, bool user) {
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly)) return;
		const QString file = QFileInfo(path).fileName();
		StartupEntry &e = by_file[file];
		e.file = file;
		(user ? e.user_path : e.system_path) = path;
		if (!user && !e.user_path.isEmpty()) return;   // the user's copy wins
		bool in_entry = false, hidden = false, disabled = false, other_desktop = false;
		QString name, icon, exec;
		QTextStream ts(&f);
		while (!ts.atEnd()) {
			const QString line = ts.readLine().trimmed();
			if (line.startsWith('[')) { in_entry = line == "[Desktop Entry]"; continue; }
			if (!in_entry) continue;
			if (line.startsWith("Name=") && name.isEmpty()) name = line.mid(5);
			else if (line.startsWith("Icon=")) icon = line.mid(5);
			else if (line.startsWith("Exec=")) exec = line.mid(5);
			else if (line == "Hidden=true") hidden = true;
			else if (line == "X-GNOME-Autostart-enabled=false") disabled = true;
			else if (line.startsWith("OnlyShowIn=")) {
				other_desktop = !line.mid(11).split(';', Qt::SkipEmptyParts).contains("Dusk");
			} else if (line.startsWith("NotShowIn=")) {
				other_desktop = other_desktop || line.mid(10).split(';', Qt::SkipEmptyParts).contains("Dusk");
			}
		}
		e.name = name.isEmpty() ? file : name;
		e.icon = icon;
		e.exec = strip_field_codes(exec);
		e.enabled = !hidden && !disabled;
		if (other_desktop) e.file.clear();   // never runs in Dusk: not listed
	};
	QDir user(user_autostart_dir());
	for (const QString &n : user.entryList({"*.desktop"}, QDir::Files)) read(user.filePath(n), true);
	for (const QString &dir : sys_dirs) {
		for (const QString &n : QDir(dir).entryList({"*.desktop"}, QDir::Files)) {
			if (!by_file.count(n) || by_file[n].system_path.isEmpty()) read(dir + "/" + n, false);
		}
	}
	std::vector<StartupEntry> out;
	for (auto &[file, e] : by_file) {
		if (!e.file.isEmpty()) out.push_back(e);
	}
	std::sort(out.begin(), out.end(), [](const StartupEntry &a, const StartupEntry &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	return out;
}

// Turns an entry on or off with a copy in ~/.config/autostart (the
// system's file is never touched).
void set_startup_enabled(const StartupEntry &e, bool on) {
	QDir().mkpath(user_autostart_dir());
	const QString target = user_autostart_dir() + "/" + e.file;
	QString text;
	{
		QFile src(e.user_path.isEmpty() ? e.system_path : e.user_path);
		if (!src.open(QIODevice::ReadOnly)) return;
		text = QString::fromUtf8(src.readAll());
	}
	QStringList lines = text.split('\n');
	lines.erase(std::remove_if(lines.begin(), lines.end(), [](const QString &l) {
		const QString t = l.trimmed();
		return t.startsWith("Hidden=") || t.startsWith("X-GNOME-Autostart-enabled=");
	}), lines.end());
	if (!on) {
		const int at = lines.indexOf("[Desktop Entry]");
		lines.insert(at >= 0 ? at + 1 : 0, "Hidden=true");
	}
	// Back on, and the system has the same file: just drop our copy.
	if (on && !e.system_path.isEmpty()) {
		QFile::remove(target);
		return;
	}
	QFile out(target);
	if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		out.write(lines.join('\n').toUtf8());
	}
}

class StartupPage {
public:
	QWidget *widget;

	StartupPage() {
		Page page = make_page("Startup apps", "Apps that open by themselves when you log in.");
		widget = page.widget;
		Card *master = card(page, "");
		toggle(master, "Start apps at login", "Off: none of the apps below start", "autostart", true);
		list_card_ = card(page, "Apps");
		holder_ = new QWidget;
		holder_layout_ = new QVBoxLayout(holder_);
		holder_layout_->setContentsMargins(0, 0, 0, 0);
		holder_layout_->setSpacing(0);
		list_card_->add(holder_);
		auto *add_row = new QWidget;
		auto *h = new QHBoxLayout(add_row);
		h->setContentsMargins(0, 8, 0, 10);
		auto *add = new ElideCombo;
		flex_width(add, 260);
		add->addItem("Add an app…", "");
		for (const DesktopApp &a : desktop_apps()) {
			add->addItem(app_icon(a.icon), a.name, a.path);
		}
		h->addWidget(add);
		h->addStretch(1);
		list_card_->add(add_row);
		QObject::connect(add, &QComboBox::currentIndexChanged, [this, add](int i) {
			if (i <= 0) return;
			const QString path = add->currentData().toString();
			QDir().mkpath(user_autostart_dir());
			const QString target = user_autostart_dir() + "/" + QFileInfo(path).fileName();
			QFile::remove(target);
			QFile::copy(path, target);
			QFile(target).setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup |
				QFile::ReadOther);
			const QSignalBlocker block(add);
			add->setCurrentIndex(0);
			fill();
		});
		finish(page);
		fill();
	}

private:
	void fill() {
		while (QLayoutItem *item = holder_layout_->takeAt(0)) {
			delete item->widget();
			delete item;
		}
		const auto entries = read_startup();
		if (entries.empty()) {
			auto *none = new QLabel("No apps start at login.");
			none->setObjectName("subtitle");
			none->setContentsMargins(0, 10, 0, 6);
			holder_layout_->addWidget(none);
		}
		for (const StartupEntry &e : entries) {
			auto *row = new QWidget;
			auto *h = new QHBoxLayout(row);
			h->setContentsMargins(0, 8, 0, 8);
			h->setSpacing(12);
			auto *icon = new QLabel;
			icon->setPixmap(app_icon(e.icon).pixmap(24, 24));
			icon->setFixedSize(24, 24);
			auto *texts = new QVBoxLayout;
			texts->setSpacing(1);
			texts->addWidget(new QLabel(e.name));
			auto *cmd = new QLabel(e.exec);
			cmd->setObjectName("subtitle");
			texts->addWidget(cmd);
			auto *sw = new Switch;
			sw->set_quietly(e.enabled);
			h->addWidget(icon);
			h->addLayout(texts, 1);
			h->addWidget(sw);
			// Added by you (no system file): it can be removed.
			auto *del = new QToolButton;
			del->setObjectName("reset");
			del->setText(QString::fromUtf8("✕"));
			del->setToolTip("Remove from startup");
			QSizePolicy sp = del->sizePolicy();
			sp.setRetainSizeWhenHidden(true);
			del->setSizePolicy(sp);
			del->setVisible(e.system_path.isEmpty());
			h->addWidget(del);
			holder_layout_->addWidget(row);
			QObject::connect(sw, &QAbstractButton::toggled, [e](bool on) { set_startup_enabled(e, on); });
			QObject::connect(del, &QToolButton::clicked, [this, e] {
				QFile::remove(e.user_path);
				QTimer::singleShot(0, [this] { fill(); });
			});
		}
	}

	Card *list_card_;
	QWidget *holder_;
	QVBoxLayout *holder_layout_;
};

QWidget *startup_page() {
	return (new StartupPage)->widget;
}

// ---- Date & time --------------------------------------------------------------------------

// ---- Language & region -------------------------------------------------------------------
// The system's language (menus and apps) and how dates, numbers and money
// are written. Set for the whole computer with localectl; a language not on
// the computer yet is installed first (one password prompt for both).

namespace {

// Common locales, so a language can be picked before it's installed.
const char *kLocales[] = {
	"en_US", "en_GB", "en_ZA", "en_ZM", "en_ZW", "en_KE", "en_NG", "en_IN", "en_AU", "en_CA", "en_IE", "en_NZ",
	"af_ZA", "ar_EG", "ar_SA", "bn_BD", "ca_ES", "cs_CZ", "da_DK", "de_DE", "de_AT", "de_CH", "el_GR",
	"es_ES", "es_MX", "es_AR", "et_EE", "fa_IR", "fi_FI", "fil_PH", "fr_FR", "fr_CA", "fr_BE", "fr_CH", "ha_NG",
	"he_IL", "hi_IN", "hr_HR", "hu_HU", "id_ID", "ig_NG", "it_IT", "ja_JP", "ko_KR", "lt_LT", "lv_LV", "ms_MY",
	"nb_NO", "nl_NL", "nl_BE", "pl_PL", "pt_PT", "pt_BR", "ro_RO", "ru_RU", "rw_RW", "sk_SK", "sl_SI", "sr_RS",
	"st_ZA", "sv_SE", "sw_KE", "sw_TZ", "ta_IN", "th_TH", "tr_TR", "uk_UA", "ur_PK", "vi_VN", "xh_ZA", "yo_NG",
	"zh_CN", "zh_TW", "zh_HK", "zu_ZA",
};

QString locale_label(const QString &code) {
	const QLocale l(code);
	QString lang = QLocale::languageToString(l.language());
	const QString country = QLocale::territoryToString(l.territory());
	QString native = l.nativeLanguageName();
	if (!native.isEmpty()) native[0] = native[0].toUpper();
	QString label = lang + " (" + country + ")";
	if (!native.isEmpty() && native.compare(lang, Qt::CaseInsensitive) != 0) label += " · " + native;
	return label;
}

// "LANG=en_US.UTF-8" etc. from localectl.
std::map<QString, QString> current_locale() {
	std::map<QString, QString> out;
	const QString st = run_tool("localectl", {"status"});
	static const QRegularExpression re("\\b(LANG|LC_[A-Z]+)=([^\\s]+)");
	for (auto it = re.globalMatch(st); it.hasNext();) {
		const QRegularExpressionMatch m = it.next();
		out[m.captured(1)] = m.captured(2);
	}
	if (!out.count("LANG")) out["LANG"] = QString::fromLocal8Bit(qgetenv("LANG"));
	return out;
}

QString base_of(QString loc) {   // "en_US.UTF-8" -> "en_US"
	loc = loc.section('.', 0, 0);
	return loc.section('@', 0, 0);
}

QStringList installed_locales() {
	QStringList out;
	for (const QString &l : run_tool("localectl", {"list-locales"}, 5000).split('\n', Qt::SkipEmptyParts)) {
		const QString b = base_of(l.trimmed());
		if (b.contains('_') && !out.contains(b)) out << b;
	}
	return out;
}

// The shell script run as root: install the language if it's missing, then set it.
QString locale_script(const QString &lang, const QString &formats, bool install) {
	static const QRegularExpression safe("^[A-Za-z]{2,3}_[A-Z]{2}$");
	if (!safe.match(lang).hasMatch() || (!formats.isEmpty() && !safe.match(formats).hasMatch())) return QString();
	QString sh;
	if (install) {
		QStringList codes = {lang};
		if (!formats.isEmpty() && formats != lang) codes << formats;
		for (const QString &c : codes) {
			const QString two = c.section('_', 0, 0);
			sh += "if command -v dnf >/dev/null; then "
				"dnf install -y --skip-unavailable glibc-langpack-" + two + " langpacks-" + two +
				" || dnf install -y glibc-langpack-" + two + "; "
				"elif [ -f /etc/locale.gen ]; then "
				"grep -q '^" + c + ".UTF-8' /etc/locale.gen || "
				"{ sed -i 's/^# *\\(" + c + ".UTF-8 UTF-8\\)/\\1/' /etc/locale.gen; "
				"grep -q '^" + c + ".UTF-8' /etc/locale.gen || echo '" + c + ".UTF-8 UTF-8' >> /etc/locale.gen; }; "
				"locale-gen; fi; ";
		}
	}
	QString set = "localectl set-locale LANG=" + lang + ".UTF-8";
	if (!formats.isEmpty() && formats != lang) {
		for (const char *k : {"LC_TIME", "LC_NUMERIC", "LC_MONETARY", "LC_MEASUREMENT", "LC_PAPER"}) {
			set += QString(" ") + k + "=" + formats + ".UTF-8";
		}
	}
	return sh + set;
}

} // namespace

QWidget *language_page() {
	Page page = make_page("Language & region",
		"The language of menus and apps, and how dates, numbers and money are written. For everyone "
		"on this computer; it takes effect when you log in again.");
	if (!have_program("localectl")) {
		Card *c = card(page, "", "Changing these needs systemd's localectl.");
		c->add(new QWidget);
		finish(page);
		return page.widget;
	}
	const auto cur = current_locale();
	const QString lang_now = base_of(cur.count("LANG") ? cur.at("LANG") : QString("en_US"));
	const QString fmt_now = cur.count("LC_TIME") ? base_of(cur.at("LC_TIME")) : QString();
	const QStringList have = installed_locales();

	QStringList codes;
	for (const char *c : kLocales) codes << c;
	for (const QString &c : have) if (!codes.contains(c)) codes << c;
	if (!codes.contains(lang_now)) codes << lang_now;
	std::vector<std::pair<QString, QString>> items;   // label, code
	for (const QString &c : codes) items.push_back({locale_label(c) + (have.contains(c) ? "" : "  ↓"), c});
	std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
		return QString::localeAwareCompare(a.first, b.first) < 0;
	});

	auto make_combo = [&](bool with_same) {
		auto *cb = new QComboBox;
		cb->setEditable(true);
		cb->setInsertPolicy(QComboBox::NoInsert);
		flex_width(cb, 280);
		if (with_same) cb->addItem("Same as the language", QString());
		for (const auto &[label, code] : items) cb->addItem(label, code);
		cb->completer()->setFilterMode(Qt::MatchContains);
		cb->completer()->setCompletionMode(QCompleter::PopupCompletion);
		return cb;
	};

	Card *c = card(page, "Language", "↓ means it's downloaded and installed first (asks for your password).");
	auto *lang = make_combo(false);
	lang->setCurrentIndex(std::max(0, lang->findData(lang_now)));
	make_row(c, "Language", "Menus, apps and messages · type to find one", lang, nullptr);
	auto *fmt = make_combo(true);
	fmt->setCurrentIndex(std::max(0, fmt_now.isEmpty() || fmt_now == lang_now ? 0 : fmt->findData(fmt_now)));
	make_row(c, "Formats", "Dates, times, numbers, money and paper size", fmt, nullptr);

	// What it will look like.
	auto *preview = new QLabel;
	preview->setObjectName("hint");
	preview->setWordWrap(true);
	auto show_preview = [lang, fmt, preview] {
		const QString code = fmt->currentData().toString().isEmpty() ? lang->currentData().toString()
			: fmt->currentData().toString();
		const QLocale l(code);
		const QDateTime now = QDateTime::currentDateTime();
		preview->setText(l.toString(now.date(), QLocale::LongFormat) + "   ·   " + l.toString(now.time(), QLocale::ShortFormat) +
			"   ·   " + l.toString(1234567.89, 'f', 2) + "   ·   " + l.toCurrencyString(99.5));
	};
	show_preview();
	make_row(c, "Looks like", QString(), preview, nullptr, true);
	QObject::connect(lang, &QComboBox::currentIndexChanged, [show_preview](int) { show_preview(); });
	QObject::connect(fmt, &QComboBox::currentIndexChanged, [show_preview](int) { show_preview(); });

	// Apply (and, after, log out to see it).
	auto *box = new QWidget;
	auto *h = new QHBoxLayout(box);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(8);
	auto *status = new QLabel;
	status->setObjectName("hint");
	status->setWordWrap(true);
	auto *apply = new QPushButton("Apply");
	apply->setObjectName("primary");
	apply->setCursor(Qt::PointingHandCursor);
	auto *logout = new QPushButton("Log out now");
	logout->setCursor(Qt::PointingHandCursor);
	logout->hide();
	h->addWidget(status, 1);
	h->addWidget(logout);
	h->addWidget(apply);
	c->add(box);
	QObject::connect(apply, &QPushButton::clicked, [=] {
		const QString l = lang->currentData().toString();
		QString f = fmt->currentData().toString();
		if (f == l) f.clear();
		const QStringList now_have = installed_locales();
		const bool missing = !now_have.contains(l) || (!f.isEmpty() && !now_have.contains(f));
		const QString script = locale_script(l, f, missing);
		if (script.isEmpty()) {
			status->setText("That isn't a language code this page can set.");
			return;
		}
		apply->setEnabled(false);
		status->setText(missing ? "Installing the language, then setting it…" : "Setting it…");
		auto *pr = new QProcess(apply);
		pr->setProcessChannelMode(QProcess::MergedChannels);
		QObject::connect(pr, &QProcess::finished, [=](int code, QProcess::ExitStatus) {
			apply->setEnabled(true);
			const QString out = QString::fromUtf8(pr->readAll()).trimmed();
			pr->deleteLater();
			if (code == 0) {
				status->setText("Done. Log out and back in to see everything in the new language.");
				logout->show();
			} else if (code == 126 || code == 127) {
				status->setText("Not changed (the password prompt was closed).");
			} else {
				status->setText("Didn't work: " + out.section('\n', -1));
			}
		});
		pr->start("pkexec", {"sh", "-c", script});
	});
	QObject::connect(logout, &QPushButton::clicked, [] {
		// Dusk ends the session when asked to stop (as Log out in the power menu does).
		QProcess::startDetached("sh", {"-c", "pkill -TERM -u \"$(id -u)\" -x dusk"});
	});

	Card *more = card(page, "Also");
	auto *kb = new QLabel("The keyboard layout is in Keyboard; the time zone in Date & time. Lyrics can be "
		"translated into this language (Media & lyrics).");
	kb->setObjectName("hint");
	kb->setWordWrap(true);
	more->add(kb);
	finish(page);
	return page.widget;
}

QWidget *datetime_page() {
	Page page = make_page("Date & time", "The clock's look is in Panels.");
	Card *zone = card(page, "Time", have_program("timedatectl") ? QString()
		: QString("Changing these needs systemd's timedatectl."));
	if (have_program("timedatectl")) {
		// Automatic time (NTP)
		auto *sw = new Switch;
		sw->set_quietly(run_tool("timedatectl", {"show", "-p", "NTP", "--value"}) == "yes");
		make_row(zone, "Set the time automatically", "From the internet", sw, nullptr);
		QObject::connect(sw, &QAbstractButton::toggled, [](bool on) {
			QProcess::startDetached("timedatectl", {"set-ntp", on ? "true" : "false"});
		});

		// Time zone: type to search.
		auto *tz = new QComboBox;
		tz->setEditable(true);
		tz->setInsertPolicy(QComboBox::NoInsert);
		flex_width(tz, 240);
		const QStringList zones = run_tool("timedatectl", {"list-timezones"}, 5000).split('\n', Qt::SkipEmptyParts);
		for (const QString &z : zones) {
			QString label = z;
			tz->addItem(label.replace('_', ' '), z);
		}
		const QString current = run_tool("timedatectl", {"show", "-p", "Timezone", "--value"});
		tz->setCurrentIndex(std::max(0, tz->findData(current)));
		tz->completer()->setFilterMode(Qt::MatchContains);
		tz->completer()->setCompletionMode(QCompleter::PopupCompletion);
		make_row(zone, "Time zone", "Type a city to find it", tz, nullptr);
		QObject::connect(tz, &QComboBox::activated, [tz](int) {
			QProcess::startDetached("timedatectl", {"set-timezone", tz->currentData().toString()});
		});
	}
	finish(page);
	return page.widget;
}

// ---- Keyboard -----------------------------------------------------------------------

// Every layout, variant and option xkb knows, with its human name.
struct XkbCatalog {
	struct Layout {
		QString code, variant, name;
	};
	std::vector<Layout> layouts;              // layouts and their variants
	std::map<QString, QString> options;       // "caps:escape" -> "Make Caps Lock an additional Esc"

	static const XkbCatalog &get() {
		static XkbCatalog c = load();
		return c;
	}
	QString name_of(const QString &code, const QString &variant) const {
		for (const Layout &l : layouts) {
			if (l.code == code && l.variant == variant) return l.name;
		}
		return variant.isEmpty() ? code : code + " (" + variant + ")";
	}

private:
	static XkbCatalog load() {
		XkbCatalog c;
		QFile f("/usr/share/X11/xkb/rules/evdev.lst");
		if (!f.open(QIODevice::ReadOnly)) {
			static const std::pair<const char *, const char *> kFallback[] = {
				{"us", "English (US)"}, {"gb", "English (UK)"}, {"de", "German"}, {"fr", "French"},
				{"es", "Spanish"}, {"it", "Italian"}, {"pt", "Portuguese"}, {"br", "Portuguese (Brazil)"},
				{"nl", "Dutch"}, {"se", "Swedish"}, {"pl", "Polish"}, {"ru", "Russian"},
				{"ua", "Ukrainian"}, {"tr", "Turkish"}, {"ara", "Arabic"}, {"jp", "Japanese"},
			};
			for (const auto &[code, name] : kFallback) c.layouts.push_back({code, "", name});
			return c;
		}
		QString section;
		QTextStream ts(&f);
		while (!ts.atEnd()) {
			const QString line = ts.readLine();
			if (line.startsWith('!')) {
				section = line.mid(1).trimmed();
				continue;
			}
			const QString t = line.trimmed();
			if (t.isEmpty()) continue;
			const int sp = t.indexOf(QRegularExpression("\\s"));
			if (sp < 0) continue;
			const QString key = t.left(sp);
			QString rest = t.mid(sp).trimmed();
			if (section == "layout") {
				c.layouts.push_back({key, "", rest});
			} else if (section == "variant") {
				const int colon = rest.indexOf(':');
				if (colon < 0) continue;
				c.layouts.push_back({rest.left(colon).trimmed(), key, rest.mid(colon + 1).trimmed()});
			} else if (section == "option") {
				c.options[key] = rest;
			}
		}
		std::stable_sort(c.layouts.begin(), c.layouts.end(), [](const Layout &a, const Layout &b) {
			return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
		});
		return c;
	}
};

// keyboard_options is a comma list; each of these groups owns some of it.
QStringList kb_options() {
	return settings->get("keyboard_options").split(',', Qt::SkipEmptyParts);
}

bool option_in_group(const QString &opt, const QStringList &prefixes) {
	for (const QString &p : prefixes) {
		if (opt.startsWith(p)) return true;
	}
	return false;
}

QString option_for_group(const QStringList &prefixes) {
	for (const QString &o : kb_options()) {
		if (option_in_group(o.trimmed(), prefixes)) return o.trimmed();
	}
	return QString();
}

void set_option_group(const QStringList &prefixes, const QString &value) {
	QStringList keep;
	for (const QString &o : kb_options()) {
		if (!option_in_group(o.trimmed(), prefixes)) keep << o.trimmed();
	}
	if (!value.isEmpty()) keep << value;
	if (keep.isEmpty()) settings->remove("keyboard_options");
	else settings->set("keyboard_options", keep.join(','));
	changed("keyboard_options");
}

// A combo that owns one group of options (e.g. what Caps Lock does).
QWidget *option_choice(Card *card, const QString &title, const QString &subtitle,
		const QStringList &prefixes, const Items &items, std::function<bool()> visible = {}) {
	auto *combo = new ElideCombo;
	flex_width(combo, 240);
	const auto &known = XkbCatalog::get().options;
	for (const auto &[label, value] : items) {
		if (value.isEmpty() || known.empty() || known.count(value)) combo->addItem(label, value);
	}
	QToolButton *reset = nullptr;
	QWidget *row = make_row(card, title, subtitle, combo, &reset);
	auto refresh = [combo, prefixes] {
		const QSignalBlocker block(combo);
		const QString now = option_for_group(prefixes);
		int i = combo->findData(now);
		if (i < 0 && !now.isEmpty()) {
			// Set by hand to something not in the list: show it as is.
			const auto &opts = XkbCatalog::get().options;
			auto it = opts.find(now);
			combo->addItem(it != opts.end() ? it->second : now, now);
			i = combo->count() - 1;
		}
		combo->setCurrentIndex(std::max(0, i));
	};
	refresh();
	QObject::connect(combo, &QComboBox::currentIndexChanged, [combo, prefixes](int) {
		set_option_group(prefixes, combo->currentData().toString());
	});
	QObject::connect(reset, &QToolButton::clicked, [prefixes, refresh] {
		set_option_group(prefixes, QString());
		refresh();
	});
	bindings.push_back(Binding{row, reset, "keyboard_options", QString(), refresh, visible,
		[prefixes] { return option_for_group(prefixes).isEmpty(); }});
	return row;
}

// Picks a layout (or a variant of one) from the full list, with search.
bool pick_layout(QWidget *parent, QString *code, QString *variant) {
	QDialog dialog(parent);
	dialog.setWindowTitle("Add a keyboard layout");
	dialog.resize(440, 520);
	auto *v = new QVBoxLayout(&dialog);
	v->setContentsMargins(18, 18, 18, 14);
	auto *search = new QLineEdit;
	search->setPlaceholderText("Search: a language or country");
	search->setClearButtonEnabled(true);
	auto *list = new QListWidget;
	list->setObjectName("editor");
	for (const XkbCatalog::Layout &l : XkbCatalog::get().layouts) {
		auto *item = new QListWidgetItem(l.variant.isEmpty() ? l.name : "      " + l.name);
		item->setData(Qt::UserRole, l.code);
		item->setData(Qt::UserRole + 1, l.variant);
		item->setData(Qt::UserRole + 2, (l.name + " " + l.code + " " + l.variant).toLower());
		list->addItem(item);
	}
	QObject::connect(search, &QLineEdit::textChanged, [list](const QString &t) {
		const QString needle = t.trimmed().toLower();
		for (int i = 0; i < list->count(); i++) {
			QListWidgetItem *item = list->item(i);
			item->setHidden(!needle.isEmpty() && !item->data(Qt::UserRole + 2).toString().contains(needle));
		}
	});
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	buttons->button(QDialogButtonBox::Ok)->setText("Add");
	buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
	QObject::connect(list, &QListWidget::currentItemChanged, [buttons](QListWidgetItem *now, QListWidgetItem *) {
		buttons->button(QDialogButtonBox::Ok)->setEnabled(now != nullptr);
	});
	QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	v->addWidget(search);
	v->addWidget(list, 1);
	v->addWidget(buttons);
	search->setFocus();
	if (dialog.exec() != QDialog::Accepted || !list->currentItem()) return false;
	*code = list->currentItem()->data(Qt::UserRole).toString();
	*variant = list->currentItem()->data(Qt::UserRole + 1).toString();
	return true;
}

QWidget *keyboard_page() {
	Page page = make_page("Keyboard", QString());
	Card *layout = card(page, "Layouts",
		"Drag to reorder; the first one is used when you log in.");
	{
		auto *box = new QWidget;
		auto *v = new QVBoxLayout(box);
		v->setContentsMargins(0, 8, 0, 8);
		auto *list = editor_list(110);
		auto *buttons = new QHBoxLayout;
		auto *add = new QPushButton("Add layout…");
		auto *remove = new QPushButton("Remove");
		buttons->addWidget(add);
		buttons->addWidget(remove);
		buttons->addStretch(1);
		v->addWidget(list);
		v->addLayout(buttons);
		layout->add(box);

		auto refresh = [list, remove] {
			const QSignalBlocker block(list);
			list->clear();
			const QStringList codes = settings->get("keyboard_layout").split(',');
			const QStringList variants = settings->get("keyboard_variant").split(',');
			for (int i = 0; i < codes.size(); i++) {
				const QString code = codes[i].trimmed();
				if (code.isEmpty()) continue;
				const QString var = variants.value(i).trimmed();
				auto *item = new QListWidgetItem(XkbCatalog::get().name_of(code, var) +
					"   ·   " + code + (var.isEmpty() ? "" : "/" + var));
				item->setData(Qt::UserRole, code);
				item->setData(Qt::UserRole + 1, var);
				list->addItem(item);
			}
			if (list->count() == 0) {
				auto *item = new QListWidgetItem("The system's layout");
				item->setFlags(Qt::ItemIsEnabled);
				list->addItem(item);
			}
			remove->setEnabled(!settings->get("keyboard_layout").isEmpty());
		};
		auto store = [list] {
			QStringList codes, variants;
			bool any_variant = false;
			for (int i = 0; i < list->count(); i++) {
				const QString code = list->item(i)->data(Qt::UserRole).toString();
				if (code.isEmpty()) continue;
				codes << code;
				variants << list->item(i)->data(Qt::UserRole + 1).toString();
				any_variant = any_variant || !variants.back().isEmpty();
			}
			if (codes.isEmpty()) settings->remove("keyboard_layout");
			else settings->set("keyboard_layout", codes.join(','));
			if (!any_variant) settings->remove("keyboard_variant");
			else settings->set("keyboard_variant", variants.join(','));
			changed("keyboard_layout");
		};
		refresh();
		QObject::connect(list->model(), &QAbstractItemModel::rowsMoved, [store] { store(); });
		QObject::connect(add, &QPushButton::clicked, [add, list, store, refresh] {
			QString code, variant;
			if (!pick_layout(add->window(), &code, &variant)) return;
			if (list->count() == 1 && list->item(0)->data(Qt::UserRole).toString().isEmpty()) {
				delete list->takeItem(0);
			}
			auto *item = new QListWidgetItem(code);
			item->setData(Qt::UserRole, code);
			item->setData(Qt::UserRole + 1, variant);
			list->addItem(item);
			store();
			refresh();
		});
		QObject::connect(remove, &QPushButton::clicked, [list, store, refresh] {
			int row = list->currentRow();
			if (row < 0) row = list->count() - 1;
			if (row >= 0) delete list->takeItem(row);
			store();
			refresh();
		});
		bindings.push_back(Binding{box, nullptr, "", "", refresh, {}, {}});
	}
	auto several = [] { return settings->get("keyboard_layout").contains(','); };
	option_choice(layout, "Switch layouts with", QString(), {"grp:"},
		{{"Nothing", ""}, {"Alt+Shift", "grp:alt_shift_toggle"},
			{"Ctrl+Shift", "grp:ctrl_shift_toggle"}, {"Super+Space", "grp:win_space_toggle"},
			{"Both Shift keys", "grp:shifts_toggle"}, {"Caps Lock", "grp:caps_toggle"},
			{"Right Alt", "grp:toggle"}}, several);

	Card *keys = card(page, "Special keys");
	option_choice(keys, "Caps Lock", QString(), {"caps:", "ctrl:nocaps", "ctrl:swapcaps"},
		{{"Caps Lock (normal)", ""}, {"Escape", "caps:escape"}, {"Swap with Escape", "caps:swapescape"},
			{"Another Ctrl", "ctrl:nocaps"}, {"Swap with Left Ctrl", "ctrl:swapcaps"},
			{"Backspace", "caps:backspace"}, {"Does nothing", "caps:none"}});
	option_choice(keys, "Compose key", "Type é with Compose, e, ' and more", {"compose:"},
		{{"None", ""}, {"Right Alt", "compose:ralt"}, {"Right Ctrl", "compose:rctrl"},
			{"Menu key", "compose:menu"}, {"Right Super", "compose:rwin"},
			{"Caps Lock", "compose:caps"}, {"Scroll Lock", "compose:sclk"}});
	advanced(option_choice(keys, "Alt and Super", QString(), {"altwin:"},
		{{"Normal", ""}, {"Swap Alt and Super", "altwin:swap_alt_win"}}));
	advanced(option_choice(keys, "Number pad", QString(), {"numpad:"},
		{{"Normal", ""}, {"Always digits (like a Mac)", "numpad:mac"}}));

	shortcut_cards(page);

	Card *locks = card(page, "Lock keys when Dusk starts",
		"Also for a keyboard plugged in later. Leave as is: however the keyboard comes up.");
	choice(locks, "Num Lock", "On: the number pad types digits", "numlock_on_start",
		{{"On", "on"}, {"Off", "off"}, {"Leave as is", "keep"}}, "keep");
	choice(locks, "Caps Lock", QString(), "capslock_on_start",
		{{"Off", "off"}, {"On", "on"}, {"Leave as is", "keep"}}, "off");

	Card *repeat = card(page, "Key repeat");
	slider(repeat, "Delay", "Before a held key repeats", "repeat_delay", 150, 1000, 600, 1,
		[](int v) { return QString("%1 ms").arg(v); });
	slider(repeat, "Speed", "Repeats per second", "repeat_rate", 5, 60, 25, 1,
		[](int v) { return QString("%1/s").arg(v); });

	Card *test = card(page, "Try it");
	auto *edit = new QLineEdit;
	edit->setPlaceholderText("Type here to test");
	test->add(edit);
	more_toggle(page);
	Card *osk = card(page, "On-screen keyboard",
		"For a touch screen. Super+K shows or hides it, or add the Keyboard button to a panel "
		"(Panels › On the panel). It types into the app you're in and has a page of emoji.");
	{
		auto *b = new QPushButton("Add the button to the panel");
		b->setCursor(Qt::PointingHandCursor);
		QObject::connect(b, &QPushButton::clicked, [b] {
			const QString key = "panel.main.widgets";
			QStringList w = settings->get(key, kDefaultWidgets).split(',', Qt::SkipEmptyParts);
			if (!w.contains("keyboard")) {
				const int at = w.indexOf("status");
				w.insert(at >= 0 ? at : w.size(), "keyboard");
				settings->set(key, w.join(","));
				changed(key);
			}
			b->setText("Added ✓");
			b->setEnabled(false);
		});
		make_row(osk, "Keyboard button", "Next to the status icons", b, nullptr);
		choice(osk, "Size", "The button on its bar goes round these too", "osk_layout",
			{{"Normal (in the middle)", "normal"}, {"Split in two (thumbs, wide screens)", "split"},
				{"The whole width", "wide"}}, "normal");
	}

	finish(page);
	return page.widget;
}

// ---- Displays --------------------------------------------------------------------

// What Dusk says each screen can do ($XDG_RUNTIME_DIR/dusk/outputs).
struct DisplayMode {
	int w = 0, h = 0;
	double hz = 0;
};

DisplayMode parse_mode(const QString &s) {
	DisplayMode m;
	const QStringList at = s.split('@');
	const QStringList wh = at.value(0).split('x');
	m.w = wh.value(0).toInt();
	m.h = wh.value(1).toInt();
	m.hz = at.value(1).toDouble();
	return m;
}

struct DisplayInfo {
	QString name, description, make, model;
	bool enabled = true, builtin = false, primary = false;
	DisplayMode mode, preferred;
	int x = 0, y = 0, lw = 0, lh = 0;
	double scale = 1;
	int rotation = 0;
	int phys_w = 0, phys_h = 0;   // mm
	bool vrr_supported = false, vrr = false;
	std::vector<DisplayMode> modes;

	QString pretty() const {
		if (builtin) return "Built-in display";
		if (name.startsWith("HEADLESS")) return "Phone or tablet (virtual)";
		// EDIDs often have junk makers ("@@@", "Unknown"): leave those out.
		auto junk = [](const QString &s) {
			const QString t = s.trimmed();
			return t.isEmpty() || t.count('@') == t.size() || t.compare("Unknown", Qt::CaseInsensitive) == 0 ||
				t.compare("Generic", Qt::CaseInsensitive) == 0;
		};
		QString n;
		if (!junk(make)) n = make.trimmed();
		if (!junk(model)) n = (n + " " + model.trimmed()).trimmed();
		return n.isEmpty() ? name : n;
	}
};

QString outputs_info_path() {
	const QString rt = qEnvironmentVariable("XDG_RUNTIME_DIR");
	return rt.isEmpty() ? QString() : rt + "/dusk/outputs";
}

std::vector<DisplayInfo> read_displays(QString *raw = nullptr) {
	std::vector<DisplayInfo> out;
	QFile f(outputs_info_path());
	if (outputs_info_path().isEmpty() || !f.open(QIODevice::ReadOnly)) {
		return out;
	}
	const QString text = QString::fromUtf8(f.readAll());
	if (raw) *raw = text;
	DisplayInfo *cur = nullptr;
	for (const QString &line0 : text.split('\n')) {
		const QString line = line0.trimmed();
		if (line.startsWith('[') && line.endsWith(']')) {
			out.push_back(DisplayInfo{});
			cur = &out.back();
			cur->name = line.mid(1, line.size() - 2);
			continue;
		}
		const int eq = line.indexOf('=');
		if (!cur || eq < 0) continue;
		const QString k = line.left(eq), v = line.mid(eq + 1);
		if (k == "description") cur->description = v;
		else if (k == "make") cur->make = v;
		else if (k == "model") cur->model = v;
		else if (k == "enabled") cur->enabled = v == "true";
		else if (k == "builtin") cur->builtin = v == "true";
		else if (k == "primary") cur->primary = v == "true";
		else if (k == "mode") cur->mode = parse_mode(v);
		else if (k == "preferred") cur->preferred = parse_mode(v);
		else if (k == "position") { cur->x = v.section(',', 0, 0).toInt(); cur->y = v.section(',', 1, 1).toInt(); }
		else if (k == "logical") { cur->lw = v.section('x', 0, 0).toInt(); cur->lh = v.section('x', 1, 1).toInt(); }
		else if (k == "scale") cur->scale = v.toDouble();
		else if (k == "rotation") cur->rotation = v.toInt();
		else if (k == "physical") { cur->phys_w = v.section('x', 0, 0).toInt(); cur->phys_h = v.section('x', 1, 1).toInt(); }
		else if (k == "vrr_supported") cur->vrr_supported = v == "true";
		else if (k == "vrr") cur->vrr = v == "true";
		else if (k == "modes") {
			for (const QString &m : v.split(' ', Qt::SkipEmptyParts)) cur->modes.push_back(parse_mode(m));
		}
	}
	return out;
}

QString aspect_name(int w, int h) {
	struct A { int a, b; const char *name; };
	static const A kAspects[] = {{16, 9, "16:9"}, {16, 10, "16:10"}, {4, 3, "4:3"}, {5, 4, "5:4"},
		{21, 9, "21:9"}, {32, 9, "32:9"}, {3, 2, "3:2"}};
	if (h <= 0) return QString();
	const double r = static_cast<double>(w) / h;
	for (const A &a : kAspects) {
		if (std::abs(r - static_cast<double>(a.a) / a.b) < 0.03) return a.name;
	}
	if (r > 2.2) return "21:9";
	return QString();
}

QString rate_label(double hz) {
	const double rounded = std::round(hz);
	if (std::abs(hz - rounded) < 0.05) return QString("%1 Hz").arg(static_cast<int>(rounded));
	return QString("%1 Hz").arg(hz, 0, 'f', 2);
}

// The scale that makes things about the size they should be (Dusk uses the
// same rule for a screen with none chosen). Ordinary screens: from the pixels
// per inch (laptops sit closer). Big screens (a TV, a 32"+ monitor) are looked
// at from further away: by how many pixels tall they are, so a 4K TV gets 200%.
double recommended_scale(const DisplayInfo &d) {
	if (d.mode.w <= 0) return 1.0;
	const bool portrait = d.rotation == 90 || d.rotation == 270;
	const int px = portrait ? d.mode.h : d.mode.w, py = portrait ? d.mode.w : d.mode.h;
	auto quarter = [](double v) { return std::clamp(std::round(v * 4.0) / 4.0, 1.0, 2.0); };
	const double diag = d.phys_w > 0 && d.phys_h > 0 ? std::hypot(d.phys_w, d.phys_h) / 25.4 : 0;
	const double ppi = d.phys_w > 0 ? (portrait ? d.mode.h : d.mode.w) / (d.phys_w / 25.4) : 0;
	(void)px;
	const bool size_known = ppi >= 30 && ppi <= 700;
	if (!d.builtin && (!size_known || diag >= 32)) return quarter(std::min(px, py) / 1080.0 * 0.95);
	if (!size_known) return 1.0;
	return quarter(ppi / (d.builtin ? 135.0 : 110.0));
}

// "Keep these display settings?" — goes back by itself after 15 seconds,
// so a mode the screen can't show doesn't leave you stuck.
void ask_to_keep(std::function<void()> revert) {
	QWidget *parent = QApplication::activeWindow();
	auto *dialog = new QDialog(parent);
	dialog->setWindowTitle("Keep these display settings?");
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	auto *v = new QVBoxLayout(dialog);
	v->setContentsMargins(24, 20, 24, 18);
	v->setSpacing(14);
	auto *title = new QLabel("Keep these display settings?");
	title->setObjectName("dialogtitle");
	QFont f = title->font();
	f.setPointSizeF(f.pointSizeF() * 1.2);
	f.setBold(true);
	title->setFont(f);
	auto *text = new QLabel;
	text->setObjectName("subtitle");
	v->addWidget(title);
	v->addWidget(text);
	auto *buttons = new QHBoxLayout;
	auto *back = new QPushButton("Revert");
	auto *keep = new QPushButton("Keep changes");
	keep->setObjectName("primary");
	keep->setDefault(true);
	buttons->addStretch(1);
	buttons->addWidget(back);
	buttons->addWidget(keep);
	v->addLayout(buttons);

	auto left = std::make_shared<int>(15);
	auto *timer = new QTimer(dialog);
	auto done = std::make_shared<bool>(false);
	auto go_back = [dialog, revert, done] {
		if (*done) return;
		*done = true;
		revert();
		dialog->close();
	};
	auto show_left = [text, left] {
		text->setText(QString("Going back to the previous settings in %1 second%2.")
			.arg(*left).arg(*left == 1 ? "" : "s"));
	};
	show_left();
	QObject::connect(timer, &QTimer::timeout, [left, show_left, go_back] {
		if (--*left <= 0) {
			go_back();
		} else {
			show_left();
		}
	});
	QObject::connect(back, &QPushButton::clicked, go_back);
	QObject::connect(keep, &QPushButton::clicked, [dialog, done] {
		*done = true;
		dialog->close();
	});
	// Closing the window counts as "revert", like other desktops.
	QObject::connect(dialog, &QDialog::rejected, go_back);
	timer->start(1000);
	dialog->open();
}

// Changes a display setting, then asks whether to keep it.
void set_display_key(const QString &key, const QString &value, bool confirm = true) {
	const bool had = settings->has(key);
	const QString old = settings->get(key);
	if (had && old == value) return;
	if (value.isEmpty()) settings->remove(key);
	else settings->set(key, value);
	settings->save();
	changed(key);
	if (confirm) {
		ask_to_keep([key, had, old] {
			if (had) settings->set(key, old);
			else settings->remove(key);
			settings->save();
			changed(key);
		});
	}
}

// The screens as boxes you drag around. Let go and the box snaps against
// its neighbours, so screens always touch and never overlap.
// The screens as they sit on the desk. Each is drawn at its real size (from
// the millimetres the screen reports), so a 24" monitor looks bigger than a
// 14" laptop; where they touch and how they line up is the layout in pixels
// (what the pointer and windows follow), worked out from the drawing when
// one is dragged.
class Arrangement : public QWidget {
public:
	struct Box {
		QString name, label;
		QRectF r;          // the layout: logical pixels
		bool primary;
		QSizeF mm;         // its real size (empty if the screen doesn't say)
		QRectF v;          // as drawn: millimetres
	};
	std::vector<Box> boxes;
	std::function<void(const std::vector<Box> &)> moved;

	Arrangement() {
		setMinimumHeight(230);
		setMouseTracking(true);
	}

	// From the layout to the drawing: the first (main) screen anchors it, and
	// each other one is put against a screen it touches, lined up the same
	// way (tops, bottoms, middles or in between).
	void lay_out() {
		if (boxes.empty()) return;
		// Millimetres per pixel: the screen's own, or the others' when it
		// doesn't say (a typical 96 dpi otherwise).
		std::vector<double> known;
		for (Box &b : boxes) {
			if (b.mm.width() > 0 && b.r.width() > 0) {
				// Turned screens: the long side goes with the long side.
				if ((b.mm.width() > b.mm.height()) != (b.r.width() > b.r.height())) b.mm.transpose();
				known.push_back(b.mm.width() / b.r.width());
			}
		}
		const double fallback = known.empty() ? 25.4 / 96 : *std::max_element(known.begin(), known.end());
		std::vector<double> k(boxes.size());
		for (size_t i = 0; i < boxes.size(); i++) {
			const Box &b = boxes[i];
			k[i] = b.mm.width() > 0 ? b.mm.width() / b.r.width() : fallback;
			boxes[i].v.setSize(b.r.size() * k[i]);
		}
		size_t first = 0;
		for (size_t i = 0; i < boxes.size(); i++) if (boxes[i].primary) first = i;
		std::vector<bool> placed(boxes.size(), false);
		boxes[first].v.moveTopLeft(boxes[first].r.topLeft() * k[first]);
		placed[first] = true;
		for (size_t round = 0; round < boxes.size(); round++) {
			bool any = false;
			for (size_t j = 0; j < boxes.size(); j++) {
				if (placed[j]) continue;
				for (size_t i = 0; i < boxes.size() && !placed[j]; i++) {
					if (!placed[i]) continue;
					if (place_against(boxes[j], boxes[i])) {
						placed[j] = true;
						any = true;
					}
				}
			}
			if (!any) break;
		}
		// Not touching anything: where it is, at the same scale as the main one.
		for (size_t j = 0; j < boxes.size(); j++) {
			if (!placed[j]) boxes[j].v.moveTopLeft(boxes[j].r.topLeft() * k[first]);
		}
	}

protected:
	// `d` against `o` in the drawing, as it is in the layout. False if they
	// don't touch.
	static bool place_against(Box &d, const Box &o) {
		const QRectF &r = d.r, &q = o.r;
		const double e = 1.0;
		const bool vert_overlap = r.top() < q.bottom() - e && r.bottom() > q.top() + e;
		const bool horiz_overlap = r.left() < q.right() - e && r.right() > q.left() + e;
		auto along = [](double a0, double a_len, double b0, double b_len, double va_len, double vb0, double vb_len) {
			// Where `a` sits along `b`'s edge: same alignment in the drawing.
			if (std::abs(a0 - b0) < 1) return vb0;                                        // starts lined up
			if (std::abs(a0 + a_len - (b0 + b_len)) < 1) return vb0 + vb_len - va_len;    // ends lined up
			if (std::abs(a0 + a_len / 2 - (b0 + b_len / 2)) < 1) return vb0 + (vb_len - va_len) / 2;
			const double t = (a0 + a_len / 2 - b0) / b_len;                              // its middle, in between
			return vb0 + t * vb_len - va_len / 2;
		};
		if (vert_overlap && std::abs(r.left() - q.right()) < e) {
			d.v.moveLeft(o.v.right());
		} else if (vert_overlap && std::abs(r.right() - q.left()) < e) {
			d.v.moveRight(o.v.left());
		} else if (horiz_overlap && std::abs(r.top() - q.bottom()) < e) {
			d.v.moveTop(o.v.bottom());
		} else if (horiz_overlap && std::abs(r.bottom() - q.top()) < e) {
			d.v.moveBottom(o.v.top());
		} else {
			return false;
		}
		if (vert_overlap) d.v.moveTop(along(r.top(), r.height(), q.top(), q.height(), d.v.height(), o.v.top(), o.v.height()));
		else d.v.moveLeft(along(r.left(), r.width(), q.left(), q.width(), d.v.width(), o.v.left(), o.v.width()));
		return true;
	}

	// Back from the drawing to the layout for the one dragged (`i`), against
	// the screen it was put next to.
	void to_layout(int i, int against, int side) {
		Box &d = boxes[i];
		const Box &o = boxes[against];
		QRectF r = d.r;
		auto along = [](double a0, double a_len, double b0, double b_len, double pa_len, double pb0, double pb_len) {
			// The same alignment in pixels as in the drawing.
			if (std::abs(a0 - b0) < 0.5) return pb0;
			if (std::abs(a0 + a_len - (b0 + b_len)) < 0.5) return pb0 + pb_len - pa_len;
			if (std::abs(a0 + a_len / 2 - (b0 + b_len / 2)) < 0.5) return pb0 + (pb_len - pa_len) / 2;
			const double t = (a0 + a_len / 2 - b0) / b_len;
			return pb0 + t * pb_len - pa_len / 2;
		};
		if (side == 0 || side == 1) {   // right of it, left of it
			if (side == 0) r.moveLeft(o.r.right());
			else r.moveRight(o.r.left());
			double y = along(d.v.top(), d.v.height(), o.v.top(), o.v.height(), r.height(), o.r.top(), o.r.height());
			y = std::clamp(y, o.r.top() - r.height() + 1, o.r.bottom() - 1);
			r.moveTop(y);
		} else {                        // below it, above it
			if (side == 2) r.moveTop(o.r.bottom());
			else r.moveBottom(o.r.top());
			double x = along(d.v.left(), d.v.width(), o.v.left(), o.v.width(), r.width(), o.r.left(), o.r.width());
			x = std::clamp(x, o.r.left() - r.width() + 1, o.r.right() - 1);
			r.moveLeft(x);
		}
		d.r = r;
	}

	void fit() {
		QRectF all;
		for (const Box &b : boxes) all = all.isNull() ? b.v : all.united(b.v);
		if (all.isNull()) return;
		const double pad = 28;
		k_ = std::min((width() - 2 * pad) / all.width(), (height() - 2 * pad) / all.height());
		k_ = std::min(k_, 0.5);
		off_ = QPointF((width() - all.width() * k_) / 2 - all.left() * k_,
			(height() - all.height() * k_) / 2 - all.top() * k_);
	}
	QRectF to_widget(const QRectF &r) const {
		return QRectF(off_ + r.topLeft() * k_, r.size() * k_);
	}
	void paintEvent(QPaintEvent *) override {
		if (drag_ < 0) fit();
		const Look l = current_look();
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		QColor bg = l.text;
		bg.setAlphaF(0.05);
		p.setPen(Qt::NoPen);
		p.setBrush(bg);
		p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 14, 14);
		for (int i = 0; i < static_cast<int>(boxes.size()); i++) {
			const Box &b = boxes[i];
			const QRectF w = to_widget(b.v).adjusted(2, 2, -2, -2);
			const bool active = i == drag_ || i == hover_;
			p.setBrush(l.card);
			p.setPen(QPen(b.primary || active ? l.accent : l.dim, active ? 2.5 : 1.5));
			p.drawRoundedRect(w, 8, 8);
			p.setPen(l.text);
			QFont f = font();
			f.setBold(true);
			p.setFont(f);
			p.drawText(w.adjusted(6, 0, -6, -w.height() / 2 + 2), Qt::AlignHCenter | Qt::AlignBottom,
				b.label);
			f.setBold(false);
			f.setPointSizeF(f.pointSizeF() * 0.85);
			p.setFont(f);
			p.setPen(l.dim);
			QString sub = b.name;
			if (b.mm.width() > 0) {
				sub += QString("  ·  %1\"").arg(std::hypot(b.mm.width(), b.mm.height()) / 25.4, 0, 'f', 1);
			}
			if (b.primary) sub += "  ·  main";
			p.drawText(w.adjusted(6, w.height() / 2 + 2, -6, 0), Qt::AlignHCenter | Qt::AlignTop, sub);
		}
	}
	int box_at(QPointF pos) const {
		for (int i = static_cast<int>(boxes.size()) - 1; i >= 0; i--) {
			if (to_widget(boxes[i].v).contains(pos)) return i;
		}
		return -1;
	}
	void mousePressEvent(QMouseEvent *e) override {
		drag_ = box_at(e->position());
		if (drag_ >= 0) {
			grab_ = e->position() - to_widget(boxes[drag_].v).topLeft();
			start_ = boxes[drag_].v.topLeft();
			setCursor(Qt::ClosedHandCursor);
		}
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (drag_ >= 0) {
			boxes[drag_].v.moveTopLeft((e->position() - grab_ - off_) / k_);
			update();
			return;
		}
		const int h = box_at(e->position());
		if (h != hover_) {
			hover_ = h;
			setCursor(h >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
			update();
		}
	}
	void mouseReleaseEvent(QMouseEvent *) override {
		if (drag_ < 0) return;
		const int i = drag_;
		drag_ = -1;
		setCursor(Qt::OpenHandCursor);
		if (boxes[i].v.topLeft() == start_) {
			update();
			return;
		}
		int against = -1, side = 0;
		snap(i, &against, &side);
		if (against >= 0) to_layout(i, against, side);
		// Top-left screen at 0,0, whole pixels.
		double minx = 1e9, miny = 1e9;
		for (const Box &b : boxes) {
			minx = std::min(minx, b.r.left());
			miny = std::min(miny, b.r.top());
		}
		for (Box &b : boxes) {
			b.r.translate(-minx, -miny);
			b.r.moveTopLeft(QPointF(std::round(b.r.left()), std::round(b.r.top())));
		}
		lay_out();
		update();
		if (moved) moved(boxes);
	}
	void leaveEvent(QEvent *) override {
		hover_ = -1;
		update();
	}
	void resizeEvent(QResizeEvent *) override { update(); }

private:
	// In the drawing: the dragged screen against the nearest edge of another,
	// lined up with its top, bottom or middle when close to it.
	void snap(int i, int *against, int *side) {
		Box &d = boxes[i];
		const QRectF r = d.v;
		auto overlaps = [&](const QRectF &c) {
			for (int j = 0; j < static_cast<int>(boxes.size()); j++) {
				if (j == i) continue;
				const QRectF x = boxes[j].v.intersected(c);
				if (x.width() > 0.5 && x.height() > 0.5) return true;
			}
			return false;
		};
		struct Cand { QPointF at; int j, side; };
		std::vector<Cand> cands;
		for (int j = 0; j < static_cast<int>(boxes.size()); j++) {
			if (j == i) continue;
			const QRectF o = boxes[j].v;
			const double ty = std::max(12.0, r.height() * 0.06), tx = std::max(12.0, r.width() * 0.06);
			auto fix_y = [&](double y) {
				y = std::clamp(y, o.top() - r.height() + 1, o.bottom() - 1);
				if (std::abs(y - o.top()) < ty) y = o.top();
				else if (std::abs(y + r.height() - o.bottom()) < ty) y = o.bottom() - r.height();
				else if (std::abs(y + r.height() / 2 - o.center().y()) < ty) y = o.center().y() - r.height() / 2;
				return y;
			};
			auto fix_x = [&](double x) {
				x = std::clamp(x, o.left() - r.width() + 1, o.right() - 1);
				if (std::abs(x - o.left()) < tx) x = o.left();
				else if (std::abs(x + r.width() - o.right()) < tx) x = o.right() - r.width();
				else if (std::abs(x + r.width() / 2 - o.center().x()) < tx) x = o.center().x() - r.width() / 2;
				return x;
			};
			cands.push_back({{o.right(), fix_y(r.top())}, j, 0});
			cands.push_back({{o.left() - r.width(), fix_y(r.top())}, j, 1});
			cands.push_back({{fix_x(r.left()), o.bottom()}, j, 2});
			cands.push_back({{fix_x(r.left()), o.top() - r.height()}, j, 3});
		}
		QPointF best = start_;
		double best_d = 1e18;
		*against = -1;
		for (const Cand &c : cands) {
			if (overlaps(QRectF(c.at, r.size()))) continue;
			const double dd = std::hypot(c.at.x() - r.left(), c.at.y() - r.top());
			if (dd < best_d) {
				best_d = dd;
				best = c.at;
				*against = c.j;
				*side = c.side;
			}
		}
		d.v.moveTopLeft(best);
	}

	double k_ = 0.1;
	QPointF off_;
	int drag_ = -1, hover_ = -1;
	QPointF grab_, start_;
};

// A combo row for the displays page (not tied to one key: picks write
// through set_display_key).
QComboBox *display_combo(Card *c, const QString &title, const QString &subtitle,
		const QString &key, QToolButton **reset_out = nullptr) {
	auto *combo = new ElideCombo;
	flex_width(combo, 240);
	QToolButton *reset = nullptr;
	QWidget *row = make_row(c, title, subtitle, combo, &reset);
	QObject::connect(reset, &QToolButton::clicked, [key] { set_display_key(key, QString()); });
	bindings.push_back(Binding{row, reset, key, QString(), {}, {}, [key] { return !settings->has(key); }});
	if (reset_out) *reset_out = reset;
	return combo;
}

class DisplaysPage {
public:
	QWidget *widget;

	DisplaysPage() {
		Page page = make_page("Displays",
			"Resolution, refresh rate, size and where your screens are.");
		widget = page.widget;
		outer_ = page.layout;
		rebuild();
		finish(page);
		watcher_ = new QFileSystemWatcher(widget);
		const QString path = outputs_info_path();
		if (!path.isEmpty()) {
			QDir().mkpath(QFileInfo(path).absolutePath());
			watcher_->addPath(QFileInfo(path).absolutePath());
		}
		debounce_ = new QTimer(widget);
		debounce_->setSingleShot(true);
		debounce_->setInterval(250);
		QObject::connect(debounce_, &QTimer::timeout, [this] {
			QString raw;
			read_displays(&raw);
			if (raw != raw_) rebuild();
		});
		QObject::connect(watcher_, &QFileSystemWatcher::directoryChanged,
			[this](const QString &) { debounce_->start(); });
	}

private:
	void clear() {
		for (Card *c : cards_) {
			all_cards.erase(std::remove(all_cards.begin(), all_cards.end(), c), all_cards.end());
			delete c;
		}
		cards_.clear();
		if (body_) {
			delete body_;
			body_ = nullptr;
		}
	}
	Card *add_card(const QString &title, const QString &description = QString()) {
		auto *c = new Card(body_layout_, title, description);
		all_cards.push_back(c);
		cards_.push_back(c);
		return c;
	}

	void rebuild() {
		clear();
		body_ = new QWidget;
		body_layout_ = new QVBoxLayout(body_);
		body_layout_->setContentsMargins(0, 0, 0, 0);
		body_layout_->setSpacing(10);
		outer_->insertWidget(outer_->count() > 2 ? 2 : outer_->count(), body_);

		displays_ = read_displays(&raw_);
		if (displays_.empty()) {
			build_fallback();
		} else {
			build();
		}
		update_rows();
	}

	// Not running inside Dusk: only the scale can be set (it's used next
	// time Dusk starts).
	void build_fallback() {
		for (QScreen *s : QGuiApplication::screens()) {
			const QString name = s->name();
			const QSize px = s->size() * s->devicePixelRatio();
			Card *c = add_card(name + (s->model().isEmpty() ? "" : "  ·  " + s->model()),
				QString("%1 × %2 at %3 Hz. Log in to Dusk to change the resolution, refresh rate "
					"and arrangement.").arg(px.width()).arg(px.height()).arg(std::lround(s->refreshRate())));
			if (!name.isEmpty()) {
				choice(c, "Scale", QString(), "scale." + name,
					{{"100%", "1.00"}, {"125%", "1.25"}, {"150%", "1.50"}, {"175%", "1.75"},
						{"200%", "2.00"}, {"250%", "2.50"}, {"300%", "3.00"}}, "1.00");
			}
		}
	}

	void build() {
		int on = 0;
		for (const DisplayInfo &d : displays_) on += d.enabled;

		if (displays_.size() > 1) {
			Card *arr = add_card("Arrangement",
				"Drag the screens to match where they are on your desk. The pointer and "
				"windows move between screens along the edges where they touch.");
			auto *a = new Arrangement;
			for (const DisplayInfo &d : displays_) {
				if (!d.enabled) continue;
				a->boxes.push_back({d.name, d.pretty(),
					QRectF(d.x, d.y, std::max(1, d.lw), std::max(1, d.lh)), d.primary,
					d.phys_w > 0 && d.phys_h > 0 ? QSizeF(d.phys_w, d.phys_h) : QSizeF(), QRectF()});
			}
			a->lay_out();
			a->moved = [](const std::vector<Arrangement::Box> &boxes) {
				for (const auto &b : boxes) {
					settings->set("output." + b.name + ".position",
						QString("%1,%2").arg(static_cast<int>(b.r.left())).arg(static_cast<int>(b.r.top())));
				}
				settings->save();
				changed("output");
			};
			arr->add(a);
			restyle_hooks.push_back([p = QPointer<Arrangement>(a)] { if (p) p->update(); });

			Items mains = {{"Automatic", ""}};
			for (const DisplayInfo &d : displays_) mains.push_back({d.pretty() + "  (" + d.name + ")", d.name});
			choice(arr, "Main display", "Gets the panels set to \"main screen\" and new windows",
				"primary_output", mains, "");
			choice(arr, "Pause between screens",
				"The pointer stops briefly at the edge, so you can drop a window there to snap it",
				"edge_resistance", {{"Light", "light"}, {"Strong", "strong"}, {"Off", "off"}}, "light");
		}

		for (const DisplayInfo &d : displays_) {
			build_display(d, on);
		}
	}

	void build_display(const DisplayInfo &d, int on) {
		const QString pre = "output." + d.name + ".";
		QString sub = d.name;
		if (d.phys_w > 0 && d.phys_h > 0) {
			const double inches = std::hypot(d.phys_w, d.phys_h) / 25.4;
			sub += QString("  ·  %1\"").arg(inches, 0, 'f', 1);
		}
		Card *c = add_card(d.pretty() + "   " + sub);

		if (displays_.size() > 1) {
			auto *sw = new Switch;
			QToolButton *reset = nullptr;
			QWidget *row = make_row(c, "Use this display",
				d.enabled && on <= 1 ? "It's the only one on, so it stays on" : QString(), sw, &reset);
			sw->set_quietly(d.enabled);
			sw->setEnabled(!(d.enabled && on <= 1));
			const QString key = pre + "enabled";
			QObject::connect(sw, &QAbstractButton::toggled, [key](bool v) {
				set_display_key(key, v ? QString() : QString("false"));
			});
			QObject::connect(reset, &QToolButton::clicked, [key] { set_display_key(key, QString()); });
			bindings.push_back(Binding{row, reset, key, "true", {}, {}, [key] { return !settings->has(key); }});
		}
		if (!d.enabled) {
			return;
		}

		// Resolution: each size once, biggest first; the preferred one is marked.
		std::vector<std::pair<int, int>> sizes;
		for (const DisplayMode &m : d.modes) {
			if (std::find(sizes.begin(), sizes.end(), std::make_pair(m.w, m.h)) == sizes.end()) {
				sizes.push_back({m.w, m.h});
			}
		}
		std::sort(sizes.begin(), sizes.end(), [](auto a, auto b) {
			return a.first * a.second > b.first * b.second;
		});
		const QString mode_key = pre + "mode";
		auto best_rate = [&d](int w, int h, double want) {
			double best = 0;
			for (const DisplayMode &m : d.modes) {
				if (m.w != w || m.h != h) continue;
				if (best == 0 || (want > 0 ? std::abs(m.hz - want) < std::abs(best - want) : m.hz > best)) best = m.hz;
			}
			return best;
		};
		if (!sizes.empty()) {
			QComboBox *res = display_combo(c, "Resolution", QString(), mode_key);
			for (const auto &[w, h] : sizes) {
				QString label = QString("%1 × %2").arg(w).arg(h);
				const QString asp = aspect_name(w, h);
				if (!asp.isEmpty() && !(w == d.preferred.w && h == d.preferred.h)) label += "  " + asp;
				if (w == d.preferred.w && h == d.preferred.h) label += " (recommended)";
				res->addItem(label, QString("%1x%2").arg(w).arg(h));
			}
			res->setCurrentIndex(std::max(0, res->findData(QString("%1x%2").arg(d.mode.w).arg(d.mode.h))));
			QObject::connect(res, &QComboBox::currentIndexChanged, [res, mode_key, best_rate, d](int) {
				const QStringList wh = res->currentData().toString().split('x');
				const int w = wh.value(0).toInt(), h = wh.value(1).toInt();
				const double hz = best_rate(w, h, d.mode.hz);
				set_display_key(mode_key, QString("%1x%2@%3").arg(w).arg(h).arg(hz, 0, 'f', 3));
			});

			// Refresh rates for the current size.
			std::vector<double> rates;
			for (const DisplayMode &m : d.modes) {
				if (m.w == d.mode.w && m.h == d.mode.h &&
						std::none_of(rates.begin(), rates.end(), [&](double r) { return std::abs(r - m.hz) < 0.01; })) {
					rates.push_back(m.hz);
				}
			}
			std::sort(rates.rbegin(), rates.rend());
			if (rates.size() > 1) {
				auto *rate = new ElideCombo;
				flex_width(rate, 240);
				for (double r : rates) {
					QString label = rate_label(r);
					if (r == rates.front()) label += " (smoothest)";
					rate->addItem(label, r);
				}
				int cur = 0;
				for (int i = 0; i < rate->count(); i++) {
					if (std::abs(rate->itemData(i).toDouble() - d.mode.hz) < 0.01) cur = i;
				}
				rate->setCurrentIndex(cur);
				make_row(c, "Refresh rate", "Higher is smoother; lower saves battery", rate, nullptr);
				QObject::connect(rate, &QComboBox::currentIndexChanged, [rate, mode_key, d](int) {
					set_display_key(mode_key, QString("%1x%2@%3").arg(d.mode.w).arg(d.mode.h)
						.arg(rate->currentData().toDouble(), 0, 'f', 3));
				});
			}
		}

		// Scale, with the one that fits this screen marked.
		{
			const double rec = recommended_scale(d);
			const QString key = "scale." + d.name;
			QComboBox *sc = display_combo(c, "Scale", "Makes text, windows and panels bigger", key);
			for (double s = 1.0; s <= 2.001; s += 0.25) {   // 200% at most: Dusk's own things are made for up to that
				QString label = QString("%1%").arg(static_cast<int>(std::lround(s * 100)));
				if (std::abs(s - rec) < 0.01) label += " (recommended)";
				sc->addItem(label, QString::number(s, 'f', 2));
			}
			int cur = 0;
			for (int i = 0; i < sc->count(); i++) {
				if (std::abs(sc->itemData(i).toString().toDouble() - d.scale) < 0.01) cur = i;
			}
			sc->setCurrentIndex(cur);
			QObject::connect(sc, &QComboBox::currentIndexChanged, [sc, key](int) {
				// (Always written: with none, Dusk picks the one that suits the screen.)
				set_display_key(key, sc->currentData().toString());
			});
		}

		// Rotation.
		{
			const QString key = pre + "rotation";
			QComboBox *rot = display_combo(c, "Rotation", QString(), key);
			rot->addItem("Normal (landscape)", 0);
			rot->addItem("Portrait (turned left)", 90);
			rot->addItem("Upside down", 180);
			rot->addItem("Portrait (turned right)", 270);
			rot->setCurrentIndex(std::max(0, rot->findData(d.rotation)));
			QObject::connect(rot, &QComboBox::currentIndexChanged, [rot, key](int) {
				const int v = rot->currentData().toInt();
				set_display_key(key, v == 0 ? QString() : QString::number(v));
			});
		}

		if (d.vrr_supported) {
			auto *sw = new Switch;
			QToolButton *reset = nullptr;
			const QString key = pre + "vrr";
			QWidget *row = make_row(c, "Variable refresh rate",
				"The screen keeps pace with games and videos (FreeSync / Adaptive-Sync)", sw, &reset);
			sw->set_quietly(d.vrr || settings->get_bool(key, false));
			QObject::connect(sw, &QAbstractButton::toggled, [key](bool v) {
				set_display_key(key, v ? QString("true") : QString(), false);
			});
			QObject::connect(reset, &QToolButton::clicked, [key] { set_display_key(key, QString(), false); });
			bindings.push_back(Binding{row, reset, key, "false", {}, {}, [key] { return !settings->has(key); }});
		}
	}

	QVBoxLayout *outer_ = nullptr;
	QWidget *body_ = nullptr;
	QVBoxLayout *body_layout_ = nullptr;
	std::vector<Card *> cards_;
	std::vector<DisplayInfo> displays_;
	QString raw_;
	QFileSystemWatcher *watcher_ = nullptr;
	QTimer *debounce_ = nullptr;
};

QWidget *displays_page() {
	static DisplaysPage *page = nullptr;
	page = new DisplaysPage;
	return page->widget;
}

// ---- Default apps -----------------------------------------------------------------

// ---- Phone & tablet --------------------------------------------------------------

QString local_address() {
	const QString out = run_tool("sh", {"-c", "ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \\([0-9.]*\\).*/\\1/p'"});
	return out.isEmpty() ? QString("this computer's address") : out;
}

QString new_password() {
	static const char kChars[] = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
	QString p;
	for (int i = 0; i < 10; i++) {
		p += QChar(kChars[QRandomGenerator::global()->bounded(static_cast<int>(sizeof kChars) - 1)]);
	}
	return p;
}

[[maybe_unused]] QWidget *phone_page() {
	Page page = make_page("Phone & tablet", "See and use your Android phone here, or use a phone or tablet as "
		"another screen.");
	auto on = [] { return settings->get_bool("phone_screen", false); };
	auto wifi = [on] { return on() && settings->get("phone_link", "wifi") == "wifi"; };
	auto usb = [on] { return on() && settings->get("phone_link", "wifi") == "usb"; };

	// Your phone on this computer, as a few plain steps: install (once), then
	// your phones with a button each, then how to add one (by cable or Wi-Fi).
	Card *mirror = card(page, "Your phone on this computer", "See your Android phone's screen in a window and "
		"use it with the mouse and keyboard.");
	{
		auto *box = new QWidget;
		auto *v = new QVBoxLayout(box);
		v->setContentsMargins(0, 8, 0, 8);
		v->setSpacing(10);
		mirror->add(box);

		auto heading = [](const QString &text) {
			auto *l = new QLabel(text);
			QFont f = l->font();
			f.setBold(true);
			l->setFont(f);
			return l;
		};
		auto note = [](const QString &text) {
			auto *l = new QLabel(text);
			l->setObjectName("subtitle");
			l->setWordWrap(true);
			return l;
		};
		// Runs `program args`, telling `done` how it went, also when it can't
		// start at all (not installed) or takes too long.
		auto run = [](QObject *owner, const QString &program, const QStringList &args, int timeout_ms,
				std::function<void(bool ok, const QString &out)> done) {
			auto *pr = new QProcess(owner);
			pr->setProcessChannelMode(QProcess::MergedChannels);
			auto finished = std::make_shared<bool>(false);
			auto finish = [pr, done, finished](bool ok) {
				if (*finished) return;
				*finished = true;
				done(ok, QString::fromUtf8(pr->readAll()));
				pr->deleteLater();
			};
			QObject::connect(pr, &QProcess::finished, [pr, finish](int code, QProcess::ExitStatus st) {
				finish(st == QProcess::NormalExit && code == 0);
				(void)pr;
			});
			QObject::connect(pr, &QProcess::errorOccurred, [finish](QProcess::ProcessError e) {
				if (e == QProcess::FailedToStart) finish(false);
			});
			QTimer::singleShot(timeout_ms, pr, [pr, finish] {
				pr->kill();
				finish(false);
			});
			pr->start(program, args);
		};

		// 1. Install (only while something is missing).
		auto *install = new QWidget;
		{
			auto *h = new QHBoxLayout(install);
			h->setContentsMargins(0, 0, 0, 0);
			auto *texts = new QVBoxLayout;
			texts->addWidget(heading("First, two small programs"));
			texts->addWidget(note("scrcpy shows the phone's screen; adb talks to the phone. Asks for your password once."));
			h->addLayout(texts, 1);
			auto *b = new QPushButton("Install");
			b->setObjectName("primary");
			b->setCursor(Qt::PointingHandCursor);
			h->addWidget(b, 0, Qt::AlignVCenter);
			QObject::connect(b, &QPushButton::clicked, [b, run] {
				b->setEnabled(false);
				b->setText("Installing…");
				run(b, "pkexec", {"sh", "-c", "dnf install -y android-tools; "
					"command -v scrcpy >/dev/null || { dnf copr enable -y zeno/scrcpy && dnf install -y scrcpy; }"},
					10 * 60 * 1000, [b](bool ok, const QString &) {
						b->setText(ok ? "Installed ✓" : "Didn't work: try again");
						b->setEnabled(!ok);
					});
			});
		}
		v->addWidget(install);

		// 2. Your phones (found by themselves).
		auto *phones_box = new QWidget;
		auto *pv = new QVBoxLayout(phones_box);
		pv->setContentsMargins(0, 0, 0, 0);
		pv->setSpacing(6);
		pv->addWidget(heading("Your phones"));
		auto *list = new QWidget;
		auto *lv = new QVBoxLayout(list);
		lv->setContentsMargins(0, 0, 0, 0);
		lv->setSpacing(6);
		pv->addWidget(list);
		auto *none = note("None connected. Add one below: it shows up here by itself.");
		pv->addWidget(none);
		v->addWidget(phones_box);

		// 3. Add a phone: by cable or over Wi-Fi.
		auto *add = new QWidget;
		auto *av = new QVBoxLayout(add);
		av->setContentsMargins(0, 6, 0, 0);
		av->setSpacing(8);
		av->addWidget(heading("Add a phone"));
		auto *tabs = new QHBoxLayout;
		tabs->setSpacing(6);
		auto *by_cable = new QPushButton("With a USB cable");
		auto *by_wifi = new QPushButton("Over Wi-Fi, no cable");
		for (QPushButton *t : {by_cable, by_wifi}) {
			t->setCheckable(true);
			t->setCursor(Qt::PointingHandCursor);
			tabs->addWidget(t);
		}
		tabs->addStretch(1);
		by_cable->setChecked(true);
		av->addLayout(tabs);
		auto *pages = new QStackedWidget;
		av->addWidget(pages);
		auto steps = [note](const std::vector<QString> &lines) {
			auto *w = new QWidget;
			auto *l = new QVBoxLayout(w);
			l->setContentsMargins(0, 0, 0, 0);
			l->setSpacing(6);
			for (size_t i = 0; i < lines.size(); i++) {
				auto *row = new QLabel(QString("<b>%1.</b>&nbsp;&nbsp;%2").arg(i + 1).arg(lines[i]));
				row->setWordWrap(true);
				l->addWidget(row);
			}
			return std::make_pair(w, l);
		};
		{
			auto [w, l] = steps({
				"On the phone, turn on <b>Developer options</b>: Settings › About phone › Software information › "
				"tap <b>Build number</b> 7 times.",
				"Then Settings › Developer options › turn on <b>USB debugging</b>.",
				"Plug the phone in. On the phone, tap <b>Allow</b> (tick “Always allow from this computer”).",
			});
			l->addWidget(note("It then shows up under Your phones. Afterwards, “Use without cable” lets you unplug it."));
			pages->addWidget(w);
		}
		{
			auto [w, l] = steps({
				"Turn on <b>Developer options</b> on the phone (Settings › About phone › Software information › "
				"tap Build number 7 times).",
				"Settings › Developer options › turn on <b>Wireless debugging</b>, then tap it and choose "
				"<b>Pair device with pairing code</b>.",
				"Type what the phone shows here (the phone and this computer on the same Wi-Fi):",
			});
			auto *form = new QWidget;
			auto *g = new QGridLayout(form);
			g->setContentsMargins(0, 0, 0, 0);
			g->setHorizontalSpacing(8);
			g->setVerticalSpacing(4);
			auto *addr = new QLineEdit;
			addr->setPlaceholderText("e.g. 192.168.0.11:46149");
			auto *code = new QLineEdit;
			code->setPlaceholderText("e.g. 940468");
			code->setMaximumWidth(160);
			auto *go = new QPushButton("Pair");
			go->setObjectName("primary");
			go->setCursor(Qt::PointingHandCursor);
			g->addWidget(note("IP address & Port"), 0, 0);
			g->addWidget(note("Wi-Fi pairing code"), 0, 1);
			g->addWidget(addr, 1, 0);
			g->addWidget(code, 1, 1);
			g->addWidget(go, 1, 2);
			g->setColumnStretch(0, 1);
			l->addWidget(form);
			auto *result = note(QString());
			l->addWidget(result);
			QObject::connect(go, &QPushButton::clicked, [addr, code, go, result, run] {
				const QString a = addr->text().trimmed(), c = code->text().trimmed();
				if (!a.contains(':') || c.isEmpty()) {
					result->setText("Type both: the address with the port after the “:”, and the 6-digit code.");
					return;
				}
				if (!have_program("adb")) {
					result->setText("Install the programs first (the Install button above).");
					return;
				}
				go->setEnabled(false);
				result->setText("Pairing…");
				run(go, "adb", {"pair", a, c}, 20000, [go, result, a](bool ok, const QString &out) {
					ok = ok && out.contains("Successfully paired", Qt::CaseInsensitive);
					result->setText(ok ? "Paired ✓  It shows up under Your phones in a moment."
						: "Didn't pair. The code changes each time the phone shows it: check it's the one on "
						  "screen now, and that both are on the same Wi-Fi.");
					if (ok) {
						settings->set("phone_address", a.section(':', 0, 0));
						settings->save();
					}
					go->setEnabled(true);
				});
			});
			pages->addWidget(w);
		}
		QObject::connect(by_cable, &QPushButton::clicked, [by_cable, by_wifi, pages] {
			by_cable->setChecked(true);
			by_wifi->setChecked(false);
			pages->setCurrentIndex(0);
		});
		QObject::connect(by_wifi, &QPushButton::clicked, [by_cable, by_wifi, pages] {
			by_cable->setChecked(false);
			by_wifi->setChecked(true);
			pages->setCurrentIndex(1);
		});
		v->addWidget(add);

		// Keeping it up to date: what's installed, and the phones adb sees.
		struct Phone { QString serial, name, state; bool wifi; };
		auto shown = std::make_shared<QString>();
		auto rebuild = [lv, none, shown, run](const std::vector<Phone> &phones) {
			QString key;
			for (const Phone &p : phones) key += p.serial + p.state + ";";
			if (key == *shown) return;
			*shown = key;
			while (QLayoutItem *it = lv->takeAt(0)) {
				if (it->widget()) it->widget()->deleteLater();
				delete it;
			}
			none->setVisible(phones.empty());
			for (const Phone &p : phones) {
				auto *row = new QFrame;
				row->setObjectName("phonerow");
				auto *h = new QHBoxLayout(row);
				h->setContentsMargins(12, 8, 8, 8);
				h->setSpacing(10);
				auto *texts = new QVBoxLayout;
				texts->setSpacing(0);
				texts->addWidget(new QLabel(p.name));
				const QString how = p.wifi ? "Over Wi-Fi" : "By USB cable";
				auto *sub = new QLabel(p.state == "device" ? how
					: p.state == "unauthorized" ? "Waiting: on the phone, tap Allow"
					: how + " (" + p.state + ")");
				sub->setObjectName("subtitle");
				texts->addWidget(sub);
				h->addLayout(texts, 1);
				if (p.state == "device") {
					if (!p.wifi) {
						auto *cable = new QPushButton("Use without cable");
						cable->setCursor(Qt::PointingHandCursor);
						cable->setToolTip("Switches it to Wi-Fi: unplug it afterwards and it stays connected");
						const QString serial = p.serial;
						QObject::connect(cable, &QPushButton::clicked, [cable, serial, run] {
							cable->setEnabled(false);
							cable->setText("Switching…");
							const QString script =
								"ip=$(adb -s \"$0\" shell ip -f inet addr show wlan0 | sed -n 's/.*inet \\([0-9.]*\\).*/\\1/p' | head -1); "
								"[ -n \"$ip\" ] || { echo NOWIFI; exit 2; }; adb -s \"$0\" tcpip 5555 >/dev/null && sleep 2 && "
								"adb connect \"$ip:5555\" >/dev/null && echo \"IP=$ip\"";
							run(cable, "sh", {"-c", script, serial}, 20000, [cable](bool ok, const QString &out) {
								const QString ip = out.section("IP=", 1).trimmed();
								if (ok && !ip.isEmpty()) {
									settings->set("phone_address", ip);
									settings->save();
									cable->setText("Done: unplug it");
								} else {
									cable->setText(out.contains("NOWIFI") ? "Turn on its Wi-Fi first" : "Didn't work: try again");
									cable->setEnabled(true);
								}
							});
						});
						h->addWidget(cable);
					}
					auto *show = new QPushButton("Show screen");
					show->setObjectName("primary");
					show->setCursor(Qt::PointingHandCursor);
					const QString serial = p.serial, name = p.name;
					QObject::connect(show, &QPushButton::clicked, [serial, name] {
						QProcess::startDetached("scrcpy", {"--serial=" + serial, "--window-title=" + name,
							"--stay-awake", "--turn-screen-off"});
					});
					h->addWidget(show);
				}
				lv->addWidget(row);
			}
		};
		auto *timer = new QTimer(box);
		timer->setInterval(2000);
		auto busy = std::make_shared<bool>(false);
		auto reconnected = std::make_shared<bool>(false);
		auto poll = [box, install, phones_box, add, rebuild, busy, reconnected] {
			if (*busy || !box->isVisible()) return;
			const bool ready = have_program("adb") && have_program("scrcpy");
			install->setVisible(!ready);
			phones_box->setVisible(ready);
			add->setVisible(ready);
			if (!have_program("adb")) return;
			// A phone set up for Wi-Fi before: connect to it again (once).
			if (!*reconnected) {
				*reconnected = true;
				const QString known = settings->get("phone_address");
				if (!known.isEmpty()) QProcess::startDetached("adb", {"connect", known.contains(':') ? known : known + ":5555"});
			}
			*busy = true;
			auto *pr = new QProcess(box);
			QObject::connect(pr, &QProcess::finished, [pr, rebuild, busy] {
				*busy = false;
				std::vector<Phone> phones;
				for (const QString &line : QString::fromUtf8(pr->readAllStandardOutput()).split('\n')) {
					const QStringList f = line.simplified().split(' ');
					if (f.size() < 2 || line.startsWith("List of") || line.startsWith("*")) continue;
					Phone p;
					p.serial = f[0];
					p.state = f[1];
					p.wifi = p.serial.contains(':') || p.serial.contains("._adb-tls-connect");
					for (const QString &kv : f) {
						if (kv.startsWith("model:")) p.name = kv.mid(6).replace('_', ' ');
					}
					if (p.name.isEmpty()) p.name = "Android phone";
					phones.push_back(p);
				}
				rebuild(phones);
				pr->deleteLater();
			});
			QObject::connect(pr, &QProcess::errorOccurred, [pr, busy](QProcess::ProcessError e) {
				if (e == QProcess::FailedToStart) { *busy = false; pr->deleteLater(); }
			});
			pr->start("adb", {"devices", "-l"});
		};
		QObject::connect(timer, &QTimer::timeout, poll);
		timer->start();
		poll();
	}

	Card *c = card(page, "As a second screen", "Dusk adds a screen that shows on your phone or tablet: move "
		"windows onto it like any other screen (arrange it in Displays). On the phone, open a VNC app such as "
		"RealVNC Viewer or AVNC and connect; touching the phone controls that screen.");
	if (!have_program("wayvnc")) {
		auto *b = new QPushButton("Install");
		b->setObjectName("primary");
		b->setCursor(Qt::PointingHandCursor);
		make_row(c, "Needs wayvnc", "Asks for your password", b, nullptr);
		QObject::connect(b, &QPushButton::clicked, [b] {
			b->setEnabled(false);
			b->setText("Installing…");
			auto *p = new QProcess(b);
			QObject::connect(p, &QProcess::finished, [p, b](int code) {
				b->setText(code == 0 ? "Installed ✓" : "Didn't work: try again");
				b->setEnabled(code != 0);
				p->deleteLater();
			});
			p->start("pkexec", {"dnf", "install", "-y", "wayvnc", "android-tools"});
		});
	}
	toggle(c, "Second screen on a phone or tablet", QString(), "phone_screen", false);
	choice(c, "Connect with", QString(), "phone_link", {{"Wi-Fi (same network)", "wifi"}, {"USB cable", "usb"}}, "wifi", on);
	choice(c, "Its size", "Pick the phone's or tablet's screen, sideways", "phone_size",
		{{"Phone (2400 × 1080)", "2400x1080"}, {"Phone, Full HD (1920 × 1080)", "1920x1080"},
			{"Tablet (2560 × 1600)", "2560x1600"}, {"Tablet 4:3 (2048 × 1536)", "2048x1536"},
			{"Small tablet (1280 × 800)", "1280x800"}}, "2400x1080", on);
	advanced(text_field(c, "Other size", "Width x height, e.g. 2800x1260", "phone_size", "2400x1080", on));
	{
		QLineEdit *edit = nullptr;
		QWidget *row = text_field(c, "Password", "Asked for by the VNC app (user name: dusk). Needed over Wi-Fi.",
			"phone_password", "A password", wifi, &edit);
		auto *gen = new QPushButton("New");
		gen->setToolTip("Make up a new password");
		gen->setCursor(Qt::PointingHandCursor);
		if (auto *h = qobject_cast<QHBoxLayout *>(row->layout()->itemAt(row->layout()->count() - 1)->layout())) {
			h->insertWidget(h->count() - 1, gen);
		}
		auto fill = [edit] {
			edit->setText(new_password());
			emit edit->editingFinished();
		};
		QObject::connect(gen, &QPushButton::clicked, fill);
		// First time on Wi-Fi with no password: make one up.
		auto *timer = new QTimer(row);
		timer->setInterval(700);
		QObject::connect(timer, &QTimer::timeout, [fill] {
			if (settings->get_bool("phone_screen", false) && settings->get("phone_link", "wifi") == "wifi" &&
					settings->get("phone_password").isEmpty()) {
				fill();
			}
		});
		timer->start();
	}
	advanced(number(c, "Port", QString(), "phone_port", 1024, 65535, 1, 5900, QString(), on));
	{
		auto *where = new QLabel;
		where->setTextInteractionFlags(Qt::TextSelectableByMouse);
		auto refresh = [where] {
			const QString port = settings->get("phone_port", "5900");
			const bool over_usb = settings->get("phone_link", "wifi") == "usb";
			where->setText((over_usb ? QString("localhost") : local_address()) + ":" + port);
		};
		refresh();
		QWidget *row = make_row(c, "On the phone, connect to", "Wi-Fi: the phone must be on the same network", where, nullptr);
		bindings.push_back(Binding{row, nullptr, "", "", refresh, on, {}});
	}
	{
		auto *again = new QPushButton("Connect again");
		again->setCursor(Qt::PointingHandCursor);
		QObject::connect(again, &QPushButton::clicked, [] {
			const QString port = settings->get("phone_port", "5900");
			QProcess::startDetached("adb", {"reverse", "tcp:" + port, "tcp:" + port});
		});
		QWidget *row = make_row(c, "USB", have_program("adb") ? "Turn on USB debugging on the phone (Developer "
			"options), plug it in, then connect again if it was plugged in later"
			: "Needs adb:  sudo dnf install android-tools", again, nullptr);
		bindings.push_back(Binding{row, nullptr, "", "", {}, usb, {}});
	}

	more_toggle(page);
	finish(page);
	return page.widget;
}

// ---- Lock screen ---------------------------------------------------------------------

// The lock screen in miniature, drawn the way Dusk draws it (same places,
// same proportions): the big preview at the top of the page, and the small
// ones on the preset buttons. `get` reads a setting (a preset passes its own
// values over the real ones).
using LockGet = std::function<QString(const QString &key, const QString &fallback)>;

// The background picture, small (made once per picture and setting).
QImage lock_backdrop(const LockGet &get) {
	static QString made_for;
	static QImage made;
	const QString kind = get("lock_background", "blur");
	const bool custom = kind == "picture" || kind == "picture_blur";
	QString path = custom ? get("lock_picture", "") : settings->get("wallpaper");
	if (!custom && settings->get("wallpaper_type", "image") != "image") path.clear();
	const QString key = kind + "|" + path + "|" + get("lock_blur", "60") + "|" + get("lock_color", "#1c1b29") + "|" +
		settings->get("wallpaper_color");
	if (key == made_for) return made;
	made_for = key;
	QImage img;
	if (kind != "color" && !path.isEmpty()) {
		QImageReader r(path);
		r.setAutoTransform(true);
		const QSize full = r.size();
		if (full.isValid()) {
			r.setScaledSize(full.scaled(QSize(640, 360), Qt::KeepAspectRatioByExpanding));
		}
		img = r.read();
	}
	if (img.isNull()) {
		img = QImage(320, 180, QImage::Format_ARGB32_Premultiplied);
		QColor c(kind == "color" ? get("lock_color", "#1c1b29")
			: settings->get("wallpaper_color", "#2a2d3e").isEmpty() ? "#2a2d3e" : settings->get("wallpaper_color", "#2a2d3e"));
		if (!c.isValid()) c = QColor("#2a2d3e");
		img.fill(c);
	} else {
		// Covering 16:9, from the middle.
		const QSize want(640, 360);
		img = img.scaled(want, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
		img = img.copy((img.width() - want.width()) / 2, (img.height() - want.height()) / 2, want.width(), want.height());
		if (kind == "blur" || kind == "picture_blur") {
			const int blur = std::clamp(get("lock_blur", "60").toInt(), 0, 100);
			if (blur > 0) {
				const int f = 4 + blur / 6;   // smaller = blurrier
				img = img.scaled(std::max(8, 640 / f), std::max(5, 360 / f), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
					.scaled(want, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
			}
		}
	}
	made = img;
	return made;
}

void paint_lock(QPainter &p, const QRectF &r, const LockGet &get, bool detailed) {
	auto on = [&](const QString &k, bool fb) {
		const QString v = get(k, fb ? "true" : "false").toLower();
		return v == "true" || v == "1" || v == "yes" || v == "on";
	};
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	QPainterPath clip;
	clip.addRoundedRect(r, detailed ? 14 : 9, detailed ? 14 : 9);
	p.setClipPath(clip);
	p.drawImage(r, lock_backdrop(get));
	p.fillRect(r, QColor(0, 0, 0, std::clamp(get("lock_dim", "25").toInt(), 0, 85) * 255 / 100));

	// Everything in the units of a 1080-high screen, like Dusk's own.
	const double k = r.height() / 1080.0, W = r.width() / k, H = 1080;
	p.translate(r.topLeft());
	p.scale(k, k);
	const QColor white(255, 255, 255), accent = current_look().accent;
	const QString where = get("lock_clock", "center"), style = get("lock_clock_style", "digital");
	const QString size_s = get("lock_clock_size", "medium");
	const double size = size_s == "small" ? 0.7 : size_s == "large" ? 1.35 : 1.0;
	const double pt = std::clamp(H * 0.068 * size, 30.0, 170.0), dpt = 16, mpt = 12;
	const bool clock = where != "hidden", date = clock && on("lock_date", true);
	const QString message = get("lock_message", "");
	double time_h = 0;
	if (clock) time_h = style == "stacked" ? pt * 1.3 * 1.3 * 2 - pt * 1.3 * 0.34 : style == "analog" ? pt * 2.5 + 16 : pt * 1.75;
	const double ch = time_h + (date ? dpt * 2 : 0) + (message.isEmpty() ? 0 : mpt * 2.6) + 12;
	const bool center = where == "center" || where == "hidden";
	QRectF cbox;
	if (where == "left") cbox = QRectF(W * 0.06, H * 0.07, W * 0.6, ch);
	else if (where == "corner") cbox = QRectF(W * 0.06, H * 0.92 - ch, W * 0.6, ch);
	else cbox = QRectF(0, H * (style == "analog" || style == "stacked" ? 0.08 : 0.11), W, ch);
	const Qt::Alignment al = (center ? Qt::AlignHCenter : Qt::AlignLeft) | Qt::AlignVCenter;
	QString family = get("lock_clock_font", "");
	if (family.isEmpty()) family = ui_font();
	const bool bold = get("lock_clock_weight", "light") == "bold";
	const bool acc = on("lock_clock_accent", false);
	const QColor col = acc ? accent : white;
	auto font = [&](const QString &fam, double px, bool b) {
		QFont f(fam);
		f.setPixelSize(std::max(1, static_cast<int>(px)));
		f.setWeight(b ? QFont::Bold : QFont::Light);
		return f;
	};
	double y = cbox.y();
	if (clock) {
		const QTime now = QTime::currentTime();
		const bool h24 = settings->get_bool("clock_24h", true);
		if (style == "stacked") {
			const double p2 = pt * 1.3, lh = p2 * 1.3;
			p.setFont(font(family, p2 * 1.33, bold));
			p.setPen(col);
			p.drawText(QRectF(cbox.x(), y, cbox.width(), lh), al, now.toString(h24 ? "HH" : "hh"));
			QColor mc = acc ? QColor::fromRgbF(accent.redF() * 0.55 + 0.45, accent.greenF() * 0.55 + 0.45,
				accent.blueF() * 0.55 + 0.45) : accent;
			p.setPen(mc);
			p.drawText(QRectF(cbox.x(), y + lh - p2 * 0.34, cbox.width(), lh), al, now.toString("mm"));
		} else if (style == "analog") {
			const double d = pt * 2.5, rr = d / 2;
			const QPointF c(center ? cbox.center().x() : cbox.x() + rr + 4, y + rr + 8);
			p.setPen(QPen(QColor(255, 255, 255, 72), 2));
			p.setBrush(QColor(0, 0, 0, 62));
			p.drawEllipse(c, rr, rr);
			for (int i = 0; i < 12; i++) {
				const double a = i * M_PI / 6;
				const bool big = i % 3 == 0;
				p.setPen(QPen(QColor(255, 255, 255, big ? 230 : 128), big ? std::max(3.0, rr * 0.035) : 2, Qt::SolidLine, Qt::RoundCap));
				p.drawLine(c + QPointF(std::sin(a), -std::cos(a)) * rr * (big ? 0.8 : 0.86),
					c + QPointF(std::sin(a), -std::cos(a)) * rr * 0.92);
			}
			const double ha = ((now.hour() % 12) + now.minute() / 60.0) * M_PI / 6, ma = now.minute() * M_PI / 30;
			p.setPen(QPen(col, std::max(5.0, rr * 0.07), Qt::SolidLine, Qt::RoundCap));
			p.drawLine(c, c + QPointF(std::sin(ha), -std::cos(ha)) * rr * 0.52);
			p.setPen(QPen(acc ? white : accent, std::max(3.5, rr * 0.045), Qt::SolidLine, Qt::RoundCap));
			p.drawLine(c, c + QPointF(std::sin(ma), -std::cos(ma)) * rr * 0.78);
			p.setPen(Qt::NoPen);
			p.setBrush(white);
			p.drawEllipse(c, std::max(4.0, rr * 0.05), std::max(4.0, rr * 0.05));
		} else {
			const QString t = now.toString(h24 ? "HH:mm" : "h:mm");
			const QFont f = font(family, pt * 1.33, bold);
			if (style == "outline") {
				QPainterPath path;
				const QFontMetricsF fm(f);
				const double tw = fm.horizontalAdvance(t);
				const double tx = center ? cbox.center().x() - tw / 2 : cbox.x();
				path.addText(tx, y + (pt * 1.75 + fm.ascent() - fm.descent()) / 2, f, t);
				p.setBrush(Qt::NoBrush);
				p.setPen(QPen(col, std::max(1.5, pt / 40), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
				p.drawPath(path);
			} else {
				p.setFont(f);
				p.setPen(col);
				p.drawText(QRectF(cbox.x(), y, cbox.width(), pt * 1.75), al, t);
			}
		}
		y += time_h;
		if (date) {
			p.setFont(font(ui_font(), dpt * 1.33, false));
			p.setPen(QColor(255, 255, 255, 217));
			const double up = style == "digital" || style == "outline" ? pt * 0.15 : 0;
			p.drawText(QRectF(cbox.x(), y - up, cbox.width(), dpt * 2), al, QLocale().toString(QDate::currentDate(), "dddd d MMMM"));
			y += dpt * 2;
		}
	}
	if (!message.isEmpty()) {
		p.setFont(font(ui_font(), mpt * 1.33, false));
		p.setPen(QColor(255, 255, 255, 204));
		p.drawText(QRectF(cbox.x(), y + 4, cbox.width(), mpt * 2.2), al, message);
	}

	// The music card (a sample one) and the status corner.
	const bool media = on("lock_media", true);
	QRectF mbox(center ? (W - 380) / 2 : W * 0.06, where == "corner" ? H * 0.08 : cbox.bottom() + 18, 380, 92);
	if (media) {
		p.setPen(QPen(QColor(255, 255, 255, 31), 1));
		p.setBrush(QColor(0, 0, 0, 72));
		p.drawRoundedRect(mbox, 20, 20);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255, 40));
		p.drawRoundedRect(QRectF(mbox.x() + 10, mbox.y() + 10, 56, 56), 12, 12);
		p.setBrush(QColor(255, 255, 255, 200));
		p.drawRoundedRect(QRectF(mbox.x() + 78, mbox.y() + 20, 150, 10), 5, 5);
		p.setBrush(QColor(255, 255, 255, 110));
		p.drawRoundedRect(QRectF(mbox.x() + 78, mbox.y() + 40, 100, 8), 4, 4);
		p.setBrush(accent);
		p.drawEllipse(QPointF(mbox.right() - 14 - 51, mbox.center().y()), 17, 17);
		p.setBrush(QColor(255, 255, 255, 60));
		p.drawRoundedRect(QRectF(mbox.x() + 48, mbox.bottom() - 18, 284, 4), 2, 2);
		p.setBrush(accent);
		p.drawRoundedRect(QRectF(mbox.x() + 48, mbox.bottom() - 18, 110, 4), 2, 2);
	}
	if (on("lock_status", true)) {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255, 230));
		const double sx = W - 20 - 100, sy = 16 + 6;
		for (int i = 0; i < 2; i++) p.drawEllipse(QRectF(sx + i * 26, sy, 16, 16));
		p.drawRoundedRect(QRectF(sx + 52, sy + 2, 24, 12), 3, 3);
	}

	// The form: picture, name, the field (on a card if that's on).
	const bool face = on("lock_avatar", true), name = on("lock_name", true), card = on("lock_card", false);
	const double kFace = 104, kFieldW = 320, kFieldH = 48, pad = card ? 26 : 8;
	const double fw = card ? 400 : 420;
	const double fh = pad * 2 + (face ? kFace + 14 : 0) + (name ? 44 : 6) + kFieldH + 14 + 22;
	const QString form = get("lock_form", "middle");
	double fx, fy;
	if (form == "right") {
		fx = W - fw - W * 0.07;
		fy = (H - fh) / 2;
	} else {
		fx = (W - fw) / 2;
		fy = H * (form == "lower" ? 0.66 : 0.52) - (face ? kFace / 2 : 0) - pad;
		if (center) fy = std::max(fy, media && where != "corner" ? mbox.bottom() + 22 : cbox.bottom() + 20);
	}
	fy = std::min(fy, H - fh - 16);
	if (card) {
		p.setPen(QPen(QColor(255, 255, 255, 36), 1));
		p.setBrush(QColor(0, 0, 0, 87));
		p.drawRoundedRect(QRectF(fx, fy, fw, fh), 30, 30);
	}
	double yy = fy + pad;
	const double cx = fx + fw / 2;
	if (face) {
		p.setPen(QPen(QColor(255, 255, 255, 90), 2));
		p.setBrush(accent);
		if (get("lock_avatar_shape", "circle") == "rounded") {
			p.drawRoundedRect(QRectF(cx - kFace / 2, yy, kFace, kFace), kFace * 0.24, kFace * 0.24);
		} else {
			p.drawEllipse(QRectF(cx - kFace / 2, yy, kFace, kFace));
		}
		if (detailed) {
			const QImage img(QDir::homePath() + "/.face");
			if (!img.isNull()) {
				QPainterPath fp;
				if (get("lock_avatar_shape", "circle") == "rounded") {
					fp.addRoundedRect(QRectF(cx - kFace / 2, yy, kFace, kFace), kFace * 0.24, kFace * 0.24);
				} else {
					fp.addEllipse(QRectF(cx - kFace / 2, yy, kFace, kFace));
				}
				p.save();
				p.setClipPath(fp, Qt::IntersectClip);
				const int side = std::min(img.width(), img.height());
				p.drawImage(QRectF(cx - kFace / 2, yy, kFace, kFace),
					img.copy((img.width() - side) / 2, (img.height() - side) / 2, side, side));
				p.restore();
			}
		}
		yy += kFace + 14;
	}
	if (name) {
		p.setPen(white);
		p.setFont(font(ui_font(), 20, true));
		QString who = qEnvironmentVariable("USER");
		p.drawText(QRectF(fx, yy, fw, 30), Qt::AlignCenter, who.isEmpty() ? "You" : who.left(1).toUpper() + who.mid(1));
	}
	yy += name ? 44 : 6;
	p.setPen(QPen(accent, 2));
	p.setBrush(QColor(255, 255, 255, 56));
	p.drawRoundedRect(QRectF(cx - kFieldW / 2, yy, kFieldW, kFieldH), kFieldH / 2, kFieldH / 2);
	p.setPen(Qt::NoPen);
	p.setBrush(white);
	for (int i = 0; i < 6; i++) p.drawEllipse(QPointF(cx - kFieldW / 2 + 24 + i * 8, yy + kFieldH / 2), 4.2, 4.2);
	p.setBrush(accent);
	p.drawEllipse(QPointF(cx + kFieldW / 2 - kFieldH / 2 - 4, yy + kFieldH / 2), kFieldH / 2 - 6, kFieldH / 2 - 6);
	p.restore();
}

// The big preview at the top of the page: redrawn as settings change.
class LockPreview : public QWidget {
public:
	LockPreview() {
		QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
		sp.setHeightForWidth(true);
		setSizePolicy(sp);
		setMinimumWidth(200);
	}
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override { return std::min(w, 620) * 9 / 16 + 4; }
	QSize sizeHint() const override { return QSize(560, heightForWidth(560)); }

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter p(this);
		const double w = std::min(width(), 620), h = w * 9 / 16;
		const QRectF r((width() - w) / 2, 2, w, h);
		paint_lock(p, r, [](const QString &k, const QString &fb) { return settings->get(k, fb); }, true);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(QPen(current_look().line, 1));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 14, 14);
	}
};

struct LockPreset {
	const char *name, *blurb;
	std::vector<std::pair<const char *, const char *>> keys;
};

const std::vector<LockPreset> &lock_presets() {
	static const std::vector<LockPreset> kPresets = {
		{"Classic", "The clock at the top, you in the middle",
			{{"lock_clock", "center"}, {"lock_clock_style", "digital"}, {"lock_clock_weight", "light"},
				{"lock_clock_size", "medium"}, {"lock_clock_accent", "false"}, {"lock_form", "middle"},
				{"lock_card", "false"}, {"lock_avatar", "true"}, {"lock_name", "true"}, {"lock_date", "true"}}},
		{"Stacked", "Big hours over the minutes, in your accent",
			{{"lock_clock", "center"}, {"lock_clock_style", "stacked"}, {"lock_clock_weight", "bold"},
				{"lock_clock_size", "large"}, {"lock_clock_accent", "false"}, {"lock_form", "lower"},
				{"lock_card", "false"}, {"lock_avatar", "true"}, {"lock_name", "true"}, {"lock_date", "true"}}},
		{"Clock face", "A clock with hands",
			{{"lock_clock", "center"}, {"lock_clock_style", "analog"}, {"lock_clock_weight", "light"},
				{"lock_clock_size", "medium"}, {"lock_clock_accent", "false"}, {"lock_form", "lower"},
				{"lock_card", "false"}, {"lock_avatar", "true"}, {"lock_name", "true"}, {"lock_date", "true"}}},
		{"Corner", "A big clock bottom left, you on a card at the side",
			{{"lock_clock", "corner"}, {"lock_clock_style", "digital"}, {"lock_clock_weight", "light"},
				{"lock_clock_size", "large"}, {"lock_clock_accent", "false"}, {"lock_form", "right"},
				{"lock_card", "true"}, {"lock_avatar", "true"}, {"lock_name", "true"}, {"lock_date", "true"}}},
		{"Outline", "The time as an outline, in your accent",
			{{"lock_clock", "center"}, {"lock_clock_style", "outline"}, {"lock_clock_weight", "bold"},
				{"lock_clock_size", "large"}, {"lock_clock_accent", "true"}, {"lock_form", "lower"},
				{"lock_card", "false"}, {"lock_avatar", "true"}, {"lock_name", "false"}, {"lock_date", "true"}}},
		{"Minimal", "A small clock and the password, nothing else",
			{{"lock_clock", "center"}, {"lock_clock_style", "digital"}, {"lock_clock_weight", "light"},
				{"lock_clock_size", "small"}, {"lock_clock_accent", "false"}, {"lock_form", "middle"},
				{"lock_card", "false"}, {"lock_avatar", "true"}, {"lock_name", "false"}, {"lock_date", "false"}}},
	};
	return kPresets;
}

// A preset: a small picture of it, its name under it; ringed when it's what
// you have now.
class LockPresetButton : public QAbstractButton {
public:
	explicit LockPresetButton(const LockPreset &preset) : preset_(preset) {
		setCursor(Qt::PointingHandCursor);
		setFixedSize(156, 118);
		setToolTip(preset.blurb);
		setAttribute(Qt::WA_Hover);
	}
	bool active() const {
		for (const auto &[k, v] : preset_.keys) {
			QString now = settings->get(k);
			if (now.isEmpty()) {
				// Unset: the default.
				static const std::map<QString, QString> kDefaults = {{"lock_clock", "center"},
					{"lock_clock_style", "digital"}, {"lock_clock_weight", "light"}, {"lock_clock_size", "medium"},
					{"lock_clock_accent", "false"}, {"lock_form", "middle"}, {"lock_card", "false"},
					{"lock_avatar", "true"}, {"lock_name", "true"}, {"lock_date", "true"}};
				auto it = kDefaults.find(k);
				now = it != kDefaults.end() ? it->second : QString();
			}
			if (now != v) return false;
		}
		return true;
	}

protected:
	void paintEvent(QPaintEvent *) override {
		const Look l = current_look();
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const QRectF pic(4, 4, width() - 8, (width() - 8) * 9.0 / 16);
		const LockPreset &pr = preset_;
		paint_lock(p, pic, [&pr](const QString &k, const QString &fb) {
			for (const auto &[pk, pv] : pr.keys) {
				if (k == pk) return QString(pv);
			}
			return settings->get(k, fb);
		}, false);
		const bool on = active();
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(on ? l.accent : underMouse() ? l.dim : l.line, on ? 3 : 1));
		p.drawRoundedRect(pic.adjusted(-1.5, -1.5, 1.5, 1.5), 11, 11);
		p.setPen(on ? l.accent : l.text);
		QFont f = font();
		f.setWeight(on ? QFont::DemiBold : QFont::Normal);
		p.setFont(f);
		p.drawText(QRectF(0, pic.bottom() + 6, width(), height() - pic.bottom() - 6), Qt::AlignHCenter | Qt::AlignTop, pr.name);
	}

private:
	const LockPreset &preset_;
};

QWidget *lockscreen_page() {
	Page page = make_page("Lock screen", "What you see when the computer is locked. Pick a style, then "
		"change anything below; the preview follows. (Logging in is GNOME's login screen.)");
	auto background = [] { return settings->get("lock_background", "blur"); };

	// The preview and the presets.
	Card *style = card(page, "Style");
	{
		auto *preview = new LockPreview;
		style->add(preview);
		auto *strip = new QWidget;
		auto *grid = new QGridLayout(strip);
		grid->setContentsMargins(0, 8, 0, 4);
		grid->setHorizontalSpacing(8);
		grid->setVerticalSpacing(8);
		std::vector<LockPresetButton *> buttons;
		int i = 0;
		for (const LockPreset &pr : lock_presets()) {
			auto *b = new LockPresetButton(pr);
			buttons.push_back(b);
			grid->addWidget(b, i / 3, i % 3, Qt::AlignHCenter);
			i++;
			QObject::connect(b, &QAbstractButton::clicked, [&pr] {
				for (const auto &[k, v] : pr.keys) settings->set(k, v);
				settings->save();
				refresh_all();
				changed("lock_clock");
			});
		}
		style->add(strip);
		auto *lock_now = new QPushButton("Try it: lock now");
		lock_now->setCursor(Qt::PointingHandCursor);
		QObject::connect(lock_now, &QPushButton::clicked, [] { QProcess::startDetached("loginctl", {"lock-session"}); });
		make_row(style, "See it for real", "Your password unlocks it", lock_now, nullptr);
		auto redraw = [preview, buttons] {
			preview->update();
			for (auto *b : buttons) b->update();
		};
		restyle_hooks.push_back(redraw);
		bindings.push_back(Binding{preview, nullptr, "", "", redraw, {}, {}});
	}

	Card *clock = card(page, "Clock");
	choice(clock, "Where", QString(), "lock_clock",
		{{"Top, in the middle", "center"}, {"Top left", "left"}, {"Bottom left, big", "corner"}, {"No clock", "hidden"}},
		"center");
	auto clock_on = [] { return settings->get("lock_clock", "center") != "hidden"; };
	choice(clock, "Style", QString(), "lock_clock_style",
		{{"Digital", "digital"}, {"Hours over minutes", "stacked"}, {"Outline", "outline"}, {"Clock face (hands)", "analog"}},
		"digital", clock_on);
	{
		Items fonts = {{"Same as everything else", ""}};
		QStringList fams = QFontDatabase::families();
		fams.sort(Qt::CaseInsensitive);
		for (const QString &f : fams) {
			if (!QFontDatabase::isPrivateFamily(f)) fonts.push_back({f, f});
		}
		QWidget *row = choice(clock, "Font", QString(), "lock_clock_font", fonts, "", [clock_on] {
			return clock_on() && settings->get("lock_clock_style", "digital") != "analog";
		});
		if (auto *combo = row->findChild<QComboBox *>()) combo->setMaxVisibleItems(14);
	}
	choice(clock, "Weight", QString(), "lock_clock_weight", {{"Light", "light"}, {"Bold", "bold"}}, "light",
		[clock_on] { return clock_on() && settings->get("lock_clock_style", "digital") != "analog"; });
	choice(clock, "Size", QString(), "lock_clock_size", {{"Small", "small"}, {"Medium", "medium"}, {"Large", "large"}},
		"medium", clock_on);
	toggle(clock, "In your accent colour", "Otherwise white", "lock_clock_accent", false, clock_on);
	toggle(clock, "Date", "Under the clock", "lock_date", true, clock_on);
	text_field(clock, "A line of your own", "Under the date, e.g. “If found, call 0966…”", "lock_message", "Nothing");

	Card *look = card(page, "Background");
	choice(look, "Background", QString(), "lock_background",
		{{"Your wallpaper, blurred", "blur"}, {"Your wallpaper", "wallpaper"}, {"Custom picture", "picture"},
			{"Custom picture, blurred", "picture_blur"}, {"A colour", "color"}}, "blur");
	{
		QLineEdit *edit = nullptr;
		QWidget *row = text_field(look, "Picture", QString(), "lock_picture", "A picture file",
			[background] { return background() == "picture" || background() == "picture_blur"; }, &edit);
		auto *pick = new QPushButton("Choose…");
		pick->setCursor(Qt::PointingHandCursor);
		if (auto *h = qobject_cast<QHBoxLayout *>(row->layout()->itemAt(row->layout()->count() - 1)->layout())) {
			h->insertWidget(h->count() - 1, pick);
		}
		QObject::connect(pick, &QPushButton::clicked, [edit, pick] {
			const QString f = QFileDialog::getOpenFileName(pick->window(), "A picture for the lock screen",
				QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
				"Pictures (*.png *.jpg *.jpeg *.webp *.jxl *.avif *.svg)");
			if (f.isEmpty()) return;
			edit->setText(f);
			emit edit->editingFinished();
		});
	}
	color_row(look, "Colour", "lock_color", "#1c1b29", [background] { return background() == "color"; });
	slider(look, "Blur", QString(), "lock_blur", 0, 100, 60, 1, [](int v) { return QString("%1%").arg(v); },
		[background] { return background() == "blur" || background() == "picture_blur"; });
	slider(look, "Darker", "So the clock and text stand out", "lock_dim", 0, 85, 25, 1,
		[](int v) { return QString("%1%").arg(v); });

	// You: your picture (changed here), its shape, your name, and the field.
	Card *you = card(page, "You and the password");
	{
		// The picture, round, with Choose and Remove.
		const QString face = QDir::homePath() + "/.face";
		auto *box = new QWidget;
		auto *h = new QHBoxLayout(box);
		h->setContentsMargins(0, 0, 0, 0);
		h->setSpacing(8);
		auto *preview = new QLabel;
		preview->setFixedSize(56, 56);
		auto show = [preview, face] {
			const qreal dpr = preview->devicePixelRatioF();
			QPixmap pm(QSize(56, 56) * dpr);
			pm.setDevicePixelRatio(dpr);
			pm.fill(Qt::transparent);
			QPainter p(&pm);
			p.setRenderHint(QPainter::Antialiasing);
			p.setRenderHint(QPainter::SmoothPixmapTransform);
			QPainterPath circle;
			circle.addEllipse(QRectF(1, 1, 54, 54));
			p.setClipPath(circle);
			QImage img(face);
			if (!img.isNull()) {
				// Cropped to a square from the middle, so it's round, not squashed.
				const int side = std::min(img.width(), img.height());
				img = img.copy((img.width() - side) / 2, (img.height() - side) / 2, side, side);
				p.drawImage(QRectF(1, 1, 54, 54), img);
			} else {
				p.fillRect(QRectF(0, 0, 56, 56), current_look().accent);
				p.setPen(current_look().on_accent);
				QFont f = p.font();
				f.setBold(true);
				f.setPointSizeF(f.pointSizeF() * 1.6);
				p.setFont(f);
				const QString who = qEnvironmentVariable("USER");
				p.drawText(QRectF(0, 0, 56, 56), Qt::AlignCenter, who.left(1).toUpper());
			}
			p.end();
			preview->setPixmap(pm);
		};
		show();
		auto *choose = new QPushButton("Choose…");
		auto *remove = new QPushButton("Remove");
		for (QPushButton *b : {choose, remove}) b->setCursor(Qt::PointingHandCursor);
		h->addWidget(preview);
		h->addWidget(choose);
		h->addWidget(remove);
		make_row(you, "Your picture", "Also used by GNOME's login screen", box, nullptr);
		// Saved as ~/.face (cropped square, 512 px), and given to the system's
		// accounts service too, so GNOME's login screen shows it.
		auto tell_system = [](const QString &path) {
			QProcess::startDetached("busctl", {"call", "--system", "org.freedesktop.Accounts",
				QString("/org/freedesktop/Accounts/User%1").arg(getuid()), "org.freedesktop.Accounts.User",
				"SetIconFile", "s", path});
		};
		auto changed_face = [] {
			settings->set("lock_face_changed", QString::number(QDateTime::currentSecsSinceEpoch()));
			settings->save();
			changed("lock_face_changed");
			refresh_all();
		};
		QObject::connect(choose, &QPushButton::clicked, [choose, face, show, tell_system, changed_face] {
			const QString f = QFileDialog::getOpenFileName(choose->window(), "Your picture",
				QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
				"Pictures (*.png *.jpg *.jpeg *.webp *.jxl *.avif *.bmp *.gif)");
			if (f.isEmpty()) return;
			QImage img(f);
			if (img.isNull()) return;
			const int side = std::min(img.width(), img.height());
			img = img.copy((img.width() - side) / 2, (img.height() - side) / 2, side, side)
				.scaled(512, 512, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
			if (!img.save(face, "PNG")) return;
			tell_system(face);
			show();
			changed_face();
		});
		QObject::connect(remove, &QPushButton::clicked, [face, show, tell_system, changed_face] {
			QFile::remove(face);
			tell_system("");
			show();
			changed_face();
		});
	}
	toggle(you, "Show your picture", QString(), "lock_avatar", true);
	choice(you, "Its shape", QString(), "lock_avatar_shape", {{"Round", "circle"}, {"Rounded square", "rounded"}},
		"circle", [] { return settings->get_bool("lock_avatar", true); });
	toggle(you, "Show your name", QString(), "lock_name", true);
	choice(you, "Password field", "Where it is on the screen", "lock_form",
		{{"In the middle", "middle"}, {"Lower down", "lower"}, {"On the right", "right"}}, "middle");
	toggle(you, "On a card", "A frosted panel behind your picture and the field", "lock_card", false);

	Card *extras = card(page, "On the lock screen too");
	toggle(extras, "What's playing", "The song, its cover, and play / pause / skip", "lock_media", true);
	toggle(extras, "Waiting notifications", "Which apps have notifications and how many, never what they say",
		"lock_notifications", true);
	toggle(extras, "Wi-Fi, sound and battery", "Top right, like a phone", "lock_status", true);
	toggle(extras, "Just the clock at first", "The rest shows when you press a key or click", "lock_curtain", false);

	Card *when = card(page, "When it locks", "Super+L locks it any time, and so does Lock in the power menu. "
		"To lock after a while on its own (not with the screen), set Lock in Power.");
	toggle(when, "When the screen turns off", "After the time in Power", "lock_screen_off", true);
	toggle(when, "Before the computer sleeps", "Also when you close the lid", "lock_sleep", true);
	more_toggle(page);
	finish(page);
	return page.widget;
}

QWidget *apps_page() {
	Page page = make_page("Default apps", QString());
	struct Term {
		const char *exe, *name;
	};
	static const Term kTerminals[] = {
		{"ptyxis", "Ptyxis"}, {"gnome-terminal", "GNOME Terminal"}, {"kgx", "Console"},
		{"konsole", "Konsole"}, {"kitty", "kitty"}, {"alacritty", "Alacritty"},
		{"wezterm", "WezTerm"}, {"ghostty", "Ghostty"}, {"foot", "foot"},
		{"tilix", "Tilix"}, {"xfce4-terminal", "Xfce Terminal"}, {"xterm", "XTerm"},
	};
	Card *c = card(page, "Terminal", "Opened with Super+Enter.");
	auto *combo = new ElideCombo;
	flex_width(combo, 240);
	combo->addItem("Automatic (first one installed)", "");
	for (const Term &t : kTerminals) {
		if (!QStandardPaths::findExecutable(t.exe).isEmpty()) {
			combo->addItem(app_icon(t.exe), t.name, t.exe);
		}
	}
	combo->addItem("Other command…", "__custom");
	QToolButton *reset = nullptr;
	QWidget *row = make_row(c, "Terminal app", QString(), combo, &reset);

	auto custom = std::make_shared<bool>(false);
	auto is_custom = [custom] { return *custom; };
	QLineEdit *edit = nullptr;
	text_field(c, "Command", "For example: kitty --single-instance", "terminal", "A command",
		is_custom, &edit);

	auto refresh = [combo, custom] {
		const QSignalBlocker block(combo);
		const QString now = settings->get("terminal");
		const int i = combo->findData(now);
		if (i >= 0 && !*custom) {
			combo->setCurrentIndex(i);
		} else if (!now.isEmpty() || *custom) {
			*custom = true;
			combo->setCurrentIndex(combo->findData("__custom"));
		} else {
			combo->setCurrentIndex(0);
		}
	};
	refresh();
	QObject::connect(combo, &QComboBox::currentIndexChanged, [combo, custom, edit](int) {
		const QString v = combo->currentData().toString();
		*custom = v == "__custom";
		if (*custom) {
			edit->setFocus();
		} else if (v.isEmpty()) {
			settings->remove("terminal");
		} else {
			settings->set("terminal", v);
		}
		changed("terminal");
	});
	QObject::connect(reset, &QToolButton::clicked, [custom, refresh] {
		*custom = false;
		settings->remove("terminal");
		refresh();
		changed("terminal");
	});
	bindings.push_back(Binding{row, reset, "terminal", "", refresh, {},
		[] { return settings->get("terminal").isEmpty(); }});
	default_app_cards(page);
	finish(page);
	return page.widget;
}

// ---- Reset everything ------------------------------------------------------------------

// The settings, grouped like the pages, for "Reset all settings".
struct SettingGroup {
	const char *title;
	QStringList exact, prefixes;
	bool keep;   // switched on to start with
};

const std::vector<SettingGroup> &setting_groups() {
	static const std::vector<SettingGroup> groups = {
		{"Appearance (theme, accent, wallpaper, cursor)",
			{"theme", "high_contrast", "dark_style", "accent", "accent_auto", "font", "style_apps", "ui_style"},
			{"wallpaper", "ui_", "cursor_"}, false},
		{"Windows and tiling",
			{"border", "shadow", "accent_border", "window_radius", "screen_corners", "titlebar_height", "show_titles",
				"title_align", "buttons_side", "button_style", "show_minimize", "show_maximize",
				"apps_follow_buttons", "double_click", "middle_click", "placement", "edge_snap",
				"focus_follows_mouse", "tile_gap", "snap_bar", "snap_layouts", "csd_radius"}, {}, false},
		{"Panels, their widgets and pinned apps",
			{"panels", "clock_24h", "show_date", "clock_click", "week_start", "battery_style", "menu_icon",
				"menu_icon_custom"},
			{"panel.", "panel_"}, false},
		{"Desktop and clipboard", {"desktop_icons", "middle_click_paste"}, {"clipboard_", "desktop_"}, false},
		{"Workspaces and the overview", {"super_key", "hot_corner"}, {"workspace", "overview_"}, false},
		{"Phone & tablet", {}, {"phone_"}, false},
		{"Control centre (tiles, night light)", {"dnd"}, {"cc_", "night_light"}, false},
		{"Notifications", {"notifications", "toast_over_fullscreen", "notif_group"}, {"notification_"}, false},
		{"Animations", {"animations", "reduce_motion"}, {"anim_"}, false},
		{"On-screen display", {"volume_step", "brightness_step"}, {"osd"}, false},
		{"Keyboard: layouts and your shortcuts", {}, {"keyboard_", "repeat_", "shortcut."}, true},
		{"Screenshots", {}, {"screenshot_"}, false},
		{"Mouse and touchpad",
			{"tap_to_click", "pointer_speed", "left_handed", "accel_profile", "disable_while_typing",
				"click_method", "middle_emulation"}, {"natural_scroll_", "gesture_"}, false},
		{"Displays: resolution, scale, arrangement", {"primary_output", "edge_resistance"},
			{"output.", "scale."}, true},
		{"Apps and power: terminal, startup, sleep",
			{"terminal", "autostart", "battery_warnings", "menu_power", "lock_command", "idle_fullscreen"},
			{"screen_off_", "sleep_", "power_"}, false},
	};
	return groups;
}

int group_of(const QString &key) {
	const auto &groups = setting_groups();
	for (size_t i = 0; i < groups.size(); i++) {
		if (groups[i].exact.contains(key)) return static_cast<int>(i);
		for (const QString &p : groups[i].prefixes) {
			if (key.startsWith(p)) return static_cast<int>(i);
		}
	}
	return -1;   // anything else (e.g. "show more settings") is always reset
}

void reset_everything(QWidget *parent) {
	QDialog dialog(parent);
	dialog.setWindowTitle("Reset settings");
	dialog.resize(520, 600);
	auto *v = new QVBoxLayout(&dialog);
	v->setContentsMargins(24, 20, 24, 18);
	v->setSpacing(12);
	auto *title = new QLabel("Reset all settings?");
	QFont f = title->font();
	f.setPointSizeF(f.pointSizeF() * 1.3);
	f.setBold(true);
	title->setFont(f);
	auto *text = new QLabel("Everything goes back to how Dusk was when you first started it. "
		"Switch on what you'd like to keep as it is now.");
	text->setObjectName("subtitle");
	text->setWordWrap(true);
	v->addWidget(title);
	v->addWidget(text);

	auto *frame = new QFrame;
	frame->setObjectName("card");
	auto *rows = new QVBoxLayout(frame);
	rows->setContentsMargins(16, 6, 12, 6);
	rows->setSpacing(0);
	std::vector<Switch *> keeps;
	const auto &groups = setting_groups();
	for (size_t i = 0; i < groups.size(); i++) {
		auto *row = new QWidget;
		auto *h = new QHBoxLayout(row);
		h->setContentsMargins(0, 7, 0, 7);
		h->setSpacing(12);
		auto *label = new QLabel(groups[i].title);
		label->setWordWrap(true);
		auto *keep = new QLabel("Keep");
		keep->setObjectName("subtitle");
		auto *sw = new Switch;
		sw->set_quietly(groups[i].keep);
		keeps.push_back(sw);
		h->addWidget(label, 1);
		h->addWidget(keep);
		h->addWidget(sw);
		if (i > 0) {
			auto *div = new QFrame;
			div->setObjectName("divider");
			rows->addWidget(div);
		}
		rows->addWidget(row);
	}
	auto *scroll = new QScrollArea;
	scroll->setWidget(frame);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	v->addWidget(scroll, 1);

	auto *buttons = new QHBoxLayout;
	auto *cancel = new QPushButton("Cancel");
	auto *go = new QPushButton("Reset");
	go->setObjectName("danger");
	buttons->addStretch(1);
	buttons->addWidget(cancel);
	buttons->addWidget(go);
	v->addLayout(buttons);
	QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
	QObject::connect(go, &QPushButton::clicked, &dialog, &QDialog::accept);
	cancel->setDefault(true);
	if (dialog.exec() != QDialog::Accepted) {
		return;
	}
	bool keep_desktop = false;
	for (const QString &key : settings->keys()) {
		const int g = group_of(key);
		if (g >= 0 && keeps[static_cast<size_t>(g)]->isChecked()) continue;
		settings->remove(key);
	}
	for (size_t i = 0; i < groups.size(); i++) {
		if (QString(groups[i].title).startsWith("Desktop")) keep_desktop = keeps[i]->isChecked();
	}
	if (!keep_desktop) {
		// Where the desktop icons were put.
		QFile::remove(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
			"/dusk/desktop-icons");
	}
	settings->save();
	apply_look(*the_app);
	if (panels_page) panels_page->refresh_from_file();
	refresh_all();
}

// ---- About ----------------------------------------------------------------------------

// ---- About ------------------------------------------------------------------------------

std::function<void(const QString &)> g_open_page;   // goes to a page by its id

QPixmap svg_pixmap(const char *svg, int px) {
	QByteArray bytes(svg);
	QBuffer buf(&bytes);
	buf.open(QIODevice::ReadOnly);
	QImageReader reader(&buf, "svg");
	reader.setScaledSize(QSize(px, px));
	const QImage img = reader.read();
	return img.isNull() ? QPixmap() : QPixmap::fromImage(img);
}

QIcon settings_icon() {
	QIcon themed = QIcon::fromTheme("org.dusk.Settings");
	if (!themed.isNull()) return themed;
	QIcon icon;
	for (int px : {16, 24, 32, 48, 64, 128, 256}) icon.addPixmap(svg_pixmap(kSettingsIconSvg, px));
	return icon;
}

QString read_first_line(const QString &path) {
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
	return QString::fromUtf8(f.readLine()).trimmed();
}

// What the computer is, in plain words (like a phone's "About phone").
struct MachineInfo {
	QString name, model, cpu, gpu, memory, storage, system;
};

MachineInfo machine_info() {
	MachineInfo m;
	m.name = run_tool("hostnamectl", {"--pretty"}, 800);
	if (m.name.isEmpty()) m.name = QSysInfo::machineHostName();
	const QString vendor = read_first_line("/sys/class/dmi/id/sys_vendor");
	QString product = read_first_line("/sys/class/dmi/id/product_name");
	const QString version = read_first_line("/sys/class/dmi/id/product_version");
	// Lenovo puts the name people know in product_version.
	if (vendor.startsWith("LENOVO", Qt::CaseInsensitive) && !version.isEmpty()) product = version;
	m.model = vendor.isEmpty() ? product : product.startsWith(vendor, Qt::CaseInsensitive) ? product
		: vendor + " " + product;
	{
		QFile f("/proc/cpuinfo");
		int threads = 0;
		if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
			for (const QString &line : QString::fromUtf8(f.readAll()).split('\n')) {
				if (line.startsWith("model name") && m.cpu.isEmpty()) m.cpu = line.section(':', 1).trimmed();
				if (line.startsWith("processor")) threads++;
			}
		}
		m.cpu.replace("(R)", "").replace("(TM)", "").replace(" CPU", "").replace(QRegularExpression("\\s+@.*"), "");
		m.cpu = m.cpu.simplified();
		if (threads > 0) m.cpu += QString("  ·  %1 threads").arg(threads);
	}
	{
		QStringList gpus;
		{
			for (const QString &line : run_tool("lspci", {}, 1500).split('\n')) {
				if (!line.contains("VGA compatible") && !line.contains("3D controller") && !line.contains("Display controller")) continue;
				QString d = line.section(": ", 1).trimmed();
				d.remove(QRegularExpression("\\s*\\(rev [0-9a-f]+\\)$"));
				// "NVIDIA Corporation TU116M [GeForce GTX 1660 Ti Mobile]" -> "NVIDIA GeForce GTX 1660 Ti Mobile"
				const QRegularExpressionMatch br = QRegularExpression("\\[([^\\]]+)\\]\\s*$").match(d);
				const QString maker = d.section(' ', 0, 0).replace("Advanced", "AMD");
				if (br.hasMatch()) d = maker + " " + br.captured(1);
				d.replace(" Corporation", "");
				gpus << d;
			}
		}
		m.gpu = gpus.join("\n");
	}
	{
		QFile f("/proc/meminfo");
		if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
			const QString line = QString::fromUtf8(f.readLine());
			const double kb = line.section(':', 1).trimmed().section(' ', 0, 0).toDouble();
			m.memory = QString("%1 GB").arg(std::round(kb / 1024 / 1024));
		}
	}
	{
		// The whole drive the system is on (not just its partition), and the
		// other drives inside the computer: "512 GB" or "512 GB + 1 TB".
		auto pretty = [](double bytes) {
			const double gb = bytes / 1e9;
			return gb >= 1000 ? QString("%1 TB").arg(gb / 1000, 0, 'f', gb >= 10000 ? 0 : 1).replace(".0 ", " ")
				: QString("%1 GB").arg(std::round(gb));
		};
		const QStorageInfo root = QStorageInfo::root();
		QString system_disk;
		{
			// The disk under the system partition (through LUKS or LVM too).
			{
				for (const QString &line : run_tool("lsblk", {"-bnso", "NAME,TYPE", QString::fromUtf8(root.device())}, 1500)
						.split('\n')) {
					const QStringList f = line.simplified().split(' ');
					if (f.size() == 2 && f[1] == "disk") system_disk = QString(f[0]).remove(QRegularExpression("^[^a-zA-Z0-9]+"));
				}
			}
		}
		QStringList drives;
		QString first;
		{
			{
				for (const QString &line : run_tool("lsblk", {"-bdno", "NAME,SIZE,TYPE,RM"}, 1500).split('\n')) {
					const QStringList f = line.simplified().split(' ');
					// Real drives only: not zram, loop devices or USB sticks.
					if (f.size() < 4 || f[2] != "disk" || f[3] == "1" || f[0].startsWith("zram") || f[0].startsWith("loop")) continue;
					const QString size = pretty(f[1].toDouble());
					if (f[0] == system_disk) first = size;
					else drives << size;
				}
			}
		}
		if (!first.isEmpty()) drives.prepend(first);
		// Free space on every partition of the computer's own drives (each
		// counted once: btrfs mounts the same one at / and /home).
		double free_bytes = 0;
		QSet<QString> counted;
		for (const QStorageInfo &v : QStorageInfo::mountedVolumes()) {
			const QString dev = QString::fromUtf8(v.device());
			if (!v.isValid() || !v.isReady() || v.isReadOnly() || !dev.startsWith("/dev/") ||
					dev.startsWith("/dev/loop") || dev.startsWith("/dev/zram") || counted.contains(dev)) continue;
			const QString mount = v.rootPath();
			if (mount.startsWith("/run/media") || mount.startsWith("/media") || mount.startsWith("/boot")) continue;
			counted.insert(dev);
			free_bytes += static_cast<double>(v.bytesAvailable());
		}
		if (free_bytes <= 0) free_bytes = static_cast<double>(root.bytesAvailable());
		const QString free = pretty(free_bytes) + " free";
		m.storage = drives.isEmpty() ? free : drives.join(" + ") + "  ·  " + free;
	}
	{
		QFile f("/etc/os-release");
		if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
			for (const QString &line : QString::fromUtf8(f.readAll()).split('\n')) {
				if (line.startsWith("PRETTY_NAME=")) m.system = line.mid(12).remove('"');
			}
		}
	}
	return m;
}

// A row that opens to show more (like "Software information" on a phone).
QVBoxLayout *disclosure(Card *c, const QString &title, const QString &subtitle) {
	auto *body = new QWidget;
	auto *v = new QVBoxLayout(body);
	v->setContentsMargins(0, 0, 0, 10);
	v->setSpacing(0);
	body->hide();
	auto *chev = new QLabel("›");
	chev->setObjectName("subtitle");
	QFont f = chev->font();
	f.setPointSizeF(f.pointSizeF() * 1.6);
	chev->setFont(f);
	QWidget *row = make_row(c, title, subtitle, chev, nullptr);
	row->setCursor(Qt::PointingHandCursor);
	row->layout()->addWidget(body);
	// Clicking anywhere on the row opens or closes it.
	class Click : public QObject {
	public:
		std::function<void()> fn;
		bool eventFilter(QObject *, QEvent *e) override {
			if (e->type() == QEvent::MouseButtonRelease) { fn(); return true; }
			return false;
		}
	};
	auto *click = new Click;
	click->setParent(row);
	click->fn = [body, chev] {
		body->setVisible(!body->isVisible());
		chev->setText(body->isVisible() ? "⌄" : "›");
	};
	for (QObject *o : row->children()) {
		if (auto *w = qobject_cast<QWidget *>(o); w && w != body) w->installEventFilter(click);
	}
	row->installEventFilter(click);
	return v;
}

// A label on the left, its value on the right (selectable).
void info_line(QVBoxLayout *into, const QString &label, const QString &value) {
	auto *w = new QWidget;
	auto *h = new QHBoxLayout(w);
	h->setContentsMargins(0, 5, 0, 5);
	auto *l = new QLabel(label);
	l->setObjectName("subtitle");
	auto *val = new QLabel(value);
	val->setTextInteractionFlags(Qt::TextSelectableByMouse);
	val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	val->setWordWrap(true);
	h->addWidget(l, 0, Qt::AlignTop);
	h->addWidget(val, 1);
	into->addWidget(w);
}

// Runs a command in a terminal window (so you can see it and type your
// password): your terminal from Default apps, else the first one installed.
bool run_in_terminal(const QString &command) {
	const QString held = command + "; echo; read -r -p 'Press Enter to close this window. ' _";
	QString chosen = settings->get("terminal").trimmed();
	struct Term {
		const char *exe;
		QStringList before;   // what goes before the command
	};
	const Term kTerms[] = {
		{"ptyxis", {"--"}}, {"gnome-terminal", {"--"}}, {"kgx", {"-e"}}, {"konsole", {"-e"}},
		{"kitty", {}}, {"alacritty", {"-e"}}, {"wezterm", {"start", "--"}}, {"ghostty", {"-e"}},
		{"foot", {}}, {"tilix", {"-e"}}, {"xfce4-terminal", {"-x"}}, {"xterm", {"-e"}},
	};
	QStringList cmd;
	if (!chosen.isEmpty()) {
		const QString exe = chosen.split(' ', Qt::SkipEmptyParts).value(0);
		for (const Term &t : kTerms) {
			if (exe == t.exe) {
				cmd = chosen.split(' ', Qt::SkipEmptyParts) + t.before;
				break;
			}
		}
		if (cmd.isEmpty()) cmd = chosen.split(' ', Qt::SkipEmptyParts) << "-e";
	} else {
		for (const Term &t : kTerms) {
			if (!QStandardPaths::findExecutable(t.exe).isEmpty()) {
				cmd = QStringList{t.exe} + t.before;
				break;
			}
		}
	}
	if (cmd.isEmpty()) return false;
	cmd << "bash" << "-c" << held;
	return QProcess::startDetached(cmd.takeFirst(), cmd);
}

// Versions as numbers: is a newer than b? ("0.21.10" > "0.21.9")
bool newer_version(const QString &a, const QString &b) {
	const QStringList x = a.split('.'), y = b.split('.');
	for (int i = 0; i < std::max(x.size(), y.size()); i++) {
		const int p = i < x.size() ? x[i].toInt() : 0, q = i < y.size() ? y[i].toInt() : 0;
		if (p != q) return p > q;
	}
	return false;
}

// What's new: "version<TAB>headline" lines the installer writes (newest first).
QList<QPair<QString, QString>> whats_new_list() {
	QList<QPair<QString, QString>> out;
	QFile f("/usr/local/share/dusk/whatsnew.txt");
	if (!f.open(QIODevice::ReadOnly)) return out;
	for (const QString &line : QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts)) {
		const int tab = line.indexOf('\t');
		if (tab > 0) out.append({line.left(tab), line.mid(tab + 1)});
	}
	return out;
}

// The Dusk installed now (the installer notes it), else this Settings' own.
QString installed_version() {
	for (const QString &path : {QDir::homePath() + "/.local/state/dusk/installed", QString("/usr/local/share/dusk/version")}) {
		QFile f(path);
		if (f.open(QIODevice::ReadOnly)) {
			const QString v = QString::fromUtf8(f.readLine()).trimmed();
			if (!v.isEmpty()) return v;
		}
	}
	return kVersion;
}

// What a zip would bring: the headlines of its README's "What's new in X"
// sections newer than the version you have, newest first.
QList<QPair<QString, QString>> news_in_zip(const QString &zip, const QString &have) {
	QList<QPair<QString, QString>> out;
	QProcess u;
	u.start("unzip", {"-p", zip, "*README.md"});
	if (!u.waitForFinished(6000)) return out;
	const QString text = QString::fromUtf8(u.readAllStandardOutput());
	static const QRegularExpression head("^## What's new in ([0-9.]+)\\s*$");
	static const QRegularExpression bullet("^- \\*\\*([^*]+)\\*\\*");
	QString current;
	for (const QString &line : text.split('\n')) {
		const QRegularExpressionMatch h = head.match(line);
		if (h.hasMatch()) {
			current = h.captured(1);
			if (!newer_version(current, have)) break;   // (newest first: the rest are older)
			continue;
		}
		if (current.isEmpty()) continue;
		const QRegularExpressionMatch b = bullet.match(line);
		if (b.hasMatch()) {
			QString t = b.captured(1).trimmed();
			while (t.endsWith(':') || t.endsWith('.')) t.chop(1);
			out.append({current, t});
		}
	}
	return out;
}

// A short list of them, as rich text: "• headline", at most `most`, then "and N more".
QString whats_new_html(const QList<QPair<QString, QString>> &list, int most) {
	const Look l = current_look();
	QString html;
	for (int i = 0; i < list.size() && i < most; i++) {
		html += QString("<div style='margin:2px 0'><span style='color:%1'>●</span>&nbsp;&nbsp;%2</div>")
			.arg(l.accent.name(), list[i].second.toHtmlEscaped());
	}
	if (list.size() > most) {
		html += QString("<div style='color:%1; margin-top:4px'>and %2 more</div>").arg(l.dim.name()).arg(list.size() - most);
	}
	return html;
}

// Is this the right password? (sudo checks it: -S reads it, -k so an earlier
// "remembered" one doesn't count, -v does nothing else.)
bool password_ok(const QString &pw) {
	QProcess p;
	p.start("sudo", {"-S", "-k", "-v", "-p", ""});
	if (!p.waitForStarted(3000)) return false;
	p.write((pw + "\n").toUtf8());
	p.closeWriteChannel();
	if (!p.waitForFinished(15000)) {
		p.kill();
		return false;
	}
	return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

// Needs no password at all? (sudo set up without one, NOPASSWD)
bool sudo_needs_no_password() {
	QProcess p;
	p.start("sudo", {"-n", "-k", "true"});
	return p.waitForFinished(4000) && p.exitCode() == 0;
}

// The password, asked for once when Dusk installs, in Dusk's own look: the
// logo, what's being installed, the field with a show/hide eye, a Caps Lock
// hint. Install checks it first (wrong: says so here, and you try again);
// every sudo during the install then gets the same one, so it's asked once.
// Empty when cancelled.
QString ask_password(QWidget *parent, const QString &what) {
	const Look l = current_look();
	QDialog dlg(parent);
	dlg.setWindowTitle("Install Dusk");
	dlg.setWindowIcon(QIcon(svg_pixmap(kDuskLogoSvg, 128)));
	dlg.setMinimumWidth(400);
	auto *v = new QVBoxLayout(&dlg);
	v->setContentsMargins(28, 26, 28, 22);
	v->setSpacing(0);

	auto *logo = new QLabel;
	const qreal dpr = dlg.devicePixelRatioF();
	QPixmap pm = svg_pixmap(kDuskLogoSvg, static_cast<int>(64 * dpr));
	pm.setDevicePixelRatio(dpr);
	logo->setPixmap(pm);
	logo->setAlignment(Qt::AlignHCenter);
	v->addWidget(logo);
	v->addSpacing(14);
	auto *title = new QLabel(what);
	QFont tf = title->font();
	tf.setPointSizeF(tf.pointSizeF() * 1.35);
	tf.setWeight(QFont::DemiBold);
	title->setFont(tf);
	title->setAlignment(Qt::AlignHCenter);
	v->addWidget(title);
	v->addSpacing(6);
	auto *say = new QLabel("Enter your password to put it in place.\nIt's the one you log in with, asked for just once.");
	say->setObjectName("hint");
	say->setAlignment(Qt::AlignHCenter);
	say->setWordWrap(true);
	v->addWidget(say);
	v->addSpacing(18);

	auto *field = new QLineEdit;
	field->setEchoMode(QLineEdit::Password);
	field->setPlaceholderText("Password");
	field->setMinimumHeight(40);
	auto field_style = [field, l](bool wrong) {
		field->setStyleSheet(QString("QLineEdit { background:%1; color:%2; border:2px solid %3; border-radius:12px; "
			"padding:0 12px; font-size: 12pt; } QLineEdit:focus { border-color:%4; }")
			.arg(l.field.name(), l.text.name(), wrong ? QString("#f85149") : css_rgba(l.line),
				wrong ? QString("#f85149") : l.accent.name()));
	};
	field_style(false);
	QAction *eye = field->addAction(QIcon::fromTheme("view-reveal-symbolic", QIcon::fromTheme("view-visible")),
		QLineEdit::TrailingPosition);
	eye->setCheckable(true);
	eye->setToolTip("Show the password");
	QObject::connect(eye, &QAction::toggled, field, [field, eye](bool on) {
		field->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
		eye->setIcon(QIcon::fromTheme(on ? "view-conceal-symbolic" : "view-reveal-symbolic",
			QIcon::fromTheme(on ? "view-hidden" : "view-visible")));
		eye->setToolTip(on ? "Hide the password" : "Show the password");
	});
	v->addWidget(field);

	auto *note = new QLabel("That password wasn't right. Try again.");
	note->setWordWrap(true);
	note->setContentsMargins(4, 8, 4, 0);
	note->setStyleSheet("color:#f85149; font-weight:600;");
	note->hide();
	v->addWidget(note);
	auto *caps = new QLabel("Caps Lock may be on");
	caps->setObjectName("hint");
	caps->setContentsMargins(4, 6, 4, 0);
	caps->hide();
	v->addWidget(caps);
	QObject::connect(field, &QLineEdit::textEdited, caps, [caps, note, field_style](const QString &t) {
		note->hide();
		field_style(false);
		if (t.isEmpty()) { caps->hide(); return; }
		const QChar c = t.back();
		const bool shift = QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;
		if (c.isLetter()) caps->setVisible(c.isUpper() != shift);
	});
	v->addSpacing(20);

	auto *buttons = new QHBoxLayout;
	buttons->setSpacing(10);
	auto *cancel = new QPushButton("Cancel");
	auto *ok = new QPushButton("Install");
	ok->setObjectName("primary");
	ok->setDefault(true);
	ok->setEnabled(false);
	for (QPushButton *b : {cancel, ok}) {
		b->setCursor(Qt::PointingHandCursor);
		b->setMinimumHeight(36);
	}
	buttons->addWidget(cancel, 1);
	buttons->addWidget(ok, 1);
	v->addLayout(buttons);
	QString result;
	auto submit = [&] {
		if (field->text().isEmpty()) return;
		ok->setEnabled(false);
		ok->setText("Checking…");
		field->setEnabled(false);
		QCoreApplication::processEvents();
		const bool right = password_ok(field->text());
		ok->setText("Install");
		field->setEnabled(true);
		if (right) {
			result = field->text();
			dlg.accept();
			return;
		}
		note->show();
		field_style(true);
		field->selectAll();
		field->setFocus();
		ok->setEnabled(true);
	};
	QObject::connect(field, &QLineEdit::textChanged, ok, [ok](const QString &t) { ok->setEnabled(!t.isEmpty()); });
	QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
	QObject::connect(ok, &QPushButton::clicked, &dlg, submit);
	QObject::connect(field, &QLineEdit::returnPressed, &dlg, submit);
	field->setFocus();
	dlg.exec();
	return result;
}

// sudo's SUDO_ASKPASS during an install: the password Settings was given
// (in a private file only you can read, gone when the install ends), or,
// if that's missing, the box itself.
int askpass_dialog() {
	const QString file = qEnvironmentVariable("DUSK_PW_FILE");
	if (!file.isEmpty()) {
		QFile f(file);
		if (f.open(QIODevice::ReadOnly)) {
			std::fputs(f.readAll().constData(), stdout);
			std::fputs("\n", stdout);
			std::fflush(stdout);
			return 0;
		}
	}
	const QString pw = ask_password(nullptr, qEnvironmentVariable("DUSK_INSTALLING", "Installing Dusk"));
	if (pw.isEmpty()) return 1;
	std::fputs(pw.toUtf8().constData(), stdout);
	std::fputs("\n", stdout);
	std::fflush(stdout);
	return 0;
}

// Updates: from Dusk's GitHub repository, or a zip you have (the newest one
// in Downloads, or one you pick), with dusk-update (./build.sh installs
// it). It runs right here in Settings: the steps and a bar as it builds,
// the password asked for in a small dialog, the details if you want them,
// and Log out now at the end. Or, as before, in a terminal window.
void updates_card(Page &page) {
	Card *c = card(page, "Updates", "New versions come from GitHub (lance-ui/dusk), "
		"or from a Dusk zip you've downloaded.");
	auto *box = new QWidget;
	auto *h = new QHBoxLayout(box);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(8);
	auto *check = new QPushButton("Check for updates");
	auto *update = new QPushButton("Update now");
	update->setObjectName("primary");
	auto *versions = new QPushButton("Other versions");
	versions->setCheckable(true);
	for (QPushButton *b : {check, update, versions}) b->setCursor(Qt::PointingHandCursor);
	update->hide();
	versions->hide();   // (going back to other versions isn't offered any more)
	h->addWidget(check);
	h->addWidget(update);
	QWidget *row = make_row(c, QString("Dusk %1").arg(kVersion), "Installed", box, nullptr);
	QLabel *sub = row->findChild<QLabel *>("subtitle");

	// Where a check got to: a mark and a line, in colour.
	auto *status = new QLabel;
	status->setTextFormat(Qt::RichText);
	status->setWordWrap(true);
	status->setContentsMargins(4, 6, 4, 6);
	status->hide();
	c->add(status);
	enum class State { Busy, Good, New, Bad, Info };
	auto show_status = [status](State st, const QString &text) {
		const Look l = current_look();
		QString mark, colour;
		switch (st) {
		case State::Busy: mark = "⟳"; colour = l.dim.name(); break;
		case State::Good: mark = "✓"; colour = "#3fb950"; break;
		case State::New: mark = "★"; colour = l.accent.name(); break;
		case State::Bad: mark = "✕"; colour = "#f85149"; break;
		case State::Info: mark = "›"; colour = l.text.name(); break;
		}
		status->setText(QString("<span style='color:%1; font-size:13pt; font-weight:700'>%2</span>&nbsp;&nbsp;"
			"<span style='color:%3; font-weight:600'>%4</span>").arg(colour, mark, l.text.name(), text.toHtmlEscaped()));
		status->show();
	};

	// ---- The install, in Settings ----
	auto *run = new QFrame;
	run->setObjectName("phonerow");
	auto *rv = new QVBoxLayout(run);
	rv->setContentsMargins(16, 14, 16, 14);
	rv->setSpacing(8);
	auto *run_top = new QHBoxLayout;
	auto *stage = new QLabel("Getting ready…");
	QFont sf = stage->font();
	sf.setWeight(QFont::DemiBold);
	stage->setFont(sf);
	auto *pct = new QLabel;
	pct->setObjectName("hint");
	run_top->addWidget(stage, 1);
	run_top->addWidget(pct);
	rv->addLayout(run_top);
	auto *bar = new QProgressBar;
	bar->setRange(0, 1000);
	bar->setTextVisible(false);
	bar->setFixedHeight(8);
	{
		const Look l = current_look();
		bar->setStyleSheet(QString("QProgressBar { background:%1; border:none; border-radius:4px; }"
			"QProgressBar::chunk { border-radius:4px; background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
			" stop:0 %2, stop:1 %2); }").arg(l.field.name(), l.accent.name()));
	}
	rv->addWidget(bar);
	// The steps, ticked off as they finish (what it's doing is never hidden).
	auto *steps_view = new QLabel;
	steps_view->setTextFormat(Qt::RichText);
	steps_view->setWordWrap(true);
	steps_view->hide();
	rv->addWidget(steps_view);
	auto step_titles = std::make_shared<QStringList>();
	auto *detail = new QLabel;
	detail->setObjectName("hint");
	detail->setWordWrap(false);
	detail->setTextFormat(Qt::PlainText);
	rv->addWidget(detail);
	// When it's done: what's new, in a few lines.
	auto *news_head = new QLabel("WHAT'S NEW");
	news_head->setObjectName("hint");
	news_head->setContentsMargins(0, 6, 0, 0);
	news_head->hide();
	rv->addWidget(news_head);
	auto *news = new QLabel;
	news->setTextFormat(Qt::RichText);
	news->setWordWrap(true);
	news->hide();
	rv->addWidget(news);
	auto *log = new QPlainTextEdit;
	log->setReadOnly(true);
	log->setMaximumBlockCount(3000);
	log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	log->setMinimumHeight(170);
	rv->addWidget(log);
	auto *run_btns = new QHBoxLayout;
	auto *more = new QPushButton("Hide the output");
	more->setCheckable(true);
	more->setChecked(true);
	auto *copy_log = new QPushButton("Copy the log");
	copy_log->hide();
	auto *cancel = new QPushButton("Cancel");
	auto *logout = new QPushButton("Log out now");
	logout->setObjectName("primary");
	logout->hide();
	for (QPushButton *b : {more, copy_log, cancel, logout}) b->setCursor(Qt::PointingHandCursor);
	run_btns->addWidget(more);
	run_btns->addWidget(copy_log);
	run_btns->addStretch(1);
	run_btns->addWidget(cancel);
	run_btns->addWidget(logout);
	rv->addLayout(run_btns);
	run->hide();
	c->add(run);
	QObject::connect(more, &QPushButton::toggled, [more, log](bool on) {
		log->setVisible(on);
		more->setText(on ? "Hide the output" : "Show the output");
	});
	QObject::connect(copy_log, &QPushButton::clicked, [log] { QGuiApplication::clipboard()->setText(log->toPlainText()); });
	QObject::connect(logout, &QPushButton::clicked, [] {
		QProcess::startDetached("sh", {"-c", "pkill -TERM -u \"$(id -u)\" -x dusk"});
	});

	// The versions, opened in the card.
	auto *list = new QFrame;
	list->setObjectName("phonerow");
	auto *lv = new QVBoxLayout(list);
	lv->setContentsMargins(14, 10, 14, 10);
	lv->setSpacing(0);
	list->hide();
	c->add(list);

	// A newer zip in Downloads (the way new versions often come).
	auto *dl_row = new QFrame;
	dl_row->setObjectName("phonerow");
	auto *dh = new QHBoxLayout(dl_row);
	dh->setContentsMargins(14, 10, 14, 10);
	auto *dl_text = new QLabel;
	dl_text->setWordWrap(true);
	dl_text->setTextFormat(Qt::RichText);
	auto *dl_go = new QPushButton("Install");
	dl_go->setObjectName("primary");
	dl_go->setCursor(Qt::PointingHandCursor);
	dh->addWidget(dl_text, 1);
	dh->addWidget(dl_go);
	dl_row->hide();
	c->add(dl_row);

	const bool have_tool = !QStandardPaths::findExecutable("dusk-update").isEmpty();
	if (!have_tool) {
		check->setEnabled(false);
		versions->setEnabled(false);
		if (sub) sub->setText("Install Dusk with ./build.sh install once to update from here");
	}

	// ---- Running it ----
	struct Job {
		QProcess *proc = nullptr;
		QString buf, version, failed;
		bool done = false;
		int step = 0, steps = 3;
	};
	auto job = std::make_shared<Job>();
	auto set_progress = [bar, pct](double f) {
		bar->setValue(static_cast<int>(std::clamp(f, 0.0, 1.0) * 1000));
		pct->setText(QString("%1%").arg(static_cast<int>(std::clamp(f, 0.0, 1.0) * 100)));
	};
	// Each step's share of the bar: packages, building, installing.
	auto step_span = [job](int n) -> std::pair<double, double> {
		if (job->steps == 3) {
			if (n <= 1) return {0.0, 0.12};
			if (n == 2) return {0.12, 0.92};
			return {0.92, 1.0};
		}
		return {static_cast<double>(n - 1) / job->steps, static_cast<double>(n) / job->steps};
	};
	auto handle_line = [=](const QString &raw) {
		QString line = raw;
		line.remove(QRegularExpression("\\x1b\\[[0-9;]*[A-Za-z]"));
		if (line.startsWith("@@step ")) {
			const QStringList f = line.mid(7).split(' ');
			if (f.size() >= 3) {
				job->step = f[0].toInt();
				job->steps = std::max(1, f[1].toInt());
				const QString title = f.mid(2).join(' ');
				stage->setText(QString("Step %1 of %2 · %3").arg(job->step).arg(job->steps).arg(title));
				while (step_titles->size() < job->step) step_titles->append(QString());
				(*step_titles)[job->step - 1] = title;
				const Look l = current_look();
				QString html;
				for (int k = 0; k < job->steps; k++) {
					const QString t = k < step_titles->size() && !(*step_titles)[k].isEmpty() ? (*step_titles)[k]
						: QString("Step %1").arg(k + 1);
					const bool done = k + 1 < job->step, now = k + 1 == job->step;
					html += QString("<div style='margin:2px 0'><span style='color:%1; font-weight:700'>%2</span>"
						"&nbsp;&nbsp;<span style='color:%3%4'>%5</span></div>")
						.arg(done ? "#3fb950" : now ? l.accent.name() : l.dim.name(), done ? "✓" : now ? "●" : "○",
							now || done ? l.text.name() : l.dim.name(), now ? "; font-weight:600" : "", t.toHtmlEscaped());
				}
				steps_view->setText(html);
				steps_view->show();
				set_progress(step_span(job->step).first);
			}
			return;
		}
		if (line.startsWith("@@installed ")) {
			job->version = line.mid(12).trimmed();
			job->done = true;
			return;
		}
		if (line.startsWith("@@failed ")) {
			job->failed = line.mid(9).trimmed();
			return;
		}
		log->appendPlainText(line);
		// ninja: "[12/180] Compiling C++ object dusk.p/src_panel.cpp.o"
		static const QRegularExpression ninja("^\\[(\\d+)/(\\d+)\\]\\s+(.*)$");
		const QRegularExpressionMatch m = ninja.match(line);
		if (m.hasMatch()) {
			const double n = m.captured(1).toDouble(), t = std::max(1.0, m.captured(2).toDouble());
			const auto [a, b] = step_span(job->step);
			set_progress(a + (b - a) * n / t);
			QString what = m.captured(3).section(' ', -1).section('/', -1);
			what.remove(QRegularExpression("\\.(o|p)$"));
			detail->setText(QString("Compiling %1 · %2 of %3").arg(what).arg(m.captured(1), m.captured(2)));
			return;
		}
		const QString t = line.trimmed();
		if (t.startsWith("›") || t.startsWith("✓") || t.startsWith("!")) {
			QString shown = t.mid(1).trimmed();
			if (shown.size() > 90) shown = shown.left(88) + "…";
			detail->setText(shown);
		}
	};
	auto read_out = [job, handle_line] {
		job->buf += QString::fromUtf8(job->proc->readAll());
		job->buf.replace('\r', '\n');
		int nl;
		while ((nl = job->buf.indexOf('\n')) >= 0) {
			const QString line = job->buf.left(nl);
			job->buf.remove(0, nl + 1);
			if (!line.trimmed().isEmpty()) handle_line(line);
		}
	};
	// A small program sudo asks for the password through (Settings' --askpass).
	auto askpass_path = [] {
		const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
		QDir().mkpath(dir);
		const QString path = dir + "/dusk-askpass";
		QFile f(path);
		if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			f.write(QString("#!/bin/sh\nexec '%1' --askpass \"$@\"\n").arg(QCoreApplication::applicationFilePath()).toUtf8());
			f.close();
			f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
		}
		return path;
	};
	auto in_terminal = [] { return settings->get("update_in_terminal", "false") == "true"; };
	// The private password file: written over, then removed, when the install ends.
	static QString job_pw_file;
	auto forget_password = [] {
		if (job_pw_file.isEmpty()) return;
		QFile f(job_pw_file);
		if (f.open(QIODevice::WriteOnly)) {
			f.write(QByteArray(64, '\0'));
			f.close();
		}
		f.remove();
		job_pw_file.clear();
	};
	QObject::connect(qApp, &QCoreApplication::aboutToQuit, [forget_password] { forget_password(); });

	auto controls = std::vector<QPushButton *>{check, update, versions, dl_go};
	auto start = [=](const QStringList &update_args, const QString &what) {
		if (job->proc) return;
		const QString cmd = "dusk-update " + update_args.join(' ');
		if (in_terminal()) {
			if (!run_in_terminal(cmd)) {
				show_status(State::Bad, "No terminal app found. Run  " + cmd + "  in one.");
				return;
			}
			show_status(State::Info, what + " in the terminal window. Log out and back in when it's done.");
			return;
		}
		const auto answer = QMessageBox::question(run->window(), what,
			what + "?\n\nDusk is built on this computer (a few minutes) and installed; your password is asked "
			"for once. Your settings stay as they are.\n\nDusk is written by an AI (Claude, by Anthropic): it may have "
			"bugs. Keep another desktop installed.", QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
		if (answer != QMessageBox::Yes) return;
		// The password, once, checked here (sudo is then given it each time it asks).
		QString pw_file;
		if (!sudo_needs_no_password()) {
			const QString pw = ask_password(run->window(), what);
			if (pw.isEmpty()) return;
			const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
			if (dir.isEmpty()) {
				// Never fall back to /tmp for a password file: if Settings dies
				// mid-install, the secret would sit in a world-listable directory.
				QMessageBox::warning(run->window(), what,
					"Cannot install: no private runtime directory is available.");
				return;
			}
			pw_file = dir + QString("/dusk-install-%1").arg(QCoreApplication::applicationPid());
			QFile f(pw_file);
			f.remove();
			if (f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
				f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
				f.write(pw.toUtf8());
				f.close();
			}
		}
		job_pw_file = pw_file;
		for (QPushButton *b : controls) b->setEnabled(false);
		versions->setChecked(false);
		dl_row->hide();
		status->hide();
		run->show();
		log->clear();
		news->hide();
		news_head->hide();
		stage->setText(what + "…");
		detail->setText(QString());
		set_progress(0);
		cancel->show();
		cancel->setEnabled(true);
		logout->hide();
		copy_log->hide();
		*job = Job{};
		auto *p = new QProcess(run);
		job->proc = p;
		QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
		env.insert("DUSK_PROGRESS", "1");
		env.insert("DUSK_YES", "1");
		env.insert("NO_COLOR", "1");
		env.insert("SUDO_ASKPASS", askpass_path());
		env.insert("DUSK_INSTALLING", what);
		if (!pw_file.isEmpty()) env.insert("DUSK_PW_FILE", pw_file);
		p->setProcessEnvironment(env);
		p->setProcessChannelMode(QProcess::MergedChannels);
		QObject::connect(p, &QProcess::readyRead, run, read_out);
		QObject::connect(p, &QProcess::finished, run, [=](int code, QProcess::ExitStatus how) {
			read_out();
			if (!job->buf.trimmed().isEmpty()) handle_line(job->buf);
			forget_password();
			job->proc = nullptr;
			p->deleteLater();
			for (QPushButton *b : controls) b->setEnabled(have_tool);
			cancel->hide();
			copy_log->show();
			if (code == 0 && how == QProcess::NormalExit && job->done) {
				set_progress(1);
				stage->setText(QString("Dusk %1 is installed").arg(job->version));
				steps_view->setText(steps_view->text().replace("●", "✓").replace("○", "✓"));
				detail->setText("Log out and back in to start it. Save your work first.");
				logout->show();
				show_status(State::Good, QString("Dusk %1 is installed. It starts the next time you log in.").arg(job->version));
				const auto list = whats_new_list();
				if (!list.isEmpty()) {
					news->setText(whats_new_html(list, 6));
					news_head->show();
					news->show();
				}
			} else if (code == 0 && how == QProcess::NormalExit) {
				// Nothing to do: the newest is the one installed.
				static const QRegularExpression have_re("You have Dusk (\\S+), the newest on GitHub is (\\S+)");
				const QRegularExpressionMatch m = have_re.match(log->toPlainText());
				const QString have = m.hasMatch() ? m.captured(1) : QString(kVersion);
				stage->setText("Already up to date");
				set_progress(1);
				detail->setText(QString("Dusk %1 is installed, the newest there is.%2").arg(have,
					have != kVersion ? QString(" Log out and back in to start it (this is still %1).").arg(kVersion) : QString()));
				if (have != kVersion) logout->show();
				show_status(State::Good, QString("You're up to date: Dusk %1.").arg(have));
				update->hide();
			} else {
				stage->setText(how == QProcess::CrashExit ? "Cancelled" : "It didn't work");
				detail->setText(job->failed.isEmpty() ? QString("See the details below.") : job->failed);
				if (how != QProcess::CrashExit) {
					more->setChecked(true);
					show_status(State::Bad, job->failed.isEmpty() ? QString("The update stopped. Nothing of the installed Dusk was changed "
						"unless it got to the last step.") : job->failed);
				}
			}
		});
		// In a session of its own, so Cancel stops everything it started.
		p->start("setsid", QStringList{"-w", "dusk-update"} + update_args);
		show_status(State::Busy, what + "…");
	};
	QObject::connect(cancel, &QPushButton::clicked, [job, cancel] {
		if (!job->proc) return;
		cancel->setEnabled(false);
		const qint64 pid = job->proc->processId();
		if (pid > 0) ::kill(static_cast<pid_t>(-pid), SIGTERM);
		job->proc->terminate();
	});

	auto install = [start](const QString &v, bool older) {
		if (v.isEmpty()) start({}, "Updating Dusk");
		else start({"--version", v}, older ? "Going back to Dusk " + v : "Installing Dusk " + v);
	};

	auto fill_list = [list, lv, install](const QString &have, const QStringList &found) {
		while (QLayoutItem *it = lv->takeAt(0)) {
			if (it->widget()) it->widget()->deleteLater();
			delete it;
		}
		auto *head = new QLabel(QString("ON GITHUB · %1 VERSION%2").arg(found.size()).arg(found.size() == 1 ? "" : "S"));
		head->setObjectName("hint");
		head->setContentsMargins(0, 2, 0, 8);
		lv->addWidget(head);
		const Look l = current_look();
		for (int i = 0; i < found.size(); i++) {
			const QString v = found[i];
			const bool is_have = v == have, is_newer = !is_have && newer_version(v, have);
			if (i > 0) {
				auto *div = new QFrame;
				div->setObjectName("divider");
				lv->addWidget(div);
			}
			auto *r = new QWidget;
			auto *rh = new QHBoxLayout(r);
			rh->setContentsMargins(0, 8, 0, 8);
			rh->setSpacing(10);
			auto *name = new QLabel(QString("Dusk %1").arg(v));
			QFont nf = name->font();
			nf.setWeight(QFont::DemiBold);
			name->setFont(nf);
			// The badge: Newer (accent), Installed (green), Older (grey).
			auto *badge = new QLabel(is_have ? "Installed" : is_newer ? "Newer" : "Older");
			const QColor bg = is_have ? QColor("#3fb950") : is_newer ? l.accent : l.dim;
			const QColor fg = is_have ? QColor("#ffffff") : is_newer ? l.on_accent : l.window;
			badge->setStyleSheet(QString("background:%1; color:%2; border-radius:9px; padding:2px 9px; font-size:9pt; font-weight:600;")
				.arg(bg.name(), fg.name()));
			rh->addWidget(name);
			rh->addWidget(badge);
			rh->addStretch(1);
			if (!is_have) {
				auto *go = new QPushButton(is_newer ? "Install" : "Go back to this");
				if (is_newer) go->setObjectName("primary");
				go->setCursor(Qt::PointingHandCursor);
				QObject::connect(go, &QPushButton::clicked, go, [install, v, is_newer] { install(v, !is_newer); });
				rh->addWidget(go);
			}
			lv->addWidget(r);
		}
		auto *note = new QLabel("Your settings stay as they are, whichever version you install.");
		note->setObjectName("hint");
		note->setWordWrap(true);
		note->setContentsMargins(0, 8, 0, 2);
		lv->addWidget(note);
		list->show();
	};

	QObject::connect(versions, &QPushButton::toggled, [versions, list, show_status, fill_list](bool on) {
		if (!on) {
			list->hide();
			versions->setText("Other versions");
			return;
		}
		versions->setText("Hide versions");
		versions->setEnabled(false);
		show_status(State::Busy, "Getting the list of versions from GitHub…");
		auto *p = new QProcess(versions);
		QObject::connect(p, &QProcess::finished, versions, [p, versions, show_status, fill_list](int code) {
			p->deleteLater();
			versions->setEnabled(true);
			const QStringList lines = QString::fromUtf8(p->readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
			QString have = kVersion;
			QStringList found;
			static const QRegularExpression num("^[0-9]+(\\.[0-9]+)*$");
			for (const QString &line : lines) {
				const QStringList f = line.split(' ', Qt::SkipEmptyParts);
				if (f.size() >= 2 && f[0] == "installed") have = f[1];
				else if (f.size() >= 2 && f[0] == "zip" && num.match(f[1]).hasMatch()) found << f[1];
			}
			if (code != 0 || found.isEmpty()) {
				show_status(State::Bad, "Couldn't get the list from GitHub. Are you online?");
				versions->setChecked(false);
				return;
			}
			show_status(State::Info, QString("%1 version%2 on GitHub. Pick one below.").arg(found.size()).arg(found.size() == 1 ? "" : "s"));
			fill_list(have, found);
		});
		p->start("dusk-update", {"--list"});
	});

	QObject::connect(check, &QPushButton::clicked, [check, update, show_status, status] {
		check->setEnabled(false);
		check->setText("Checking…");
		update->hide();
		show_status(State::Busy, "Checking GitHub for a newer Dusk…");
		auto *p = new QProcess(check);
		QObject::connect(p, &QProcess::finished, check, [p, check, update, show_status, status](int code) {
			p->deleteLater();
			check->setEnabled(true);
			check->setText("Check again");
			const QString out = QString::fromUtf8(p->readAllStandardOutput());
			const QRegularExpressionMatch m = QRegularExpression("latest\\s+(\\S+)").match(out);
			const QString when = QTime::currentTime().toString("HH:mm");
			if (code != 0 || !m.hasMatch()) {
				show_status(State::Bad, "Couldn't reach GitHub. Are you online? (checked at " + when + ")");
				return;
			}
			const QString latest = m.captured(1);
			const QString have = installed_version();
			if (newer_version(latest, have)) {
				show_status(State::New, QString("Dusk %1 is out (you have %2). Update to get it.").arg(latest, have));
				const QRegularExpressionMatch fm = QRegularExpression("^file (.+)$", QRegularExpression::MultilineOption).match(out);
				const auto coming = fm.hasMatch() ? news_in_zip(fm.captured(1).trimmed(), have) : QList<QPair<QString, QString>>{};
				if (!coming.isEmpty()) {
					status->setText(status->text() + "<div style='margin:8px 0 0 26px'>" + whats_new_html(coming, 6) + "</div>");
				}
				update->setText(QString("Update to %1").arg(latest));
				update->setEnabled(true);
				update->show();
			} else if (latest == have) {
				show_status(State::Good, "You're up to date: Dusk " + latest + " is the newest (checked at " + when + ")");
			} else {
				show_status(State::Good, QString("You're up to date. GitHub's newest is %1, older than yours (checked at %2)")
					.arg(latest, when));
			}
		});
		p->start("dusk-update", {"--check"});
	});
	QObject::connect(update, &QPushButton::clicked, [install] { install(QString(), false); });

	// ---- A zip you have ----
	auto zip_version_of = [](const QString &path) {
		QProcess u;
		u.start("sh", {"-c", "unzip -p \"$1\" '*meson.build' 2>/dev/null | sed -n \"s/^[[:space:]]*version: '\\([^']*\\)'.*/\\1/p\" | head -1",
			"sh", path});
		u.waitForFinished(4000);
		return QString::fromUtf8(u.readAllStandardOutput()).trimmed();
	};
	{
		QString dl = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
		QStringList zips = QDir(dl).entryList({"Dusk-*-source*.zip"}, QDir::Files, QDir::Time);
		if (have_tool && !zips.isEmpty()) {
			const QString path = dl + "/" + zips.first();
			const QString v = zip_version_of(path);
			const QString have = installed_version();
			if (!v.isEmpty() && newer_version(v, have)) {
				// What you'd get, from the version you have (every one in between too).
				const auto coming = news_in_zip(path, have);
				dl_text->setText(QString("<b>Dusk %1</b> is in your Downloads<br><span style='color:%2'>%3 · you have %4</span>%5")
					.arg(v, current_look().dim.name(), zips.first().toHtmlEscaped(), have,
						coming.isEmpty() ? QString() : "<div style='margin-top:6px'>" + whats_new_html(coming, 5) + "</div>"));
				dl_go->setText(QString("Install %1").arg(v));
				QObject::connect(dl_go, &QPushButton::clicked, [start, path, v] {
					start({"--zip", path}, "Installing Dusk " + v);
				});
				dl_row->show();
			}
		}
	}
	auto *opts = new QWidget;
	auto *oh = new QHBoxLayout(opts);
	oh->setContentsMargins(0, 0, 0, 0);
	oh->setSpacing(8);
	auto *file_btn = new QPushButton("Install from a zip…");
	file_btn->setCursor(Qt::PointingHandCursor);
	file_btn->setEnabled(have_tool);
	oh->addWidget(file_btn);
	make_row(c, "From a file", "A Dusk-…-source.zip you have (downloaded, or from a friend)", opts, nullptr);
	QObject::connect(file_btn, &QPushButton::clicked, [file_btn, start, zip_version_of, show_status] {
		const QString path = QFileDialog::getOpenFileName(file_btn->window(), "Install Dusk from a zip",
			QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), "Dusk source (Dusk-*.zip);;Zip files (*.zip)");
		if (path.isEmpty()) return;
		const QString v = zip_version_of(path);
		if (v.isEmpty()) {
			show_status(State::Bad, "That doesn't look like a Dusk zip (there's no meson.build in it).");
			return;
		}
		start({"--zip", path}, newer_version(kVersion, v) ? "Going back to Dusk " + v : "Installing Dusk " + v);
	});

	toggle(c, "Use a terminal window instead", "Shows the installer's own output, and asks for the password there",
		"update_in_terminal", false);

	// The commands, for a terminal.
	auto *how = new QLabel("In a terminal:  <b>dusk-update</b>  ·  from a zip:  <b>dusk-update --zip FILE</b>");
	how->setObjectName("hint");
	how->setWordWrap(true);
	how->setTextInteractionFlags(Qt::TextSelectableByMouse);
	how->setContentsMargins(4, 4, 4, 4);
	c->add(how);
}


// ---- For you ---------------------------------------------------------------------

// Which pages you open (and search for), so "For you" can put them first.
QSettings &usage_store() {
	static QSettings store(QDir::homePath() + "/.config/dusk/settings-usage.ini", QSettings::IniFormat);
	return store;
}

void record_use(const QString &page) {
	if (page.isEmpty() || page == "foryou") return;
	QSettings &u = usage_store();
	u.setValue("count/" + page, u.value("count/" + page, 0).toInt() + 1);
	QStringList recent = u.value("recent").toStringList();
	recent.removeAll(page);
	recent.prepend(page);
	while (recent.size() > 6) recent.removeLast();
	u.setValue("recent", recent);
}

QString os_name() {
	QFile f("/etc/os-release");
	if (f.open(QIODevice::ReadOnly)) {
		for (const QByteArray &line : f.readAll().split('\n')) {
			if (line.startsWith("PRETTY_NAME=")) return QString::fromUtf8(line.mid(12)).remove('"');
		}
	}
	return "Linux";
}

// Your picture, round: ~/.face, the one from your account, or your initial.
QPixmap avatar_pixmap(int size, const QColor &accent, const QString &name) {
	const qreal dpr = std::max<qreal>(2.0, qApp->devicePixelRatio());
	QPixmap out(QSize(size, size) * dpr);
	out.setDevicePixelRatio(dpr);
	out.fill(Qt::transparent);
	QPainter p(&out);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	QPainterPath circle;
	circle.addEllipse(QRectF(0, 0, size, size));
	p.setClipPath(circle);
	QImage img;
	for (const QString &path : {QDir::homePath() + "/.face",
			"/var/lib/AccountsService/icons/" + qEnvironmentVariable("USER")}) {
		if (QFileInfo(path).isReadable() && img.load(path)) break;
	}
	if (!img.isNull()) {
		const QImage scaled = img.scaled(QSize(size, size) * dpr, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
		p.drawImage(QRectF(0, 0, size, size), scaled,
			QRectF((scaled.width() - size * dpr) / 2, (scaled.height() - size * dpr) / 2, size * dpr, size * dpr));
	} else {
		p.fillRect(QRectF(0, 0, size, size), accent);
		QFont f = p.font();
		f.setPixelSize(size * 2 / 5);
		f.setBold(true);
		p.setFont(f);
		p.setPen(Qt::white);
		p.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, name.left(1).toUpper());
	}
	return out;
}

struct PageInfo {
	const char *id, *title, *hint;
};
const PageInfo kPageInfo[] = {
	{"appearance", "Appearance", "Dark mode, accent, wallpaper"},
	{"displays", "Displays", "Size, layout, night light"},
	{"panels", "Panels & dock", "Your panels and widgets"},
	{"power", "Power & battery", "Sleep, power modes"},
	{"apps", "Default apps", "Browser, files, terminal"},
	{"about", "About & updates", "Dusk version, updates"},
	{"controlcentre", "Control centre", "Tiles and layout"},
	{"desktop", "Desktop & clipboard", "Icons, clipboard history"},
	{"lockscreen", "Lock screen", "When it locks"},
	{"animations", "Animations", "Motion and speed"},
	{"notifications", "Notifications", "Alerts, do not disturb"},
	{"osd", "On-screen display", "Volume and brightness pop-ups"},
	{"datetime", "Date & time", "Clock and time zone"},
	{"language", "Language & region", "Language, formats"},
	{"windows", "Windows", "Title bars, snapping"},
	{"workspaces", "Workspaces", "Desktops and overview"},
	{"keyboard", "Keyboard", "Layouts and shortcuts"},
	{"pointer", "Mouse & touchpad", "Speed, scrolling, taps"},
	{"media", "Media & lyrics", "Player, sound, lyrics"},
	{"screenshots", "Screenshots & recording", "Where they're saved"},
	{"startup", "Startup apps", "Apps that open with you"},
};

const PageInfo *page_info(const QString &id) {
	for (const PageInfo &pi : kPageInfo) {
		if (id == pi.id) return &pi;
	}
	return nullptr;
}

// A tile: the page's icon in a soft accent square, its name and a hint.
QPushButton *page_tile(const PageInfo &pi) {
	auto *b = new QPushButton;
	b->setObjectName("tile");
	b->setCursor(Qt::PointingHandCursor);
	b->setMinimumHeight(64);
	b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	auto *h = new QHBoxLayout(b);
	h->setContentsMargins(12, 10, 12, 10);
	h->setSpacing(12);
	auto *icon = new QLabel;
	icon->setFixedSize(36, 36);
	icon->setAlignment(Qt::AlignCenter);
	icon->setAttribute(Qt::WA_TransparentForMouseEvents);
	auto *texts = new QVBoxLayout;
	texts->setSpacing(0);
	auto *t = new QLabel(pi.title);
	t->setObjectName("tiletitle");
	auto *d = new QLabel(pi.hint);
	d->setObjectName("subtitle");
	for (QLabel *l : {t, d}) l->setAttribute(Qt::WA_TransparentForMouseEvents);
	texts->addWidget(t);
	texts->addWidget(d);
	h->addWidget(icon);
	h->addLayout(texts, 1);
	const QString id = pi.id;
	auto paint = [icon, id] {
		const Look l = current_look();
		QColor soft = l.accent;
		soft.setAlphaF(l.dark ? 0.20 : 0.13);
		icon->setStyleSheet(QString("background: rgba(%1,%2,%3,%4); border-radius: 11px;")
			.arg(soft.red()).arg(soft.green()).arg(soft.blue()).arg(soft.alphaF()));
		icon->setPixmap(dusk_icon(id, l.accent, 20).pixmap(20, 20));
	};
	paint();
	restyle_hooks.push_back(paint);
	QObject::connect(b, &QPushButton::clicked, [id] { if (g_open_page) g_open_page(id); });
	return b;
}

bool have_app(const QStringList &commands, const QString &flatpak) {
	for (const QString &c : commands) {
		if (!QStandardPaths::findExecutable(c).isEmpty()) return true;
	}
	return !flatpak.isEmpty() && (QDir("/var/lib/flatpak/app/" + flatpak).exists() ||
		QDir(QDir::homePath() + "/.local/share/flatpak/app/" + flatpak).exists());
}

QWidget *foryou_page() {
	const passwd *pw = getpwuid(getuid());
	QString full = pw && pw->pw_gecos ? QString::fromLocal8Bit(pw->pw_gecos).section(',', 0, 0) : QString();
	const QString login = pw ? QString::fromLocal8Bit(pw->pw_name) : qEnvironmentVariable("USER");
	if (full.isEmpty()) full = login.left(1).toUpper() + login.mid(1);
	const int hour = QTime::currentTime().hour();
	const QString hello = hour < 12 ? "Good morning" : hour < 18 ? "Good afternoon" : "Good evening";
	Page page = make_page(hello + ", " + full.section(' ', 0, 0),
		"What you use most, and a few things worth setting up.");

	// You: your picture, your name, this computer.
	{
		Card *c = card(page, "");
		auto *row = new QWidget;
		auto *h = new QHBoxLayout(row);
		h->setContentsMargins(0, 6, 0, 6);
		h->setSpacing(16);
		auto *pic = new QLabel;
		pic->setFixedSize(64, 64);
		auto *texts = new QVBoxLayout;
		texts->setSpacing(2);
		auto *name = new QLabel(full);
		name->setObjectName("bigname");
		auto *sub = new QLabel(login + "  ·  " + os_name() + "  ·  Dusk " + QString(kVersion));
		sub->setObjectName("subtitle");
		texts->addStretch(1);
		texts->addWidget(name);
		texts->addWidget(sub);
		texts->addStretch(1);
		auto *change = new QPushButton("Change picture…");
		h->addWidget(pic);
		h->addLayout(texts, 1);
		h->addWidget(change, 0, Qt::AlignVCenter);
		auto paint = [pic, full] { pic->setPixmap(avatar_pixmap(64, current_look().accent, full)); };
		paint();
		restyle_hooks.push_back(paint);
		QObject::connect(change, &QPushButton::clicked, [change, paint] {
			const QString file = QFileDialog::getOpenFileName(change->window(), "Choose a picture",
				QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
				"Pictures (*.png *.jpg *.jpeg *.webp *.bmp)");
			if (file.isEmpty()) return;
			QImage img(file);
			if (img.isNull()) return;
			// A square, 256 px: what login screens expect of ~/.face.
			const int side = std::min(img.width(), img.height());
			img = img.copy((img.width() - side) / 2, (img.height() - side) / 2, side, side)
				.scaled(256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
			img.save(QDir::homePath() + "/.face", "PNG");
			paint();
		});
		c->rows->addWidget(row);
		c->members.push_back(row);
	}

	// The pages you open most (a sensible few until you've used it a while).
	{
		Card *c = card(page, "You use these most");
		QSettings &u = usage_store();
		std::vector<std::pair<int, QString>> ranked;
		int order = 0;
		for (const PageInfo &pi : kPageInfo) {
			// Unused pages keep their place in the list, after the used ones.
			ranked.push_back({u.value(QString("count/") + pi.id, 0).toInt() * 100 - order++, pi.id});
		}
		std::stable_sort(ranked.begin(), ranked.end(), [](auto a, auto b) { return a.first > b.first; });
		auto *grid = new QWidget;
		auto *g = new QGridLayout(grid);
		g->setContentsMargins(0, 4, 0, 4);
		g->setHorizontalSpacing(10);
		g->setVerticalSpacing(10);
		for (int k = 0; k < 6; k++) {
			if (const PageInfo *pi = page_info(ranked[static_cast<size_t>(k)].second)) g->addWidget(page_tile(*pi), k / 2, k % 2);
		}
		g->setColumnStretch(0, 1);
		g->setColumnStretch(1, 1);
		c->rows->addWidget(grid);
		c->members.push_back(grid);
	}

	// Suggestions: your phone, updates, shortcuts.
	{
		Card *c = card(page, "Suggestions");
		auto add = [c](const QString &icon, const QString &title, const QString &text,
				const std::vector<std::pair<QString, std::function<void()>>> &buttons) {
			auto *row = new QWidget;
			auto *outer = new QVBoxLayout(row);
			outer->setContentsMargins(0, 0, 0, 0);
			outer->setSpacing(0);
			if (!c->members.empty()) {
				auto *div = new QFrame;
				div->setObjectName("divider");
				outer->addWidget(div);
			}
			auto *h = new QHBoxLayout;
			h->setContentsMargins(0, 10, 0, 10);
			h->setSpacing(14);
			auto *ic = new QLabel;
			ic->setFixedSize(36, 36);
			ic->setAlignment(Qt::AlignCenter);
			auto paint = [ic, icon] {
				const Look l = current_look();
				ic->setStyleSheet(QString("background: %1; border-radius: 11px;").arg(l.field.name(QColor::HexArgb)));
				ic->setPixmap(dusk_icon(icon, l.text, 20).pixmap(20, 20));
			};
			paint();
			restyle_hooks.push_back(paint);
			auto *texts = new QVBoxLayout;
			texts->setSpacing(1);
			auto *t = new QLabel(title);
			auto *d = new QLabel(text);
			d->setObjectName("subtitle");
			d->setWordWrap(true);
			texts->addWidget(t);
			texts->addWidget(d);
			h->addWidget(ic, 0, Qt::AlignTop);
			h->addLayout(texts, 1);
			bool first = true;
			for (const auto &[label, act] : buttons) {
				auto *b = new QPushButton(label);
				if (first) b->setObjectName("primary");
				first = false;
				QObject::connect(b, &QPushButton::clicked, act);
				h->addWidget(b, 0, Qt::AlignVCenter);
			}
			outer->addLayout(h);
			c->rows->addWidget(row);
			c->members.push_back(row);
		};
		// Your phone: KDE Connect (notifications, files, clipboard, remote
		// input) or LocalSend (files only, any phone); you pick.
		auto open_or_get = [](const QStringList &cmds, const QString &flatpak, const QString &appstream) {
			return [cmds, flatpak, appstream] {
				for (const QString &cmd : cmds) {
					if (!QStandardPaths::findExecutable(cmd).isEmpty()) {
						QProcess::startDetached(cmd, {});
						return;
					}
				}
				if (!flatpak.isEmpty() && have_app({}, flatpak)) {
					QProcess::startDetached("flatpak", {"run", flatpak});
					return;
				}
				// Not installed: the software centre's page for it.
				QProcess::startDetached("xdg-open", {"appstream://" + appstream});
			};
		};
		const bool kde = have_app({"kdeconnect-app", "kdeconnect-settings"}, "org.kde.kdeconnect");
		const bool local = have_app({"localsend", "localsend_app"}, "org.localsend.localsend_app");
		add("phone", "Connect your phone",
			"KDE Connect shares notifications, files and the clipboard (Android, and iPhone in part). "
			"LocalSend just sends files, to and from any phone.",
			{{kde ? "Open KDE Connect" : "Get KDE Connect",
				open_or_get({"kdeconnect-app", "kdeconnect-settings"}, "org.kde.kdeconnect", "org.kde.kdeconnect.app")},
			 {local ? "Open LocalSend" : "Get LocalSend",
				open_or_get({"localsend", "localsend_app"}, "org.localsend.localsend_app", "org.localsend.localsend_app")}});
		add("about", "Keep Dusk up to date", "See what's new and install updates without a terminal.",
			{{"Updates", [] { if (g_open_page) g_open_page("about"); }}});
		add("keyboard", "Learn a few shortcuts",
			"Super opens the app menu, Super + 1 to 9 jumps to a workspace, Super + F makes a window full screen.",
			{{"Shortcuts", [] { if (g_open_page) g_open_page("keyboard"); }}});
	}

	// Where you were lately.
	{
		const QStringList recent = usage_store().value("recent").toStringList();
		if (!recent.isEmpty()) {
			Card *c = card(page, "Recently opened");
			auto *wrap = new QWidget;
			auto *flow = new QHBoxLayout(wrap);
			flow->setContentsMargins(0, 6, 0, 6);
			flow->setSpacing(8);
			for (const QString &id : recent.mid(0, 4)) {
				const PageInfo *pi = page_info(id);
				if (!pi) continue;
				auto *b = new QPushButton(pi->title);
				b->setIcon(dusk_icon(id, current_look().dim, 16));
				QObject::connect(b, &QPushButton::clicked, [id] { if (g_open_page) g_open_page(id); });
				flow->addWidget(b);
			}
			flow->addStretch(1);
			c->rows->addWidget(wrap);
			c->members.push_back(wrap);
		}
	}
	finish(page);
	return page.widget;
}

QWidget *about_page() {
	Page page = make_page("About", QString());
	const MachineInfo m = machine_info();

	// The top: Dusk's logo, this computer's name, and Rename.
	{
		auto *head = new QWidget;
		auto *v = new QVBoxLayout(head);
		v->setContentsMargins(0, 6, 0, 10);
		v->setSpacing(6);
		auto *logo = new QLabel;
		const qreal dpr = head->devicePixelRatioF();
		QPixmap pm = svg_pixmap(kDuskLogoSvg, static_cast<int>(104 * dpr));
		pm.setDevicePixelRatio(dpr);
		logo->setPixmap(pm);
		logo->setAlignment(Qt::AlignHCenter);
		v->addWidget(logo);
		auto *name = new QLabel(m.name);
		QFont nf = name->font();
		nf.setPointSizeF(nf.pointSizeF() * 2.0);
		name->setFont(nf);
		name->setAlignment(Qt::AlignHCenter);
		v->addWidget(name);
		auto *rename = new QPushButton("Rename");
		rename->setCursor(Qt::PointingHandCursor);
		rename->setFixedWidth(120);
		v->addWidget(rename, 0, Qt::AlignHCenter);
		QObject::connect(rename, &QPushButton::clicked, [name, rename] {
			bool ok = false;
			const QString now = name->text();
			const QString to = QInputDialog::getText(rename->window(), "Rename this computer",
				"How it's shown on your network and to Bluetooth devices:", QLineEdit::Normal, now, &ok).trimmed();
			if (!ok || to.isEmpty() || to == now) return;
			// hostnamectl asks for your password (polkit) when it needs to.
			auto *p = new QProcess(rename);
			QObject::connect(p, &QProcess::finished, [p, name, to](int code) {
				if (code == 0) name->setText(to);
				p->deleteLater();
			});
			p->start("hostnamectl", {"set-hostname", "--pretty", to});
		});
		page.layout->addWidget(head);
	}

	// What it is, like a phone's list.
	Card *info = card(page, "");
	{
		auto add = [info](const QString &title, const QString &value) {
			if (value.trimmed().isEmpty()) return;
			auto *v = new QLabel(value);
			v->setTextInteractionFlags(Qt::TextSelectableByMouse);
			v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			make_row(info, title, QString(), v, nullptr);
		};
		add("Computer", m.model);
		add("Processor", m.cpu);
		add("Graphics", m.gpu);
		add("Memory", m.memory);
		add("Storage", m.storage);
		add("System", m.system);
		add("Desktop", QString("Dusk %1").arg(kVersion));
	}

	// More, opened when you want it.
	Card *more = card(page, "");
	{
		QVBoxLayout *keys = disclosure(more, "Keyboard shortcuts", "Super, Alt+Tab, screenshots and more");
		const char *shortcuts[][2] = {
			{"Super (tap) or Alt+F1", "App menu"},
			{"Super+Tab or Super+S", "Overview: open windows and workspaces"},
			{"3 fingers up / down / left / right", "Overview, close it, next / previous workspace (touchpad)"},
			{"Super+1…9, 0", "Go to that workspace"},
			{"Super+Page Up / Down, Ctrl+Alt+Left / Right", "Previous / next workspace"},
			{"…with Shift", "Take the window along"},
			{"In the overview", "Type to find an open window; drag a window onto a workspace"},
			{"Super+A or Super+N", "Control centre"},
			{"Super+Enter", "Terminal"},
			{"Super+I", "Settings"},
			{"Super+E", "Your files"},
			{"Print", "Screenshot tool (Shift: all screens at once, Alt: the window)"},
			{"Super+D", "Show the desktop"},
			{"Super+V", "Clipboard history"},
			{"On the desktop", "Delete, F2 to rename, Enter, Ctrl+C / X / V / A"},
			{"Alt+Tab", "Switch windows"},
			{"Alt+F4 or Super+Q", "Close window"},
			{"Super+F", "Fullscreen"},
			{"Super+Up / Down", "Maximise / restore or minimise"},
			{"Super+Left / Right", "Snap to half the screen"},
			{"Super+Z", "Snap layouts for the active window"},
			{"Super+Shift+Left / Right", "Move the window to the next screen"},
			{"Super + drag", "Move (left button) or resize (right button)"},
			{"Volume, brightness, mute keys", "With an on-screen display"},
			{"Ctrl+Alt+Backspace", "Quit Dusk"},
		};

		for (const auto &sc : shortcuts) info_line(keys, sc[0], sc[1]);

		QVBoxLayout *soft = disclosure(more, "Software information", "Versions, and where your settings are kept");
		info_line(soft, "Dusk", kVersion);
		info_line(soft, "Qt", qVersion());
		info_line(soft, "Settings file", settings->path());

		QVBoxLayout *credits = disclosure(more, "Made with", "The projects and ideas Dusk stands on. Thank you.");
		const char *made[][3] = {
			{"wlroots", "The Wayland compositor library Dusk is built on", "https://gitlab.freedesktop.org/wlroots/wlroots"},
			{"SceneFX", "Rounded corners, shadows and blur (from SwayFX)", "https://github.com/wlrfx/scenefx"},
			{"Wayland", "The display protocol", "https://wayland.freedesktop.org"},
			{"libinput and xkbcommon", "Mice, touchpads and keyboard layouts", "https://wayland.freedesktop.org/libinput/doc/latest/"},
			{"Lucide", "The icons in the panels, control centre and menus (ISC licence)", "https://lucide.dev"},
			{"cairo, Pango, GdkPixbuf, HarfBuzz", "Drawing, text and pictures", "https://www.cairographics.org"},
			{"Qt", "This Settings app", "https://www.qt.io"},
			{"GLib and sd-bus", "Talking to the system (notifications, tray, power)", "https://docs.gtk.org/glib/"},
			{"GNOME", "Ideas: title bars, the overview and workspaces, the dark greys", "https://developer.gnome.org/hig/"},
			{"Material Symbols and One UI", "Ideas for the shapes of icons and controls", "https://fonts.google.com/icons"},
			{"adw-gtk3 and qt6ct", "GTK 3 and Qt apps that match", "https://github.com/lassekongo83/adw-gtk3"},
			{"KDE StatusNotifierItem", "App status icons (the tray)", "https://www.freedesktop.org/wiki/Specifications/StatusNotifierItem/"},
			{"freedesktop.org specifications", "Notifications, desktop files, icon themes, the trash", "https://specifications.freedesktop.org"},
			{"grim, wl-clipboard, Satty, Swappy", "Screenshots and the clipboard", "https://sr.ht/~emersion/grim/"},
			{"wayvnc, scrcpy, adb", "Phone & tablet", "https://github.com/any1/wayvnc"},
			{"NetworkManager, BlueZ, PipeWire, UPower, systemd", "Wi-Fi, Bluetooth, sound, battery, sessions", "https://www.freedesktop.org"},
			{"Claude (Anthropic)", "Helped write Dusk", "https://www.anthropic.com"},
		};

		for (const auto &mm : made) {
			auto *w = new QWidget;
			auto *h = new QHBoxLayout(w);
			h->setContentsMargins(0, 4, 0, 4);
			auto *texts = new QVBoxLayout;
			texts->setSpacing(0);
			texts->addWidget(new QLabel(mm[0]));
			auto *sub = new QLabel(mm[1]);
			sub->setObjectName("subtitle");
			sub->setWordWrap(true);
			texts->addWidget(sub);
			h->addLayout(texts, 1);
			auto *open = new QPushButton("Website");
			open->setCursor(Qt::PointingHandCursor);
			const QString url = mm[2];
			QObject::connect(open, &QPushButton::clicked, [url] { QProcess::startDetached("xdg-open", {url}); });
			h->addWidget(open, 0, Qt::AlignVCenter);
			credits->addWidget(w);
		}

		// Something wrong: the report dusk-doctor makes, straight to the clipboard.
		auto *copy = new QPushButton("Copy a report");
		copy->setCursor(Qt::PointingHandCursor);
		make_row(more, "Something not working?", "Copies what's wrong (from dusk-doctor) so you can paste it "
			"to whoever is helping", copy, nullptr);
		QObject::connect(copy, &QPushButton::clicked, [copy] {
			copy->setEnabled(false);
			copy->setText("Checking…");
			auto *p = new QProcess(copy);
			p->setProcessChannelMode(QProcess::MergedChannels);
			QObject::connect(p, &QProcess::finished, [p, copy] {
				QGuiApplication::clipboard()->setText(QString::fromUtf8(p->readAll()));
				copy->setText("Copied ✓");
				copy->setEnabled(true);
				QTimer::singleShot(2500, copy, [copy] { copy->setText("Copy a report"); });
				p->deleteLater();
			});
			p->start("dusk-doctor", {});
		});
	}

	updates_card(page);
	// What came with the last update (the versions since the one before).
	{
		const auto list = whats_new_list();
		if (!list.isEmpty()) {
			const QString newest = list.first().first, oldest = list.last().first;
			Card *wn = card(page, newest == oldest ? QString("What's new in %1").arg(newest)
				: QString("What's new (%1 – %2)").arg(oldest, newest), QString());
			auto *lbl = new QLabel(whats_new_html(list, 8));
			lbl->setTextFormat(Qt::RichText);
			lbl->setWordWrap(true);
			lbl->setContentsMargins(6, 4, 6, 4);
			wn->add(lbl);
		}
	}

	Card *also = card(page, "");
	{
		auto *b = new QPushButton("Reset settings…");
		b->setObjectName("danger");
		b->setCursor(Qt::PointingHandCursor);
		make_row(also, "Reset all settings", "Everything back to how Dusk first started. You choose what to keep.",
			b, nullptr);
		QObject::connect(b, &QPushButton::clicked, [b] { reset_everything(b->window()); });
	}
	finish(page);
	return page.widget;
}

} // namespace

// ============================================================================
// Window
// ============================================================================

namespace {
// Calls `fn` whenever the watched widget changes size.
class ResizeWatcher : public QObject {
public:
	explicit ResizeWatcher(std::function<void()> fn) : fn_(std::move(fn)) {}
	bool eventFilter(QObject *, QEvent *e) override {
		if (e->type() == QEvent::Resize) fn_();
		return false;
	}

private:
	std::function<void()> fn_;
};
} // namespace

// When Settings crashes: what it was doing, in ~/.cache/dusk/settings-crash.log
// (dusk-doctor shows it), so the crash can be found and fixed.
int g_crash_fd = -1;
void on_crash(int sig) {
	if (g_crash_fd >= 0) {
		char head[96];
		const int n = std::snprintf(head, sizeof head, "\n=== Dusk Settings %s crashed (signal %d) at %ld ===\n",
			kVersion, sig, static_cast<long>(time(nullptr)));
		if (n > 0) {
			const ssize_t w = ::write(g_crash_fd, head, static_cast<size_t>(n));
			(void)w;
		}
		void *frames[64];
		const int count = backtrace(frames, 64);
		backtrace_symbols_fd(frames, count, g_crash_fd);
		::fsync(g_crash_fd);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}
void install_crash_log() {
	const QString dir = QDir::homePath() + "/.cache/dusk";
	QDir().mkpath(dir);
	const QByteArray path = (dir + "/settings-crash.log").toLocal8Bit();
	g_crash_fd = ::open(path.constData(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	void *warm[1];
	backtrace(warm, 1);   // loads what backtrace needs now, not inside the crash
	for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) signal(sig, on_crash);
}

// The sidebar's rows, drawn here: a group name above the first page of
// each group ("Personalise", "System"…), and the page itself as an icon and
// a name; the open one a raised card with its icon in the accent, hovering a
// soft fill. (Narrow windows: just the icons.)
// A header bar: drag it to move the window, double-click to maximise.
class HeaderBar : public QWidget {
public:
	HeaderBar() {
		setObjectName("headerbar");
		setAttribute(Qt::WA_StyledBackground, true);
		setFixedHeight(46);
	}

protected:
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton && window()->windowHandle()) {
			window()->windowHandle()->startSystemMove();
			e->accept();
			return;
		}
		QWidget::mousePressEvent(e);
	}
	void mouseDoubleClickEvent(QMouseEvent *) override {
		QWidget *w = window();
		if (w->isMaximized()) w->showNormal();
		else w->showMaximized();
	}
};

// The window's edges resize it (a window without a frame has none of its own).
class EdgeResizer : public QObject {
public:
	explicit EdgeResizer(QWidget *win) : win_(win) {}

protected:
	bool eventFilter(QObject *o, QEvent *e) override {
		if (e->type() != QEvent::MouseMove && e->type() != QEvent::MouseButtonPress) return false;
		auto *w = qobject_cast<QWidget *>(o);
		if (!w || w->window() != win_) return false;
		auto *me = static_cast<QMouseEvent *>(e);
		Qt::Edges edges;
		if (!win_->isMaximized() && !win_->isFullScreen()) {
			const QPoint pt = me->position().toPoint();
			const QPoint p = w == win_ ? pt : w->mapTo(win_, pt);
			constexpr int m = 6;
			if (p.x() < m) edges |= Qt::LeftEdge;
			if (p.x() >= win_->width() - m) edges |= Qt::RightEdge;
			if (p.y() < m) edges |= Qt::TopEdge;
			if (p.y() >= win_->height() - m) edges |= Qt::BottomEdge;
		}
		if (e->type() == QEvent::MouseMove && me->buttons() == Qt::NoButton) {
			if (edges != edges_) {
				edges_ = edges;
				Qt::CursorShape shape = Qt::ArrowCursor;
				const bool h = edges & (Qt::LeftEdge | Qt::RightEdge), v = edges & (Qt::TopEdge | Qt::BottomEdge);
				if (h && v) {
					shape = ((edges & Qt::LeftEdge) && (edges & Qt::TopEdge)) || ((edges & Qt::RightEdge) && (edges & Qt::BottomEdge))
						? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
				} else if (h) {
					shape = Qt::SizeHorCursor;
				} else if (v) {
					shape = Qt::SizeVerCursor;
				}
				if (edges && !overriding_) {
					QGuiApplication::setOverrideCursor(shape);
					overriding_ = true;
				} else if (edges) {
					QGuiApplication::changeOverrideCursor(shape);
				} else if (overriding_) {
					QGuiApplication::restoreOverrideCursor();
					overriding_ = false;
				}
			}
			return false;
		}
		if (e->type() == QEvent::MouseButtonPress && edges && me->button() == Qt::LeftButton && win_->windowHandle()) {
			win_->windowHandle()->startSystemResize(edges);
			return true;
		}
		return false;
	}

private:
	QWidget *win_;
	Qt::Edges edges_;
	bool overriding_ = false;
};

// The title bar's buttons, drawn like GNOME's: small round grey buttons.
QIcon window_button_icon(const QString &kind, const QColor &c) {
	const qreal dpr = std::max<qreal>(2.0, qApp->devicePixelRatio());
	QPixmap pm(QSize(16, 16) * dpr);
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	if (kind == "close") {
		p.drawLine(QPointF(5, 5), QPointF(11, 11));
		p.drawLine(QPointF(11, 5), QPointF(5, 11));
	} else if (kind == "min") {
		p.drawLine(QPointF(5, 10.5), QPointF(11, 10.5));
	} else {
		p.drawRoundedRect(QRectF(5, 5, 6, 6), 1, 1);
	}
	p.end();
	return QIcon(pm);
}

class SideDelegate : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	QSize sizeHint(const QStyleOptionViewItem &o, const QModelIndex &idx) const override {
		const bool narrow = o.rect.width() > 0 && o.rect.width() < 100;
		const bool group = !narrow && !idx.data(Qt::UserRole + 3).toString().isEmpty();
		return QSize(180, 38 + (group ? 30 : 0));
	}
	void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &idx) const override {
		const Look l = current_look();
		p->save();
		p->setRenderHint(QPainter::Antialiasing);
		QRect r = o.rect;
		const bool narrow = r.width() < 100;
		const QString group = idx.data(Qt::UserRole + 3).toString();
		if (!group.isEmpty() && !narrow) {
			QFont f = o.font;
			f.setPointSizeF(f.pointSizeF() * 0.9);
			f.setWeight(QFont::Medium);
			p->setFont(f);
			QColor c = l.dim;
			c.setAlphaF(0.8);
			p->setPen(c);
			p->drawText(QRect(r.left() + 12, r.top() + 8, r.width() - 12, 22), Qt::AlignLeft | Qt::AlignVCenter, group);
			r.setTop(r.top() + 30);
		}
		const bool sel = o.state & QStyle::State_Selected;
		const bool hover = o.state & QStyle::State_MouseOver;
		const QRectF row = QRectF(r).adjusted(0.5, 1.5, -0.5, -1.5);
		const double rad = std::min(14.0, row.height() / 2.0);
		if (sel) {
			p->setPen(QPen(l.line, 1));
			p->setBrush(l.card);
			p->drawRoundedRect(row, rad, rad);
		} else if (hover) {
			p->setPen(Qt::NoPen);
			p->setBrush(l.hover);
			p->drawRoundedRect(row, rad, rad);
		}
		const QIcon icon = idx.data(Qt::DecorationRole).value<QIcon>();
		const int is = 18;
		const int ix = narrow ? static_cast<int>(row.center().x()) - is / 2 : static_cast<int>(row.left()) + 12;
		icon.paint(p, QRect(ix, static_cast<int>(row.center().y()) - is / 2, is, is));
		if (!narrow) {
			QFont f = o.font;
			f.setWeight(sel ? QFont::DemiBold : QFont::Normal);
			p->setFont(f);
			p->setPen(l.text);
			const QString text = idx.data(Qt::DisplayRole).toString();
			p->drawText(QRectF(row.left() + 42, row.top(), row.width() - 50, row.height()),
				Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(f).elidedText(text, Qt::ElideRight, static_cast<int>(row.width() - 50)));
			// A count (unread notifications…) at the end, in a little bubble.
			const QString badge = idx.data(Qt::UserRole + 4).toString();
			if (!badge.isEmpty()) {
				QFont bf = o.font;
				bf.setPointSizeF(bf.pointSizeF() * 0.82);
				const int bw = QFontMetrics(bf).horizontalAdvance(badge) + 14;
				const QRectF b(row.right() - bw - 10, row.center().y() - 9, bw, 18);
				p->setPen(Qt::NoPen);
				p->setBrush(l.field);
				p->drawRoundedRect(b, 9, 9);
				p->setFont(bf);
				p->setPen(l.dim);
				p->drawText(b, Qt::AlignCenter, badge);
			}
		}
		p->restore();
	}
};

int main(int argc, char **argv) {
	install_crash_log();
	// Fonts and icons from GTK's settings, like the rest of the desktop
	// (needs qt6-qtbase-gtk; Qt quietly does without it).
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORMTHEME")) {
		qputenv("QT_QPA_PLATFORMTHEME", "gtk3");
	}
	QApplication app(argc, argv);
	// Without an icon theme the sidebar would have no icons.
	QIcon::setFallbackThemeName("Adwaita");
	if (QIcon::themeName().isEmpty() || QIcon::themeName() == "hicolor") {
		QProcess gs;
		gs.start("gsettings", {"get", "org.gnome.desktop.interface", "icon-theme"});
		QString theme;
		if (gs.waitForFinished(1500)) {
			theme = QString::fromUtf8(gs.readAllStandardOutput()).trimmed().remove('\'');
		}
		QIcon::setThemeName(theme.isEmpty() ? QString("Adwaita") : theme);
	}
	the_app = &app;
	QApplication::setApplicationName("Dusk Settings");
	QApplication::setDesktopFileName("org.dusk.Settings");   // the Wayland app id
	SettingsFile file;
	settings = &file;

	QString start_page;
	const QStringList args = app.arguments();
	const int pi = args.indexOf("--page");
	if (pi >= 0 && pi + 1 < args.size()) {
		start_page = args[pi + 1];
	}
	// Asked for the password by sudo while Dusk updates (SUDO_ASKPASS): a
	// small dialog, the answer on stdout. (Before the one-at-a-time check:
	// Settings itself is open, running the update.)
	if (args.contains("--askpass")) {
		apply_look(app);
		return askpass_dialog();
	}

	// One Settings at a time: when one is open already, this one leaves it a
	// note with the page to show and goes (Dusk brings that window forward).
	const QString run_dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation).isEmpty()
		? QDir::tempPath() : QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
	const QString lock_path = run_dir + "/dusk-settings.lock", note_path = run_dir + "/dusk-settings.page";
	const int lock_fd = ::open(lock_path.toLocal8Bit().constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
		QFile note(note_path + ".new");
		if (note.open(QIODevice::WriteOnly)) {
			note.write((start_page.isEmpty() ? QString("-") : start_page).toUtf8());
			note.close();
			QFile::remove(note_path);
			QFile::rename(note_path + ".new", note_path);
		}
		// The open one takes the note at once. If it doesn't, it's stuck (an
		// old Settings that never finished closing, with no window): end it
		// and open properly instead of doing nothing.
		bool taken = false;
		for (int i = 0; i < 30 && !taken; i++) {
			usleep(50 * 1000);
			taken = !QFile::exists(note_path);
		}
		if (taken) return 0;
		char buf[64] = {0};
		const ssize_t n = ::pread(lock_fd, buf, sizeof buf - 1, 0);
		// Still starting up (its window isn't there yet): it reads the note
		// when it is.
		if (n > 0 && !std::strstr(buf, "ready")) return 0;
		QFile::remove(note_path);
		pid_t stuck = n > 0 ? static_cast<pid_t>(std::atoi(buf)) : 0;
		if (stuck <= 1) {
			// (A Settings from before 0.21 doesn't say who it is: look for it.)
			const QStringList procs = QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
			for (const QString &d : procs) {
				bool ok = false;
				const pid_t pid = static_cast<pid_t>(d.toInt(&ok));
				if (!ok || pid == getpid()) continue;
				QFile comm("/proc/" + d + "/comm");
				if (!comm.open(QIODevice::ReadOnly)) continue;
				if (QString::fromUtf8(comm.readAll()).trimmed() == "dusk-settings" &&
						QFileInfo("/proc/" + d).ownerId() == getuid()) {
					stuck = pid;
					break;
				}
			}
		}
		if (stuck > 1 && stuck != getpid()) ::kill(stuck, SIGKILL);
		bool got = false;
		for (int i = 0; i < 40 && !got; i++) {
			got = flock(lock_fd, LOCK_EX | LOCK_NB) == 0;
			if (!got) usleep(50 * 1000);
		}
		if (!got) return 0;
	}
	if (lock_fd >= 0) {
		// Who holds it, for a second Settings to check on ("ready" once the
		// window is up).
		const QByteArray pid = QByteArray::number(static_cast<qint64>(getpid())) + "\n";
		if (::ftruncate(lock_fd, 0) == 0) {
			const ssize_t w = ::pwrite(lock_fd, pid.constData(), static_cast<size_t>(pid.size()), 0);
			(void)w;
		}
	}
	QFile::remove(note_path);

	// 0.7 dropped the Ink and Soft presets and the screentone dots: the
	// old Comic look becomes the new, calmer Comic.
	if (settings->get("ui_texture") == "halftone") {
		if (settings->get("ui_style") == "comic") {
			apply_preset(kPresets[1]);
		} else {
			settings->remove("ui_texture");
		}
	}

	settings_translucent = settings->get("settings_opacity", "100").toInt() < 100;
	apply_look(app);
	// What the pages will ask the system, asked now, all at once (see run_tool).
	{
		prefetch_desktop_apps();
		for (const char *t : {"x-scheme-handler/https", "inode/directory", "x-scheme-handler/mailto", "audio/mpeg",
				"video/mp4", "image/png", "text/plain", "application/pdf"}) {
			prefetch_tool("xdg-mime", {"query", "default", t});
		}
		const QString pp = "/org/freedesktop/UPower/PowerProfiles", pi = "org.freedesktop.UPower.PowerProfiles";
		prefetch_tool("busctl", {"--system", "get-property", pi, pp, pi, "ActiveProfile"});
		prefetch_tool("busctl", {"--system", "get-property", pi, pp, pi, "Profiles"});
		prefetch_tool("localectl", {"status"});
		prefetch_tool("localectl", {"list-locales"});
		prefetch_tool("timedatectl", {"show", "-p", "NTP", "--value"});
		prefetch_tool("timedatectl", {"list-timezones"});
		prefetch_tool("timedatectl", {"show", "-p", "Timezone", "--value"});
		prefetch_tool("hostnamectl", {"--pretty"});
		prefetch_tool("lspci", {});
		prefetch_tool("lsblk", {"-bdno", "NAME,SIZE,TYPE,RM"});
		prefetch_tool("lsblk", {"-bnso", "NAME,TYPE", QString::fromUtf8(QStorageInfo::root().device())});
	}

	QWidget window;
	window.setObjectName("root");
	g_csd = qEnvironmentVariable("XDG_SESSION_DESKTOP") == "Dusk" && settings->get("settings_csd", "true") != "false";
	if (g_csd) {
		// Its own header bar, like GNOME apps; Dusk rounds the corners and
		// casts the shadow.
		window.setWindowFlag(Qt::FramelessWindowHint);
		app.installEventFilter(new EdgeResizer(&window));
	}
	if (settings_translucent) {
		window.setAttribute(Qt::WA_TranslucentBackground);   // Dusk blurs what's behind it
	}
	window.setWindowTitle("Dusk Settings");
	window.setWindowIcon(settings_icon());
	// As big as usual, but never bigger than the screen (a big scale leaves
	// less room); narrow windows fold the sidebar into icons.
	{
		QSize want(1000, 720);
		if (QScreen *scr = QGuiApplication::primaryScreen()) {
			const QRect a = scr->availableGeometry();
			want = want.boundedTo(QSize(static_cast<int>(a.width() * 0.92), static_cast<int>(a.height() * 0.9)));
		}
		window.resize(want);
	}
	window.setMinimumSize(360, 320);
	auto *h = new QHBoxLayout(&window);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(0);

	// Left: search every setting (like Arqen's), then the pages.
	auto *left = new QWidget;
	left->setObjectName("side");
	left->setAttribute(Qt::WA_StyledBackground, true);
	left->setFixedWidth(256);
	auto *lv = new QVBoxLayout(left);
	lv->setContentsMargins(0, 0, 0, 0);
	lv->setSpacing(0);
	if (g_csd) {
		auto *side_head = new HeaderBar;
		auto *shl = new QHBoxLayout(side_head);
		shl->setContentsMargins(12, 0, 12, 0);
		auto *t = new QLabel("Settings");
		t->setObjectName("headertitle");
		t->setAlignment(Qt::AlignCenter);
		t->setAttribute(Qt::WA_TransparentForMouseEvents);
		shl->addWidget(t, 1);
		lv->addWidget(side_head);
	}
	auto *search = new QLineEdit;
	search->setObjectName("search");
	search->setPlaceholderText("Search settings");
	search->setClearButtonEnabled(true);
	auto *search_box = new QWidget;
	auto *sb = new QVBoxLayout(search_box);
	sb->setContentsMargins(12, g_csd ? 2 : 14, 12, 2);
	sb->addWidget(search);
	lv->addWidget(search_box);
	auto *sidebar = new QListWidget;
	sidebar->setObjectName("sidebar");
	sidebar->setIconSize(QSize(18, 18));
	sidebar->setItemDelegate(new SideDelegate(sidebar));
	sidebar->setMouseTracking(true);
	sidebar->setUniformItemSizes(false);
	sidebar->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	lv->addWidget(sidebar, 1);
	auto *stack = new QStackedWidget;
	h->addWidget(left);
	// The page's header bar: its name in the middle, the window's buttons.
	QLabel *page_title = new QLabel;
	if (g_csd) {
		auto *right = new QWidget;
		auto *rv = new QVBoxLayout(right);
		rv->setContentsMargins(0, 0, 0, 0);
		rv->setSpacing(0);
		auto *head = new HeaderBar;
		auto *hl = new QHBoxLayout(head);
		hl->setContentsMargins(12, 0, 11, 0);
		hl->setSpacing(10);
		page_title->setObjectName("headertitle");
		page_title->setAlignment(Qt::AlignCenter);
		page_title->setAttribute(Qt::WA_TransparentForMouseEvents);
		std::vector<std::pair<QString, std::function<void()>>> buttons;
		if (settings->get("show_minimize", "true") != "false") buttons.push_back({"min", [&window] { window.showMinimized(); }});
		if (settings->get("show_maximize", "true") != "false") {
			buttons.push_back({"max", [&window] { window.isMaximized() ? window.showNormal() : window.showMaximized(); }});
		}
		buttons.push_back({"close", [&window] { window.close(); }});
		const int side = static_cast<int>(buttons.size()) * 34;
		hl->addSpacing(side);   // (so the title stays in the middle)
		hl->addWidget(page_title, 1);
		for (const auto &[kind, act] : buttons) {
			auto *b = new QToolButton;
			b->setObjectName("winbutton");
			b->setCursor(Qt::PointingHandCursor);
			b->setIconSize(QSize(16, 16));
			const QString k = kind;
			auto paint = [b, k] { b->setIcon(window_button_icon(k, current_look().text)); };
			paint();
			restyle_hooks.push_back(paint);
			QObject::connect(b, &QToolButton::clicked, act);
			hl->addWidget(b);
		}
		rv->addWidget(head);
		rv->addWidget(stack, 1);
		h->addWidget(right, 1);
	} else {
		h->addWidget(stack, 1);
	}

	struct Section {
		const char *id, *title;
		QStringList icons;
		std::function<QWidget *()> build;
		const char *group = "";   // the sidebar's heading above it (first of each group)
	};
	// In groups, the way people look for things: how it looks, the
	// computer itself, windows, what you plug in, apps and media.
	const std::vector<Section> sections = {
		{"foryou", "For you", {"starred-symbolic"}, foryou_page, ""},
		{"appearance", "Appearance", {"preferences-desktop-appearance-symbolic", "applications-graphics-symbolic"}, appearance_page, "Personalise"},
		{"panels", "Panels & dock", {"view-app-grid-symbolic", "view-grid-symbolic"}, [] {
			panels_page = new PanelsPage;
			return panels_page->widget;
		}},
		{"controlcentre", "Control centre", {"emblem-system-symbolic", "preferences-system-symbolic"}, control_centre_page},
		{"desktop", "Desktop & clipboard", {}, desktop_page},
		{"lockscreen", "Lock screen", {"system-lock-screen-symbolic"}, lockscreen_page},
		{"animations", "Animations", {"emblem-videos-symbolic", "media-playback-start-symbolic"}, animations_page},
		{"displays", "Displays", {"video-display-symbolic", "computer-symbolic"}, displays_page, "System"},
		{"power", "Power & battery", {"battery-good-symbolic", "system-shutdown-symbolic"}, power_page},
		{"notifications", "Notifications", {"preferences-system-notifications-symbolic", "notifications-symbolic"}, notifications_page},
		{"osd", "On-screen display", {"audio-volume-high-symbolic"}, osd_page},
		{"datetime", "Date & time", {"preferences-system-time-symbolic", "alarm-symbolic"}, datetime_page},
		{"language", "Language & region", {"preferences-desktop-locale-symbolic", "preferences-desktop-locale"}, language_page},
		{"windows", "Windows", {"focus-windows-symbolic", "window-new-symbolic"}, windows_page, "Windows"},
		{"workspaces", "Workspaces", {"view-grid-symbolic", "focus-windows-symbolic"}, workspaces_page},
		{"keyboard", "Keyboard", {"input-keyboard-symbolic"}, keyboard_page, "Devices"},
		{"pointer", "Mouse & touchpad", {"input-mouse-symbolic", "input-touchpad-symbolic"}, pointer_page},
		{"media", "Media & lyrics", {"audio-x-generic-symbolic", "media-playback-start-symbolic"}, media_page, "Apps & media"},
		{"screenshots", "Screenshots & recording", {"applets-screenshooter-symbolic", "camera-photo-symbolic"}, screenshots_page},
		{"apps", "Default apps", {"applications-system-symbolic", "utilities-terminal-symbolic"}, apps_page},
		{"startup", "Startup apps", {"system-run-symbolic", "media-playback-start-symbolic"}, startup_page},
		{"about", "About", {"help-about-symbolic"}, about_page, " "},
	};

	// Each page is built the first time it's shown (all 25 at once made
	// Settings slow to open); searching builds the rest first, so it finds
	// everything.
	struct Lazy {
		std::vector<std::function<QWidget *()>> build;
		std::vector<bool> done;
	};
	auto lazy = std::make_shared<Lazy>();
	for (const Section &s : sections) {
		auto *item = new QListWidgetItem(s.title);
		item->setData(Qt::UserRole, QString(s.id));
		item->setData(Qt::UserRole + 1, s.icons);
		item->setData(Qt::UserRole + 3, QString(s.group).trimmed().isEmpty() && *s.group ? QString(" ") : QString(s.group));
		sidebar->addItem(item);
		stack->addWidget(new QWidget);   // (a stand-in until it's built)
		lazy->build.push_back(s.build);
		lazy->done.push_back(false);
	}
	g_page = -1;
	auto ensure_page = [stack, lazy](int i) {
		if (i < 0 || i >= static_cast<int>(lazy->done.size()) || lazy->done[static_cast<size_t>(i)]) return;
		lazy->done[static_cast<size_t>(i)] = true;
		g_page = i;
		QWidget *page = lazy->build[static_cast<size_t>(i)]();
		g_page = -1;
		QWidget *stand_in = stack->widget(i);
		const bool current = stack->currentIndex() == i;
		stack->insertWidget(i, page);
		stack->removeWidget(stand_in);
		stand_in->deleteLater();
		if (current) stack->setCurrentIndex(i);
		update_rows();
	};
	auto ensure_all = [lazy, ensure_page] {
		for (int i = 0; i < static_cast<int>(lazy->done.size()); i++) ensure_page(i);
	};

	// Search results float under the search field.
	auto *results = new QListWidget(&window);
	results->setObjectName("results");
	results->hide();
	auto go = [sidebar, stack, results, search](int entry) {
		if (entry < 0 || entry >= static_cast<int>(search_index.size())) return;
		const SearchEntry e = search_index[static_cast<size_t>(entry)];
		results->hide();
		if (e.page >= 0) sidebar->setCurrentRow(e.page);
		QWidget *target = e.target;
		if (!target) return;
		// A setting that's normally tucked away: show the extra settings.
		if (std::find(advanced_rows.begin(), advanced_rows.end(), target) != advanced_rows.end() &&
				!settings->get_bool("show_advanced", false)) {
			settings->set_bool("show_advanced", true);
			changed("show_advanced");
		}
		QPointer<QWidget> t = target;
		QTimer::singleShot(60, [t, stack, e] {
			if (!t) return;
			if (auto *sa = qobject_cast<QScrollArea *>(stack->widget(e.page))) sa->ensureWidgetVisible(t, 0, 80);
			// It glows for a moment, so you see which one it is.
			const QString name = t->objectName();
			t->setObjectName("flashrow");
			t->setAttribute(Qt::WA_StyledBackground, true);
			QColor a = current_look().accent;
			t->setStyleSheet(QString("QWidget#flashrow { background: rgba(%1,%2,%3,0.20); border-radius: 12px; }")
				.arg(a.red()).arg(a.green()).arg(a.blue()));
			QTimer::singleShot(1400, [t, name] {
				if (!t) return;
				t->setStyleSheet(QString());
				t->setObjectName(name);
			});
		});
		search->clear();
	};
	auto find = [results, search, sidebar, &window, left, ensure_all] {
		const QString q = search->text().trimmed().toLower();
		results->clear();
		if (q.isEmpty()) {
			results->hide();
			return;
		}
		ensure_all();
		std::vector<std::pair<int, int>> scored;   // score, entry
		for (size_t i = 0; i < search_index.size(); i++) {
			const SearchEntry &e = search_index[i];
			if (!e.target || e.title.isEmpty()) continue;
			const QString t = e.title.toLower();
			int score = t.startsWith(q) ? 4 : t.contains(q) ? 3
				: e.subtitle.toLower().contains(q) ? 2 : e.card.toLower().contains(q) ? 1 : 0;
			if (score > 0) scored.push_back({score, static_cast<int>(i)});
		}
		std::stable_sort(scored.begin(), scored.end(), [](auto a, auto b) { return a.first > b.first; });
		QSet<QString> seen;
		for (const auto &[score, i] : scored) {
			const SearchEntry &e = search_index[static_cast<size_t>(i)];
			const QString page = e.page >= 0 && e.page < sidebar->count()
				? sidebar->item(e.page)->data(Qt::UserRole + 2).toString() : QString();
			const QString label = e.title + "   ·   " + (page.isEmpty() ? sidebar->item(std::max(0, e.page))->text() : page);
			if (seen.contains(label)) continue;
			seen.insert(label);
			auto *it = new QListWidgetItem(label);
			it->setData(Qt::UserRole, i);
			results->addItem(it);
			if (results->count() >= 12) break;
		}
		if (results->count() == 0) {
			auto *it = new QListWidgetItem("Nothing found");
			it->setFlags(Qt::NoItemFlags);
			results->addItem(it);
		}
		const QPoint at = search->mapTo(&window, QPoint(0, search->height() + 6));
		results->setGeometry(at.x(), at.y(), std::min(420, window.width() - at.x() - 16),
			std::min(results->count(), 12) * 34 + 12);
		results->show();
		results->raise();
		results->setCurrentRow(0);
	};
	QObject::connect(search, &QLineEdit::textChanged, find);
	QObject::connect(search, &QLineEdit::returnPressed, [results, go] {
		if (results->isVisible() && results->currentItem() && results->currentItem()->flags() != Qt::NoItemFlags) {
			go(results->currentItem()->data(Qt::UserRole).toInt());
		}
	});
	QObject::connect(results, &QListWidget::itemClicked, [go](QListWidgetItem *it) {
		if (it->flags() != Qt::NoItemFlags) go(it->data(Qt::UserRole).toInt());
	});
	auto paint_icons = [sidebar] {
		const Look l = current_look();
		for (int i = 0; i < sidebar->count(); i++) {
			QListWidgetItem *item = sidebar->item(i);
			const bool selected = i == sidebar->currentRow();
			// Dusk's own icons (the icon theme's were a mixed bag here).
			item->setIcon(dusk_icon(item->data(Qt::UserRole).toString(), selected ? l.accent : l.dim));
		}
	};
	restyle_hooks.push_back(paint_icons);
	restyle_hooks.push_back([] {
		for (Card *c : all_cards) c->apply_shadow();
	});
	QObject::connect(sidebar, &QListWidget::currentRowChanged, [sidebar, stack, paint_icons, ensure_page, page_title](int row) {
		if (row >= 0 && sidebar->item(row)) page_title->setText(sidebar->item(row)->data(Qt::UserRole + 2).toString().isEmpty()
			? sidebar->item(row)->text() : sidebar->item(row)->data(Qt::UserRole + 2).toString());
		if (row >= 0 && sidebar->item(row)) record_use(sidebar->item(row)->data(Qt::UserRole).toString());
		ensure_page(row);
		stack->setCurrentIndex(row);
		paint_icons();
	});
	int first = 0;
	for (int i = 0; i < sidebar->count(); i++) {
		if (sidebar->item(i)->data(Qt::UserRole).toString() == start_page) first = i;
	}
	sidebar->setCurrentRow(first);
	// The other pages are built while you're looking at the first, one at a
	// time with a pause between (the window stays responsive), so clicking
	// one later opens it straight away instead of building it then.
	{
		auto prebuild = std::make_shared<std::function<void(int)>>();
		*prebuild = [ensure_page, lazy, prebuild](int i) {
			while (i < static_cast<int>(lazy->done.size()) && lazy->done[static_cast<size_t>(i)]) i++;
			if (i >= static_cast<int>(lazy->done.size())) return;
			ensure_page(i);
			QTimer::singleShot(30, [prebuild, i] { (*prebuild)(i + 1); });
		};
		QTimer::singleShot(500, [prebuild] { (*prebuild)(0); });
	}
	g_open_page = [sidebar](const QString &id) {
		for (int i = 0; i < sidebar->count(); i++) {
			if (sidebar->item(i)->data(Qt::UserRole).toString() == id) sidebar->setCurrentRow(i);
		}
	};
	paint_icons();
	update_rows();

	// Narrow window: the sidebar shrinks to just its icons (if there are
	// icons to show) so the settings keep their room.
	for (int i = 0; i < sidebar->count(); i++) {
		sidebar->item(i)->setData(Qt::UserRole + 2, sidebar->item(i)->text());
	}
	auto fit = [&window, sidebar] {
		bool icons = true;
		for (int i = 0; i < sidebar->count(); i++) {
			icons = icons && !sidebar->item(i)->icon().isNull();
		}
		const bool narrow = icons && window.width() < 860;
		if (narrow == (sidebar->width() < 100)) return;
		sidebar->parentWidget()->setFixedWidth(narrow ? 66 : 256);
		if (auto *sbox = sidebar->parentWidget()->findChild<QLineEdit *>("search")) {
			sbox->parentWidget()->setVisible(!narrow);
		}
		for (int i = 0; i < sidebar->count(); i++) {
			QListWidgetItem *item = sidebar->item(i);
			const QString title = item->data(Qt::UserRole + 2).toString();
			item->setText(narrow ? QString() : title);
			item->setToolTip(narrow ? title : QString());
		}
	};
	auto *watcher = new ResizeWatcher(fit);
	window.installEventFilter(watcher);

	// Changed from outside (the control centre, or by hand): show it here.
	settings->changed_elsewhere = [&app] {
		apply_look(app);
		if (panels_page) panels_page->refresh_from_file();
		refresh_all();
	};

	window.show();
	// Notes from a second Settings: show the page it was asked for.
	{
		auto *notes = new QFileSystemWatcher(&window);
		notes->addPath(run_dir);
		auto take_note = [note_path, &window] {
			QFile note(note_path);
			if (!note.open(QIODevice::ReadOnly)) return;
			const QString page = QString::fromUtf8(note.readAll()).trimmed();
			note.close();
			QFile::remove(note_path);
			if (!page.isEmpty() && page != "-" && g_open_page) g_open_page(page);
			if (window.isMinimized()) window.showNormal();
			window.raise();
			window.activateWindow();
		};
		QObject::connect(notes, &QFileSystemWatcher::directoryChanged, take_note);
		take_note();   // one left while this was starting
		if (lock_fd >= 0) {
			const QByteArray ready = QByteArray::number(static_cast<qint64>(getpid())) + " ready\n";
			if (::ftruncate(lock_fd, 0) == 0) {
				const ssize_t w = ::pwrite(lock_fd, ready.constData(), static_cast<size_t>(ready.size()), 0);
				(void)w;
			}
		}
	}
	const int status = app.exec();
	file.save();
	// Gone at once: let the next Settings open straight away, and don't wait
	// for the picture threads (waiting on them at exit could hang, leaving a
	// Settings with no window that every later one deferred to).
	if (lock_fd >= 0) {
		flock(lock_fd, LOCK_UN);
		::close(lock_fd);
	}
	std::fflush(nullptr);
	std::_Exit(status);
}
