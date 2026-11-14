#include "utils.h"
#include "debug.h"
#include "gettext.h"
#include "sources/lrcspotify.h"
#include "sources/lrclrclib.h"
#include "ui.h"
#include "gettext.h"

#include <sys/stat.h>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <cstring>
#include <system_error>
#include <pthread.h>
#include <atomic>
#include <mutex>


using namespace std;

//Global variables:

//static const char *home_cache = getenv("XDG_CACHE_HOME");
//static const string lyrics_dir = (home_cache ? string(home_cache) : string(getenv("HOME")) + "/.cache") + "/deadbeef/lyrics/";


std::atomic<bool> death_signal{false};
std::atomic<unsigned> listener_generation{0};
extern "C" void lyricbar_request_shutdown(void) {
	death_signal.store(true);
}
void cancel_synced_lyrics_listener() {
	++listener_generation;
}
bool lyricstart     = false;
bool syncedlyrics   = false;
int previous_pad    = 0;
struct timespec ts  = {0, 50000000};
static vector<int> linessizes;
static std::mutex linessizes_mutex;
void set_linessizes(vector<int> sizes) {
	std::lock_guard<std::mutex> lock(linessizes_mutex);
	linessizes = std::move(sizes);
}
vector<int> get_linessizes() {
	std::lock_guard<std::mutex> lock(linessizes_mutex);
	return linessizes;
}
struct sync lrc;
struct sync_view lrc_view;
vector<int> linescharactersizes;


// For debug:
bool is_playing(DB_playItem_t *track) {
	DB_playItem_t *pl_track = deadbeef->streamer_get_playing_track_safe();
	if ((pl_track) && (pl_track != track)){
		deadbeef->pl_item_unref(pl_track);
		return false;
	}
	else if (!pl_track){
		return false;
	}
	else{
	deadbeef->pl_item_unref(pl_track);
	return pl_track == track;
	}
}

//---------------------------------------------------------------------
//************ Functions to make scroll on sync lyrics. ***************
//---------------------------------------------------------------------

// Functions to bubble sort lyrics and timestamps.
void swap(double *xp, double *yp)
{
    double temp = *xp;
    *xp = *yp;
    *yp = temp;
}

void swaptext(string *xp, string *yp)
{
    string temp = *xp;
    *xp = *yp;
    *yp = temp;
}

// Fuctions to order lyrics and timestamps.
struct sync bubbleSort(vector<double> arr, vector<string> text ,  int n)
{
	if ((arr.size() == 0) || (text.size() != arr.size())){
		vector<double> nullposition;
		vector<string> errortext;
		nullposition.push_back(0);
		errortext.push_back(_("Error with lyrics file or metadata"));
		cout << "Error with lyrics file or metadata \n";
		struct sync error = {errortext, nullposition};
		return error;
	}
	else{
    	int i, j;
    	for (i = 0; i < n-1; i++){   
    		// Last i elements are already in place.
    		for (j = 0; j < n-i-1; j++){
        		if (arr[j] > arr[j+1]){
        		    swap(&arr[j], &arr[j+1]);
					swaptext(&text[j], &text[j+1]);
				}
			}
		}
	struct sync retur = {text,arr};
	return retur;
	}
}

// Function to parse synced lyrics:
struct sync lyric2vector( string lyrics){
	//cout << "lyric2vector" "\n";
	vector<string> synclyrics;
	vector<double> position;
	string line;
	int squarebracket;
	int repeats = 0; 
	//If last character is a ] add an space to have same number of lyrics and positions.
	if (lyrics.at(lyrics.length() - 1) == ']'){
		lyrics.push_back(' ');
	}
	if (lyrics.length() <= 3){
		position.push_back(0);
		struct sync  emptylyrics = bubbleSort(position, synclyrics, 1);
		return emptylyrics;
	}

	for (unsigned i=0; i < lyrics.length() - 3; ++i){
		if ((lyrics.at(i) == '[') && (lyrics.at(i+3) == ':') && (lyrics.at(i+6) == '.') ) {
  			++repeats;
			position.push_back ((lyrics.at(i + 1) - 48)*600 + (lyrics.at(i + 2) - 48)*60 + (lyrics.at(i + 4) - 48)*10 + (lyrics.at(i + 5) - 48) + (lyrics.at(i + 7) - 48)*0.1 + (lyrics.at(i + 8) - 48)*0.01);
			if ((lyrics.length() > i + 10) && (lyrics.at(i+10) != '[')) {
				line = lyrics.substr(i + 10, lyrics.length() - i - 10);
				squarebracket = line.find_first_of('[');
				if (((lyrics.at(i+8 + squarebracket)) != '\n') && (lyrics.at(i+8 + squarebracket)) != '\r'){
                    line = lyrics.substr(i + 10, squarebracket-1);
				}
				else {
                    line = lyrics.substr(i + 10, squarebracket-2);
				}
				++repeats;
				while (--repeats ) {
					synclyrics.push_back (line);
				}	
			}
		}
	}

	int n = position.size();
	struct sync  goodlyrics = bubbleSort(position, synclyrics, n);

// For debug:
//	for (auto k = goodlyrics.position.begin(); k != goodlyrics.position.end(); ++k)
//    	cout << *k << " ";
//
//	for (auto i = goodlyrics.synclyrics.begin(); i != goodlyrics.synclyrics.end(); ++i)
//    	cout << *i << '\n';
	
	return goodlyrics;
}

// Function to scroll and remark line:
void write_synced( DB_playItem_t *it){
	float pos = deadbeef->streamer_get_playpos();
	int presentpos = 0;
	int minimuntopad = 0;
	int pad = 0;
	string past = "";
	string present = "";
	string future = "";
	string padding = "";

	// The per-line heights are produced on the GTK main thread; take a
	// snapshot so the vector cannot be reallocated under us while we read it.
	vector<int> sizes = get_linessizes();
	
	//If to avoid write again when nothing has changed or DeadBeef is paused.
	if ((lrc.position.size() < 2) || (deadbeef->get_output()->state() == 2)) {
		return;
	}
	
	//Set actual line, and fill future with next lines.
	for (unsigned i = 0; i < lrc.position.size()-2; i++){
		if (lrc.position[i] < pos){
			if (lrc.position[i+1] > pos){
				present = " ";
				present.append(lrc.synclyrics[i]);
				present.append(" ");
				present.append("\n");
				presentpos = i;
			}
		}
		else if (pos < lrc.position[i]){
				future.append(lrc.synclyrics[i]);
				future.append("\n");
		}
	}

	//Add padding variable at beginning of lyrics to show to make scroll with first lines.
	if (!sizes.empty() && (size_t)(sizes[1]+1) < lrc.position.size()){
	    pad = int((lrc.position[sizes[1]+1] - pos)/(lrc.position[sizes[1]+1])*sizes[0]);
	    if (pad == previous_pad){
	        return; //If to avoid write again when nothing has changed.
	    }
		padding.assign(std::max(0, pad), '\n');
	    if (pad > 0){
	        previous_pad = pad;	//Update only when is really something to pad to don't override next use of pad/previous_pad.
	    }	
	}

	//Add padding variable at beginning of lyrics to show to make scroll when removing a past line.
	if (((!sizes.empty()) && (presentpos - sizes[1]) > 0) && (size_t)(presentpos+1) < lrc.position.size()){
		minimuntopad = presentpos - sizes[1];
		int lineheight = 0;
		int idx = minimuntopad + 5;
		if (idx >= 0 && idx < (int)sizes.size()) {
			lineheight = sizes[idx] - 1;
		}
		pad = int(((lrc.position[presentpos +1] - pos)/(lrc.position[presentpos+1]
		     -lrc.position[presentpos]))*lineheight);
	    if (pad == previous_pad){
	        return; //"if" to avoid write again when nothing has changed.
	    }
		padding.assign(std::max(0, pad), '\n');
        previous_pad = pad;
	}

	//Removing first past lyrics lines to make scroll.
	for (unsigned i = minimuntopad; i < lrc.position.size()-2 && lrc.position[i+1] < pos; i++){
		past.append(lrc.synclyrics[i] + "\n");
	}
//		cout << "Pos: " << pos << " min2pad: " << minimuntopad << " lrc.position[minimuntopad + 1]: " << lrc.synclyrics[minimuntopad + 1] << "                                 " << "LCS: " << linescharactersizes.size() << "\t\r" << std::flush;
	set_lyrics(it, past, present, future, padding, true);
}

// Main loop to update lyrics on real time.
void thread_listener(DB_playItem_t *track, unsigned generation) {
	while (!death_signal.load() &&
		listener_generation.load() == generation &&
		is_playing(track)) {
		nanosleep(&ts, NULL);

		if (death_signal.load() ||
			listener_generation.load() != generation) {
			break;
		}

		write_synced(track);
	}

	deadbeef->pl_item_unref(track);
}

// Main loop thread caller.
void chopset_lyrics(DB_playItem_t *track, string lyrics){
//	cout << "Chopset lyrics" "\n";
	lrc = lyric2vector(lyrics);
	DB_playItem_t *it = deadbeef->streamer_get_playing_track_safe();

	if (!it) {
		return;
	}

	float length = deadbeef->pl_get_item_duration(it);
	
    linescharactersizes.clear();
	lrc.position.push_back((float)length -0.2);
	lrc.position.push_back((float)length);
	lrc.synclyrics.push_back("\n");
	lrc.synclyrics.push_back("\n");
	string prelyrics = "";
	
	for (unsigned i = 0; i < lrc.synclyrics.size()-2; i++){
		prelyrics.append(lrc.synclyrics[i] + "\n");
//		linescharactersizes.push_back(lrc.synclyrics[i].size());
	}
//	for (unsigned i = 0; i < linescharactersizes.size()-2; i++){
//	    cout << linescharactersizes[i] << " ";
//	}
	
	linescharactersizes.push_back(1);
	linescharactersizes.push_back(1);
	
	if (track == it){
		// sizelines() touches the lyrics TextView, and GTK is not thread-safe:
		// request_sizelines() marshals the work onto the GTK main thread and
		// stores the result in the linessizes global.
		request_sizelines(track, prelyrics);
	}

	deadbeef->pl_item_ref(it);
	unsigned generation = ++listener_generation;
	thread t1(thread_listener, it, generation);
	t1.detach();
	deadbeef->pl_item_unref(it);
}

//---------------------------------------------------------------------
//*********** /Functions to make scroll on sync lyrics. ***************
//---------------------------------------------------------------------


//---------------------------------------------------------------------
//************ Share functions of lyrics downloaders. *****************
//---------------------------------------------------------------------

//	Function to save lyrics to tag.
void save_meta_data(DB_playItem_t *playing_song, struct parsed_lyrics lyrics){

	deadbeef->pl_lock();
	const char *SYLT = deadbeef->pl_find_meta(playing_song, "SYLT");
	const char *LYRICS = deadbeef->pl_find_meta(playing_song, "LYRICS");
	const char *UNSYNCEDLYRICS = deadbeef->pl_find_meta(playing_song, "UNSYNCEDLYRICS");
	deadbeef->pl_unlock();

	// Be sure it's UTF-8.
	char *safe_lyrics = g_utf8_make_valid(lyrics.lyrics.c_str(), -1);

	if (lyrics.sync == true){	
		if (SYLT){
			deadbeef->pl_delete_meta(playing_song, "SYLT");
		}

		if (LYRICS){
			deadbeef->pl_delete_meta(playing_song, "LYRICS");
		}
		deadbeef->pl_add_meta(playing_song, "LYRICS", safe_lyrics);	
	}
	else{
		if (UNSYNCEDLYRICS){
			deadbeef->pl_delete_meta(playing_song, "UNSYNCEDLYRICS");
		}
		deadbeef->pl_add_meta(playing_song, "UNSYNCEDLYRICS", safe_lyrics);	
	}

	g_free(safe_lyrics);

	deadbeef->pl_lock(); 
	const char *dec = deadbeef->pl_find_meta_raw(playing_song, ":DECODER"); 
	char decoder_id[100];

	if (dec){
		strncpy(decoder_id, dec, sizeof(decoder_id));
	}
	int match = playing_song && dec;
	deadbeef->pl_unlock();

	if (match){
		DB_decoder_t *dec = NULL;
		DB_decoder_t **decoders = deadbeef->plug_get_decoder_list();

		for (int i = 0; decoders[i]; i++){

			if (!strcmp(decoders[i]->plugin.id, decoder_id)){
				dec = decoders[i];

				if (dec->write_metadata) {
					dec->write_metadata(playing_song);
				}
				break;
			}
		}
	}
}


// Function to encode characters to URL hexadecimal.
std::string urlencode(const std::string &s) {
    static const char hexachars[]= "0123456789abcdef";
    std::string converted;
    for(int i=0, ix=s.length(); i<ix; i++) {
        const char& c = s[i];
        if ( (c >= 'A' &&  c <= 'Z') || (c >= 'a' &&  c <= 'z') || (c >= '0' &&  c <= '9') || (c=='-' || c=='_' || c=='.' || c=='~')) {
            converted = converted + c;
        }
        else {
            converted = converted + '%';
            converted = converted + hexachars[(c&0xF0)>>4];
            converted = converted + hexachars[(c&0x0F)];
        }
    }
    return converted;
}

// Function to remove special characters.
string specialforplus(const char* text) {
	string counter = string(text);
	for(unsigned i = 0; i < counter.size(); i++)
    {
        if( (counter[i] < 'A' ||  counter[i] > 'Z') &&  (counter[i] < 'a' ||  counter[i] > 'z') &&  (counter[i] < '0' ||  counter[i] > '9')) {  
            counter[i] = '+';
		}
    }
    return counter;
}
// Function to remove replace substrings.
std::string replace_string(string subject, const string& search, const string& replace) {
    size_t pos = 0;
    while ((pos = subject.find(search, pos)) != string::npos) {
		subject.replace(pos, search.length(), replace);
		pos += replace.length();
    }
    return subject;
}

size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Function to split strings.
vector<string> split(string s, string delimiter) {
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    string token;
    vector<string> res;

    while ((pos_end = s.find(delimiter, pos_start)) != string::npos) {
        token = s.substr (pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back (token);
    }
    res.push_back (s.substr (pos_start));
    return res;
}

// Function to parse webpages.
string text_downloader(curl_slist *slist, string url, string post) {
	CURL *curl_handle;
	curl_handle = curl_easy_init();
	string readBuffer = "";
	if (curl_handle) {	
		curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION,
						 [](void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
							 return death_signal.load() ? 1 : 0;
						 });
		curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 600);
		curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0);
		curl_easy_setopt(curl_handle, CURLOPT_VERBOSE, 0);
		curl_easy_setopt(curl_handle, CURLOPT_HEADER, 0);
		curl_easy_setopt(curl_handle, CURLOPT_ENCODING, NULL);
		curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, false);
		curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, slist);
		if (post != ""){
			curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, "POST");
		    curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, post.c_str());
		}
		else{
		    curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, "GET");
		}
		curl_easy_setopt(curl_handle, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteCallback);
		curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &readBuffer);
		curl_easy_perform(curl_handle);
		curl_easy_cleanup(curl_handle);
	}
	return readBuffer;
}

//---------------------------------------------------------------------
//************ /Share functions of lyrics downloaders. ****************
//---------------------------------------------------------------------

//inline string cached_filename(string artist, string title) {
//	return 0;
//}

extern "C"
bool is_cached(const char *artist, const char *title) {
	return false;
}

//---------------------------------------------------------------------
//****** Lyrics file location *****************************************
//---------------------------------------------------------------------

// Folder with lyrics files. Empty means "next to the audio file".
static string lyrics_folder() {
	const char *dir = deadbeef->conf_get_str_fast("lyricbar.lyrics_dir", "");
	return dir ? dir : "";
}

// File name pattern used inside lyricbar.lyrics_dir, foobar2000 style,
// e.g. "[%artist% - ][%title%]".
static string lyrics_filename_format() {
	const char *fmt = deadbeef->conf_get_str_fast("lyricbar.lyrics_filename_format", "[%artist% - ][%title%]");
	return (fmt && *fmt) ? fmt : "[%artist% - ][%title%]";
}

static string track_meta(DB_playItem_t *track, const char *key) {
	deadbeef->pl_lock();
	const char *value = deadbeef->pl_find_meta(track, key);
	string result = value ? value : "";
	deadbeef->pl_unlock();
	return result;
}

static string trim_spaces(string s) {
	size_t start = s.find_first_not_of(" \t");
	if (start == string::npos) {
		return "";
	}
	size_t end = s.find_last_not_of(" \t");
	return s.substr(start, end - start + 1);
}

// Replaces the characters foobar2000 replaces with '-' in lyrics file names,
// so that both players resolve the same files.
static string sanitize_lyrics_filename(string name) {
	static const char invalid[] = "/\\<>:\"|?*";
	for (size_t i = 0; i < name.size(); i++) {
		if (strchr(invalid, name[i])) {
			name[i] = '-';
		}
	}
	return trim_spaces(name);
}

// Expands %tag% tokens and [optional groups], the way foobar2000 does:
// a [group] is dropped when any %tag% inside it is empty, so
// "[%artist% - ][%title%]" gives "Artist - Title" or just "Title".
static string expand_lyrics_format(const string &format, DB_playItem_t *track, bool &missing) {
	missing = false;
	string out;
	size_t i = 0;
	while (i < format.size()) {
		char c = format[i];
		if (c == '[') {
			int depth = 1;
			string inner;
			size_t j = i + 1;
			while (j < format.size()) {
				if (format[j] == '[') {
					depth++;
				}
				else if (format[j] == ']') {
					depth--;
					if (depth == 0) {
						break;
					}
				}
				inner += format[j];
				j++;
			}
			bool inner_missing = false;
			string expanded = expand_lyrics_format(inner, track, inner_missing);
			if (!inner_missing && !expanded.empty()) {
				out += expanded;
			}
			i = (j < format.size()) ? j + 1 : j;
		}
		else if (c == '%') {
			size_t j = format.find('%', i + 1);
			if (j == string::npos) {
				out += format.substr(i);
				break;
			}
			string token = format.substr(i + 1, j - i - 1);
			if (token == "filename" || token == "filename_base") {
				string uri = track_meta(track, ":URI");
				size_t slash = uri.find_last_of('/');
				string base = (slash == string::npos) ? uri : uri.substr(slash + 1);
				if (token == "filename_base") {
					size_t dot = base.find_last_of('.');
					if (dot != string::npos) {
						base = base.substr(0, dot);
					}
				}
				if (base.empty()) {
					missing = true;
				}
				out += base;
			}
			else {
				string value = track_meta(track, token.c_str());
				if (value.empty()) {
					missing = true;
				}
				out += value;
			}
			i = j + 1;
		}
		else {
			out += c;
			i++;
		}
	}
	return out;
}

static string expand_lyrics_format(const string &format, DB_playItem_t *track) {
	bool missing = false;
	return expand_lyrics_format(format, track, missing);
}

// Path of the lyrics file next to the audio file (track path without extension).
static string sidecar_file_base_path(DB_playItem_t *track) {
	string uri = track_meta(track, ":URI");
	size_t dot = uri.find_last_of('.');
	if (dot != string::npos) {
		uri = uri.substr(0, dot);
	}
	return uri;
}

// Path of the lyrics file for the track, without the .lrc/.txt extension.
static string lyrics_base_path(DB_playItem_t *track) {
	string dir = lyrics_folder();
	if (!dir.empty()) {
		string name = sanitize_lyrics_filename(expand_lyrics_format(lyrics_filename_format(), track));
		if (!name.empty()) {
			while (dir.size() > 1 && dir[dir.size() - 1] == '/') {
				dir.erase(dir.size() - 1);
			}
			return dir + "/" + name;
		}
	}
	return sidecar_file_base_path(track);
}

static bool read_lyrics_file(const string &path, string &lyrics) {
	ifstream infile(path.c_str());
	if (!infile.is_open()) {
		return false;
	}
	string contents, line;
	while (getline(infile, line)) {
		contents += line + "\n";
	}
	// Empty/whitespace-only cache files are not lyrics. In particular, a
	// blank .lrc must not hide a valid .txt or prevent the online lookup.
	if (contents.find_first_not_of(" \t\r\n") == string::npos) {
		return false;
	}
	lyrics = std::move(contents);
	return true;
}

extern "C"
void ensure_lyrics_path_exists() {
	string dir = lyrics_folder();
	if (dir.empty()) {
		return;
	}
	std::error_code error;
	std::filesystem::create_directories(dir, error);
}

//---------------------------------------------------------------------
//****** Main: Try to get lyrics from Metadata,file,LrcLib. **********
//---------------------------------------------------------------------

struct parsed_lyrics get_lyrics_next_to_file(DB_playItem_t *track) {
	string lyrics;
	bool sync = false;

	// Try the configured lyrics folder first, then the file next to the track.
	string configured = lyrics_base_path(track);
	string sidecar = sidecar_file_base_path(track);
	vector<string> bases;
	if (!configured.empty()) {
		bases.push_back(configured);
	}
	if (!sidecar.empty() && sidecar != configured) {
		bases.push_back(sidecar);
	}

	for (size_t i = 0; i < bases.size(); i++) {
		if (read_lyrics_file(bases[i] + ".lrc", lyrics) || read_lyrics_file(bases[i] + ".txt", lyrics)) {
			break;
		}
	}

	if (lyrics.empty()) {
		return {"", false};
	}

	bool first_timestamped = false;
	int total_lines = 1;
	int timestamped_lines = 1;
	size_t pos = 0;
	while (pos < lyrics.size()) {
		size_t eol = lyrics.find('\n', pos);
		if (eol == string::npos) {
			eol = lyrics.size();
		}
		string line = lyrics.substr(pos, eol - pos);
		pos = eol + 1;

		if (line.length() > 4) {
			for (size_t i = 0; i + 6 < line.length(); i++) {
				if (line[i] == '[' && line[i + 3] == ':' && line[i + 6] == '.') {
					first_timestamped = true;
					timestamped_lines += 1;
				}
			}
		}
		if (first_timestamped == true) {
			total_lines += 1;
		}
	}
	if ((timestamped_lines * 100 / total_lines > 75) && (total_lines != 1)) {
		sync = true;
	}
	return {lyrics, sync};
}

struct parsed_lyrics get_lyrics_from_metadata(DB_playItem_t *track) {
	syncedlyrics = true;
	deadbeef->pl_lock();
	const char *lyrics = deadbeef->pl_find_meta(track, "lyrics")
							?: deadbeef->pl_find_meta(track, "SYLT");
	if (!lyrics) {
		syncedlyrics = false;
		lyrics = deadbeef->pl_find_meta(track, "unsynced lyrics")
	                  ?: deadbeef->pl_find_meta(track, "UNSYNCEDLYRICS");
		if (!lyrics) {
			lyrics = "";
		}
	}
	deadbeef->pl_unlock();
	string string_lyrics = lyrics;
	return {string_lyrics, syncedlyrics};
}


void save_next_to_file(DB_playItem_t *track, struct parsed_lyrics lyrics) {
	string base = lyrics_base_path(track);
	if (base.empty()) {
		debug_out << "Unable to write lyrics file: no path for track\n";
		return;
	}

	size_t slash = base.find_last_of('/');
	if (slash != string::npos) {
		std::error_code error;
		std::filesystem::create_directories(base.substr(0, slash), error);
	}

	string path = base + ((lyrics.sync == true) ? ".lrc" : ".txt");
	ofstream outfile(path.c_str(), ios_base::trunc);//ios_base::app
	if (!outfile.is_open()) {
		debug_out << "Unable to write lyrics file " << path << "\n";
		return;
	}
	outfile << lyrics.lyrics << endl;
	outfile.close();
}

// Saves lyrics picked by the user (search/edit dialogs) to the location
// selected in the config dialog: "Save next to file" writes the lyrics file,
// anything else writes the ID tag.
void save_lyrics(DB_playItem_t *track, struct parsed_lyrics lyrics) {
	if (deadbeef->conf_get_int("save_method", 0) == 1) {
		save_next_to_file(track, lyrics);
	}
	else {
		save_meta_data(track, lyrics);
	}
}

//Sets track info if no lyrics founded.
void set_info(DB_playItem_t *track) {
	string info = "";
	deadbeef->pl_lock();
    int count  = deadbeef->pl_find_meta_int(track, "PLAY_COUNTER", -1);
	if (count == -1){
		count  = deadbeef->pl_find_meta_int(track, "play_count", -1);
	}
    const char *las  = deadbeef->pl_find_meta (track, "LAST_PLAYED");
    const char *firs  = deadbeef->pl_find_meta (track, "FIRST_PLAYED");
    const char *publisher = deadbeef->pl_find_meta (track, "PUBLISHER");
    const char *media = deadbeef->pl_find_meta (track, "MEDIA TYPE");
    const char *catalog_number = deadbeef->pl_find_meta (track, "CATALOGNUMBER");
    const char *release_country = deadbeef->pl_find_meta (track, "MUSICBRAINZ ALBUM RELEASE COUNTRY");
    const char *original_date = deadbeef->pl_find_meta (track, "ORIGINAL_RELEASE_TIME");
    const char *genre = deadbeef->pl_find_meta (track, "GENRE");
    
    
    
    //INVOLVED_PEOPLE_LIST and GENRE are a little more tricky to retrieve.
    DB_metaInfo_t *involved_people_meta = deadbeef->pl_meta_for_key (track, "InvolvedPeople");
    deadbeef->pl_unlock();
    
    deadbeef->pl_lock();
	int playcount = deadbeef->playqueue_get_count();
	deadbeef->pl_unlock();
	
	int bitrate = deadbeef->streamer_get_apx_bitrate();
    
    void *involved_buffer = malloc(100000);
    char *p2 = (char *)involved_buffer;
    *p2 = 0;
    size_t buffer_size = 100000;
    const char *involved_people = NULL;

    if (involved_people_meta != NULL) {
        size_t value_size = involved_people_meta->valuesize;
        if (value_size > buffer_size - 1) {
            value_size = buffer_size - 1;
        }
        memcpy(p2, involved_people_meta->value, value_size);
        char *p = p2;
        for (size_t i = 0; i <= value_size; i++, p++) {
            if (*p == 0) {
                *p = '\n';
            }
        }
        p2[value_size] = 0;
        involved_people = p2;
    }
    
	info.append("\n \n");

	if (count == -1){
		count = 0;
	}
	
	if (count == 1){
		info.append(_("Listened once"));
		info.append(" \n \n");
	}
	else {
			info.append(_("Listened "));
			info.append(to_string(count));
			info.append(_(" times"));
			info.append("\n \n");
	}

	if (firs != NULL) {
		string first = firs;	
		info.append(_("For the first time "));
		info.append(first.substr(0,10));
		info.append("\n");
		info.append(_("at "));
		info.append(first.substr(11, first.length() -1));
		info.append("\n \n");
	}
	else{
		info.append(_("Never listened before"));
		info.append("\n \n");
	}

	if (las != NULL) {
		string last = las;
		info.append(_("For the last time the "));
		info.append(last.substr(0,10));
		info.append("\n");
		info.append(_("at "));
		info.append( last.substr(11, last.length() -1));
		info.append("\n \n");
	}


	info.append("Bitrate: ");
	if (bitrate >= 0) {
		info.append(to_string(bitrate));
		info.append(" kbps\n \n");
	}
	else {
		info.append(_("Unknown"));
		info.append("\n \n");
	}
	if (playcount == 1){
		info.append(_("There is one file in line"));
		info.append("\n \n");
	}
	else{
		info.append(_("There are "));
		info.append(to_string(playcount));
		info.append(_(" files in line"));
		info.append("\n \n");
	}
	
	if (publisher != NULL) {
	    info.append(_("Publisher: "));
	    info.append(publisher);
	    info.append("\n \n");
	}
	
	if (media != NULL && catalog_number != NULL && release_country != NULL) {
	    info.append(media);
	    info.append(" - ");
	    info.append(catalog_number);
	    info.append(" - ");
	    info.append(release_country);
	    info.append("\n \n");
	}
	
	if (original_date != NULL) {
	    info.append(_("Released date: "));
	    info.append(original_date);
	    info.append("\n \n");
	}

	if (genre != NULL) {
	    info.append(_("Genre: "));
	    info.append(genre);
	    info.append("\n \n");
	}

	if (involved_people != NULL) {
	    info.append(_("Creators:"));
	    info.append("\n");
	    vector<string> involved = split(involved_people, " / ");
	    if (involved.size() == 1){
	        involved = split(involved[0], "\n");
	    }
	    if (involved.size() > 1){
	        for(unsigned int i = 0; i < involved.size(); i+=1) {
	            info.append(involved[i]);
	    	    if (i % 2 == 0){
	                info.append(" - ");
	            }
	            else {
	                info.append("\n");
	            }       
	        }
	    }
	    else{
	        info.append(involved[0]);
	    }
	    info.append("\n");
	}

	set_lyrics(track, info, "",  "", "");
}

//Main function:
void update_lyrics(void *tr) {
	set_linessizes({});
	DB_playItem_t *track = static_cast<DB_playItem_t*>(tr);
	struct parsed_lyrics meta_lyrics = get_lyrics_from_metadata(track);
	if (meta_lyrics.lyrics != "") {
		if (meta_lyrics.sync == true) {
			chopset_lyrics(track, meta_lyrics.lyrics);
		}
		else{
			set_lyrics(track, "", "", meta_lyrics.lyrics, "");
		}
		sync_or_unsync(meta_lyrics.sync);
	return;
	}

	const char *artist;
	const char *title;
	{
		deadbeef->pl_lock();
		artist = deadbeef->pl_find_meta(track, "artist") ?: _("Unknown Artist");
		title  = deadbeef->pl_find_meta(track, "title") ?: _("Unknown Title");
		deadbeef->pl_unlock();
	}


	if (artist && title) {
		struct parsed_lyrics cached_lyrics = get_lyrics_next_to_file(track);

		if (cached_lyrics.lyrics != "") {
			if (cached_lyrics.sync == true) {
				chopset_lyrics(track, cached_lyrics.lyrics);
			}
			else{
				set_lyrics(track, "", "", cached_lyrics.lyrics, "");
			}
			sync_or_unsync(cached_lyrics.sync);
			return;
		}
	}
	set_lyrics(track, "", "", _("Loading..."), "");

//	 No lyrics in the tag or cache; try to get some and cache if succeeded.
//	Search for lyrics on LRCLIB:
    if (deadbeef->conf_get_int("lyricbar.autosearch.enable", 1)){
    	struct parsed_lyrics lrclib_lyrics = {"",false};
    	lrclib_lyrics = lrclib(string(title), string(artist), "");
    	if (lrclib_lyrics.lyrics != "") {
    		if (lrclib_lyrics.sync){
    			chopset_lyrics(track, lrclib_lyrics.lyrics);

	    	}
		    else{
		    	set_lyrics(track, "", "", lrclib_lyrics.lyrics, "");
		    }
	    	sync_or_unsync(lrclib_lyrics.sync);
	    	
	    	if (deadbeef->conf_get_int("save_method", 0) == 0){
	    		save_meta_data(track, lrclib_lyrics);
	    	}
	    	else if (deadbeef->conf_get_int("save_method", 0) == 1){
	    	    save_next_to_file(track, lrclib_lyrics);
	    	}
	    	
	    	return;
    	}
    }
//	If no lyrics founded in any site, show track info:
	set_info(track);
}

//---------------------------------------------------------------------
//****** /Main: Try to get lyrics from Metadata,file,LRCLIB. *********
//---------------------------------------------------------------------


/**
 * Creates the directory tree.
 * @param name the directory path, including trailing slash
 * @return 0 on success; errno after mkdir call if something went wrong
 */

int mkpath(const string &name, mode_t mode) {
	string dir;
	size_t pos = 0;
	while ((pos = name.find_first_of('/', pos)) != string::npos){
		dir = name.substr(0, pos++);
		if (dir.empty())
			continue; // ignore the leading slash
		if (mkdir(dir.c_str(), mode) && errno != EEXIST)
			return errno;
	}
	return 0;
}

int remove_from_cache_action(DB_plugin_action_t *, ddb_action_context_t ctx) {
    bindtextdomain("deadbeef-lyricbar", "gettext");
    textdomain("deadbeef-lyricbar");
  
	if (ctx == DDB_ACTION_CTX_SELECTION) {
		deadbeef->pl_lock();
		ddb_playlist_t *playlist = deadbeef->plt_get_curr();
		if (playlist) {
			DB_playItem_t *current = deadbeef->plt_get_first(playlist, PL_MAIN);
			while (current) {
//				if (deadbeef->pl_is_selected (current)) {
//					const char *artist = deadbeef->pl_find_meta(current, "artist");
//					const char *title  = deadbeef->pl_find_meta(current, "title");
//					if (is_cached(artist, title)){
//						remove(cached_filename(artist, title).c_str());
//					}
//				}
				DB_playItem_t *next = deadbeef->pl_get_next(current, PL_MAIN);
				deadbeef->pl_unlock();
				deadbeef->pl_item_unref(current);
				current = next;
			}
			deadbeef->plt_unref(playlist);
		}
	}
	return 0;
}
