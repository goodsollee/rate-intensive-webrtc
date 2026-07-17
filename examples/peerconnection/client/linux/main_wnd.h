/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef EXAMPLES_PEERCONNECTION_CLIENT_LINUX_MAIN_WND_H_
#define EXAMPLES_PEERCONNECTION_CLIENT_LINUX_MAIN_WND_H_

#include <glib.h>     // Add this for gboolean and gpointer
#include <gtk/gtk.h>  // Add this for GTK types
#include <stdint.h>

#include <fstream>
#include <memory>
#include <string>

#include "api/array_view.h"
#include "api/media_stream_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "examples/peerconnection/client/main_wnd.h"
#include "examples/peerconnection/client/peer_connection_client.h"
#include "rtc_base/buffer.h"

// Forward declarations.
typedef struct _GtkWidget GtkWidget;
typedef union _GdkEvent GdkEvent;
typedef struct _GdkEventKey GdkEventKey;
typedef struct _GtkTreeView GtkTreeView;
typedef struct _GtkTreePath GtkTreePath;
typedef struct _GtkTreeViewColumn GtkTreeViewColumn;
typedef struct _cairo cairo_t;

typedef struct _GdkEventConfigure GdkEventConfigure;

// Implements the main UI of the peer connection client.
// This is functionally equivalent to the MainWnd class in the Windows
// implementation.
class GtkMainWnd : public MainWindow {
 public:
  // juheon added
  GtkMainWnd(const char* server,
             int port,
             bool autoconnect,
             bool autocall,
             bool headless);
  ~GtkMainWnd();

  virtual void RegisterObserver(MainWndCallback* callback);
  virtual bool IsWindow();
  virtual void SwitchToConnectUI();
  virtual void SwitchToPeerList(const Peers& peers);
  virtual void SwitchToStreamingUI();
  virtual void MessageBox(const char* caption, const char* text, bool is_error);
  virtual MainWindow::UI current_ui();
  virtual void StartLocalRenderer(webrtc::VideoTrackInterface* local_video);
  virtual void StopLocalRenderer();
  virtual void StartRemoteRenderer(webrtc::VideoTrackInterface* remote_video);
  virtual void StopRemoteRenderer();

  virtual void QueueUIThreadCallback(int msg_id, void* data);

  // Creates and shows the main window with the |Connect UI| enabled.
  bool Create();

  // Destroys the window.  When the window is destroyed, it ends the
  // main message loop.
  bool Destroy();

  // Callback for when the main window is destroyed.
  void OnDestroyed(GtkWidget* widget, GdkEvent* event);

  // Callback for when the user clicks the "Connect" button.
  void OnClicked(GtkWidget* widget);

  // Callback for the Bulk traffic button.
  void OnBulkClicked(GtkWidget* widget);

  // Callback for keystrokes.  Used to capture Esc and Return.
  void OnKeyPress(GtkWidget* widget, GdkEventKey* key);

  // Callback when the user double clicks a peer in order to initiate a
  // connection.
  void OnRowActivated(GtkTreeView* tree_view,
                      GtkTreePath* path,
                      GtkTreeViewColumn* column);

  void OnRedraw();

  void Draw(GtkWidget* widget, cairo_t* cr);

  // Display size reconfiguration
  int desired_width_;     // Desired window width
  int desired_height_;    // Desired window height
  double scale_;          // Current scale factor
  bool window_resizing_;  // Flag to prevent recursive resize

  // Add with other static callbacks at the top of the class
  static gboolean OnConfigureCallback(GtkWidget* widget,
                                      GdkEventConfigure* event,
                                      gpointer data);
  void OnConfigure(GtkWidget* widget, GdkEventConfigure* event);
  void ResizeWindow(int width, int height);
  std::string GetLogFolder() const;

 protected:
  class VideoRenderer : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
   public:
    VideoRenderer(GtkMainWnd* main_wnd,
                  webrtc::VideoTrackInterface* track_to_render);
    virtual ~VideoRenderer();

    // VideoSinkInterface implementation
    void OnFrame(const webrtc::VideoFrame& frame) override;

    rtc::ArrayView<const uint8_t> image() const { return image_; }
    int width() const { return width_; }
    int height() const { return height_; }
    void SetHeadless(bool headless) { headless_ = headless; }
    float fps() const { return current_fps_; }
    float bitrate() const { return current_bitrate_; }

    void InitializeLogging(const std::string& log_folder);
    void LogFrameMetrics(const webrtc::VideoFrame& frame);

   protected:
    void SetSize(int width, int height);

    // Basic rendering members
    rtc::Buffer image_;
    int width_;
    int height_;
    GtkMainWnd* main_wnd_;
    rtc::scoped_refptr<webrtc::VideoTrackInterface> rendered_track_;
    bool headless_ = false;
    int frame_id_ = 0;

    // Frame timing and statistics
    int64_t last_frame_time_ = 0;
    int frame_count_ = 0;
    float current_fps_ = 0.0f;
    size_t total_bytes_ = 0;
    float current_bitrate_ = 0.0f;
    int64_t start_time_ = 0;

    // New members for jitter calculation
    int64_t last_departure_ts_ = 0;
    int64_t last_arrival_ts_ = 0;
    bool first_frame_ = true;

    // Logging related members
    std::string log_folder_;
    std::ofstream frame_log_file_;
    bool logging_initialized_ = false;
    bool offset_initialized_ = false;
    int64_t rtp_time_offset_ = 0;
    int64_t last_rtp_ms_ = 0;
  };

 protected:
  GtkWidget* window_;     // Our main window.
  GtkWidget* overlay_;    // Container for video and controls.
  GtkWidget* draw_area_;  // The drawing surface for rendering video streams.
  GtkWidget* vbox_;       // Container for the Connect UI.
  GtkWidget* server_edit_;
  GtkWidget* port_edit_;
  GtkWidget* peer_list_;    // The list of peers.
  GtkWidget* bulk_button_;  // Start/stop bulk SCTP traffic.
  MainWndCallback* callback_;
  std::string server_;
  std::string port_;
  bool autoconnect_;
  bool autocall_;
  std::unique_ptr<VideoRenderer> local_renderer_;
  std::unique_ptr<VideoRenderer> remote_renderer_;
  int width_ = 0;
  int height_ = 0;
  rtc::Buffer draw_buffer_;
  int draw_buffer_size_;
  bool bulk_started_ = false;  // Tracks bulk SCTP state.

  // juheon added
  bool headless_ = false;
};

#endif  // EXAMPLES_PEERCONNECTION_CLIENT_LINUX_MAIN_WND_H_
