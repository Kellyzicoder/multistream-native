/*
 * Multistream channels: the one picture OBS makes is sent to many channels at once
 * (YouTube, Facebook, Instagram or any RTMP server).
 *
 * The video is made into H.264 only once (by the main stream's encoder, on the graphics
 * card when it has one). Every channel gets a copy of that same video, so adding a channel
 * costs internet upload, not computer work.
 *
 * The main channel is the one in Settings > Stream. The channels in this list go live with it
 * when Start Streaming is pressed, and stop with it.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <string>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("multistream-channels", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Multistream channels: stream to many places at once";
}

namespace {

struct Preset {
	const char *name;
	const char *server;
};

// The usual server for each place. The stream key comes from the platform's live page.
const Preset PRESETS[] = {
	{"YouTube", "rtmp://a.rtmp.youtube.com/live2"},
	{"Facebook", "rtmps://live-api-s.facebook.com:443/rtmp/"},
	{"Instagram", "rtmps://live-upload.instagram.com:443/rtmp/"},
	{"Other (RTMP)", ""},
};

struct Channel {
	std::string name;
	std::string kind;
	std::string server;
	std::string key;
	bool on = true;

	obs_output_t *out = nullptr;
	obs_service_t *svc = nullptr;
	std::string status = "Off";
	QPointer<QLabel> statusLabel;
};

std::vector<std::unique_ptr<Channel>> channels;
QPointer<QWidget> dock;
QPointer<QVBoxLayout> listBox;
bool streaming = false;

std::string configFile()
{
	char *p = obs_module_config_path("channels.json");
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

void save()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	obs_data_t *root = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	for (auto &c : channels) {
		obs_data_t *d = obs_data_create();
		obs_data_set_string(d, "name", c->name.c_str());
		obs_data_set_string(d, "kind", c->kind.c_str());
		obs_data_set_string(d, "server", c->server.c_str());
		obs_data_set_string(d, "key", c->key.c_str());
		obs_data_set_bool(d, "on", c->on);
		obs_data_array_push_back(arr, d);
		obs_data_release(d);
	}
	obs_data_set_array(root, "channels", arr);
	obs_data_save_json_safe(root, configFile().c_str(), "tmp", "bak");
	obs_data_array_release(arr);
	obs_data_release(root);
}

void load()
{
	obs_data_t *root = obs_data_create_from_json_file_safe(configFile().c_str(), "bak");
	if (!root)
		return;
	obs_data_array_t *arr = obs_data_get_array(root, "channels");
	size_t n = obs_data_array_count(arr);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *d = obs_data_array_item(arr, i);
		auto c = std::make_unique<Channel>();
		c->name = obs_data_get_string(d, "name");
		c->kind = obs_data_get_string(d, "kind");
		c->server = obs_data_get_string(d, "server");
		c->key = obs_data_get_string(d, "key");
		c->on = obs_data_get_bool(d, "on");
		channels.push_back(std::move(c));
		obs_data_release(d);
	}
	obs_data_array_release(arr);
	obs_data_release(root);
}

// Status changes come from the streaming threads: the label is changed on the screen's thread
void setStatus(Channel *c, const std::string &s)
{
	if (!dock)
		return;
	QMetaObject::invokeMethod(
		dock,
		[c, s]() {
			for (auto &x : channels) {
				if (x.get() != c)
					continue;
				c->status = s;
				if (c->statusLabel)
					c->statusLabel->setText(QString::fromStdString(s));
			}
		},
		Qt::QueuedConnection);
}

void onStart(void *data, calldata_t *)
{
	setStatus(static_cast<Channel *>(data), "Live");
}

void onReconnect(void *data, calldata_t *)
{
	setStatus(static_cast<Channel *>(data), "Reconnecting...");
}

void onReconnected(void *data, calldata_t *)
{
	setStatus(static_cast<Channel *>(data), "Live");
}

void onStop(void *data, calldata_t *cd)
{
	auto *c = static_cast<Channel *>(data);
	long long code = calldata_int(cd, "code");
	if (code == OBS_OUTPUT_SUCCESS) {
		setStatus(c, "Off");
		return;
	}
	const char *err = c->out ? obs_output_get_last_error(c->out) : nullptr;
	std::string msg = "Stopped";
	if (code == OBS_OUTPUT_BAD_PATH)
		msg = "Stopped: wrong server address";
	else if (code == OBS_OUTPUT_CONNECT_FAILED)
		msg = "Stopped: could not connect";
	else if (code == OBS_OUTPUT_INVALID_STREAM)
		msg = "Stopped: the stream key was refused";
	else if (code == OBS_OUTPUT_DISCONNECTED)
		msg = "Stopped: the connection was lost";
	if (err && *err)
		msg += std::string(" (") + err + ")";
	setStatus(c, msg);
}

void connectSignals(Channel *c, bool on)
{
	signal_handler_t *sh = obs_output_get_signal_handler(c->out);
	if (on) {
		signal_handler_connect(sh, "start", onStart, c);
		signal_handler_connect(sh, "stop", onStop, c);
		signal_handler_connect(sh, "reconnect", onReconnect, c);
		signal_handler_connect(sh, "reconnect_success", onReconnected, c);
	} else {
		signal_handler_disconnect(sh, "start", onStart, c);
		signal_handler_disconnect(sh, "stop", onStop, c);
		signal_handler_disconnect(sh, "reconnect", onReconnect, c);
		signal_handler_disconnect(sh, "reconnect_success", onReconnected, c);
	}
}

void stopChannel(Channel *c)
{
	if (!c->out)
		return;
	connectSignals(c, false);
	obs_output_stop(c->out);
	obs_output_release(c->out);
	obs_service_release(c->svc);
	c->out = nullptr;
	c->svc = nullptr;
	setStatus(c, "Off");
}

// The main stream has started: every channel that is on gets a copy of its video and sound
void startAll()
{
	obs_output_t *mainOut = obs_frontend_get_streaming_output();
	if (!mainOut)
		return;
	obs_encoder_t *venc = obs_output_get_video_encoder(mainOut);
	obs_encoder_t *aenc = obs_output_get_audio_encoder(mainOut, 0);
	obs_output_release(mainOut);
	if (!venc || !aenc) {
		blog(LOG_WARNING, "[multistream] the main stream has no encoders; channels not started");
		return;
	}

	for (auto &cp : channels) {
		Channel *c = cp.get();
		if (!c->on || c->out)
			continue;
		if (c->server.empty() || c->key.empty()) {
			setStatus(c, "Not started: add the server and stream key");
			continue;
		}

		obs_data_t *ss = obs_data_create();
		obs_data_set_string(ss, "server", c->server.c_str());
		obs_data_set_string(ss, "key", c->key.c_str());
		obs_data_set_bool(ss, "use_auth", false);
		std::string sname = "multistream service: " + c->name;
		c->svc = obs_service_create_private("rtmp_custom", sname.c_str(), ss);
		obs_data_release(ss);

		std::string oname = "multistream channel: " + c->name;
		c->out = obs_output_create("rtmp_output", oname.c_str(), nullptr, nullptr);
		if (!c->svc || !c->out) {
			obs_output_release(c->out);
			obs_service_release(c->svc);
			c->out = nullptr;
			c->svc = nullptr;
			setStatus(c, "Not started: could not set up the stream");
			continue;
		}
		obs_output_set_video_encoder(c->out, venc);
		obs_output_set_audio_encoder(c->out, aenc, 0);
		obs_output_set_service(c->out, c->svc);
		obs_output_set_reconnect_settings(c->out, 20, 3);
		connectSignals(c, true);
		setStatus(c, "Connecting...");
		if (!obs_output_start(c->out)) {
			const char *err = obs_output_get_last_error(c->out);
			std::string msg = std::string("Could not start") + (err && *err ? std::string(": ") + err : "");
			stopChannel(c);
			setStatus(c, msg);
		}
	}
}

void stopAll()
{
	for (auto &c : channels)
		stopChannel(c.get());
}

void rebuildList();

// Add or change a channel
bool editChannel(Channel *c, bool isNew)
{
	QDialog dlg(dock);
	dlg.setWindowTitle(isNew ? "Add channel" : "Edit channel");
	dlg.setMinimumWidth(420);
	auto *form = new QFormLayout(&dlg);

	auto *name = new QLineEdit(QString::fromStdString(c->name));
	auto *kind = new QComboBox();
	for (const Preset &p : PRESETS)
		kind->addItem(p.name);
	if (!c->kind.empty())
		kind->setCurrentText(QString::fromStdString(c->kind));
	auto *server = new QLineEdit(QString::fromStdString(c->server));
	auto *key = new QLineEdit(QString::fromStdString(c->key));
	key->setEchoMode(QLineEdit::Password);
	auto *show = new QCheckBox("Show the key");
	QObject::connect(show, &QCheckBox::toggled, key,
			 [key](bool b) { key->setEchoMode(b ? QLineEdit::Normal : QLineEdit::Password); });
	QObject::connect(kind, &QComboBox::currentIndexChanged, server, [server, name, kind](int i) {
		if (i < 0 || static_cast<size_t>(i) >= sizeof(PRESETS) / sizeof(PRESETS[0]))
			return;
		if (*PRESETS[i].server)
			server->setText(PRESETS[i].server);
		if (name->text().isEmpty())
			name->setText(kind->currentText());
	});
	if (isNew && server->text().isEmpty())
		server->setText(PRESETS[0].server);

	form->addRow("Name", name);
	form->addRow("Where", kind);
	form->addRow("Server", server);
	form->addRow("Stream key", key);
	form->addRow("", show);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	form->addRow(buttons);

	if (dlg.exec() != QDialog::Accepted)
		return false;
	c->name = name->text().trimmed().toStdString();
	if (c->name.empty())
		c->name = kind->currentText().toStdString();
	c->kind = kind->currentText().toStdString();
	c->server = server->text().trimmed().toStdString();
	c->key = key->text().trimmed().toStdString();
	return true;
}

void rebuildList()
{
	if (!listBox)
		return;
	while (QLayoutItem *it = listBox->takeAt(0)) {
		if (it->widget())
			it->widget()->deleteLater();
		delete it;
	}

	if (channels.empty()) {
		auto *none = new QLabel("No extra channels yet. Add YouTube, Facebook, Instagram or any RTMP server.");
		none->setWordWrap(true);
		listBox->addWidget(none);
	}

	for (auto &cp : channels) {
		Channel *c = cp.get();
		auto *row = new QWidget();
		auto *h = new QHBoxLayout(row);
		h->setContentsMargins(0, 2, 0, 2);

		auto *on = new QCheckBox(QString::fromStdString(c->name));
		on->setChecked(c->on);
		on->setToolTip("On: this channel goes live when you press Start Streaming");
		QObject::connect(on, &QCheckBox::toggled, on, [c](bool b) {
			c->on = b;
			save();
			// switched on while live: it joins straight away; switched off: it stops
			if (streaming && b)
				startAll();
			else if (!b)
				stopChannel(c);
		});

		auto *st = new QLabel(QString::fromStdString(c->status));
		st->setStyleSheet("color: gray;");
		c->statusLabel = st;

		auto *edit = new QPushButton("Edit");
		QObject::connect(edit, &QPushButton::clicked, edit, [c]() {
			if (c->out) {
				QMessageBox::information(dock, "Channel is live", "Stop this channel first, then change it.");
				return;
			}
			if (editChannel(c, false)) {
				save();
				rebuildList();
			}
		});

		auto *del = new QPushButton("Remove");
		QObject::connect(del, &QPushButton::clicked, del, [c]() {
			if (QMessageBox::question(dock, "Remove channel",
						  QString("Remove %1?").arg(QString::fromStdString(c->name))) !=
			    QMessageBox::Yes)
				return;
			stopChannel(c);
			for (auto it = channels.begin(); it != channels.end(); ++it) {
				if (it->get() == c) {
					channels.erase(it);
					break;
				}
			}
			save();
			rebuildList();
		});

		h->addWidget(on, 1);
		h->addWidget(st);
		h->addWidget(edit);
		h->addWidget(del);
		listBox->addWidget(row);
	}
}

void createDock()
{
	if (dock)
		return;
	auto *w = new QWidget();
	auto *v = new QVBoxLayout(w);

	auto *about = new QLabel("These channels go live together with the main one (Settings > Stream) when you press "
				 "Start Streaming. The video is made once and sent to each.");
	about->setWordWrap(true);
	v->addWidget(about);

	auto *list = new QVBoxLayout();
	v->addLayout(list);
	listBox = list;

	auto *add = new QPushButton("Add channel");
	QObject::connect(add, &QPushButton::clicked, add, []() {
		auto c = std::make_unique<Channel>();
		if (!editChannel(c.get(), true))
			return;
		channels.push_back(std::move(c));
		save();
		rebuildList();
	});
	v->addWidget(add);
	v->addStretch(1);

	dock = w;
	rebuildList();
	obs_frontend_add_dock_by_id("multistream-channels", "Channels", w);
}

void onEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		createDock();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		streaming = true;
		startAll();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
	case OBS_FRONTEND_EVENT_EXIT:
		streaming = false;
		stopAll();
		break;
	default:
		break;
	}
}

} // namespace

bool obs_module_load(void)
{
	load();
	obs_frontend_add_event_callback(onEvent, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onEvent, nullptr);
	stopAll();
	channels.clear();
}
