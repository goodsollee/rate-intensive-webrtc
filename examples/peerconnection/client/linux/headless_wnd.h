#ifndef EXAMPLES_PEERCONNECTION_CLIENT_HEADLESS_WND_H_
#define EXAMPLES_PEERCONNECTION_CLIENT_HEADLESS_WND_H_

#include <stdint.h>
#include <memory>
#include <string>

#include "api/media_stream_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "examples/peerconnection/client/main_wnd.h"
#include "examples/peerconnection/client/peer_connection_client.h"
#include "rtc_base/buffer.h"

class HeadlessWnd : public MainWindow {
 public:
  HeadlessWnd(const char* server, int port, bool autoconnect, bool autocall);
  ~HeadlessWnd() override;

  virtual void RegisterObserver(MainWndCallback* callback) override;
  virtual bool IsWindow() override;
  virtual void SwitchToConnectUI() override;
  virtual void SwitchToPeerList(const Peers& peers) override;
  virtual void SwitchToStreamingUI() override;
  virtual void MessageBox(const char* caption, const char* text, bool is_error) override;
  virtual MainWindow::UI current_ui() override;
  virtual void StartLocalRenderer(webrtc::VideoTrackInterface* local_video) override;
  virtual void StopLocalRenderer() override;
  virtual void StartRemoteRenderer(webrtc::VideoTrackInterface* remote_video) override;
  virtual void StopRemoteRenderer() override;
  virtual void QueueUIThreadCallback(int msg_id, void* data) override;

  bool Create();
  bool Destroy();

 protected:
  class VideoRenderer : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
   public:
    VideoRenderer(HeadlessWnd* main_wnd, webrtc::VideoTrackInterface* track_to_render);
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
    HeadlessWnd* main_wnd_;
    rtc::scoped_refptr<webrtc::VideoTrackInterface> rendered_track_;
  };

 protected:
  MainWndCallback* callback_;
  std::string server_;
  std::string port_;
  bool autoconnect_;
  bool autocall_;
  std::unique_ptr<VideoRenderer> local_renderer_;
  std::unique_ptr<VideoRenderer> remote_renderer_;
  int width_;
  int height_;
  rtc::Buffer frame_buffer_;
  UI current_ui_;
  bool window_created_;
  rtc::Thread* ui_thread_;
};

#endif  // EXAMPLES_PEERCONNECTION_CLIENT_HEADLESS_WND_H_