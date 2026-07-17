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
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <iostream>
#include <map>

#include "api/media_stream_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_frame_type.h"
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

static const char* ToCString(webrtc::VideoFrameType t) {
  switch (t) {
    case webrtc::VideoFrameType::kEmptyFrame:
      return "empty";
    case webrtc::VideoFrameType::kVideoFrameKey:
      return "key";
    case webrtc::VideoFrameType::kVideoFrameDelta:
      return "delta";
    default:
      return "unknown";
  }
}

gboolean OnDestroyedCallback(GtkWidget* widget,
                             GdkEvent* event,
                             gpointer data) {
  reinterpret_cast<GtkMainWnd*>(data)->OnDestroyed(widget, event);
  return FALSE;
}

void OnClickedCallback(GtkWidget* widget, gpointer data) {
  reinterpret_cast<GtkMainWnd*>(data)->OnClicked(widget);
}

void OnBulkClickedCallback(GtkWidget* widget, gpointer data) {
  reinterpret_cast<GtkMainWnd*>(data)->OnBulkClicked(widget);
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
  int rows = gtk_tree_model_iter_n_children(model, NULL);
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
                       bool headless)
    : window_(NULL),
      overlay_(NULL),
      draw_area_(NULL),
      vbox_(NULL),
      server_edit_(NULL),
      port_edit_(NULL),
      peer_list_(NULL),
      bulk_button_(NULL),
      callback_(NULL),
      server_(server),
      autoconnect_(autoconnect),
      autocall_(autocall),
      headless_(headless) {
  char buffer[10];
  snprintf(buffer, sizeof(buffer), "%i", port);
  port_ = buffer;
}

GtkMainWnd::~GtkMainWnd() {
  RTC_DCHECK(!IsWindow());
}

void GtkMainWnd::RegisterObserver(MainWndCallback* callback) {
  callback_ = callback;
  // juheon added: in headless mode, call SwitchToConnectUI() once more for
  // autoconnection
  if (headless_) {
    SwitchToConnectUI();
  }
}

bool GtkMainWnd::IsWindow() {
  // juheon added: in headless mode, always return true
  if (headless_)
    return true;
  else
    return window_ != NULL && GTK_IS_WINDOW(window_);
}

void GtkMainWnd::MessageBox(const char* caption,
                            const char* text,
                            bool is_error) {
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
  local_renderer_.reset(new VideoRenderer(this, local_video));
  // juheon added: set renderer in headless mode
  local_renderer_->SetHeadless(headless_);
}

void GtkMainWnd::StopLocalRenderer() {
  local_renderer_.reset();
}

void GtkMainWnd::StartRemoteRenderer(
    webrtc::VideoTrackInterface* remote_video) {
  remote_renderer_.reset(new VideoRenderer(this, remote_video));
  // juheon added: sent renderer in headless mode
  remote_renderer_->SetHeadless(headless_);
}

void GtkMainWnd::StopRemoteRenderer() {
  remote_renderer_.reset();
}

void GtkMainWnd::QueueUIThreadCallback(int msg_id, void* data) {
  g_idle_add(HandleUIThreadCallback,
             new UIThreadCallbackData(callback_, msg_id, data));
}

bool GtkMainWnd::Create() {
  // juheon added: trigger this only when not in headless mode
  if (headless_) {
    RTC_LOG(LS_INFO) << "headless mode, do not create window!\n";
    // SwitchToConnectUI(); // no use doing this here.. don't have callback now
    return true;
  } else {
    RTC_DCHECK(window_ == NULL);

    window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    if (window_) {
      gtk_window_set_position(GTK_WINDOW(window_), GTK_WIN_POS_CENTER);
      gtk_window_set_default_size(GTK_WINDOW(window_), 640, 480);
      gtk_window_set_title(GTK_WINDOW(window_), "PeerConnection client");
      g_signal_connect(G_OBJECT(window_), "delete-event",
                       G_CALLBACK(&OnDestroyedCallback), this);
      g_signal_connect(window_, "key-press-event",
                       G_CALLBACK(OnKeyPressCallback), this);

      SwitchToConnectUI();
    }

    return window_ != NULL;
  }
}

bool GtkMainWnd::Destroy() {
  // juheon added: skip in headless mode
  if (headless_) {
    return true;
  } else {
    if (!IsWindow())
      return false;

    gtk_widget_destroy(window_);
    window_ = NULL;

    return true;
  }
}

void GtkMainWnd::SwitchToConnectUI() {
  if (headless_) {  // juheon added: in headless mode: make connection right
                    // away
    int port = port_.length() ? atoi(port_.c_str()) : 0;
    RTC_LOG(LS_INFO) << "server: " << server_ << "port: " << port << "\n";
    if (callback_) {
      callback_->StartLogin(server_, port);
    } else {
      RTC_LOG(LS_INFO) << "null callback!\n";
    }
  } else {
    RTC_LOG(LS_INFO) << __FUNCTION__;

    RTC_DCHECK(IsWindow());
    RTC_DCHECK(vbox_ == NULL);

    gtk_container_set_border_width(GTK_CONTAINER(window_), 10);

    if (peer_list_) {
      gtk_widget_destroy(peer_list_);
      peer_list_ = NULL;
    } else if (overlay_) {
      gtk_widget_destroy(overlay_);
      overlay_ = NULL;
      draw_area_ = NULL;
      bulk_button_ = NULL;
      draw_buffer_.SetSize(0);
      bulk_started_ = false;
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
}

void GtkMainWnd::SwitchToPeerList(const Peers& peers) {
  if (headless_) {
    if (peers.begin() != peers.end()) {
      if (autocall_) {
        // juheon added: in headless mode, make connection from peers right away
        int id = 2;  // temp: this should be replaced with the output of
                     // gtk_tree_model_get(model, &iter, 0, &text, 1, &id, -1);
        RTC_LOG(LS_INFO) << "(headless) Connect to peer " << id << "\n";
        callback_->ConnectToPeer(id);
      }  // else{
      //  RTC_LOG(LS_INFO) << "not in autocall mode\n";
      //}
    } else {
      RTC_LOG(LS_INFO) << "no peers to connect!\n";
    }
  } else {
    RTC_LOG(LS_INFO) << __FUNCTION__;

    if (!peer_list_) {
      gtk_container_set_border_width(GTK_CONTAINER(window_), 0);
      if (vbox_) {
        gtk_widget_destroy(vbox_);
        vbox_ = NULL;
        server_edit_ = NULL;
        port_edit_ = NULL;
      } else if (overlay_) {
        gtk_widget_destroy(overlay_);
        overlay_ = NULL;
        draw_area_ = NULL;
        bulk_button_ = NULL;
        draw_buffer_.SetSize(0);
        bulk_started_ = false;
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
}

void GtkMainWnd::SwitchToStreamingUI() {
  RTC_LOG(LS_INFO) << "SwitchToStreamingUI: Current UI state=" << current_ui()
                   << ", draw_area_=" << (draw_area_ ? "exists" : "null");
  if (headless_) {
    RTC_LOG(LS_INFO) << "headless mode, skip!";
  } else {
    // First clean up any existing UI elements
    if (vbox_) {
      gtk_container_remove(GTK_CONTAINER(window_), vbox_);
      gtk_widget_destroy(vbox_);
      vbox_ = NULL;
      server_edit_ = NULL;
      port_edit_ = NULL;
    }

    if (overlay_) {
      gtk_widget_destroy(overlay_);
      overlay_ = NULL;
      draw_area_ = NULL;
      bulk_button_ = NULL;
      bulk_started_ = false;
    }

    if (peer_list_) {
      gtk_container_remove(GTK_CONTAINER(window_), peer_list_);
      gtk_widget_destroy(peer_list_);
      peer_list_ = NULL;
    }

    gtk_container_set_border_width(GTK_CONTAINER(window_), 0);

    // Set fixed window size
    desired_width_ = 1280;  // Set your desired fixed width
    desired_height_ = 720;  // Set your desired fixed height
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);  // Prevent resizing

    overlay_ = gtk_overlay_new();
    draw_area_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(draw_area_, desired_width_, desired_height_);
    gtk_container_add(GTK_CONTAINER(overlay_), draw_area_);
    g_signal_connect(G_OBJECT(draw_area_), "draw", G_CALLBACK(&::Draw), this);

    bulk_button_ = gtk_button_new_with_label("Start Bulk");
    gtk_widget_set_halign(bulk_button_, GTK_ALIGN_START);
    gtk_widget_set_valign(bulk_button_, GTK_ALIGN_START);
    g_signal_connect(bulk_button_, "clicked", G_CALLBACK(OnBulkClickedCallback),
                     this);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), bulk_button_);
    bulk_started_ = false;

    gtk_container_add(GTK_CONTAINER(window_), overlay_);

    gtk_widget_show_all(window_);
  }
}

void GtkMainWnd::OnDestroyed(GtkWidget* widget, GdkEvent* event) {
  callback_->Close();
  window_ = NULL;
  overlay_ = NULL;
  draw_area_ = NULL;
  vbox_ = NULL;
  server_edit_ = NULL;
  port_edit_ = NULL;
  peer_list_ = NULL;
  bulk_button_ = NULL;
  bulk_started_ = false;
}

void GtkMainWnd::OnClicked(GtkWidget* widget) {
  // Make the connect button insensitive, so that it cannot be clicked more than
  // once.  Now that the connection includes auto-retry, it should not be
  // necessary to click it more than once.
  gtk_widget_set_sensitive(widget, false);
  server_ = gtk_entry_get_text(GTK_ENTRY(server_edit_));
  port_ = gtk_entry_get_text(GTK_ENTRY(port_edit_));
  int port = port_.length() ? atoi(port_.c_str()) : 0;
  callback_->StartLogin(server_, port);
}

void GtkMainWnd::OnBulkClicked(GtkWidget* widget) {
  if (!bulk_started_) {
    callback_->StartBulkSctp();
    bulk_started_ = true;
    gtk_button_set_label(GTK_BUTTON(widget), "Stop Bulk");
  } else {
    callback_->StopBulkSctp();
    bulk_started_ = false;
    gtk_button_set_label(GTK_BUTTON(widget), "Start Bulk");
  }
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
          OnClicked(NULL);
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
  RTC_DCHECK(peer_list_ != NULL);
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
      draw_area_ != NULL) {
    if (width_ != remote_renderer->width() ||
        height_ != remote_renderer->height()) {
      width_ = remote_renderer->width();
      height_ = remote_renderer->height();
      gtk_widget_set_size_request(draw_area_, remote_renderer->width(),
                                  remote_renderer->height());
    }
    draw_buffer_.SetData(remote_renderer->image());
    gtk_widget_queue_draw(draw_area_);
  }
  // Here we can draw the local preview as well if we want....
  gdk_threads_leave();
}

void GtkMainWnd::Draw(GtkWidget* widget, cairo_t* cr) {
  if (!draw_buffer_.data())
    return;

  // Draw video content first
  double scale_x = static_cast<double>(desired_width_) / width_;
  double scale_y = static_cast<double>(desired_height_) / height_;
  double scale = std::min(scale_x, scale_y);

  double x = (desired_width_ - (width_ * scale)) / 2;
  double y = (desired_height_ - (height_ * scale)) / 2;

  cairo_translate(cr, x, y);
  cairo_scale(cr, scale, scale);

  cairo_format_t format = CAIRO_FORMAT_ARGB32;
  cairo_surface_t* surface = cairo_image_surface_create_for_data(
      draw_buffer_.data(), format, width_, height_,
      cairo_format_stride_for_width(format, width_));

  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_rectangle(cr, 0, 0, width_, height_);
  cairo_fill(cr);

  cairo_surface_destroy(surface);

  // Reset transform for text overlay
  cairo_identity_matrix(cr);

  // Draw stats overlay
  cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 14.0);
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);  // White text

  float fps = remote_renderer_ ? remote_renderer_->fps() : 0.0f;
  float bitrate = remote_renderer_ ? remote_renderer_->bitrate() : 0.0f;

  char stats_text[128];
  snprintf(stats_text, sizeof(stats_text),
           "Resolution: %dx%d  FPS: %.1f  Bitrate: %.3f Mbps", width_, height_,
           fps, bitrate / 1000.0f);

  // Add black background for better readability
  cairo_text_extents_t extents;
  cairo_text_extents(cr, stats_text, &extents);

  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);  // Semi-transparent black
  cairo_rectangle(cr, 8, 8, extents.width + 4, extents.height + 4);
  cairo_fill(cr);

  // Draw text
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_move_to(cr, 10, 20);
  cairo_show_text(cr, stats_text);
}

// In the anonymous namespace where other callbacks are defined
gboolean GtkMainWnd::OnConfigureCallback(GtkWidget* widget,
                                         GdkEventConfigure* event,
                                         gpointer data) {
  reinterpret_cast<GtkMainWnd*>(data)->OnConfigure(widget, event);
  return FALSE;
}

void GtkMainWnd::OnConfigure(GtkWidget* widget, GdkEventConfigure* event) {
  if (!window_resizing_) {
    ResizeWindow(event->width, event->height);
  }
}

void GtkMainWnd::ResizeWindow(int width, int height) {
  if (window_resizing_)
    return;

  window_resizing_ = true;

  // Calculate scale to fit video in window while maintaining aspect ratio
  double video_aspect = static_cast<double>(width_) / height_;
  double window_aspect = static_cast<double>(width) / height;

  if (video_aspect > window_aspect) {
    // Video is wider than window - scale to fit width
    scale_ = static_cast<double>(width) / width_;
    desired_width_ = width;
    desired_height_ = height_ * scale_;
  } else {
    // Video is taller than window - scale to fit height
    scale_ = static_cast<double>(height) / height_;
    desired_height_ = height;
    desired_width_ = width_ * scale_;
  }

  // gtk_window_resize(GTK_WINDOW(window_), desired_width_, desired_height_);
  window_resizing_ = false;
}

std::string GtkMainWnd::GetLogFolder() const {
  return callback_->GetLogFolder();  // callback_ is MainWndCallback which
                                     // Conductor implements
}

GtkMainWnd::VideoRenderer::VideoRenderer(
    GtkMainWnd* main_wnd,
    webrtc::VideoTrackInterface* track_to_render)
    : width_(0),
      height_(0),
      main_wnd_(main_wnd),
      rendered_track_(track_to_render) {
  rendered_track_->AddOrUpdateSink(this, rtc::VideoSinkWants());

  // Get log folder from Conductor through MainWnd
  InitializeLogging(main_wnd_->GetLogFolder());
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

// juheon added: print current time to check frame interval
#include <sys/time.h>

#include <ctime>

void print_current_time() {
  struct timeval time_now{};
  gettimeofday(&time_now, nullptr);

  long int sec = static_cast<long int>(time_now.tv_sec) % 86400;
  long int usec = static_cast<long int>(time_now.tv_usec);

  long int hour = (sec / 3600 + 9) % 24;
  long int min = (sec % 3600) / 60;
  long int s = (sec % 3600) % 60;
  long int ms = usec / 1000;

  // std::cout<<hour<<":"<<min<<":"<<s<<"."<<ms<<", ";
  printf("%ld:%ld:%ld.%ld, ", hour, min, s, ms);
}

void GtkMainWnd::VideoRenderer::OnFrame(const webrtc::VideoFrame& video_frame) {
  gdk_threads_enter();

  int64_t current_time = rtc::TimeMillis();

  // Initialize start time with first frame
  if (start_time_ == 0) {
    start_time_ = current_time;
  }

  // FPS Calculation
  if (last_frame_time_ == 0) {
    last_frame_time_ = current_time;
  }

  frame_count_++;

  // Calculate bitrate
  size_t frame_size =
      video_frame.frame_timing().encoded_size;  // Get frame size in bytes
  total_bytes_ += frame_size;

  // Update FPS and bitrate every second
  if (current_time - last_frame_time_ >= 1000) {
    // FPS calculation
    current_fps_ = frame_count_ * 1000.0f / (current_time - last_frame_time_);

    // Bitrate calculation (bits per second)
    current_bitrate_ = (total_bytes_ * 8.0f / 1024) *
                       (1000.0f / (current_time - last_frame_time_));

    // Reset counters
    frame_count_ = 0;
    total_bytes_ = 0;
    last_frame_time_ = current_time;

    // Calculate elapsed time in seconds since start
    double elapsed_seconds = (current_time - start_time_) / 1000.0;
    std::cout << "Elapsed time: " << elapsed_seconds
              << "s, Frame rate: " << current_fps_
              << ", Bitrate: " << current_bitrate_ << std::endl;
  }

  // Log frame metrics
  LogFrameMetrics(video_frame);

  if (!headless_) {
    rtc::scoped_refptr<webrtc::I420BufferInterface> buffer(
        video_frame.video_frame_buffer()->ToI420());
    if (video_frame.rotation() != webrtc::kVideoRotation_0) {
      buffer = webrtc::I420Buffer::Rotate(*buffer, video_frame.rotation());
    }

    // Keep original video dimensions
    SetSize(buffer->width(), buffer->height());

    libyuv::I420ToARGB(buffer->DataY(), buffer->StrideY(), buffer->DataU(),
                       buffer->StrideU(), buffer->DataV(), buffer->StrideV(),
                       image_.data(), width_ * 4, buffer->width(),
                       buffer->height());

    gdk_threads_leave();

    // This will trigger a redraw with the current scale
    g_idle_add(Redraw, main_wnd_);
  }
}

void GtkMainWnd::VideoRenderer::InitializeLogging(
    const std::string& log_folder) {
  if (logging_initialized_)
    return;

  log_folder_ = log_folder;
  std::string log_path = log_folder_ + "/frame_metrics.csv";

  frame_log_file_.open(log_path, std::ios::out);
  if (frame_log_file_.is_open()) {
    // Write CSV header
    frame_log_file_
        << "timestamp,rtp_timestamp,first_packet_departure,estimated_first_"
           "packet_departure,first_packet_arrival,last_packet_arrival,render,"
        << "encode_ms,pacing_ms,network_ms,estimated_network_ms,decode_ms,"
        << "frame_construction_delay_ms,inter_frame_delay_ms,"
        << "inter_frame_departure_ms,frame_jitter_ms,"  // New columns
        << "encoded_size,height,width,min_rtt,avail_bw,"
        << "is_keyframe,frame_type\n";
    logging_initialized_ = true;
  }
}

void GtkMainWnd::VideoRenderer::LogFrameMetrics(
    const webrtc::VideoFrame& frame) {
  if (!logging_initialized_ || !frame_log_file_.is_open())
    return;

  const webrtc::VideoFrame::FrameTiming& timing = frame.frame_timing();
  int64_t current_time = rtc::TimeMillis();

  // Calculate RTP timestamp in milliseconds (90kHz clock -> ms)
  int rtp_ms = (frame.rtp_timestamp() / 90);

  // Calculate inter-frame departure time
  int64_t inter_frame_departure_ms = 0;
  if (!first_frame_ && last_departure_ts_ > 0) {
    inter_frame_departure_ms =
        timing.first_packet_departure_timestamp - last_departure_ts_;
  }
  last_departure_ts_ = timing.first_packet_departure_timestamp;

  // Calculate frame-level jitter (difference between inter-arrival and
  // inter-departure times)
  int64_t frame_jitter_ms = 0;
  if (!first_frame_ && last_arrival_ts_ > 0) {
    int64_t inter_frame_arrival_ms =
        timing.last_packet_arrival_timestamp - last_arrival_ts_;
    frame_jitter_ms = inter_frame_arrival_ms - inter_frame_departure_ms;
  }
  last_arrival_ts_ = timing.last_packet_arrival_timestamp;

  if (first_frame_) {
    first_frame_ = false;
  }

  // Initialize offset using first frame's data
  if (!offset_initialized_ && timing.network_delay_ms > 0) {
    // Calculate offset using RTP timestamp and actual departure time
    // Note: We don't include encode_ms in offset calculation as requested
    rtp_time_offset_ = timing.first_packet_arrival_timestamp -
                       (timing.network_delay_ms - 5) -
                       (rtp_ms + timing.encode_ms);
    offset_initialized_ = true;
  }

  if (offset_initialized_) {
    // Calculate estimated departure and network delay
    int estimated_departure =
        rtp_ms + rtp_time_offset_ +
        timing.encode_ms;  // rtp_ms + offset --> capture time
    int estimated_network_ms =
        timing.last_packet_arrival_timestamp - estimated_departure;

    // Before the frame_log_file_ << line, add:
    double avail_bw = 0.0;
    if (timing.frame_construction_delay_ms + 0.5 > 0) {
      // Convert bytes to bits (* 8)
      // Convert ms to seconds (/ 1000)
      // Convert to Mbps (/ 1000000)
      // Final formula: (bytes * 8) / (ms / 1000) / 1000000
      // Simplified: (bytes * 8 * 1000) / (ms * 1000000)
      avail_bw =
          (static_cast<double>(timing.encoded_size) * 8.0 * 1000.0) /
          ((static_cast<double>(timing.frame_construction_delay_ms) + 0.5) *
           1000000.0);
    }

    const bool is_keyframe = timing.is_keyframe;
    const webrtc::VideoFrameType ftype = timing.frame_type;

    frame_log_file_ << current_time << "," << frame.rtp_timestamp() << ","
                    << timing.first_packet_departure_timestamp << ","
                    << estimated_departure << ","
                    << timing.first_packet_arrival_timestamp << ","
                    << timing.last_packet_arrival_timestamp << ","
                    << timing.render_ms << "," << timing.encode_ms << ","
                    << timing.pacing_ms << "," << timing.network_ms << ","
                    << estimated_network_ms << "," << timing.decode_ms << ","
                    << timing.frame_construction_delay_ms << ","
                    << timing.inter_frame_delay_ms << ","
                    << inter_frame_departure_ms << ","  // New column
                    << frame_jitter_ms << ","           // New column
                    << timing.encoded_size << "," << frame.width() << ","
                    << frame.height() << "," << timing.network_delay_ms << ","
                    << avail_bw << "," << (is_keyframe ? 1 : 0) << ","
                    << ToCString(ftype) << "\n";
  }

  // Flush to ensure data is written immediately
  frame_log_file_.flush();
}