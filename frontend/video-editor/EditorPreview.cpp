#include "EditorPreview.hpp"

#include <utility/display-helpers.hpp>

#include <QPainter>

#include <algorithm>

EditorPreview::EditorPreview(QWidget *parent) : OBSQTDisplay(parent)
{
	setMinimumSize(320, 180);
	SetDisplayBackgroundColor(Qt::black);
	connect(this, &OBSQTDisplay::DisplayCreated, this, &EditorPreview::Attach);
}

EditorPreview::~EditorPreview()
{
	Clear();
	Detach();
	DestroyDisplay();
	obs_enter_graphics();
	gs_texture_destroy(subtitleTexture);
	obs_leave_graphics();
}

void EditorPreview::Attach()
{
	if (GetDisplay())
		obs_display_add_draw_callback(GetDisplay(), Render, this);
}

void EditorPreview::showEvent(QShowEvent *event)
{
	OBSQTDisplay::showEvent(event);
	obs_display_set_enabled(GetDisplay(), true);
}

void EditorPreview::hideEvent(QHideEvent *event)
{
	obs_display_set_enabled(GetDisplay(), false);
	OBSQTDisplay::hideEvent(event);
}

void EditorPreview::Detach()
{
	if (GetDisplay())
		obs_display_remove_draw_callback(GetDisplay(), Render, this);
}

void EditorPreview::Clear()
{
	if (!source)
		return;
	Detach();
	obs_source_media_stop(source);
	obs_source_set_monitoring_enabled(source, false);
	obs_source_dec_showing(source);
	source = nullptr;
	currentPath.clear();
	Attach();
}

bool EditorPreview::Play(const QString &path, int timeMs)
{
	if (source && path == currentPath) {
		if (obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED) {
			obs_source_media_play_pause(source, false);
			return true;
		}
	}

	Clear();
	if (!(obs_get_source_output_flags("ffmpeg_source") & OBS_SOURCE_CONTROLLABLE_MEDIA))
		return false;
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "local_file", path.toUtf8().constData());
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_bool(settings, "restart_on_activate", true);
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_bool(settings, "clear_on_media_end", false);
	obs_data_set_string(settings, "ffmpeg_options", "protocol_whitelist=file");
	auto *media = obs_source_create_private("ffmpeg_source", "OBS Video Editor Preview", settings);
	if (!media)
		return false;
	Detach();
	source = media;
	currentPath = path;
	obs_source_set_audio_mixers(source, 0);
	obs_source_inc_showing(source);
	Attach();
	obs_source_media_restart(source);
	Seek(timeMs);
	obs_source_set_monitoring_enabled(source, true);
	return true;
}

void EditorPreview::Pause()
{
	if (Playing())
		obs_source_media_play_pause(source, true);
}

bool EditorPreview::Playing() const
{
	return source && obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING;
}

int EditorPreview::Time() const
{
	return source ? static_cast<int>(obs_source_media_get_time(source)) : 0;
}

void EditorPreview::Seek(int timeMs)
{
	if (source)
		obs_source_media_set_time(source, timeMs);
}

void EditorPreview::Render(void *data, uint32_t width, uint32_t height)
{
	auto *preview = static_cast<EditorPreview *>(data);
	const auto &media = preview->source;
	if (!width || !height)
		return;
	const auto videoWidth = obs_source_get_width(media);
	const auto videoHeight = obs_source_get_height(media);
	const auto w = videoWidth && videoHeight ? videoWidth : width;
	const auto h = videoWidth && videoHeight ? videoHeight : height;

	int x, y;
	float scale;
	GetScaleAndCenterPos(static_cast<int>(w), static_cast<int>(h), static_cast<int>(width), static_cast<int>(height),
			     x, y, scale);
	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport(x, y, static_cast<int>(static_cast<float>(w) * scale),
			static_cast<int>(static_cast<float>(h) * scale));
	gs_ortho(0.0f, static_cast<float>(w), 0.0f, static_cast<float>(h), -100.0f, 100.0f);
	if (media)
		obs_source_video_render(media);
	preview->RenderSubtitle(w, h);
	gs_projection_pop();
	gs_viewport_pop();
}

void EditorPreview::SetSubtitle(const QString &text)
{
	if (text == subtitleText)
		return;
	subtitleText = text;
	setAccessibleDescription(text);
	QImage image;
	if (!text.isEmpty()) {
		image = QImage(640, 360, QImage::Format_RGBA8888);
		image.fill(Qt::transparent);
		QPainter painter(&image);
		painter.setRenderHint(QPainter::TextAntialiasing);
		QFont font = painter.font();
		font.setPixelSize(23);
		font.setBold(true);
		painter.setFont(font);
		const int flags = Qt::AlignHCenter | Qt::TextWordWrap;
		auto bounds = painter.boundingRect(QRect(0, 0, 568, 310), flags, text);
		bounds.moveCenter(QPoint(320, 332 - bounds.height() / 2));
		painter.fillRect(bounds.adjusted(-10, -5, 10, 5), QColor(0, 0, 0, 180));
		painter.setPen(Qt::white);
		painter.drawText(bounds, flags, text);
	}
	QMutexLocker lock(&subtitleMutex);
	subtitleImage = std::move(image);
}

void EditorPreview::RenderSubtitle(uint32_t width, uint32_t height)
{
	QImage image;
	{
		QMutexLocker lock(&subtitleMutex);
		image = subtitleImage;
	}
	if (image.isNull())
		return;
	if (image.cacheKey() != renderedSubtitle) {
		if (!subtitleTexture)
			subtitleTexture = gs_texture_create(640, 360, GS_RGBA, 1, nullptr, GS_DYNAMIC);
		if (!subtitleTexture)
			return;
		gs_texture_set_image(subtitleTexture, image.constBits(), static_cast<uint32_t>(image.bytesPerLine()),
				     false);
		renderedSubtitle = image.cacheKey();
	}
	auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), subtitleTexture);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	const auto overlayWidth = std::min(width, static_cast<uint32_t>(height * (640.0 / 360)));
	const auto overlayHeight = std::min(height, static_cast<uint32_t>(width * (360.0 / 640)));
	gs_matrix_push();
	gs_matrix_translate3f(static_cast<float>((width - overlayWidth) / 2), static_cast<float>(height - overlayHeight),
			      0.0f);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(subtitleTexture, 0, overlayWidth, overlayHeight);
	gs_matrix_pop();
	gs_blend_state_pop();
}
