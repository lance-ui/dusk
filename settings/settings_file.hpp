// Reads and writes ~/.config/dusk/dusk.conf, keeping lines it doesn't know
// (and comments) as they are. Saves are batched and atomic, so Dusk never
// sees half a file. Changes made elsewhere (e.g. the control centre's dark
// mode tile) are picked up too.
#pragma once

#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>

class SettingsFile {
public:
	SettingsFile() {
		dir_ = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/dusk";
		path_ = dir_ + "/dusk.conf";
		migrate();
		load();
		save_timer_.setSingleShot(true);
		save_timer_.setInterval(450);   // (a burst of changes is one save, one restyle in Dusk)
		QObject::connect(&save_timer_, &QTimer::timeout, [this] { save(); });

		QDir().mkpath(dir_);
		watcher_.addPath(dir_);
		if (QFile::exists(path_)) {
			watcher_.addPath(path_);
		}
		auto check = [this] {
			if (!watcher_.files().contains(path_) && QFile::exists(path_)) {
				watcher_.addPath(path_);   // it was replaced (atomic save)
			}
			if (save_timer_.isActive()) {
				return;   // our own change is about to be written
			}
			const QString now = read_all();
			if (now != last_written_) {
				last_written_ = now;
				parse(now);
				if (changed_elsewhere) changed_elsewhere();
			}
		};
		QObject::connect(&watcher_, &QFileSystemWatcher::fileChanged, check);
		QObject::connect(&watcher_, &QFileSystemWatcher::directoryChanged, check);
	}

	const QString &path() const { return path_; }

	QString get(const QString &key, const QString &fallback = QString()) const {
		const int i = find(key);
		return i < 0 ? fallback : value_of(lines_[i]);
	}
	bool has(const QString &key) const { return find(key) >= 0; }
	// Every key in the file, in file order.
	QStringList keys() const { return keys_with_prefix(QString()); }
	// Every key that starts with `prefix`, in file order.
	QStringList keys_with_prefix(const QString &prefix) const {
		QStringList out;
		for (const QString &line : lines_) {
			const QString k = key_of(line);
			if (k.startsWith(prefix) && !out.contains(k)) out << k;
		}
		return out;
	}
	bool get_bool(const QString &key, bool fallback) const {
		const QString v = get(key);
		if (v == "true" || v == "yes" || v == "1" || v == "on") return true;
		if (v == "false" || v == "no" || v == "0" || v == "off") return false;
		return fallback;
	}

	void set(const QString &key, const QString &value) {
		const QString line = key + " = " + value;
		const int i = find(key);
		if (i >= 0) {
			if (lines_[i] == line) return;
			lines_[i] = line;
		} else {
			lines_.append(line);
		}
		save_timer_.start();
	}
	void set_bool(const QString &key, bool on) { set(key, on ? "true" : "false"); }

	// Back to the default: the line goes away.
	void remove(const QString &key) {
		const int i = find(key);
		if (i >= 0) {
			lines_.removeAt(i);
			save_timer_.start();
		}
	}
	// All keys starting with `prefix` (used when a panel is removed).
	void remove_prefix(const QString &prefix) {
		for (int i = lines_.size() - 1; i >= 0; i--) {
			if (key_of(lines_[i]).startsWith(prefix)) {
				lines_.removeAt(i);
				save_timer_.start();
			}
		}
	}

	void save() {
		save_timer_.stop();
		QDir().mkpath(dir_);
		QString text = lines_.join('\n') + '\n';
		if (!text.startsWith('#')) {
			text = "# Dusk settings. Written by Dusk Settings; you can also edit it by hand.\n"
				"# Dusk picks up changes as soon as the file is saved.\n" + text;
			lines_ = text.split('\n');
			while (!lines_.isEmpty() && lines_.last().isEmpty()) lines_.removeLast();
			text = lines_.join('\n') + '\n';
		}
		QSaveFile file(path_);
		if (file.open(QIODevice::WriteOnly)) {
			file.write(text.toUtf8());
			if (file.commit()) {
				last_written_ = text;
			}
		}
	}

	// Called after the file was changed by something else.
	std::function<void()> changed_elsewhere;

private:
	static QString key_of(const QString &line) {
		const QString t = line.trimmed();
		if (t.isEmpty() || t.startsWith('#') || t.startsWith(';')) return QString();
		const int eq = t.indexOf('=');
		return eq < 0 ? QString() : t.left(eq).trimmed();
	}
	static QString value_of(const QString &line) {
		const int eq = line.indexOf('=');
		return eq < 0 ? QString() : line.mid(eq + 1).trimmed();
	}
	int find(const QString &key) const {
		for (int i = lines_.size() - 1; i >= 0; i--) {   // the last one wins, like in Dusk
			if (key_of(lines_[i]) == key) return i;
		}
		return -1;
	}
	QString read_all() const {
		QFile file(path_);
		if (!file.open(QIODevice::ReadOnly)) return QString();
		return QString::fromUtf8(file.readAll());
	}
	void parse(const QString &text) {
		lines_ = text.split('\n');
		while (!lines_.isEmpty() && lines_.last().isEmpty()) lines_.removeLast();
	}
	void load() {
		last_written_ = read_all();
		parse(last_written_);
	}
	// First run after the rename from Floe: bring the old settings along.
	void migrate() {
		const QString old = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
			"/floe/floe.conf";
		if (!QFile::exists(path_) && QFile::exists(old)) {
			QDir().mkpath(dir_);
			QFile::copy(old, path_);
		}
	}

	QString dir_, path_;
	QStringList lines_;
	QString last_written_;
	QTimer save_timer_;
	QFileSystemWatcher watcher_;
};
