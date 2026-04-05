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

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>

#include <ytkmm/box.h>
#include <ytkmm/button.h>
#include <ytkmm/entry.h>
#include <ytkmm/label.h>
#include <ytkmm/scrolledwindow.h>
#include <ytkmm/textview.h>
#include <ytkmm/window.h>
#include <ytkmm/separator.h>

#include "pbd/signals.h"

#include "ardour/session_handle.h"

#include "widgets/ardour_button.h"

#include "ardour_window.h"

/**
 * AI Assistant Window for SoundForge AI integration.
 * 
 * Provides a chat interface to interact with the AI backend
 * for music production assistance (stem separation, analysis, etc.)
 */
class AIWindow : public ArdourWindow, public PBD::ScopedConnectionList
{
public:
	static AIWindow* instance();
	~AIWindow();

	void set_session (ARDOUR::Session* s);

	/** Send a message to the AI backend */
	void send_message (const std::string& message);

	/** Get connection status */
	bool is_connected () const { return _connected.load(); }

	/** Append response to chat */
	void append_response (const std::string& text, bool is_user = false);

	/** Clear chat history */
	void clear_chat ();

private:
	AIWindow ();
	static AIWindow* _instance;

	// Connection state
	std::atomic<bool> _connected;
	std::atomic<bool> _connecting;
	std::string _backend_url;
	int _backend_port;

	// Chat history
	struct ChatMessage {
		std::string content;
		bool is_user;
		std::string timestamp;
	};
	std::vector<ChatMessage> _chat_history;

	// Message queue for thread-safe updates
	std::queue<std::string> _response_queue;
	std::mutex _queue_mutex;

	// UI Elements
	Gtk::VBox _main_vbox;
	Gtk::HBox _header_hbox;
	Gtk::HBox _input_hbox;
	Gtk::HBox _status_hbox;
	Gtk::HBox _tools_hbox;

	Gtk::Label _title_label;
	Gtk::Label _status_label;
	Gtk::Label _connection_label;

	Gtk::TextView _chat_view;
	Gtk::ScrolledWindow _chat_scroll;
	Glib::RefPtr<Gtk::TextBuffer> _chat_buffer;

	Gtk::Entry _input_entry;
	ArdourWidgets::ArdourButton _send_button;
	ArdourWidgets::ArdourButton _clear_button;
	ArdourWidgets::ArdourButton _connect_button;

	// Tool buttons
	ArdourWidgets::ArdourButton _btn_separate;
	ArdourWidgets::ArdourButton _btn_analyze;
	ArdourWidgets::ArdourButton _btn_transcribe;
	ArdourWidgets::ArdourButton _btn_create_project;
	ArdourWidgets::ArdourButton _btn_apply_project;

	Gtk::HSeparator _separator1;
	Gtk::HSeparator _separator2;

	// Network thread
	std::unique_ptr<std::thread> _network_thread;
	std::atomic<bool> _thread_running;

	// Methods
	void setup_ui ();
	void update_title ();
	void update_status (const std::string& status);
	void update_connection_status ();

	void on_send_clicked ();
	void on_clear_clicked ();
	void on_connect_clicked ();
	void on_input_activate ();

	void on_separate_clicked ();
	void on_analyze_clicked ();
	void on_transcribe_clicked ();
	void on_create_project_clicked ();
	void on_apply_project_clicked ();

	void session_going_away ();

	void scroll_to_bottom ();
	void append_text (const std::string& text, const std::string& tag = "");

	// Network operations
	void connect_to_backend ();
	void disconnect_from_backend ();
	void send_http_request (const std::string& message);
	void create_project_http ();
	void apply_project_plan_http (const std::string& project_plan_path);
	void apply_project_locally ();
	void process_response (const std::string& response);

	std::string _last_project_plan_path;
	std::string _last_project_assets_path;
	std::vector<std::string> _last_midi_files;

	// Timer for UI updates
	bool on_timeout ();
	sigc::connection _timeout_connection;
};
