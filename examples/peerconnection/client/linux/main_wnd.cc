/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "examples/peerconnection/client/linux/main_wnd.h"

#include <cairo.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <glib-object.h>
#include <glib.h>
#include <glibconfig.h>
#include <gobject/gclosure.h>
#include <gtk/gtk.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "api/media_stream_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_rotation.h"
#include "api/video/video_source_interface.h"
#include "examples/peerconnection/client/main_wnd.h"
#include "examples/peerconnection/client/peer_connection_client.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/convert_from.h"

namespace {

//
// Simple static functions that simply forward the callback to the
// GtkMainWnd instance.
//

gboolean OnDestroyedCallback(GtkWidget* widget,
                             GdkEvent* event,
                             gpointer data) {
  reinterpret_cast<GtkMainWnd*>(data)->OnDestroyed(widget, event);
  return FALSE;
}

void OnClickedCallback(GtkWidget* widget, gpointer data) {
  reinterpret_cast<GtkMainWnd*>(data)->OnClicked(widget);
}

gboolean SimulateButtonClick(gpointer button) {
  g_signal_emit_by_name(button, "clicked");
  return false;
}

gboolean OnKeyPressCallback(GtkWidget* widget,
                            GdkEventKey* key,
                            gpointer data) {
  reinterpret_cast<GtkMainWnd*>(data)->OnKeyPress(widget, key);
  return false;
}

void OnRowActivatedCallback(GtkTreeView* tree_view,
                            GtkTreePath* path,
                            GtkTreeViewColumn* column,
                            gpointer data) {
  reinterpret_cast<GtkMainWnd*>(data)->OnRowActivated(tree_view, path, column);
}

gboolean SimulateLastRowActivated(gpointer data) {
  GtkTreeView* tree_view = reinterpret_cast<GtkTreeView*>(data);
  GtkTreeModel* model = gtk_tree_view_get_model(tree_view);

  // "if iter is NULL, then the number of toplevel nodes is returned."
  int rows = gtk_tree_model_iter_n_children(model, nullptr);
  GtkTreePath* lastpath = gtk_tree_path_new_from_indices(rows - 1, -1);

  // Select the last item in the list
  GtkTreeSelection* selection = gtk_tree_view_get_selection(tree_view);
  gtk_tree_selection_select_path(selection, lastpath);

  // Our TreeView only has one column, so it is column 0.
  GtkTreeViewColumn* column = gtk_tree_view_get_column(tree_view, 0);

  gtk_tree_view_row_activated(tree_view, lastpath, column);

  gtk_tree_path_free(lastpath);
  return false;
}

// Creates a tree view, that we use to display the list of peers.
void InitializeList(GtkWidget* list) {
  GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes(
      "List Items", renderer, "text", 0, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(list), column);
  GtkListStore* store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);
  gtk_tree_view_set_model(GTK_TREE_VIEW(list), GTK_TREE_MODEL(store));
  g_object_unref(store);
}

// Adds an entry to a tree view.
void AddToList(GtkWidget* list, const gchar* str, int value) {
  GtkListStore* store =
      GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(list)));

  GtkTreeIter iter;
  gtk_list_store_append(store, &iter);
  gtk_list_store_set(store, &iter, 0, str, 1, value, -1);
}

struct UIThreadCallbackData {
  explicit UIThreadCallbackData(MainWndCallback* cb, int id, void* d)
      : callback(cb), msg_id(id), data(d) {}
  MainWndCallback* callback;
  int msg_id;
  void* data;
};

gboolean HandleUIThreadCallback(gpointer data) {
  UIThreadCallbackData* cb_data = reinterpret_cast<UIThreadCallbackData*>(data);
  cb_data->callback->UIThreadCallback(cb_data->msg_id, cb_data->data);
  delete cb_data;
  return false;
}

gboolean Redraw(gpointer data) {
  GtkMainWnd* wnd = reinterpret_cast<GtkMainWnd*>(data);
  wnd->OnRedraw();
  return false;
}

void OnSendClickedCallback(GtkWidget* widget, gpointer data) {
  reinterpret_cast<GtkMainWnd*>(data)->OnSendClicked(widget);
}

struct ChatMessageData {
  GtkMainWnd* wnd;
  std::string role;
  std::string text;
};

struct ContextDocsData {
  GtkMainWnd* wnd;
  std::vector<std::string> texts;
};

gboolean HandleContextDocsCallback(gpointer data) {
  ContextDocsData* d = reinterpret_cast<ContextDocsData*>(data);
  d->wnd->LoadContextDocumentsImpl(d->texts);
  delete d;
  return FALSE;
}

gboolean SampleThroughputCallback(gpointer data) {
  reinterpret_cast<GtkMainWnd*>(data)->SampleThroughput();
  return TRUE;  // Keep timer running
}

gboolean HandleChatMessageCallback(gpointer data) {
  ChatMessageData* msg = reinterpret_cast<ChatMessageData*>(data);
  msg->wnd->AppendChatMessageImpl(msg->role, msg->text);
  delete msg;
  return FALSE;
}

gboolean Draw(GtkWidget* widget, cairo_t* cr, gpointer data) {
  GtkMainWnd* wnd = reinterpret_cast<GtkMainWnd*>(data);
  wnd->Draw(widget, cr);
  return false;
}

}  // namespace

//
// GtkMainWnd implementation.
//

GtkMainWnd::GtkMainWnd(const char* server,
                       int port,
                       bool autoconnect,
                       bool autocall,
                       bool headless,
                       bool demo_mode)
    : window_(nullptr),
      draw_area_(nullptr),
      vbox_(nullptr),
      server_edit_(nullptr),
      port_edit_(nullptr),
      peer_list_(nullptr),
      callback_(nullptr),
      server_(server),
      autoconnect_(autoconnect),
      autocall_(autocall),
      headless_(headless),
      demo_mode_(demo_mode) {
  char buffer[10];
  snprintf(buffer, sizeof(buffer), "%i", port);
  port_ = buffer;
}

GtkMainWnd::~GtkMainWnd() {
  // IsWindow() is hardcoded to always return true in headless mode (see
  // below), so this invariant only applies to the real GTK window case.
  RTC_DCHECK(headless_ || !IsWindow());
}

void GtkMainWnd::RegisterObserver(MainWndCallback* callback) {
  callback_ = callback;
}

bool GtkMainWnd::IsWindow() {
  if (headless_) {
    return true;  // Always "windowed" in headless mode
  }
  return window_ != nullptr && GTK_IS_WINDOW(window_);
}

void GtkMainWnd::MessageBox(const char* caption,
                            const char* text,
                            bool is_error) {
  if (headless_) {
<<<<<<< ours
    printf("[%s] %s: %s\n", is_error ? "ERROR" : "INFO", caption, text);
=======
    if (is_error) {
      RTC_LOG(LS_ERROR) << "MessageBox(" << caption << "): " << text;
    } else {
      RTC_LOG(LS_INFO) << "MessageBox(" << caption << "): " << text;
    }
>>>>>>> theirs
    return;
  }
  GtkWidget* dialog = gtk_message_dialog_new(
      GTK_WINDOW(window_), GTK_DIALOG_DESTROY_WITH_PARENT,
      is_error ? GTK_MESSAGE_ERROR : GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "%s",
      text);
  gtk_window_set_title(GTK_WINDOW(dialog), caption);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

MainWindow::UI GtkMainWnd::current_ui() {
  if (vbox_)
    return CONNECT_TO_SERVER;

  if (peer_list_)
    return LIST_PEERS;

  return STREAMING;
}

void GtkMainWnd::StartLocalRenderer(webrtc::VideoTrackInterface* local_video) {
  if (headless_) {
    return;  // Skip renderer in headless mode
  }
  local_renderer_.reset(new VideoRenderer(this, local_video));
}

void GtkMainWnd::StopLocalRenderer() {
  local_renderer_.reset();
}

void GtkMainWnd::StartRemoteRenderer(
    webrtc::VideoTrackInterface* remote_video) {
  if (headless_) {
    // In headless mode, create lightweight sink for frame delay measurement
    fprintf(stderr, "[FrameDelay] StartRemoteRenderer called in headless mode\n");
    frame_delay_sink_.reset(new FrameDelaySink(remote_video));
    return;
  }
  remote_renderer_.reset(new VideoRenderer(this, remote_video));
}

void GtkMainWnd::StopRemoteRenderer() {
  remote_renderer_.reset();
  frame_delay_sink_.reset();
}

void GtkMainWnd::QueueUIThreadCallback(int msg_id, void* data) {
  if (headless_) {
    // In headless mode, directly invoke the callback
    if (callback_) {
      callback_->UIThreadCallback(msg_id, data);
    }
    return;
  }
  g_idle_add(HandleUIThreadCallback,
             new UIThreadCallbackData(callback_, msg_id, data));
}

bool GtkMainWnd::Create() {
  if (headless_) {
    // In headless mode, skip GTK window creation
    printf("[Headless] Window creation skipped\n");
    if (autoconnect_ && callback_) {
      // Auto-connect in headless mode
      callback_->StartLogin(server_, atoi(port_.c_str()));
    }
    return true;
  }

  RTC_DCHECK(window_ == nullptr);

  window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  if (window_) {
    gtk_window_set_position(GTK_WINDOW(window_), GTK_WIN_POS_CENTER);
    gtk_window_set_default_size(GTK_WINDOW(window_), 640, 480);
    gtk_window_set_title(GTK_WINDOW(window_), "PeerConnection client");
    g_signal_connect(G_OBJECT(window_), "delete-event",
                     G_CALLBACK(&OnDestroyedCallback), this);
    g_signal_connect(window_, "key-press-event", G_CALLBACK(OnKeyPressCallback),
                     this);

    SwitchToConnectUI();
  }

  return window_ != nullptr;
}

bool GtkMainWnd::Destroy() {
  if (headless_) {
    return true;
  }
  if (!IsWindow())
    return false;

  gtk_widget_destroy(window_);
  window_ = nullptr;

  return true;
}

void GtkMainWnd::SwitchToConnectUI() {
  RTC_LOG(LS_INFO) << __FUNCTION__;

  if (headless_) {
    // In headless mode, auto-connect if configured
    if (autoconnect_ && callback_) {
      callback_->StartLogin(server_, atoi(port_.c_str()));
    }
    return;
  }

  RTC_DCHECK(IsWindow());
  RTC_DCHECK(vbox_ == nullptr);

  gtk_container_set_border_width(GTK_CONTAINER(window_), 10);

  if (peer_list_) {
    gtk_widget_destroy(peer_list_);
    peer_list_ = nullptr;
  }

  vbox_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  GtkWidget* valign = gtk_alignment_new(0, 1, 0, 0);
  gtk_container_add(GTK_CONTAINER(vbox_), valign);
  gtk_container_add(GTK_CONTAINER(window_), vbox_);

  GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  GtkWidget* label = gtk_label_new("Server");
  gtk_container_add(GTK_CONTAINER(hbox), label);

  server_edit_ = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(server_edit_), server_.c_str());
  gtk_widget_set_size_request(server_edit_, 400, 30);
  gtk_container_add(GTK_CONTAINER(hbox), server_edit_);

  port_edit_ = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(port_edit_), port_.c_str());
  gtk_widget_set_size_request(port_edit_, 70, 30);
  gtk_container_add(GTK_CONTAINER(hbox), port_edit_);

  GtkWidget* button = gtk_button_new_with_label("Connect");
  gtk_widget_set_size_request(button, 70, 30);
  g_signal_connect(button, "clicked", G_CALLBACK(OnClickedCallback), this);
  gtk_container_add(GTK_CONTAINER(hbox), button);

  GtkWidget* halign = gtk_alignment_new(1, 0, 0, 0);
  gtk_container_add(GTK_CONTAINER(halign), hbox);
  gtk_box_pack_start(GTK_BOX(vbox_), halign, FALSE, FALSE, 0);

  gtk_widget_show_all(window_);

  if (autoconnect_)
    g_idle_add(SimulateButtonClick, button);
}

void GtkMainWnd::SwitchToPeerList(const Peers& peers) {
  RTC_LOG(LS_INFO) << __FUNCTION__;

  if (headless_) {
    printf("[Headless] Peer list: %zu peers\n", peers.size());
    for (const auto& peer : peers) {
      printf("[Headless]   Peer %d: %s\n", peer.first, peer.second.c_str());
    }
    // In headless mode with autocall, call the first peer
    if (autocall_ && !peers.empty() && callback_) {
      int first_peer_id = peers.begin()->first;
      printf("[Headless] Auto-calling peer %d\n", first_peer_id);
      callback_->ConnectToPeer(first_peer_id);
    }
    return;
  }

  if (!peer_list_) {
    gtk_container_set_border_width(GTK_CONTAINER(window_), 0);
    if (vbox_) {
      gtk_widget_destroy(vbox_);
      vbox_ = nullptr;
      server_edit_ = nullptr;
      port_edit_ = nullptr;
    } else if (main_hbox_) {
      if (throughput_sample_timer_id_ != 0) {
        g_source_remove(throughput_sample_timer_id_);
        throughput_sample_timer_id_ = 0;
      }
      gtk_widget_destroy(main_hbox_);
      main_hbox_ = nullptr;
      draw_area_ = nullptr;
      chat_box_ = nullptr;
      response_view_ = nullptr;
      query_entry_ = nullptr;
      send_button_ = nullptr;
      docs_box_ = nullptr;
      docs_scroll_ = nullptr;
      docs_view_ = nullptr;
      draw_buffer_.SetSize(0);
    } else if (draw_area_) {
      gtk_widget_destroy(draw_area_);
      draw_area_ = nullptr;
      draw_buffer_.SetSize(0);
    }

    peer_list_ = gtk_tree_view_new();
    g_signal_connect(peer_list_, "row-activated",
                     G_CALLBACK(OnRowActivatedCallback), this);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(peer_list_), FALSE);
    InitializeList(peer_list_);
    gtk_container_add(GTK_CONTAINER(window_), peer_list_);
    gtk_widget_show_all(window_);
  } else {
    GtkListStore* store =
        GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(peer_list_)));
    gtk_list_store_clear(store);
  }

  AddToList(peer_list_, "List of currently connected peers:", -1);
  for (Peers::const_iterator i = peers.begin(); i != peers.end(); ++i)
    AddToList(peer_list_, i->second.c_str(), i->first);

  if (autocall_ && peers.begin() != peers.end())
    g_idle_add(SimulateLastRowActivated, peer_list_);
}

void GtkMainWnd::SwitchToStreamingUI() {
  RTC_LOG(LS_INFO) << __FUNCTION__;

  if (headless_) {
    printf("[Headless] Switched to streaming mode\n");
    return;
  }

  // Already in streaming mode
  if (draw_area_ != nullptr)
    return;

  gtk_container_set_border_width(GTK_CONTAINER(window_), 0);

  // Remove the connect UI vbox (Server/Port/Connect button)
  if (vbox_) {
    gtk_widget_destroy(vbox_);
    vbox_ = nullptr;
    server_edit_ = nullptr;
    port_edit_ = nullptr;
  }

  if (peer_list_) {
    gtk_widget_destroy(peer_list_);
    peer_list_ = nullptr;
  }

  if (demo_mode_) {
    // =================================================================
    // V11: Compact sidebar (900x680) — for PPT side-by-side
    // Left: Video+Graph (480px) | Right: Docs + Status + Chat (418px)
    // =================================================================
    main_hbox_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(window_), main_hbox_);

    // Left: Video+Graph (480px)
    draw_area_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(draw_area_, 480, 680);
    g_signal_connect(G_OBJECT(draw_area_), "draw", G_CALLBACK(&::Draw), this);
    gtk_box_pack_start(GTK_BOX(main_hbox_), draw_area_, FALSE, FALSE, 0);

    // Separator
    gtk_box_pack_start(GTK_BOX(main_hbox_), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);

    // Right sidebar (418px)
    GtkWidget* sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(sidebar, 418, -1);
    gtk_box_pack_start(GTK_BOX(main_hbox_), sidebar, TRUE, TRUE, 0);

    // -- Documents section (top ~50%) --
    docs_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(sidebar), docs_box_, TRUE, TRUE, 0);

    GtkWidget* docs_label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(docs_label),
        "<span font_desc='9'><b>Context Documents</b></span>");
    gtk_widget_set_halign(docs_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(docs_label, 8);
    gtk_widget_set_margin_top(docs_label, 2);
    gtk_box_pack_start(GTK_BOX(docs_box_), docs_label, FALSE, FALSE, 0);

    docs_scroll_ = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(docs_scroll_),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    docs_view_ = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(docs_view_), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(docs_view_), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(docs_view_), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(docs_view_), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(docs_view_), 8);

    GtkTextBuffer* docs_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(docs_view_));
    gtk_text_buffer_create_tag(docs_buf, "context0", "foreground", "#2196F3", NULL);
    gtk_text_buffer_create_tag(docs_buf, "context1", "foreground", "#4CAF50", NULL);
    gtk_text_buffer_create_tag(docs_buf, "context2", "foreground", "#FF9800", NULL);
    gtk_text_buffer_create_tag(docs_buf, "doc_header",
                               "weight", PANGO_WEIGHT_BOLD, "scale", 1.1, NULL);

    gtk_container_add(GTK_CONTAINER(docs_scroll_), docs_view_);
    gtk_box_pack_start(GTK_BOX(docs_box_), docs_scroll_, TRUE, TRUE, 0);

    // -- Separator --
    gtk_box_pack_start(GTK_BOX(sidebar), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    // -- Status bar (elapsed + phase) --
    GtkWidget* status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(status_bar, 8);
    gtk_widget_set_margin_end(status_bar, 8);
    gtk_widget_set_margin_top(status_bar, 4);
    gtk_widget_set_margin_bottom(status_bar, 4);

    elapsed_label_ = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(elapsed_label_),
        "<span font_desc='10' weight='bold' foreground='#4FC3F7'>⏱ Ready</span>");
    gtk_widget_set_halign(elapsed_label_, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(status_bar), elapsed_label_, FALSE, FALSE, 0);

    phase_label_ = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(phase_label_),
        "<span font_desc='9' foreground='#888888'>Waiting for query...</span>");
    gtk_widget_set_halign(phase_label_, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(status_bar), phase_label_, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(sidebar), status_bar, FALSE, FALSE, 0);

    // -- Separator --
    gtk_box_pack_start(GTK_BOX(sidebar), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    // -- Chat section (bottom ~45%) --
    chat_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(sidebar), chat_box_, TRUE, TRUE, 0);

    GtkWidget* chat_label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(chat_label),
        "<span font_desc='9'><b>LLM Response</b></span>");
    gtk_widget_set_halign(chat_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(chat_label, 8);
    gtk_widget_set_margin_top(chat_label, 2);
    gtk_box_pack_start(GTK_BOX(chat_box_), chat_label, FALSE, FALSE, 0);

    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    response_view_ = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(response_view_), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(response_view_), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(response_view_), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(response_view_), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(response_view_), 8);
    gtk_container_add(GTK_CONTAINER(scrolled), response_view_);
    gtk_box_pack_start(GTK_BOX(chat_box_), scrolled, TRUE, TRUE, 0);

    // Input row
    GtkWidget* input_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(input_row), 4);
    query_entry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(query_entry_), "Ask a question...");
    gtk_box_pack_start(GTK_BOX(input_row), query_entry_, TRUE, TRUE, 0);
    g_signal_connect(query_entry_, "activate", G_CALLBACK(OnSendClickedCallback), this);
    send_button_ = gtk_button_new_with_label("Send");
    g_signal_connect(send_button_, "clicked", G_CALLBACK(OnSendClickedCallback), this);
    gtk_box_pack_start(GTK_BOX(input_row), send_button_, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(chat_box_), input_row, FALSE, FALSE, 0);

    // Window size: 900x680
    gtk_window_set_default_size(GTK_WINDOW(window_), 900, 680);
    gtk_widget_show_all(window_);

    throughput_sample_timer_id_ = g_timeout_add(1000, SampleThroughputCallback, this);

    // Elapsed timer: update every 200ms
    elapsed_timer_id_ = g_timeout_add(200, [](gpointer data) -> gboolean {
      auto* wnd = static_cast<GtkMainWnd*>(data);
      int64_t start = wnd->query_start_ms_.load(std::memory_order_relaxed);
      if (start > 0 && wnd->elapsed_label_) {
        int64_t now = g_get_monotonic_time() / 1000;  // ms
        double elapsed = (now - start) / 1000.0;
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "<span font_desc='10' weight='bold' foreground='#4FC3F7'>"
                 "⏱ %.1fs</span>", elapsed);
        gtk_label_set_markup(GTK_LABEL(wnd->elapsed_label_), buf);
      }
      return TRUE;
    }, this);

    printf("[GTK] Demo UI v11: Compact sidebar 900x680\n");
    fflush(stdout);
  } else {
    // Standard mode: video only
    draw_area_ = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(window_), draw_area_);
    g_signal_connect(G_OBJECT(draw_area_), "draw", G_CALLBACK(&::Draw), this);

    gtk_widget_show_all(window_);
    printf("[GTK] Switched to streaming UI\n");
    fflush(stdout);
  }
}

void GtkMainWnd::OnSendClicked(GtkWidget* widget) {
  if (!query_entry_ || !callback_)
    return;

  const char* text = gtk_entry_get_text(GTK_ENTRY(query_entry_));
  if (!text || strlen(text) == 0)
    return;

  std::string query(text);
  gtk_entry_set_text(GTK_ENTRY(query_entry_), "");

  // Show user message in chat
  AppendChatMessageImpl("You", query);

  // Notify conductor
  callback_->OnQuerySubmitted(query);
}

void GtkMainWnd::AppendChatMessage(const std::string& role,
                                    const std::string& text) {
  if (headless_) {
    printf("[Chat] [%s] %s\n", role.c_str(), text.c_str());
    fflush(stdout);
    return;
  }
  // Thread-safe: post to GTK main loop
  auto* data = new ChatMessageData{this, role, text};
  g_idle_add(HandleChatMessageCallback, data);
}

void GtkMainWnd::AppendChatMessageImpl(const std::string& role,
                                        const std::string& text) {
  if (!response_view_)
    return;

  GtkTextBuffer* buffer =
      gtk_text_view_get_buffer(GTK_TEXT_VIEW(response_view_));
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buffer, &end);

  if (role.empty()) {
    // Empty role = append token to current line (streaming mode)
    gtk_text_buffer_insert(buffer, &end, text.c_str(), -1);
  } else {
    // Normal message with role prefix
    std::string line = "[" + role + "] " + text + "\n";
    gtk_text_buffer_insert(buffer, &end, line.c_str(), -1);
  }

  // Auto-scroll to bottom
  GtkTextMark* mark = gtk_text_buffer_get_insert(buffer);
  gtk_text_buffer_get_end_iter(buffer, &end);
  gtk_text_buffer_move_mark(buffer, mark, &end);
  gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(response_view_), mark);
}

void GtkMainWnd::OnDestroyed(GtkWidget* widget, GdkEvent* event) {
  if (throughput_sample_timer_id_ != 0) {
    g_source_remove(throughput_sample_timer_id_);
    throughput_sample_timer_id_ = 0;
  }
  callback_->Close();
  window_ = nullptr;
  draw_area_ = nullptr;
  vbox_ = nullptr;
  server_edit_ = nullptr;
  port_edit_ = nullptr;
  peer_list_ = nullptr;
  main_hbox_ = nullptr;
  chat_box_ = nullptr;
  response_view_ = nullptr;
  query_entry_ = nullptr;
  send_button_ = nullptr;
  docs_box_ = nullptr;
  docs_scroll_ = nullptr;
  docs_view_ = nullptr;
}

void GtkMainWnd::OnClicked(GtkWidget* widget) {
  // Make the connect button insensitive, so that it cannot be clicked more than
  // once.  Now that the connection includes auto-retry, it should not be
  // necessary to click it more than once.
  gtk_widget_set_sensitive(widget, false);
  server_ = gtk_entry_get_text(GTK_ENTRY(server_edit_));
  port_ = gtk_entry_get_text(GTK_ENTRY(port_edit_));
  int port = !port_.empty() ? atoi(port_.c_str()) : 0;
  callback_->StartLogin(server_, port);
}

void GtkMainWnd::OnKeyPress(GtkWidget* widget, GdkEventKey* key) {
  if (key->type == GDK_KEY_PRESS) {
    switch (key->keyval) {
      case GDK_KEY_Escape:
        if (draw_area_) {
          callback_->DisconnectFromCurrentPeer();
        } else if (peer_list_) {
          callback_->DisconnectFromServer();
        }
        break;

      case GDK_KEY_KP_Enter:
      case GDK_KEY_Return:
        if (vbox_) {
          OnClicked(nullptr);
        } else if (peer_list_) {
          // OnRowActivated will be called automatically when the user
          // presses enter.
        }
        break;

      default:
        break;
    }
  }
}

void GtkMainWnd::OnRowActivated(GtkTreeView* tree_view,
                                GtkTreePath* path,
                                GtkTreeViewColumn* column) {
  RTC_DCHECK(peer_list_ != nullptr);
  GtkTreeIter iter;
  GtkTreeModel* model;
  GtkTreeSelection* selection =
      gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
  if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
    char* text;
    int id = -1;
    gtk_tree_model_get(model, &iter, 0, &text, 1, &id, -1);
    if (id != -1)
      callback_->ConnectToPeer(id);
    g_free(text);
  }
}

void GtkMainWnd::OnRedraw() {
  gdk_threads_enter();

  VideoRenderer* remote_renderer = remote_renderer_.get();
  if (remote_renderer && !remote_renderer->image().empty() &&
      draw_area_ != nullptr) {
    if (width_ != remote_renderer->width() ||
        height_ != remote_renderer->height()) {
      width_ = remote_renderer->width();
      height_ = remote_renderer->height();
      // Don't resize draw_area to original resolution - keep fixed size
      // and let Draw() scale the video to fit
    }
    draw_buffer_.SetData(remote_renderer->image());
    gtk_widget_queue_draw(draw_area_);
  }
  // Here we can draw the local preview as well if we want....
  gdk_threads_leave();
}

void GtkMainWnd::Draw(GtkWidget* widget, cairo_t* cr) {
  int alloc_w = gtk_widget_get_allocated_width(widget);
  int alloc_h = gtk_widget_get_allocated_height(widget);

  // Clear background
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_paint(cr);

  if (demo_mode_) {
    // Demo mode: graph area + optional video below
    int graph_h = std::min(kGraphHeight, alloc_h);
    int video_area_h = alloc_h - graph_h;

    // Draw performance graph at top (or full area if no room for video)
    cairo_save(cr);
    if (video_area_h < 50) {
      // Graph-only mode (e.g., thin top bar)
      DrawPerformanceGraph(cr, alloc_w, alloc_h);
    } else {
      DrawPerformanceGraph(cr, alloc_w, graph_h);
    }
    cairo_restore(cr);

    // Draw video below graph (only if enough space)
    if (video_area_h >= 50 && width_ > 0 && height_ > 0 && draw_buffer_.data()) {
      cairo_save(cr);
      cairo_translate(cr, 0, graph_h);

      double scale_x = static_cast<double>(alloc_w) / width_;
      double scale_y = static_cast<double>(video_area_h) / height_;
      double scale = std::min(scale_x, scale_y);

      double video_w = width_ * scale;
      double video_h = height_ * scale;
      double offset_x = (alloc_w - video_w) / 2.0;
      double offset_y = (video_area_h - video_h) / 2.0;

      cairo_translate(cr, offset_x, offset_y);
      cairo_scale(cr, scale, scale);

      cairo_format_t format = CAIRO_FORMAT_ARGB32;
      cairo_surface_t* surface = cairo_image_surface_create_for_data(
          draw_buffer_.data(), format, width_, height_,
          cairo_format_stride_for_width(format, width_));
      cairo_set_source_surface(cr, surface, 0, 0);
      cairo_rectangle(cr, 0, 0, width_, height_);
      cairo_fill(cr);
      cairo_surface_destroy(surface);
      cairo_restore(cr);
    }
  } else {
    // Standard mode: video only
    if (width_ <= 0 || height_ <= 0 || !draw_buffer_.data())
      return;

    double scale_x = static_cast<double>(alloc_w) / width_;
    double scale_y = static_cast<double>(alloc_h) / height_;
    double scale = std::min(scale_x, scale_y);

    double video_w = width_ * scale;
    double video_h = height_ * scale;
    double offset_x = (alloc_w - video_w) / 2.0;
    double offset_y = (alloc_h - video_h) / 2.0;

    cairo_save(cr);
    cairo_translate(cr, offset_x, offset_y);
    cairo_scale(cr, scale, scale);

    cairo_format_t format = CAIRO_FORMAT_ARGB32;
    cairo_surface_t* surface = cairo_image_surface_create_for_data(
        draw_buffer_.data(), format, width_, height_,
        cairo_format_stride_for_width(format, width_));
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_rectangle(cr, 0, 0, width_, height_);
    cairo_fill(cr);
    cairo_surface_destroy(surface);
    cairo_restore(cr);
  }
}

void GtkMainWnd::DrawPerformanceGraph(cairo_t* cr, int width, int height) {
  // Dark background for graph area
  cairo_set_source_rgb(cr, 0.1, 0.1, 0.15);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // Graph margins
  const int margin_l = 60;
  const int margin_r = 60;
  const int margin_t = 20;
  const int margin_b = 25;
  int gw = width - margin_l - margin_r;
  int gh = height - margin_t - margin_b;

  if (gw <= 0 || gh <= 0)
    return;

  // Draw grid lines (horizontal)
  cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.5);
  cairo_set_line_width(cr, 0.5);
  for (int i = 0; i <= 4; ++i) {
    double y = margin_t + gh * (1.0 - i / 4.0);
    cairo_move_to(cr, margin_l, y);
    cairo_line_to(cr, margin_l + gw, y);
    cairo_stroke(cr);
  }

  // Left Y-axis labels (Video, blue)
  cairo_set_source_rgb(cr, 0.13, 0.59, 0.95);  // #2196F3
  cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 10);
  for (int i = 0; i <= 4; ++i) {
    double y = margin_t + gh * (1.0 - i / 4.0);
    char label[16];
    snprintf(label, sizeof(label), "%.0f", kVideoMaxMbps * i / 4.0);
    cairo_move_to(cr, 5, y + 4);
    cairo_show_text(cr, label);
  }
  // Left axis title
  cairo_move_to(cr, 5, margin_t - 5);
  cairo_show_text(cr, "Video Mbps");

  // Right Y-axis labels (SCTP, orange)
  cairo_set_source_rgb(cr, 1.0, 0.6, 0.0);  // #FF9800
  for (int i = 0; i <= 4; ++i) {
    double y = margin_t + gh * (1.0 - i / 4.0);
    char label[16];
    snprintf(label, sizeof(label), "%.0f", kSctpMaxMbps * i / 4.0);
    cairo_move_to(cr, margin_l + gw + 5, y + 4);
    cairo_show_text(cr, label);
  }
  // Right axis title
  cairo_move_to(cr, margin_l + gw + 5, margin_t - 5);
  cairo_show_text(cr, "Context Mbps");

  // Draw video throughput line (blue)
  if (video_throughput_history_.size() > 1) {
    cairo_set_source_rgba(cr, 0.13, 0.59, 0.95, 0.9);
    cairo_set_line_width(cr, 2.0);
    size_t n = video_throughput_history_.size();
    for (size_t i = 0; i < n; ++i) {
      double x = margin_l + gw * static_cast<double>(i) / (n - 1);
      double val = std::min(video_throughput_history_[i] / kVideoMaxMbps, 1.0f);
      double y = margin_t + gh * (1.0 - val);
      if (i == 0)
        cairo_move_to(cr, x, y);
      else
        cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
  }

  // Draw SCTP throughput line (orange)
  if (sctp_throughput_history_.size() > 1) {
    cairo_set_source_rgba(cr, 1.0, 0.6, 0.0, 0.9);
    cairo_set_line_width(cr, 2.0);
    size_t n = sctp_throughput_history_.size();
    for (size_t i = 0; i < n; ++i) {
      double x = margin_l + gw * static_cast<double>(i) / (n - 1);
      double val = std::min(sctp_throughput_history_[i] / kSctpMaxMbps, 1.0f);
      double y = margin_t + gh * (1.0 - val);
      if (i == 0)
        cairo_move_to(cr, x, y);
      else
        cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
  }

  // Legend
  cairo_set_font_size(cr, 11);
  // Video legend
  cairo_set_source_rgb(cr, 0.13, 0.59, 0.95);
  cairo_rectangle(cr, margin_l + 10, margin_t + 5, 12, 12);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
  cairo_move_to(cr, margin_l + 26, margin_t + 15);
  cairo_show_text(cr, "Video");
  // SCTP legend
  cairo_set_source_rgb(cr, 1.0, 0.6, 0.0);
  cairo_rectangle(cr, margin_l + 80, margin_t + 5, 12, 12);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
  cairo_move_to(cr, margin_l + 96, margin_t + 15);
  cairo_show_text(cr, "Context (SCTP)");

  // Show current values
  if (!video_throughput_history_.empty() || !sctp_throughput_history_.empty()) {
    char info[128];
    float v = video_throughput_history_.empty() ? 0.0f : video_throughput_history_.back();
    float s = sctp_throughput_history_.empty() ? 0.0f : sctp_throughput_history_.back();
    snprintf(info, sizeof(info), "Video: %.1f Mbps  |  Context: %.1f Mbps", v, s);
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
    cairo_set_font_size(cr, 12);
    cairo_move_to(cr, margin_l + gw / 2 - 100, height - 5);
    cairo_show_text(cr, info);
  }
}

void GtkMainWnd::SetCurrentQueryPath(const std::string& path) {
  // Currently unused — reserved for future query-specific behavior
}

void GtkMainWnd::OnSctpDataReceived(size_t bytes) {
  sctp_bytes_received_.fetch_add(bytes, std::memory_order_relaxed);
}

void GtkMainWnd::UpdateThroughput(float video_mbps, float sctp_mbps) {
  stats_video_mbps_.store(video_mbps, std::memory_order_relaxed);
  stats_sctp_mbps_.store(sctp_mbps, std::memory_order_relaxed);
}

void GtkMainWnd::LoadContextDocuments(const std::vector<std::string>& texts) {
  if (headless_) {
    printf("[Docs] Received %zu context documents\n", texts.size());
    for (size_t i = 0; i < texts.size(); ++i) {
      printf("[Docs]   [%zu] %zu bytes\n", i, texts[i].size());
    }
    fflush(stdout);
    return;
  }
  // Thread-safe: post to GTK main loop
  auto* data = new ContextDocsData{this, texts};
  g_idle_add(HandleContextDocsCallback, data);
}

void GtkMainWnd::LoadContextDocumentsImpl(const std::vector<std::string>& texts) {
  if (!docs_view_)
    return;

  GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(docs_view_));
  gtk_text_buffer_set_text(buffer, "", 0);  // Clear existing

  GtkTextIter end;
  for (size_t i = 0; i < texts.size(); ++i) {
    // Document header
    gtk_text_buffer_get_end_iter(buffer, &end);
    char header[64];
    snprintf(header, sizeof(header), "--- Context %zu ---\n", i);
    gtk_text_buffer_insert_with_tags_by_name(buffer, &end, header, -1,
                                              "doc_header", NULL);

    // Document body with color tag
    gtk_text_buffer_get_end_iter(buffer, &end);
    char tag_name[16];
    snprintf(tag_name, sizeof(tag_name), "context%zu", i % 3);
    gtk_text_buffer_insert_with_tags_by_name(buffer, &end,
                                              texts[i].c_str(), -1,
                                              tag_name, NULL);

    // Separator
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, "\n\n", -1);
  }

  printf("[GTK] Loaded %zu context documents into Documents panel\n",
         texts.size());
  fflush(stdout);
}

void GtkMainWnd::SampleThroughput() {
  // Use RTCStats-reported throughput for video (actual RTP bitrate)
  float video_mbps = stats_video_mbps_.load(std::memory_order_relaxed);

  // SCTP: always use direct byte counter from OnSctpDataReceived().
  // RTCStats averages poorly over burst MAFS transfers (showed 6 Mbps
  // when actual aggregate was ~140 Mbps). The byte counter captures
  // every received byte in real-time, giving accurate 1-second deltas.
  size_t current_sctp = sctp_bytes_received_.load(std::memory_order_relaxed);
  size_t delta_sctp = current_sctp - last_sctp_bytes_;
  last_sctp_bytes_ = current_sctp;
  float sctp_mbps = static_cast<float>(delta_sctp * 8) / 1e6f;

  // Keep 60 samples (1 minute of history)
  constexpr size_t kMaxSamples = 60;
  sctp_throughput_history_.push_back(sctp_mbps);
  if (sctp_throughput_history_.size() > kMaxSamples)
    sctp_throughput_history_.pop_front();

  video_throughput_history_.push_back(video_mbps);
  if (video_throughput_history_.size() > kMaxSamples)
    video_throughput_history_.pop_front();

  // Trigger redraw of the graph
  if (draw_area_)
    gtk_widget_queue_draw(draw_area_);
}

void GtkMainWnd::OnQueryStarted() {
  query_start_ms_.store(g_get_monotonic_time() / 1000,
                        std::memory_order_relaxed);
  SetQueryPhase("Sending query...");
}

void GtkMainWnd::SetQueryPhase(const std::string& phase) {
  if (!phase_label_)
    return;
  // Must run on GTK thread
  std::string* copy = new std::string(phase);
  g_idle_add(
      [](gpointer data) -> gboolean {
        auto* args = static_cast<std::pair<GtkMainWnd*, std::string*>*>(data);
        args->first->SetQueryPhaseImpl(*args->second);
        delete args->second;
        delete args;
        return FALSE;
      },
      new std::pair<GtkMainWnd*, std::string*>(this, copy));
}

void GtkMainWnd::SetQueryPhaseImpl(const std::string& phase) {
  if (!phase_label_)
    return;
  // Color-code phases
  const char* color = "#888888";
  if (phase.find("Transfer") != std::string::npos ||
      phase.find("Receiv") != std::string::npos)
    color = "#FF9800";  // orange
  else if (phase.find("Encod") != std::string::npos ||
           phase.find("Load") != std::string::npos)
    color = "#FFC107";  // amber
  else if (phase.find("Generat") != std::string::npos)
    color = "#4CAF50";  // green
  else if (phase.find("Complete") != std::string::npos)
    color = "#2196F3";  // blue
  else if (phase.find("No context") != std::string::npos)
    color = "#F44336";  // red

  char buf[512];
  snprintf(buf, sizeof(buf),
           "<span font_desc='9' foreground='%s'>%s</span>",
           color, phase.c_str());
  gtk_label_set_markup(GTK_LABEL(phase_label_), buf);
}

GtkMainWnd::VideoRenderer::VideoRenderer(
    GtkMainWnd* main_wnd,
    webrtc::VideoTrackInterface* track_to_render)
    : width_(0),
      height_(0),
      main_wnd_(main_wnd),
      rendered_track_(track_to_render) {
  rendered_track_->AddOrUpdateSink(this, rtc::VideoSinkWants());
}

GtkMainWnd::VideoRenderer::~VideoRenderer() {
  rendered_track_->RemoveSink(this);
}

void GtkMainWnd::VideoRenderer::SetSize(int width, int height) {
  gdk_threads_enter();

  if (width_ == width && height_ == height) {
    return;
  }

  width_ = width;
  height_ = height;
  // ARGB
  image_.SetSize(width * height * 4);
  gdk_threads_leave();
}

void GtkMainWnd::VideoRenderer::OnFrame(const webrtc::VideoFrame& video_frame) {
  gdk_threads_enter();

  webrtc::scoped_refptr<webrtc::I420BufferInterface> buffer(
      video_frame.video_frame_buffer()->ToI420());
  if (video_frame.rotation() != webrtc::kVideoRotation_0) {
    buffer = webrtc::I420Buffer::Rotate(*buffer, video_frame.rotation());
  }
  SetSize(buffer->width(), buffer->height());

  // TODO(bugs.webrtc.org/6857): This conversion is correct for little-endian
  // only. Cairo ARGB32 treats pixels as 32-bit values in *native* byte order,
  // with B in the least significant byte of the 32-bit value. Which on
  // little-endian means that memory layout is BGRA, with the B byte stored at
  // lowest address. Libyuv's ARGB format (surprisingly?) uses the same
  // little-endian format, with B in the first byte in memory, regardless of
  // native endianness.
  libyuv::I420ToARGB(buffer->DataY(), buffer->StrideY(), buffer->DataU(),
                     buffer->StrideU(), buffer->DataV(), buffer->StrideV(),
                     image_.data(), width_ * 4, buffer->width(),
                     buffer->height());

  gdk_threads_leave();

  g_idle_add(Redraw, main_wnd_);
}

//
// FrameDelaySink — lightweight sink for per-frame delay measurement
//

GtkMainWnd::FrameDelaySink::FrameDelaySink(
    webrtc::VideoTrackInterface* track)
    : track_(track) {
  track_->AddOrUpdateSink(this, rtc::VideoSinkWants());
  fprintf(stderr, "[FrameDelaySink] Registered as remote video sink\n");
}

GtkMainWnd::FrameDelaySink::~FrameDelaySink() {
  track_->RemoveSink(this);
  if (csv_.is_open())
    csv_.close();
  fprintf(stderr, "[FrameDelaySink] Removed\n");
}

void GtkMainWnd::FrameDelaySink::OnFrame(
    const webrtc::VideoFrame& frame) {
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();

  const auto& ft = frame.frame_timing();

  // Ported to older baseline: this tree's VideoFrame::FrameTiming does not
  // carry receive_start/receive_finish/decode_start/decode_finish/
  // jitter_buffer_ms/timing_valid. Map to the closest available fields:
  //   receive_start  -> first_packet_arrival_timestamp
  //   receive_finish -> last_packet_arrival_timestamp
  //   decode_start   -> last packet arrival (approximation)
  //   decode_finish  -> last packet arrival + decode_ms (approximation)
  //   jitter_buffer  -> -1 (not tracked on this baseline)
  const int64_t receive_start_ms = ft.first_packet_arrival_timestamp;
  const int64_t receive_finish_ms = ft.last_packet_arrival_timestamp;
  const int64_t decode_start_ms = receive_finish_ms;
  const int64_t decode_finish_ms =
      receive_finish_ms > 0 ? receive_finish_ms + ft.decode_ms : 0;
  const int64_t jitter_buffer_ms = -1;
  const bool timing_valid = receive_start_ms > 0 && receive_finish_ms > 0;

  // rtp_ms: sender capture time in ms (RTP clock = 90kHz)
  int64_t rtp_ms = static_cast<int64_t>(frame.rtp_timestamp()) / 90;

  // Compute inter-frame delay from consecutive receive_finish timestamps
  int64_t inter_frame_delay_ms = 0;
  if (last_receive_finish_ms_ > 0 && receive_finish_ms > 0) {
    inter_frame_delay_ms = receive_finish_ms - last_receive_finish_ms_;
  }
  if (receive_finish_ms > 0) {
    last_receive_finish_ms_ = receive_finish_ms;
  }

  // RTT-based offset calibration (from original WebRTC headless_wnd.cc)
  // Calibrate the clock offset between sender RTP clock and receiver clock.
  // This allows estimating E2E delay for ALL subsequent frames.
  // Sources of RTT (in priority order):
  //   1. network_delay_ms from RTCP (bidirectional video)
  //   2. NETWORK_LATENCY_MS env var (experiment config, one-way latency)
  if (!offset_initialized_ && receive_start_ms > 0) {
    int64_t rtt_ms = ft.network_delay_ms;  // From RTCP (RTT)
    if (rtt_ms <= 0) {
      // Fallback: use env var NETWORK_LATENCY_MS (one-way latency → RTT = 2x)
      const char* lat_env = std::getenv("NETWORK_LATENCY_MS");
      if (lat_env && std::strlen(lat_env) > 0) {
        rtt_ms = std::atol(lat_env) * 2;
      }
    }
    if (rtt_ms > 0) {
      // rtp_time_offset_ maps rtp_ms to receiver clock:
      //   estimated_departure = rtp_ms + rtp_time_offset_ + encode_ms
      //   estimated_network_ms = last_packet_arrival - estimated_departure
      rtp_time_offset_ = receive_start_ms -
                         (rtt_ms / 2 - 5) -
                         (rtp_ms + ft.encode_ms);
      offset_initialized_ = true;
      fprintf(stderr,
              "[FrameDelaySink] Offset calibrated: rtp_time_offset=%lld "
              "rtt_ms=%lld (network_delay_ms=%lld)\n",
              (long long)rtp_time_offset_, (long long)rtt_ms,
              (long long)ft.network_delay_ms);
    }
  }

  // Estimate E2E delay using calibrated offset
  int64_t estimated_network_ms = -1;
  int64_t e2e_delay_ms = -1;
  if (offset_initialized_) {
    int64_t estimated_departure = rtp_ms + rtp_time_offset_ + ft.encode_ms;
    estimated_network_ms = receive_finish_ms - estimated_departure;
    // E2E = encode + pacing + network + jitter_buffer + decode
    e2e_delay_ms = decode_finish_ms - (rtp_ms + rtp_time_offset_);
  }

  // Lazy-open CSV file
  if (!csv_.is_open()) {
    const char* dir = std::getenv("FRAME_DELAY_CSV_DIR");
    if (!dir)
      dir = std::getenv("UNIFIED_CSV_DIR");
    if (dir && std::strlen(dir) > 0) {
      std::string path = std::string(dir) + "/frame_delay.csv";
      csv_.open(path, std::ios::out | std::ios::trunc);
      if (csv_.is_open()) {
        csv_ << "wall_ms,rtp_timestamp,width,height,"
                "receive_start_ms,receive_finish_ms,"
                "decode_start_ms,decode_finish_ms,"
                "frame_construction_ms,jitter_buffer_ms,decode_ms,"
                "inter_frame_delay_ms,"
                "encode_ms,pacing_ms,"
                "network_delay_ms,estimated_network_ms,e2e_delay_ms,"
                "is_keyframe,timing_valid\n";
        fprintf(stderr, "[FrameDelaySink] CSV opened: %s\n", path.c_str());
      }
    }
  }

  if (csv_.is_open()) {
    csv_ << now_ms << ","
         << frame.rtp_timestamp() << ","
         << frame.width() << "," << frame.height() << ","
         << receive_start_ms << "," << receive_finish_ms << ","
         << decode_start_ms << "," << decode_finish_ms << ","
         << ft.frame_construction_delay_ms << ","
         << jitter_buffer_ms << "," << ft.decode_ms << ","
         << inter_frame_delay_ms << ","
         << ft.encode_ms << "," << ft.pacing_ms << ","
         << ft.network_delay_ms << "," << estimated_network_ms << ","
         << e2e_delay_ms << ","
         << (ft.is_keyframe ? 1 : 0) << ","
         << (timing_valid ? 1 : 0) << "\n";
    if (++flush_counter_ >= 30) {
      csv_.flush();
      flush_counter_ = 0;
    }
  }
}
