#include "EditorPreview.hpp"

#include <utility/display-helpers.hpp>

EditorPreview::EditorPreview(QWidget *parent) : OBSQTDisplay(parent)
{
	setMinimumSize(320, 180);
	SetDisplayBackgroundColor(Qt::black);
	connect(this, &OBSQTDisplay::DisplayCreated, this, &EditorPreview::Attach);
}

EditorPreview::~EditorPreview()
{
	Clear();
	DestroyDisplay();
}

void EditorPreview::Attach()
{
	if (source && GetDisplay())
		obs_display_add_draw_callback(GetDisplay(), Render, this);
}

void EditorPreview::Detach()
{
	if (source && GetDisplay())
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
}

bool EditorPreview::Play(const QString &path, bool audio)
{
	if (source && path == currentPath) {
		if (Playing())
			obs_source_media_play_pause(source, true);
		else if (obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED)
			obs_source_media_play_pause(source, false);
		else
			obs_source_media_restart(source);
		return true;
	}

	Clear();
	if (!(obs_get_source_output_flags("ffmpeg_source") & OBS_SOURCE_CONTROLLABLE_MEDIA))
		return false;
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "local_file", path.toUtf8().constData());
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_bool(settings, "restart_on_activate", true);
	obs_data_set_bool(settings, "close_when_inactive", true);
	obs_data_set_bool(settings, "clear_on_media_end", false);
	obs_data_set_string(settings, "ffmpeg_options", "protocol_whitelist=file");
	source = obs_source_create_private("ffmpeg_source", "OBS Video Editor Preview", settings);
	if (!source)
		return false;
	currentPath = path;
	obs_source_set_audio_mixers(source, 0);
	if (audio)
		obs_source_set_monitoring_enabled(source, true);
	obs_source_inc_showing(source);
	Attach();
	obs_source_media_restart(source);
	return true;
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
	const auto &media = static_cast<EditorPreview *>(data)->source;
	const auto w = obs_source_get_width(media);
	const auto h = obs_source_get_height(media);
	if (!width || !height || !w || !h)
		return;

	int x, y;
	float scale;
	GetScaleAndCenterPos(static_cast<int>(w), static_cast<int>(h), static_cast<int>(width), static_cast<int>(height),
			     x, y, scale);
	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport(x, y, static_cast<int>(static_cast<float>(w) * scale),
			static_cast<int>(static_cast<float>(h) * scale));
	gs_ortho(0.0f, static_cast<float>(w), 0.0f, static_cast<float>(h), -100.0f, 100.0f);
	obs_source_video_render(media);
	gs_projection_pop();
	gs_viewport_pop();
}
