#include "ui.h"
#include "debug.h"
#include "gettext.h"
#include <deadbeef/deadbeef.h>

#include <vector>
#include <regex>
#include <gtkmm.h>
#include <time.h>
#include <string>
#include <functional>

using namespace std;
using namespace Gtk;
using namespace Glib;

struct linessizes{
	int titlesize;
	int artistsize;
	int newlinesize;
};

const DB_playItem_t *last;

// TODO: eliminate all the global objects, as their initialization is not well defined
	
static TextView *lyricView;
static ScrolledWindow *lyricbar;
static RefPtr<TextBuffer> refBuffer;
static RefPtr<TextTag> tagItalic, tagBold, tagLarge, tagCenter, tagSmall, tagForegroundColorHighlight,tagForegroundColorRegular, tagLeftmargin, tagRightmargin, tagRegular;
static RefPtr<TextTag> tagForegroundColorPast;
static vector<RefPtr<TextTag>> tagsTitle, tagsArtist, tagsSyncline, tagsNosyncline, tagsPast, tagPadding;

bool isValidHexaCode(string str) {
    regex hexaCode("^#([a-fA-F0-9]{6}|[a-fA-F0-9]{3})$");
    return regex_match(str, hexaCode);
}

static void check_playing_track(void);

static void apply_lyrics(DB_playItem_t *track, string past, string present, string future, string padding, bool synced = false);

// Runs fn on the GTK main thread. GTK3 is not thread-safe and glibmm's
// signal_idle() must not be called from other threads either, so worker
// threads marshal every widget access through GLib's thread-safe idle source.
template <typename F>
static void run_on_main_thread(F fn) {
	auto *call = new std::function<void()>(std::move(fn));
	g_idle_add([](gpointer data) -> gboolean {
		auto *f = static_cast<std::function<void()>*>(data);
		(*f)();
		delete f;
		return G_SOURCE_REMOVE;
	}, call);
}

// Reads the geometry of the lyrics that are currently in the view: the height
// of half the panel, the first line that must stay on screen, and the height of
// every line. Must run on the GTK main thread, and only makes sense once the
// view has actually laid out the text (see request_sizelines).
static vector<int> measure_sizes (void) {
	if (!lyricView || !lyricbar || !refBuffer) {
		return vector<int>();
	}

	int sumatory = 0;
	int temporaly = 0;
	vector<int>  values;

	deadbeef->pl_lock();
	values.push_back(lyricbar->get_allocation().get_height()*(deadbeef->conf_get_int("lyricbar.vpostion", 50))/100);
	deadbeef->pl_unlock();
	values.push_back(0);
	Gdk::Rectangle rectangle;
	int last_height = 0;
	for (int i = 2; i < refBuffer->get_line_count()-1; i++) {
		lyricView->get_iter_location(refBuffer->get_iter_at_line(i-2), rectangle);
		int height = rectangle.get_y() - temporaly;
		if (height <= 0 && last_height > 0) {
			// Positions are only reported down to the bottom of the visible
			// area, so every line below the fold measures as no height at all.
			// The lyrics timer reads these heights to decide how far to slide a
			// line, and a 0 there stops the smooth scrolling halfway through a
			// long song, so reuse the last height that was really measured.
			height = last_height;
		}
		else if (height > 0) {
			last_height = height;
		}
		values.push_back(height);
		temporaly = rectangle.get_y();
	}

	values[1] = std::max(0, (int)values.size()-3);

	for (unsigned i = 2; i < values.size()-2; i++) {
		sumatory += values[i];
		if (sumatory > (values[0] - values[3] - values[4])) {
			values[1] = i-2;
			break;
		}
	}

	return values;
}

// "Size" lyrics to be able to make lines "dissapear" on top.
// Must run on the GTK main thread: it touches the lyrics TextView.
vector<int> sizelines(DB_playItem_t * track, string lyrics) {
	// Check if widgets are being destroyed

	// Validate widget pointers
	if (!lyricView || !lyricbar || !refBuffer) {
		return vector<int>();
	}

	if (!is_playing(track)) {
		return vector<int>();
	}
	apply_lyrics(track, lyrics,"","","");

	return measure_sizes ();
}

// A GtkTextView lays out text it was just given during the following
// main-loop iteration, so the positions cannot be read back in the same
// callback that filled the view: every line comes back at y=0, and the
// scrolling maths then has no geometry to work with -- the highlight keeps
// moving through the sheet while the sheet itself stays put. Retry until the
// view reports real line positions, and stop after a second in case the panel
// is not visible at all.
static guint sizes_source = 0;

static gboolean
measure_lyrics_view (gpointer user_data) {
	int attempt = GPOINTER_TO_INT (user_data);
	sizes_source = 0;

	if (!lyricView || !lyricbar || !refBuffer) {
		return G_SOURCE_REMOVE;
	}

	vector<int> values = measure_sizes ();
	bool laid_out = values.size() > 3 && (values[2] > 0 || values[3] > 0);

	if (laid_out || attempt >= 20) {
		set_linessizes (values);
		return G_SOURCE_REMOVE;
	}

	sizes_source = g_timeout_add (50, measure_lyrics_view, GINT_TO_POINTER (attempt + 1));
	return G_SOURCE_REMOVE;
}

// Main-thread-safe entry point for sizelines(): worker threads call this
// instead of touching the widget directly, and the result is stored in the
// linessizes global once the main loop runs.
void request_sizelines(DB_playItem_t *track, string lyrics) {
	run_on_main_thread([track, lyrics]{
		if (!is_playing(track)) {
			return;
		}
		if (!lyricView || !lyricbar || !refBuffer) {
			return;
		}
		apply_lyrics(track, lyrics,"","","");

		if (sizes_source) {
			g_source_remove (sizes_source);
		}
		sizes_source = g_timeout_add (20, measure_lyrics_view, GINT_TO_POINTER (0));
	});
}

// Fills the lyrics text buffer. Must run on the GTK main thread.
static void apply_lyrics(DB_playItem_t *track, string past, string present, string future, string padding, bool synced) {
	if (!refBuffer) {
		return;
	}
	if (!is_playing(track)) {
		return;
	}
	string artist, title;
	deadbeef->pl_lock();
	artist = deadbeef->pl_find_meta(track, "artist") ?: _("Unknown Artist");
	title  = deadbeef->pl_find_meta(track, "title") ?: _("Unknown Title");
	deadbeef->pl_unlock();

	refBuffer->erase(refBuffer->begin(), refBuffer->end());

	refBuffer->insert_with_tags(refBuffer->begin(), title, tagsTitle);

	if (g_utf8_validate(future.c_str(),-1,NULL)){
		refBuffer->insert_with_tags(refBuffer->end(), string{"\n"} + artist + "\n\n", tagsArtist);
		refBuffer->insert_with_tags(refBuffer->end(), padding, tagPadding);
		refBuffer->insert_with_tags(refBuffer->end(),past, synced ? tagsPast : tagsNosyncline);
		refBuffer->insert_with_tags(refBuffer->end(),present, tagsSyncline);
		refBuffer->insert_with_tags(refBuffer->end(),future, tagsNosyncline);
	}
	else{
		string error = "Wrong character encoding";
		refBuffer->insert_with_tags(refBuffer->end(), padding, tagPadding);
		refBuffer->insert_with_tags(refBuffer->end(),error, tagsSyncline);
	}
}

void set_lyrics(DB_playItem_t *track, string past, string present, string future, string padding, bool synced) {
	run_on_main_thread([track, past, present, future, padding, synced] {
		apply_lyrics(track, past, present, future, padding, synced);
	});
}

// To have scroll bars or not when lyrics are synced or not.
void sync_or_unsync(bool syncedlyrics) {
	run_on_main_thread([syncedlyrics]{
		if (!lyricbar) {
			return;
		}
		if (syncedlyrics == true) {
			lyricbar->set_policy(POLICY_EXTERNAL, POLICY_EXTERNAL);
		}
		else{
			lyricbar->set_policy(POLICY_AUTOMATIC, POLICY_AUTOMATIC);
		}
	});
}

Justification get_justification() {

	int align = deadbeef->conf_get_int("lyricbar.lyrics.alignment", 1);
	switch (align) {
		case 0:
			return JUSTIFY_LEFT;
		case 2:
			return JUSTIFY_RIGHT;
		default:
			return JUSTIFY_CENTER;
	}
}

	void get_tags() {
	// Global font scale (DeaDBeeF preferences) times the per-part scale set in
	// the plugin Config dialog, so both places can be used together.
	float fontscale = deadbeef->conf_get_float("lyricbar.fontscale", 1);
	float titleScale = deadbeef->conf_get_float("lyricbar.title.fontscale", 1);
	float artistScale = deadbeef->conf_get_float("lyricbar.artist.fontscale", 1);
	float lyricsScale = deadbeef->conf_get_float("lyricbar.lyrics.fontscale", 1);

	// Font family from the Config dialog. Only the family is used: the size
	// stays under the font scale controls above.
	string fontDesc = deadbeef->conf_get_str_fast("lyricbar.font", "");
	const char *fontFamily = nullptr;
	PangoFontDescription *fontDescription = nullptr;
	if (!fontDesc.empty()) {
		fontDescription = pango_font_description_from_string(fontDesc.c_str());
		fontFamily = pango_font_description_get_family(fontDescription);
	}

	tagItalic = refBuffer->create_tag();
	tagItalic->property_style() = Pango::STYLE_ITALIC;
	tagItalic->property_scale() = artistScale*fontscale;

	tagRegular = refBuffer->create_tag();
	tagRegular->property_scale() = lyricsScale*fontscale;

	tagLeftmargin = refBuffer->create_tag();
	tagLeftmargin->property_left_margin() = deadbeef->conf_get_float("lyricbar.border", 22)*deadbeef->conf_get_float("lyricbar.fontscale", 1);

	tagRightmargin = refBuffer->create_tag();
	tagRightmargin->property_right_margin() = deadbeef->conf_get_float("lyricbar.border", 22)*deadbeef->conf_get_float("lyricbar.fontscale", 1);

	tagBold = refBuffer->create_tag();
	tagBold->property_scale() = lyricsScale*fontscale;
	tagBold->property_weight() = Pango::WEIGHT_BOLD;

	tagLarge = refBuffer->create_tag();
	tagLarge->property_scale() = titleScale*fontscale;
	tagLarge->property_weight() = Pango::WEIGHT_BOLD;

	if (fontFamily) {
		tagItalic->property_family() = fontFamily;
		tagRegular->property_family() = fontFamily;
		tagBold->property_family() = fontFamily;
		tagLarge->property_family() = fontFamily;
	}
	if (fontDescription) {
		pango_font_description_free(fontDescription);
	}

	tagSmall = refBuffer->create_tag();
	tagSmall->property_scale() = 0.0001;

	tagCenter = refBuffer->create_tag();
	tagCenter->property_justification() = JUSTIFY_CENTER;

	tagForegroundColorHighlight = refBuffer->create_tag();
	tagForegroundColorRegular = refBuffer->create_tag();

	if (isValidHexaCode(deadbeef->conf_get_str_fast("lyricbar.highlightcolor", "#571c1c"))){
		tagForegroundColorHighlight->property_foreground() = deadbeef->conf_get_str_fast("lyricbar.highlightcolor", "#571c1c");
	}
	else{
		tagForegroundColorHighlight->property_foreground() = "#571c1c";
	}

	if (isValidHexaCode(deadbeef->conf_get_str_fast("lyricbar.regularcolor", "#000000"))){
		tagForegroundColorRegular->property_foreground() = deadbeef->conf_get_str_fast("lyricbar.regularcolor", "#000000");
	}
	else{
		tagForegroundColorRegular->property_foreground() = "#000000";
	}

	// Like OpenLyrics' background blend, but retain half the text contrast.
	// A separate tag leaves upcoming and untimestamped lyrics untouched.
	Gdk::RGBA pastColor, backgroundColor;
	pastColor.set(isValidHexaCode(deadbeef->conf_get_str_fast("lyricbar.regularcolor", "#000000"))
		? deadbeef->conf_get_str_fast("lyricbar.regularcolor", "#000000") : "#000000");
	backgroundColor.set(isValidHexaCode(deadbeef->conf_get_str_fast("lyricbar.backgroundcolor", "#F6F6F6"))
		? deadbeef->conf_get_str_fast("lyricbar.backgroundcolor", "#F6F6F6") : "#F6F6F6");
	pastColor.set_rgba((pastColor.get_red() + backgroundColor.get_red()) / 2.0,
		(pastColor.get_green() + backgroundColor.get_green()) / 2.0,
		(pastColor.get_blue() + backgroundColor.get_blue()) / 2.0, 1.0);
	tagForegroundColorPast = refBuffer->create_tag();
	tagForegroundColorPast->property_foreground_rgba() = pastColor;

	tagsTitle = {tagLarge, tagCenter};
	tagsArtist = {tagItalic, tagCenter};

	tagsSyncline = {tagBold, tagForegroundColorHighlight};

	if (deadbeef->conf_get_int("lyricbar.bold", 1) == 1) {
		tagsSyncline = {tagBold, tagForegroundColorHighlight};
		tagsNosyncline = {tagRegular, tagLeftmargin, tagRightmargin, tagForegroundColorRegular};
	}
	else{
		tagsSyncline = {tagRegular, tagForegroundColorHighlight};
		tagsNosyncline = {tagRegular, tagForegroundColorRegular};
    	}

    if (get_justification() == JUSTIFY_LEFT) {
		tagRightmargin->property_right_margin() = deadbeef->conf_get_float("lyricbar.border", 22)*deadbeef->conf_get_float("lyricbar.fontscale", 1);
		tagsNosyncline = {tagRegular, tagRightmargin};
    }
	if (get_justification() == JUSTIFY_RIGHT) {
    	tagLeftmargin->property_left_margin() = deadbeef->conf_get_float("lyricbar.border", 22)*deadbeef->conf_get_float("lyricbar.fontscale", 1);
		tagsNosyncline = {tagRegular, tagLeftmargin};
    }

	tagsPast = tagsNosyncline;
	tagsPast.push_back(tagForegroundColorPast);

	tagPadding = {tagSmall};
}

extern "C"

GtkWidget *construct_lyricbar() {
	Gtk::Main::init_gtkmm_internals();
	refBuffer = TextBuffer::create();
	get_tags();
	lyricView = new TextView(refBuffer);
	lyricView->set_editable(false);
	lyricView->set_can_focus(false);
	lyricView->set_name("lyricView");
	lyricView->set_left_margin(2);
	lyricView->set_right_margin(2);
	lyricView->set_justification(get_justification());
    	lyricView->get_vadjustment()->set_value(1000);
	lyricView->set_wrap_mode(WRAP_WORD_CHAR);
    	if (get_justification() == JUSTIFY_LEFT) {
    		lyricView->set_left_margin(20);
    	}
	lyricView->show();
	lyricbar = new ScrolledWindow();
	lyricbar->add(*lyricView);
	lyricbar->set_name("lyricbar");
	lyricbar->set_policy(POLICY_EXTERNAL, POLICY_EXTERNAL);


	/**********/

	//Create css config text.
	std::string cssconfig("\
  		#lyricView text {\
  		background-color: ");
	if (isValidHexaCode(deadbeef->conf_get_str_fast("lyricbar.backgroundcolor", "#F6F6F6"))){
		cssconfig.append(deadbeef->conf_get_str_fast("lyricbar.backgroundcolor", "#F6F6F6"));
	}
	else{
		cssconfig.append("#F6F6F6");
	}
	cssconfig.append(";\
		}\
	");

	//load css
	auto data = g_strdup_printf("%s", cssconfig.c_str());

	RefPtr<Gtk::CssProvider> cssProvider = Gtk::CssProvider::create();
	cssProvider->load_from_data(data);
	
	RefPtr<Gtk::StyleContext> styleContext = Gtk::StyleContext::create();
	
	//get default screen
	RefPtr<Gdk::Screen> screen = Gdk::Screen::get_default();
	
	//add provider for screen in all application
	styleContext->add_provider_for_screen(screen, cssProvider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	// Pick up a track that is already playing when this widget appears: with
	// "deadbeef <file>" playback starts before the widget exists, so the
	// SONGSTARTED event is never delivered to it.
	signal_idle().connect_once([]{ check_playing_track(); });
	signal_timeout().connect_once([]{ check_playing_track(); }, 1500);

	return GTK_WIDGET(lyricbar->gobj());
}




// Thread body: load lyrics, then release the reference taken by
// start_lyrics_update().
static void lyrics_thread(void *tr) {
	update_lyrics(tr);
	deadbeef->pl_item_unref(static_cast<DB_playItem_t *>(tr));
}

// Kick off lyrics loading for a track, unless it is the one already being
// displayed. The track is referenced for the detached worker thread.
static void start_lyrics_update(DB_playItem_t *track) {
	if (!track || track == last || deadbeef->pl_get_item_duration(track) <= 0.0) {
		return;
	}
	last = track;
	deadbeef->pl_item_ref(track);
	auto tid = deadbeef->thread_start(lyrics_thread, track);
	deadbeef->thread_detach(tid);
}

// Load lyrics for whatever the streamer is currently playing. This recovers the
// track when playback started before this widget was created -- e.g. deadbeef
// was launched with a file (double-click / "deadbeef <file>") -- because gtkui
// only starts delivering events to widgets after its own initialization, by
// which point DB_EV_SONGSTARTED has already been dropped.
static void check_playing_track(void) {
	DB_playItem_t *track = deadbeef->streamer_get_playing_track_safe();
	if (track) {
		start_lyrics_update(track);
		deadbeef->pl_item_unref(track);
	}
}

extern "C"
int message_handler(struct ddb_gtkui_widget_s*, uint32_t id, uintptr_t ctx, uint32_t, uint32_t) {
	auto event = reinterpret_cast<ddb_event_track_t *>(ctx);
	switch (id) {
		case DB_EV_CONFIGCHANGED:
			debug_out << "CONFIG CHANGED\n";
			// message() is delivered on the player's mainloop thread, which is
			// not the GTK main thread. get_tags() creates and configures
			// Gtk::TextTags and set_justification() touches the view, so both
			// must be marshalled onto the GTK main thread. (glibmm's
			// signal_idle() must not be used from other threads either.)
			run_on_main_thread([]{
				if (!refBuffer) {
					return;
				}
				get_tags();
				if (lyricView) {
					lyricView->set_justification(get_justification());
				}
			});
			break;
		case DB_EV_TERMINATE:
			debug_out << "DEADBEEF CLOSED \n";
			lyricbar_request_shutdown();
			break;
//		case DB_EV_TRACKINFOCHANGED:
//			debug_out << "TRACKINFOCHANGED" << "\n";
//			break;
		case DB_EV_PLUGINSLOADED:
		case DB_EV_SONGSTARTED:
			debug_out << "SONG STARTED\n";
			start_lyrics_update(event->track);
			break;
		case DB_EV_SONGCHANGED:
			// When deadbeef is launched with a file (double-click / "deadbeef
			// <file>"), playback can start before this widget exists and
			// SONGSTARTED is dropped, so ask the streamer what is playing.
			debug_out << "SONG CHANGED\n";
			check_playing_track();
			break;
		case DB_EV_PLAYLISTCHANGED:
		case DB_EV_PLAYBACK_STATE_DID_CHANGE:
			// These are delivered during startup (gtkui sends
			// DB_EV_PLAYLISTCHANGED as soon as it starts accepting messages),
			// which gives us a chance to pick up a track that is already
			// playing.
			check_playing_track();
			break;
	}

	return 0;
}

extern "C"
void lyricbar_destroy() {
	// A pending measure must not run against the deleted widgets.
	if (sizes_source) {
		g_source_remove (sizes_source);
		sizes_source = 0;
	}
	delete lyricbar;
	delete lyricView;
	lyricbar = nullptr;
	lyricView = nullptr;
	tagsArtist.clear();
	tagPadding.clear();
	tagsSyncline.clear();
	tagsNosyncline.clear();
	tagsPast.clear();
	tagForegroundColorPast.reset();
	tagRegular.clear();
	tagsTitle.clear();
	tagLarge.reset();
	tagSmall.reset();
	tagBold.reset();
	tagItalic.reset();
	tagRightmargin.reset();
	tagLeftmargin.reset();
	refBuffer.reset();
}
