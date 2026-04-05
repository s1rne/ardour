/*
 * Copyright (C) 2024 SoundForge AI Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef WAF_BUILD
#include "gtk2ardour-config.h"
#endif

#include <cstdio>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <functional>
#include <filesystem>
#include <fstream>
#include <regex>

#include <curl/curl.h>

#include <glibmm/main.h>

#include "gtkmm2ext/utils.h"
#include "gtkmm2ext/window_title.h"

#include "widgets/tooltips.h"

#include "pbd/ccurl.h"

#include "ardour/session.h"

#include "ardour_ui.h"
#include "ai_window.h"
#include "gui_thread.h"
#include "instrument_selector.h"
#include "public_editor.h"
#include "utils.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace PBD;
using namespace Gtk;
using namespace Glib;
using namespace Gtkmm2ext;
using namespace std;

AIWindow* AIWindow::_instance = nullptr;

namespace {

	size_t ai_window_curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
		string* str = static_cast<string*>(userp);
		str->append(static_cast<char*>(contents), size * nmemb);
		return size * nmemb;
	}

	/** Minimal JSON string escape for UTF-8 user message in {"message":"..."} */
	string json_escape_utf8(const string& s) {
		string out;
		out.reserve(s.size() + 8);
		for (unsigned char c : s) {
			switch (c) {
				case '"':  out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\b': out += "\\b"; break;
				case '\f': out += "\\f"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (c < 0x20) {
						char buf[7];
						snprintf(buf, sizeof(buf), "\\u%04x", c);
						out += buf;
					} else {
						out += static_cast<char>(c);
					}
			}
		}
		return out;
	}

	bool extract_json_string_field(const string& json, const string& key, string& out_value) {
		const string needle = "\"" + key + "\"";
		size_t key_pos = json.find(needle);
		if (key_pos == string::npos) {
			return false;
		}
		size_t colon = json.find(":", key_pos + needle.size());
		if (colon == string::npos) {
			return false;
		}
		size_t first_quote = json.find("\"", colon + 1);
		if (first_quote == string::npos) {
			return false;
		}
		size_t end_quote = json.find("\"", first_quote + 1);
		while (end_quote != string::npos && end_quote > 0 && json[end_quote - 1] == '\\') {
			end_quote = json.find("\"", end_quote + 1);
		}
		if (end_quote == string::npos || end_quote <= first_quote + 1) {
			return false;
		}
		out_value = json.substr(first_quote + 1, end_quote - first_quote - 1);
		return true;
	}

	vector<string> extract_midi_paths(const string& json) {
		vector<string> paths;
		static const regex mid_re("\"([^\"]+\\.mid)\"");
		for (sregex_iterator it(json.begin(), json.end(), mid_re), end; it != end; ++it) {
			paths.push_back((*it)[1].str());
		}
		return paths;
	}

	string read_text_file(const string& path) {
		ifstream in(path.c_str(), ios::in | ios::binary);
		if (!in) {
			return "";
		}
		ostringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	string resolve_backend_path(const string& maybe_relative) {
		if (maybe_relative.empty()) {
			return maybe_relative;
		}
		if (!maybe_relative.empty() && maybe_relative[0] == '/') {
			return maybe_relative;
		}
		// Backend returns paths relative to ai-backend workdir, e.g. data/output/...
		if (maybe_relative.rfind("data/", 0) == 0) {
			std::error_code ec;
			auto p = filesystem::current_path(ec);
			if (!ec) {
				auto abs = filesystem::weakly_canonical(p / ".." / "ai-backend" / maybe_relative, ec);
				if (!ec) {
					return abs.string();
				}
			}
		}
		return maybe_relative;
	}

} /* namespace */

AIWindow*
AIWindow::instance ()
{
	if (!_instance) {
		_instance = new AIWindow;
	}
	return _instance;
}

AIWindow::AIWindow ()
	: ArdourWindow (_("SoundForge AI"))
	, _connected (false)
	, _connecting (false)
	, _backend_url ("localhost")
	, _backend_port (8000)
	, _thread_running (false)
	, _title_label (_("<b>SoundForge AI Assistant</b>"))
	, _status_label (_("Ready"))
	, _connection_label (_("Disconnected"))
	, _send_button (_("Send"))
	, _clear_button (_("Clear"))
	, _connect_button (_("Connect"))
	, _btn_separate (_("Separate Stems"))
	, _btn_analyze (_("Analyze"))
	, _btn_transcribe (_("To MIDI"))
	, _btn_create_project (_("Create Project"))
	, _btn_apply_project (_("Apply Project"))
{
	set_name ("AIWindow");
	set_wmclass (X_("ardour_ai"), PROGRAM_NAME);
	set_default_size (500, 600);
	set_border_width (6);

	setup_ui ();
	update_title ();
	update_connection_status ();

	_timeout_connection = Glib::signal_timeout().connect (
		sigc::mem_fun (*this, &AIWindow::on_timeout), 100);
}

AIWindow::~AIWindow ()
{
	_timeout_connection.disconnect ();
	_thread_running = false;
	
	if (_network_thread && _network_thread->joinable()) {
		_network_thread->join();
	}
}

void
AIWindow::setup_ui ()
{
	_title_label.set_use_markup (true);
	_title_label.set_alignment (0.0, 0.5);

	_header_hbox.set_spacing (6);
	_header_hbox.pack_start (_title_label, true, true);
	_header_hbox.pack_start (_connection_label, false, false);
	_header_hbox.pack_start (_connect_button, false, false);

	_chat_buffer = Gtk::TextBuffer::create ();
	_chat_view.set_buffer (_chat_buffer);
	_chat_view.set_editable (false);
	_chat_view.set_wrap_mode (Gtk::WRAP_WORD_CHAR);
	_chat_view.set_cursor_visible (false);
	_chat_view.set_left_margin (8);
	_chat_view.set_right_margin (8);

	auto tag_table = _chat_buffer->get_tag_table ();
	
	auto user_tag = Gtk::TextTag::create ("user");
	user_tag->property_foreground () = "#8b5cf6";
	user_tag->property_weight () = Pango::WEIGHT_BOLD;
	tag_table->add (user_tag);

	auto ai_tag = Gtk::TextTag::create ("ai");
	ai_tag->property_foreground () = "#06b6d4";
	tag_table->add (ai_tag);

	auto system_tag = Gtk::TextTag::create ("system");
	system_tag->property_foreground () = "#6b7280";
	system_tag->property_style () = Pango::STYLE_ITALIC;
	tag_table->add (system_tag);

	auto tool_tag = Gtk::TextTag::create ("tool");
	tool_tag->property_foreground () = "#22c55e";
	tag_table->add (tool_tag);

	_chat_scroll.set_policy (Gtk::POLICY_AUTOMATIC, Gtk::POLICY_ALWAYS);
	_chat_scroll.add (_chat_view);

	_tools_hbox.set_spacing (4);
	_tools_hbox.pack_start (_btn_separate, false, false);
	_tools_hbox.pack_start (_btn_analyze, false, false);
	_tools_hbox.pack_start (_btn_transcribe, false, false);
	_tools_hbox.pack_start (_btn_create_project, false, false);
	_tools_hbox.pack_start (_btn_apply_project, false, false);

	ArdourWidgets::set_tooltip (_btn_separate, _("Separate audio into stems (vocals, drums, bass, other)"));
	ArdourWidgets::set_tooltip (_btn_analyze, _("Analyze BPM, key, loudness"));
	ArdourWidgets::set_tooltip (_btn_transcribe, _("Convert audio to MIDI"));
	ArdourWidgets::set_tooltip (_btn_create_project, _("Create MIDI-first arrangement blueprint"));
	ArdourWidgets::set_tooltip (_btn_apply_project, _("Apply last generated project to Ardour"));

	_input_hbox.set_spacing (6);
	_input_hbox.pack_start (_input_entry, true, true);
	_input_hbox.pack_start (_send_button, false, false);
	_input_hbox.pack_start (_clear_button, false, false);

	_status_hbox.set_spacing (6);
	_status_hbox.pack_start (_status_label, true, true);

	_main_vbox.set_spacing (6);
	_main_vbox.pack_start (_header_hbox, false, false);
	_main_vbox.pack_start (_separator1, false, false);
	_main_vbox.pack_start (_tools_hbox, false, false);
	_main_vbox.pack_start (_chat_scroll, true, true);
	_main_vbox.pack_start (_separator2, false, false);
	_main_vbox.pack_start (_input_hbox, false, false);
	_main_vbox.pack_start (_status_hbox, false, false);

	add (_main_vbox);

	_send_button.signal_clicked.connect (sigc::mem_fun (*this, &AIWindow::on_send_clicked));
	_clear_button.signal_clicked.connect (sigc::mem_fun (*this, &AIWindow::on_clear_clicked));
	_connect_button.signal_clicked.connect (sigc::mem_fun (*this, &AIWindow::on_connect_clicked));
	_input_entry.signal_activate ().connect (sigc::mem_fun (*this, &AIWindow::on_input_activate));

	_btn_separate.signal_clicked.connect (sigc::mem_fun (*this, &AIWindow::on_separate_clicked));
	_btn_analyze.signal_clicked.connect (sigc::mem_fun (*this, &AIWindow::on_analyze_clicked));
	_btn_transcribe.signal_clicked.connect (sigc::mem_fun (*this, &AIWindow::on_transcribe_clicked));
	_btn_create_project.signal_clicked.connect (sigc::mem_fun (*this, &AIWindow::on_create_project_clicked));
	_btn_apply_project.signal_clicked.connect (sigc::mem_fun (*this, &AIWindow::on_apply_project_clicked));

	append_text (_("Welcome to SoundForge AI Assistant!\n\n"), "system");
	append_text (_("Commands you can try:\n"), "system");
	append_text (_("• \"Separate stems\" - Extract vocals, drums, bass\n"), "system");
	append_text (_("• \"Analyze this track\" - Get BPM, key, loudness\n"), "system");
	append_text (_("• \"Convert to MIDI\" - Transcribe audio to MIDI\n"), "system");
	append_text (_("• \"Create Project\" - Build MIDI-first arrangement blueprint\n"), "system");
	append_text (_("• \"Apply Project\" - Apply last generated project to Ardour\n"), "system");
	append_text (_("• \"Analyze the mix\" - Get mixing recommendations\n\n"), "system");
	append_text (_("Click 'Connect' to connect to the AI backend.\n"), "system");

	show_all_children ();
}

void
AIWindow::update_title ()
{
	Gtkmm2ext::WindowTitle title (_("SoundForge AI"));
	
	if (_session) {
		title += _session->name ();
	}
	
	if (_connected) {
		title += _("Connected");
	}
	
	set_title (title.get_string ());
}

void
AIWindow::update_status (const string& status)
{
	_status_label.set_text (status);
}

void
AIWindow::update_connection_status ()
{
	if (_connected) {
		_connection_label.set_markup ("<span foreground='#22c55e'>● Connected</span>");
		_connect_button.set_text (_("Disconnect"));
	} else if (_connecting) {
		_connection_label.set_markup ("<span foreground='#f59e0b'>● Connecting...</span>");
		_connect_button.set_text (_("Cancel"));
	} else {
		_connection_label.set_markup ("<span foreground='#ef4444'>● Disconnected</span>");
		_connect_button.set_text (_("Connect"));
	}
}

void
AIWindow::set_session (Session* s)
{
	SessionHandlePtr::set_session (s);

	if (!_session) {
		return;
	}

	_session->DropReferences.connect (_session_connections, invalidator (*this),
		std::bind (&AIWindow::session_going_away, this), gui_context ());

	update_title ();
}

void
AIWindow::session_going_away ()
{
	SessionHandlePtr::session_going_away ();
	update_title ();
}

void
AIWindow::on_send_clicked ()
{
	string message = _input_entry.get_text ();
	if (message.empty ()) {
		return;
	}

	send_message (message);
	_input_entry.set_text ("");
}

void
AIWindow::on_input_activate ()
{
	on_send_clicked ();
}

void
AIWindow::on_clear_clicked ()
{
	clear_chat ();
}

void
AIWindow::on_connect_clicked ()
{
	if (_connected || _connecting) {
		disconnect_from_backend ();
	} else {
		connect_to_backend ();
	}
}

void
AIWindow::on_separate_clicked ()
{
	send_message (_("Separate the selected audio into stems (vocals, drums, bass, other)"));
}

void
AIWindow::on_analyze_clicked ()
{
	send_message (_("Analyze the selected audio - tell me the BPM, key, and loudness"));
}

void
AIWindow::on_transcribe_clicked ()
{
	send_message (_("Convert the selected audio to MIDI"));
}

void
AIWindow::on_create_project_clicked ()
{
	if (!_connected) {
		append_text (_("\n[Not connected to AI backend. Click 'Connect' first.]\n"), "system");
		return;
	}
	update_status (_("Creating project..."));
	std::thread ([this]() {
		create_project_http ();
	}).detach ();
}

void
AIWindow::on_apply_project_clicked ()
{
	if (_last_project_plan_path.empty () && _last_project_assets_path.empty () && _last_midi_files.empty()) {
		append_text (_("\n[No project plan found in last response. Create a project first.]\n"), "system");
		return;
	}
	update_status (_("Applying project..."));
	apply_project_locally ();
}

void
AIWindow::apply_project_locally ()
{
	vector<string> midi_files = _last_midi_files;

	if (midi_files.empty() && !_last_project_assets_path.empty()) {
		const string assets_path = resolve_backend_path(_last_project_assets_path);
		const string assets_json = read_text_file(assets_path);
		if (!assets_json.empty()) {
			midi_files = extract_midi_paths(assets_json);
		}
	}

	if (midi_files.empty() && !_last_project_plan_path.empty()) {
		// Fallback: try sibling assets file near project plan.
		const string plan_path = resolve_backend_path(_last_project_plan_path);
		std::error_code ec;
		auto maybe_assets = filesystem::path(plan_path).parent_path() / "project_plan_with_assets.json";
		const string assets_json = read_text_file(maybe_assets.string());
		if (!assets_json.empty()) {
			midi_files = extract_midi_paths(assets_json);
		}
	}

	if (midi_files.empty()) {
		update_status (_("Error"));
		append_text (_("\n[Error: No MIDI files found to import.]\n"), "system");
		return;
	}

	vector<string> resolved;
	for (auto const& p : midi_files) {
		const string abs = resolve_backend_path(p);
		ifstream test(abs.c_str(), ios::in | ios::binary);
		if (test.good()) {
			resolved.push_back(abs);
		}
	}

	if (resolved.empty()) {
		update_status (_("Error"));
		append_text (_("\n[Error: MIDI files were found in plan, but not accessible from Ardour process.]\n"), "system");
		return;
	}

	Temporal::timepos_t pos (0);
	InstrumentSelector instrument_selector(InstrumentSelector::ForTrackDefault);
	ARDOUR::PluginInfoPtr instrument = instrument_selector.selected_instrument();
	PublicEditor::instance().do_import (
		resolved,
		Editing::ImportDistinctFiles,
		Editing::ImportAsTrack,
		ARDOUR::SrcBest,
		ARDOUR::SMFFileAndTrackName,
		ARDOUR::SMFTempoIgnore,
		pos,
		instrument
	);

	update_status (_("Ready"));
	if (instrument) {
		append_text (_("\n[Project applied to Ardour: MIDI tracks imported with default instrument]\n"), "system");
	} else {
		append_text (_("\n[Project applied to Ardour: MIDI tracks imported, no instrument found]\n"), "system");
	}
}

void
AIWindow::send_message (const string& message)
{
	if (message.empty ()) {
		return;
	}

	append_response (message, true);

	if (!_connected) {
		append_text (_("\n[Not connected to AI backend. Click 'Connect' first.]\n"), "system");
		return;
	}

	update_status (_("Sending..."));

	std::thread ([this, message]() {
		send_http_request (message);
	}).detach ();
}

void
AIWindow::append_response (const string& text, bool is_user)
{
	string prefix = is_user ? "\nYou: " : "\nAI: ";
	string tag = is_user ? "user" : "ai";
	
	append_text (prefix, tag);
	append_text (text + "\n", "");

	ChatMessage msg;
	msg.content = text;
	msg.is_user = is_user;
	
	time_t now = time (nullptr);
	char buf[64];
	strftime (buf, sizeof(buf), "%H:%M:%S", localtime (&now));
	msg.timestamp = buf;
	
	_chat_history.push_back (msg);

	scroll_to_bottom ();
}

void
AIWindow::append_text (const string& text, const string& tag)
{
	auto iter = _chat_buffer->end ();
	
	if (tag.empty ()) {
		_chat_buffer->insert (iter, text);
	} else {
		_chat_buffer->insert_with_tag (iter, text, tag);
	}
}

void
AIWindow::scroll_to_bottom ()
{
	auto iter = _chat_buffer->end ();
	_chat_view.scroll_to (iter);
}

void
AIWindow::clear_chat ()
{
	_chat_buffer->set_text ("");
	_chat_history.clear ();
	
	append_text (_("Chat cleared.\n"), "system");
}

void
AIWindow::connect_to_backend ()
{
	_connecting = true;
	update_connection_status ();
	update_status (_("Connecting to AI backend..."));
	append_text (_("\nConnecting to AI backend at localhost:8000...\n"), "system");

	std::thread ([this]() {
		try {
			PBD::CCurl ccurl;
			CURL* curl = ccurl.curl();
			
			string url = "http://" + _backend_url + ":" + to_string(_backend_port) + "/health";
			string response_data;
			
			curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ai_window_curl_write_cb);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
			
			CURLcode res = curl_easy_perform(curl);
			
			long response_code = 0;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
			
			if (res == CURLE_OK && response_code == 200) {
				_connected = true;
				_connecting = false;
				
				std::lock_guard<std::mutex> lock(_queue_mutex);
				_response_queue.push ("__CONNECTED__");
			} else {
				_connected = false;
				_connecting = false;
				
				std::lock_guard<std::mutex> lock(_queue_mutex);
				_response_queue.push ("__CONNECTION_FAILED__");
			}
		} catch (...) {
			_connected = false;
			_connecting = false;
			
			std::lock_guard<std::mutex> lock(_queue_mutex);
			_response_queue.push ("__CONNECTION_FAILED__");
		}
	}).detach ();
}

void
AIWindow::disconnect_from_backend ()
{
	_connected = false;
	_connecting = false;
	update_connection_status ();
	update_status (_("Disconnected"));
	append_text (_("\nDisconnected from AI backend.\n"), "system");
}

void
AIWindow::send_http_request (const string& message)
{
	try {
		PBD::CCurl ccurl;
		CURL* curl = ccurl.curl();
		
		string url = "http://" + _backend_url + ":" + to_string(_backend_port) + "/chat";
		string response_data;
		
		string payload = string("{\"message\": \"") + json_escape_utf8(message) + "\"}";
		
		struct curl_slist* headers = nullptr;
		headers = curl_slist_append(headers, "Content-Type: application/json");
		
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		/* LLM + MusicGen на CPU могут идти десятки минут — 120 с даёт HTTP 0 (таймаут) */
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3600L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ai_window_curl_write_cb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
		
		CURLcode res = curl_easy_perform(curl);
		curl_slist_free_all(headers);
		
		long response_code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		
		std::lock_guard<std::mutex> lock(_queue_mutex);
		
		if (res == CURLE_OK && response_code == 200) {
			_response_queue.push (response_data);
		} else if (res != CURLE_OK) {
			_response_queue.push (string("__ERROR__: ") + curl_easy_strerror(res));
		} else {
			_response_queue.push ("__ERROR__: HTTP " + to_string(response_code));
		}
	} catch (const exception& e) {
		std::lock_guard<std::mutex> lock(_queue_mutex);
		_response_queue.push (string("__ERROR__: ") + e.what());
	}
}

void
AIWindow::create_project_http ()
{
	try {
		PBD::CCurl ccurl;
		CURL* curl = ccurl.curl();
		string url = "http://" + _backend_url + ":" + to_string(_backend_port) + "/chat/project/create";
		string response_data;
		string payload = "{\"prompt\":\"lo-fi hip-hop project, midi-first arrangement\",\"bars\":16,\"tempo\":84,\"key\":\"A minor\",\"style\":\"lofi\"}";
		struct curl_slist* headers = nullptr;
		headers = curl_slist_append(headers, "Content-Type: application/json");
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ai_window_curl_write_cb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);

		CURLcode res = curl_easy_perform(curl);
		curl_slist_free_all(headers);
		long response_code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

		std::lock_guard<std::mutex> lock(_queue_mutex);
		if (res == CURLE_OK && response_code == 200) {
			_response_queue.push (response_data);
		} else if (res != CURLE_OK) {
			_response_queue.push (string("__ERROR__: ") + curl_easy_strerror(res));
		} else {
			_response_queue.push ("__ERROR__: HTTP " + to_string(response_code));
		}
	} catch (const exception& e) {
		std::lock_guard<std::mutex> lock(_queue_mutex);
		_response_queue.push (string("__ERROR__: ") + e.what());
	}
}

void
AIWindow::apply_project_plan_http (const string& project_plan_path)
{
	try {
		PBD::CCurl ccurl;
		CURL* curl = ccurl.curl();
		string url = "http://" + _backend_url + ":" + to_string(_backend_port) + "/chat/project/apply";
		string response_data;
		string payload = string("{\"project_plan_path\": \"") + json_escape_utf8(project_plan_path) + "\"}";
		struct curl_slist* headers = nullptr;
		headers = curl_slist_append(headers, "Content-Type: application/json");
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ai_window_curl_write_cb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);

		CURLcode res = curl_easy_perform(curl);
		curl_slist_free_all(headers);
		long response_code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

		std::lock_guard<std::mutex> lock(_queue_mutex);
		if (res == CURLE_OK && response_code == 200) {
			_response_queue.push ("__PROJECT_APPLIED__");
		} else if (res != CURLE_OK) {
			_response_queue.push (string("__ERROR__: ") + curl_easy_strerror(res));
		} else {
			if (!response_data.empty()) {
				_response_queue.push ("__ERROR__: HTTP " + to_string(response_code) + " " + response_data);
			} else {
				_response_queue.push ("__ERROR__: HTTP " + to_string(response_code));
			}
		}
	} catch (const exception& e) {
		std::lock_guard<std::mutex> lock(_queue_mutex);
		_response_queue.push (string("__ERROR__: ") + e.what());
	}
}

void
AIWindow::process_response (const string& response)
{
	string project_plan_path;
	if (extract_json_string_field(response, "project_plan_path", project_plan_path)) {
		_last_project_plan_path = project_plan_path;
		append_text (_("[Stored project plan for Apply Project]\n"), "tool");
	}
	string project_assets_path;
	if (extract_json_string_field(response, "project_assets_path", project_assets_path)) {
		_last_project_assets_path = project_assets_path;
		append_text (_("[Stored project assets for Apply Project]\n"), "tool");
	}
	vector<string> midi_paths = extract_midi_paths(response);
	if (!midi_paths.empty()) {
		_last_midi_files = midi_paths;
		append_text (_("[Stored MIDI files for Apply Project]\n"), "tool");
	}

	if (response == "__CONNECTED__") {
		update_connection_status ();
		update_status (_("Connected to AI backend"));
		append_text (_("Connected! You can now ask questions.\n"), "system");
		return;
	}
	
	if (response == "__CONNECTION_FAILED__") {
		update_connection_status ();
		update_status (_("Connection failed"));
		append_text (_("Failed to connect. Make sure the AI backend is running.\n"), "system");
		append_text (_("Run: cd ai-backend && make dev\n"), "system");
		return;
	}
	
	if (response.substr(0, 9) == "__ERROR__") {
		update_status (_("Error"));
		append_text (_("\n[Error: ") + response.substr(10) + "]\n", "system");
		return;
	}
	
	if (response == "__PROJECT_APPLIED__") {
		update_status (_("Ready"));
		append_text (_("\n[Project applied to Ardour]\n"), "system");
		return;
	}

	size_t pos = response.find("\"response\"");
	if (pos != string::npos) {
		size_t start = response.find("\"", pos + 11);
		if (start != string::npos) {
			size_t end = response.find("\"", start + 1);
			while (end != string::npos && response[end - 1] == '\\') {
				end = response.find("\"", end + 1);
			}
			if (end != string::npos) {
				string ai_response = response.substr(start + 1, end - start - 1);
				size_t p;
				while ((p = ai_response.find("\\n")) != string::npos) {
					ai_response.replace(p, 2, "\n");
				}
				while ((p = ai_response.find("\\\"")) != string::npos) {
					ai_response.replace(p, 2, "\"");
				}
				
				append_response (ai_response, false);
				update_status (_("Ready"));
				return;
			}
		}
	}

	append_response (response, false);
	update_status (_("Ready"));
}

bool
AIWindow::on_timeout ()
{
	std::lock_guard<std::mutex> lock(_queue_mutex);
	
	while (!_response_queue.empty()) {
		string response = _response_queue.front();
		_response_queue.pop();
		process_response (response);
	}
	
	return true;
}
