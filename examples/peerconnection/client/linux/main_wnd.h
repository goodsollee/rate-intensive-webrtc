/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

// Ported header for the transplanted research linux/main_wnd.cc: declares the
// newer GtkMainWnd (demo chat UI, performance graph, FrameDelaySink) that the
// research .cc implements. Baseline-only members whose definitions no longer
// exist in the .cc (OnBulkClicked, OnConfigure/ResizeWindow, GetLogFolder,
// VideoRenderer logging helpers) were removed to avoid latent link errors.

#ifndef EXAMPLES_PEERCONNECTION_CLIENT_LINUX_MAIN_WND_H_
#define EXAMPLES_PEERCONNECTION_CLIENT_LINUX_MAIN_WND_H_

#include <glib.h>     // gboolean and gpointer
#include <gtk/gtk.h>  // GTK types
#include <stdint.h>

#include <atomic>
#include <deque>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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

// Implements the main UI of the peer connection client.
// This is functionally equivalent to the MainWnd class in the Windows
// implementation.
class GtkMainWnd : public MainWindow {
 public:
  GtkMainWnd(const char* server,
             int port,
             bool autoconnect,
             bool autocall,
             bool headless,
             bool demo_mode);
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

  // Research UI hooks (MainWindow overrides).
  void AppendChatMessage(const std::string& role,
                         const std::string& text) override;
  void OnQueryStarted() override;
  void SetQueryPhase(const std::string& phase) override;
  void LoadContextDocuments(const std::vector<std::string>& texts) override;
  void OnSctpDataReceived(size_t bytes) override;
  void UpdateThroughput(float video_mbps, float sctp_mbps) override;
  uint64_t GetSctpBytesReceived() const override {
    return sctp_bytes_received_.load(std::memory_order_relaxed);
  }

  // GTK-thread implementations of the hooks above.
  void AppendChatMessageImpl(const std::string& role, const std::string& text);
  void LoadContextDocumentsImpl(const std::vector<std::string>& texts);
  void SetQueryPhaseImpl(const std::string& phase);

  // Periodic (1 s) sampler feeding the performance graph.
  void SampleThroughput();

  // Reserved for query-specific behavior.
  void SetCurrentQueryPath(const std::string& path);

  // Creates and shows the main window with the |Connect UI| enabled.
  bool Create();

  // Destroys the window.  When the window is destroyed, it ends the
  // main message loop.
  bool Destroy();

  // Callback for when the main window is destroyed.
  void OnDestroyed(GtkWidget* widget, GdkEvent* event);

  // Callback for when the user clicks the "Connect" button.
  void OnClicked(GtkWidget* widget);

  // Callback for the demo chat "Send" button / query entry activation.
  void OnSendClicked(GtkWidget* widget);

  // Callback for keystrokes.  Used to capture Esc and Return.
  void OnKeyPress(GtkWidget* widget, GdkEventKey* key);

  // Callback when the user double clicks a peer in order to initiate a
  // connection.
  void OnRowActivated(GtkTreeView* tree_view,
                      GtkTreePath* path,
                      GtkTreeViewColumn* column);

  void OnRedraw();

  void Draw(GtkWidget* widget, cairo_t* cr);
  void DrawPerformanceGraph(cairo_t* cr, int width, int height);

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

   protected:
    void SetSize(int width, int height);

    rtc::Buffer image_;
    int width_;
    int height_;
    GtkMainWnd* main_wnd_;
    rtc::scoped_refptr<webrtc::VideoTrackInterface> rendered_track_;
  };

  // Lightweight sink for per-frame delay measurement in headless mode.
  // Writes frame_delay.csv using the research FrameTiming metadata carried by
  // webrtc::VideoFrame.
  class FrameDelaySink : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
   public:
    explicit FrameDelaySink(webrtc::VideoTrackInterface* track);
    ~FrameDelaySink() override;

    void OnFrame(const webrtc::VideoFrame& frame) override;

   private:
    rtc::scoped_refptr<webrtc::VideoTrackInterface> track_;
    std::ofstream csv_;
    int flush_counter_ = 0;
    int64_t last_receive_finish_ms_ = 0;
    bool offset_initialized_ = false;
    int64_t rtp_time_offset_ = 0;
  };

 protected:
  // Performance-graph geometry / scale constants.
  static constexpr int kGraphHeight = 200;
  static constexpr float kVideoMaxMbps = 20.0f;
  static constexpr float kSctpMaxMbps = 200.0f;

  GtkWidget* window_;     // Our main window.
  GtkWidget* draw_area_;  // The drawing surface for rendering video streams.
  GtkWidget* vbox_;       // Container for the Connect UI.
  GtkWidget* server_edit_;
  GtkWidget* port_edit_;
  GtkWidget* peer_list_;  // The list of peers.
  MainWndCallback* callback_;
  std::string server_;
  std::string port_;
  bool autoconnect_;
  bool autocall_;
  std::unique_ptr<VideoRenderer> local_renderer_;
  std::unique_ptr<VideoRenderer> remote_renderer_;
  std::unique_ptr<FrameDelaySink> frame_delay_sink_;
  int width_ = 0;
  int height_ = 0;
  rtc::Buffer draw_buffer_;
  int draw_buffer_size_;

  bool headless_ = false;
  bool demo_mode_ = false;

  // Demo chat / docs UI widgets (demo_mode_ only).
  GtkWidget* main_hbox_ = nullptr;
  GtkWidget* chat_box_ = nullptr;
  GtkWidget* response_view_ = nullptr;
  GtkWidget* query_entry_ = nullptr;
  GtkWidget* send_button_ = nullptr;
  GtkWidget* docs_box_ = nullptr;
  GtkWidget* docs_scroll_ = nullptr;
  GtkWidget* docs_view_ = nullptr;
  GtkWidget* elapsed_label_ = nullptr;
  GtkWidget* phase_label_ = nullptr;
  guint throughput_sample_timer_id_ = 0;
  guint elapsed_timer_id_ = 0;

  // Query timing (ms since monotonic epoch; 0 = no query running).
  std::atomic<int64_t> query_start_ms_{0};

  // Throughput bookkeeping for the performance graph.
  std::atomic<size_t> sctp_bytes_received_{0};
  size_t last_sctp_bytes_ = 0;
  std::atomic<float> stats_video_mbps_{0.0f};
  std::atomic<float> stats_sctp_mbps_{0.0f};
  std::deque<float> video_throughput_history_;
  std::deque<float> sctp_throughput_history_;
};

#endif  // EXAMPLES_PEERCONNECTION_CLIENT_LINUX_MAIN_WND_H_
