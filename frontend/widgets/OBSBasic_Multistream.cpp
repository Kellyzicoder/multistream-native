/*
 * Multistream: the app's own layout.
 *
 * A sidebar on the left with the pages of Multistream (Go Live, Studio, Multiview, Stream health,
 * Recordings, Scene collections) and Settings. The Studio page is the editor (scenes, sources,
 * mixer, transitions, controls). The other pages show only what they need, and the parts they
 * don't show stop drawing (the Multiview, for example, only exists while its page is open).
 */

#include "OBSBasic.hpp"
#include "OBSBasicStats.hpp"
#include "OBSQTDisplay.hpp"

#include <components/Multiview.hpp>
#include <utility/display-helpers.hpp>

#include <obs-frontend-api.h>

#include <QButtonGroup>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QFrame>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include <cstring>
#include <string>

class MultistreamPages;

enum MsPage { PageGoLive = 0, PageStudio, PageMultiview, PageHealth, PageRecordings, PageCollections };

// Keeps the editor's panels as they were when the window closes on another page
class MsCloseFilter : public QObject {
public:
	explicit MsCloseFilter(MultistreamPages *pages_, QObject *parent) : QObject(parent), pages(pages_) {}

protected:
	bool eventFilter(QObject *obj, QEvent *event) override;

private:
	MultistreamPages *pages;
};

class MultistreamPages {
public:
	explicit MultistreamPages(OBSBasic *basic_) : basic(basic_) {}

	OBSBasic *basic;
	QToolBar *sidebar = nullptr;
	QStackedWidget *stack = nullptr;
	QButtonGroup *nav = nullptr;
	QLabel *liveChip = nullptr;

	// Go Live
	QPushButton *goButton = nullptr;
	QLabel *goState = nullptr;
	QLabel *mainChannel = nullptr;
	QVBoxLayout *channelsBox = nullptr;
	OBSQTDisplay *program = nullptr;

	// Multiview: made while its page is open, thrown away when it closes
	OBSQTDisplay *mvDisplay = nullptr;
	Multiview *multiview = nullptr;
	bool mvUpdating = false;

	// Recordings and scene collections
	QListWidget *recList = nullptr;
	QLabel *recFolder = nullptr;
	QListWidget *collList = nullptr;

	QList<QPointer<QDockWidget>> hiddenDocks;
	int current = -1;

	void Build();
	QWidget *BuildGoLive();
	QWidget *BuildMultiview();
	QWidget *BuildHealth();
	QWidget *BuildRecordings();
	QWidget *BuildCollections();
	void TakeChannelsPanel();

	void Show(int page);
	void HideEditorPanels();
	void RestoreEditorPanels();

	void UpdateStreamState();
	void UpdateMainChannel();
	void RefreshRecordings();
	void RefreshCollections();
	void StartMultiview();
	void StopMultiview();
	void UpdateMultiview();
};

bool MsCloseFilter::eventFilter(QObject *obj, QEvent *event)
{
	if (event->type() == QEvent::Close) {
		pages->RestoreEditorPanels();
	}
	return QObject::eventFilter(obj, event);
}

/* ---------- drawing ---------- */

// The picture that goes out, fitted into the box
static void MsRenderProgram(void *, uint32_t cx, uint32_t cy)
{
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		return;
	}

	int x, y;
	float scale;
	GetScaleAndCenterPos(ovi.base_width, ovi.base_height, cx, cy, x, y, scale);
	int newCX = int(scale * float(ovi.base_width));
	int newCY = int(scale * float(ovi.base_height));

	startRegion(x, y, newCX, newCY, 0.0f, float(ovi.base_width), 0.0f, float(ovi.base_height));
	obs_render_main_texture();
	endRegion();
}

static void MsRenderMultiview(void *data, uint32_t cx, uint32_t cy)
{
	auto *pages = static_cast<MultistreamPages *>(data);
	if (pages->mvUpdating || !pages->multiview) {
		return;
	}
	pages->multiview->Render(cx, cy);
}

/* ---------- small helpers ---------- */

static QLabel *MsTitle(const QString &text)
{
	auto *l = new QLabel(text);
	l->setObjectName("msPageTitle");
	return l;
}

static QWidget *MsCard(QWidget *inner, const QString &title = QString())
{
	auto *card = new QFrame();
	card->setObjectName("msCard");
	auto *v = new QVBoxLayout(card);
	v->setContentsMargins(16, 14, 16, 14);
	if (!title.isEmpty()) {
		auto *t = new QLabel(title);
		t->setObjectName("msCardTitle");
		v->addWidget(t);
	}
	v->addWidget(inner, 1);
	return card;
}

/* ---------- building ---------- */

void MultistreamPages::Build()
{
	// the editor that was the whole window becomes the Studio page
	QWidget *editor = basic->takeCentralWidget();

	stack = new QStackedWidget(basic);
	stack->setObjectName("msPages");
	stack->addWidget(BuildGoLive());
	stack->addWidget(editor);
	stack->addWidget(BuildMultiview());
	stack->addWidget(BuildHealth());
	stack->addWidget(BuildRecordings());
	stack->addWidget(BuildCollections());
	basic->setCentralWidget(stack);

	// the sidebar
	sidebar = new QToolBar(basic);
	sidebar->setObjectName("msSidebar");
	sidebar->setWindowTitle("Multistream");
	sidebar->setMovable(false);
	sidebar->setFloatable(false);
	sidebar->toggleViewAction()->setVisible(false);
	sidebar->setContextMenuPolicy(Qt::PreventContextMenu);

	auto *side = new QWidget();
	auto *sv = new QVBoxLayout(side);
	sv->setContentsMargins(10, 14, 10, 14);
	sv->setSpacing(4);

	auto *brand = new QLabel("Multistream");
	brand->setObjectName("msBrand");
	sv->addWidget(brand);
	sv->addSpacing(12);

	nav = new QButtonGroup(side);
	nav->setExclusive(true);
	const char *names[] = {"Go Live", "Studio", "Multiview", "Stream health", "Recordings", "Scene collections"};
	for (int i = 0; i < 6; i++) {
		auto *b = new QPushButton(names[i]);
		b->setObjectName("msNavButton");
		b->setCheckable(true);
		b->setCursor(Qt::PointingHandCursor);
		nav->addButton(b, i);
		sv->addWidget(b);
	}
	QObject::connect(nav, &QButtonGroup::idClicked, basic, [this](int id) { Show(id); });

	sv->addStretch(1);

	auto *settings = new QPushButton("Settings");
	settings->setObjectName("msNavButton");
	settings->setCursor(Qt::PointingHandCursor);
	QObject::connect(settings, &QPushButton::clicked, basic, [this]() { basic->ui->action_Settings->trigger(); });
	sv->addWidget(settings);

	liveChip = new QLabel();
	liveChip->setObjectName("msLiveChip");
	liveChip->setAlignment(Qt::AlignCenter);
	sv->addSpacing(8);
	sv->addWidget(liveChip);

	sidebar->addWidget(side);
	basic->addToolBar(Qt::LeftToolBarArea, sidebar);

	basic->installEventFilter(new MsCloseFilter(this, basic));

	UpdateStreamState();
	UpdateMainChannel();

	// the Channels panel is ready once everything has loaded
	QTimer::singleShot(0, basic, [this]() {
		TakeChannelsPanel();
		Show(PageGoLive);
	});
}

QWidget *MultistreamPages::BuildGoLive()
{
	auto *page = new QWidget();
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(24, 20, 24, 20);
	v->setSpacing(16);

	// top bar: title, state, Start streaming
	auto *top = new QHBoxLayout();
	top->addWidget(MsTitle("Go Live"));
	top->addStretch(1);
	goState = new QLabel();
	goState->setObjectName("msGoState");
	top->addWidget(goState);
	goButton = new QPushButton("Start streaming");
	goButton->setObjectName("msGoButton");
	goButton->setCursor(Qt::PointingHandCursor);
	QObject::connect(goButton, &QPushButton::clicked, basic, [this]() {
		if (basic->StreamingActive()) {
			basic->StopStreaming();
		} else {
			basic->StartStreaming();
		}
	});
	top->addWidget(goButton);
	v->addLayout(top);

	auto *body = new QHBoxLayout();
	body->setSpacing(16);

	// the picture that goes out
	program = new OBSQTDisplay();
	program->setObjectName("msProgram");
	program->setMinimumSize(480, 270);
	QObject::connect(program, &OBSQTDisplay::DisplayCreated, program, [this]() {
		obs_display_add_draw_callback(program->GetDisplay(), MsRenderProgram, this);
		obs_display_set_background_color(program->GetDisplay(), 0x000000);
	});
	body->addWidget(MsCard(program, "Program"), 3);

	// the channels
	auto *channels = new QWidget();
	auto *cv = new QVBoxLayout(channels);
	cv->setContentsMargins(0, 0, 0, 0);
	auto *mainRow = new QHBoxLayout();
	mainChannel = new QLabel();
	mainChannel->setObjectName("msMainChannel");
	mainChannel->setWordWrap(true);
	auto *editMain = new QPushButton("Edit");
	QObject::connect(editMain, &QPushButton::clicked, basic, [this]() { basic->ui->action_Settings->trigger(); });
	mainRow->addWidget(mainChannel, 1);
	mainRow->addWidget(editMain);
	cv->addLayout(mainRow);
	channelsBox = new QVBoxLayout();
	cv->addLayout(channelsBox);
	cv->addStretch(1);
	body->addWidget(MsCard(channels, "Channels"), 2);

	v->addLayout(body, 1);
	return page;
}

QWidget *MultistreamPages::BuildMultiview()
{
	auto *page = new QWidget();
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(24, 20, 24, 20);
	v->addWidget(MsTitle("Multiview"));

	mvDisplay = new OBSQTDisplay();
	mvDisplay->setMinimumSize(480, 270);
	QObject::connect(mvDisplay, &OBSQTDisplay::DisplayCreated, mvDisplay, [this]() {
		obs_display_add_draw_callback(mvDisplay->GetDisplay(), MsRenderMultiview, this);
		obs_display_set_background_color(mvDisplay->GetDisplay(), 0x000000);
	});
	v->addWidget(mvDisplay, 1);
	return page;
}

QWidget *MultistreamPages::BuildHealth()
{
	auto *page = new QWidget();
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(24, 20, 24, 20);
	v->addWidget(MsTitle("Stream health"));
	v->addWidget(MsCard(new OBSBasicStats(nullptr, false)), 1);
	return page;
}

QWidget *MultistreamPages::BuildRecordings()
{
	auto *page = new QWidget();
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(24, 20, 24, 20);

	auto *top = new QHBoxLayout();
	top->addWidget(MsTitle("Recordings"));
	top->addStretch(1);
	auto *open = new QPushButton("Open folder");
	QObject::connect(open, &QPushButton::clicked, basic, [this]() {
		const char *path = basic->GetCurrentOutputPath();
		if (path && *path) {
			QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromUtf8(path)));
		}
	});
	top->addWidget(open);
	v->addLayout(top);

	recFolder = new QLabel();
	recFolder->setObjectName("msMuted");
	v->addWidget(recFolder);

	recList = new QListWidget();
	recList->setObjectName("msList");
	QObject::connect(recList, &QListWidget::itemDoubleClicked, basic, [](QListWidgetItem *item) {
		QDesktopServices::openUrl(QUrl::fromLocalFile(item->data(Qt::UserRole).toString()));
	});
	v->addWidget(MsCard(recList), 1);
	return page;
}

QWidget *MultistreamPages::BuildCollections()
{
	auto *page = new QWidget();
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(24, 20, 24, 20);

	auto *top = new QHBoxLayout();
	top->addWidget(MsTitle("Scene collections"));
	top->addStretch(1);
	struct {
		const char *text;
		QAction *action;
	} buttons[] = {
		{"New", basic->ui->actionNewSceneCollection},       {"Duplicate", basic->ui->actionDupSceneCollection},
		{"Rename", basic->ui->actionRenameSceneCollection}, {"Remove", basic->ui->actionRemoveSceneCollection},
		{"Import", basic->ui->actionImportSceneCollection}, {"Export", basic->ui->actionExportSceneCollection},
	};
	for (auto &b : buttons) {
		QAction *action = b.action;
		if (!action) {
			continue;
		}
		auto *btn = new QPushButton(b.text);
		QObject::connect(btn, &QPushButton::clicked, action, [action]() { action->trigger(); });
		top->addWidget(btn);
	}
	v->addLayout(top);

	auto *hint = new QLabel("Double-click a collection to use it.");
	hint->setObjectName("msMuted");
	v->addWidget(hint);

	collList = new QListWidget();
	collList->setObjectName("msList");
	QObject::connect(collList, &QListWidget::itemDoubleClicked, basic, [](QListWidgetItem *item) {
		std::string name = item->text().toStdString();
		obs_frontend_set_current_scene_collection(name.c_str());
	});
	v->addWidget(MsCard(collList), 1);
	return page;
}

// The Channels panel (from the Channels plugin) lives on the Go Live page, not as a floating panel
void MultistreamPages::TakeChannelsPanel()
{
	auto *dock = basic->findChild<QDockWidget *>("multistream-channels");
	if (!dock || !dock->widget() || !channelsBox) {
		return;
	}
	QWidget *w = dock->widget();
	dock->setWidget(new QWidget());
	channelsBox->addWidget(w);
	w->show();
	dock->hide();
	dock->toggleViewAction()->setVisible(false);
}

/* ---------- pages ---------- */

void MultistreamPages::HideEditorPanels()
{
	hiddenDocks.clear();
	for (QDockWidget *dock : basic->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
		if (dock->objectName() == "multistream-channels") {
			continue;
		}
		if (dock->isVisible() && !dock->isFloating()) {
			hiddenDocks.append(dock);
			dock->hide();
		}
	}
}

void MultistreamPages::RestoreEditorPanels()
{
	for (auto &dock : hiddenDocks) {
		if (dock) {
			dock->show();
		}
	}
	hiddenDocks.clear();
}

void MultistreamPages::Show(int page)
{
	if (page == current) {
		return;
	}

	if (current == PageMultiview) {
		StopMultiview();
	}

	if (page == PageStudio) {
		RestoreEditorPanels();
	} else if (current == PageStudio || current == -1) {
		HideEditorPanels();
	}

	current = page;
	stack->setCurrentIndex(page);
	if (QAbstractButton *b = nav->button(page)) {
		b->setChecked(true);
	}

	if (page == PageGoLive) {
		UpdateMainChannel();
	} else if (page == PageMultiview) {
		StartMultiview();
	} else if (page == PageRecordings) {
		RefreshRecordings();
	} else if (page == PageCollections) {
		RefreshCollections();
	}
}

void MultistreamPages::UpdateStreamState()
{
	bool live = basic->StreamingActive();
	if (goButton) {
		goButton->setText(live ? "Stop streaming" : "Start streaming");
		goButton->setProperty("live", live);
		goButton->style()->unpolish(goButton);
		goButton->style()->polish(goButton);
	}
	if (goState) {
		goState->setText(live ? "LIVE" : "Offline");
	}
	if (liveChip) {
		liveChip->setText(live ? "● LIVE" : "● Offline");
		liveChip->setProperty("live", live);
		liveChip->style()->unpolish(liveChip);
		liveChip->style()->polish(liveChip);
	}
}

void MultistreamPages::UpdateMainChannel()
{
	if (!mainChannel) {
		return;
	}
	QString where = "not set";
	obs_service_t *service = obs_frontend_get_streaming_service();
	if (service) {
		OBSDataAutoRelease settings = obs_service_get_settings(service);
		const char *name = obs_data_get_string(settings, "service");
		const char *server = obs_data_get_string(settings, "server");
		if (name && *name) {
			where = QString::fromUtf8(name);
		} else if (server && *server) {
			where = QString::fromUtf8(server);
		}
	}
	mainChannel->setText("Main channel: " + where);
}

void MultistreamPages::RefreshRecordings()
{
	if (!recList) {
		return;
	}
	recList->clear();
	const char *path = basic->GetCurrentOutputPath();
	QString folder = path ? QString::fromUtf8(path) : QString();
	recFolder->setText(folder.isEmpty() ? "No recording folder set (Settings > Output)." : "Folder: " + folder);
	if (folder.isEmpty()) {
		return;
	}

	QDir dir(folder);
	const QStringList kinds = {"*.mkv", "*.mp4", "*.mov", "*.flv", "*.ts", "*.m4v", "*.hybrid.mp4"};
	const QFileInfoList files = dir.entryInfoList(kinds, QDir::Files, QDir::Time);
	for (const QFileInfo &f : files) {
		QString size = QString::number(double(f.size()) / (1024.0 * 1024.0), 'f', 1) + " MB";
		auto *item = new QListWidgetItem(f.fileName() + "    " +
						 f.lastModified().toString("d MMM yyyy, h:mm ap") + "    " + size);
		item->setData(Qt::UserRole, f.absoluteFilePath());
		recList->addItem(item);
	}
	if (files.isEmpty()) {
		recList->addItem("No recordings yet.");
	}
}

void MultistreamPages::RefreshCollections()
{
	if (!collList) {
		return;
	}
	collList->clear();
	char *cur = obs_frontend_get_current_scene_collection();
	char **names = obs_frontend_get_scene_collections();
	for (char **n = names; n && *n; n++) {
		auto *item = new QListWidgetItem(QString::fromUtf8(*n));
		if (cur && strcmp(cur, *n) == 0) {
			QFont f = item->font();
			f.setBold(true);
			item->setFont(f);
			item->setSelected(true);
		}
		collList->addItem(item);
	}
	bfree(names);
	bfree(cur);
}

/* ---------- Multiview ---------- */

void MultistreamPages::StartMultiview()
{
	if (multiview) {
		return;
	}
	multiview = new Multiview();
	UpdateMultiview();
}

void MultistreamPages::StopMultiview()
{
	if (!multiview) {
		return;
	}
	// the picture stops using it first, then it is thrown away
	obs_enter_graphics();
	Multiview *old = multiview;
	multiview = nullptr;
	obs_leave_graphics();
	delete old;
}

void MultistreamPages::UpdateMultiview()
{
	if (!multiview) {
		return;
	}
	auto layout =
		static_cast<MultiviewLayout>(config_get_int(App()->GetUserConfig(), "BasicWindow", "MultiviewLayout"));
	bool drawLabel = config_get_bool(App()->GetUserConfig(), "BasicWindow", "MultiviewDrawNames");
	bool drawSafeArea = config_get_bool(App()->GetUserConfig(), "BasicWindow", "MultiviewDrawAreas");

	obs_enter_graphics();
	mvUpdating = true;
	obs_leave_graphics();
	multiview->Update(layout, drawLabel, drawSafeArea);
	obs_enter_graphics();
	mvUpdating = false;
	obs_leave_graphics();
}

/* ---------- OBSBasic hooks ---------- */

void OBSBasic::MultistreamSetup()
{
	if (msPages) {
		return;
	}
	msPages = new MultistreamPages(this);
	msPages->Build();
}

void OBSBasic::MultistreamEvent(enum obs_frontend_event event)
{
	if (!msPages) {
		return;
	}
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		msPages->UpdateStreamState();
		msPages->UpdateMainChannel();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		if (msPages->current == PageRecordings) {
			msPages->RefreshRecordings();
		}
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_LIST_CHANGED:
		msPages->RefreshCollections();
		msPages->UpdateMultiview();
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		msPages->UpdateMultiview();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		msPages->UpdateMainChannel();
		break;
	default:
		break;
	}
}
