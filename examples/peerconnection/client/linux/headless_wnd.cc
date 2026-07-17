#include "examples/peerconnection/client/linux/headless_wnd.h"

#include <stddef.h>
#include <stdio.h>
#include <memory>
#include "api/video/i420_buffer.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/convert_from.h"
#include "rtc_base/thread.h"

/*
namespace {
void QueueThreadCallback(MainWndCallback* callback, int msg_id, void* data, rtc::Thread* ui_thread) {
  ui_thread->PostTask([callback, msg_id, data]() {
    callback->UIThreadCallback(msg_id, data);
  });
}
}  // namespace
*/
HeadlessWnd::HeadlessWnd(const char* server, 
                         int port,
                         bool autoconnect, 
                         bool autocall)
    : callback_(NULL),
      server_(server),
      autoconnect_(autoconnect),
      autocall_(autocall),
      width_(0),
      height_(0),
      current_ui_(CONNECT_TO_SERVER),
      window_created_(false),
      ui_thread_(rtc::Thread::Current()) {
  char buffer[10];
  snprintf(buffer, sizeof(buffer), "%i", port);
  port_ = buffer;
}

HeadlessWnd::~HeadlessWnd() {
  RTC_DCHECK(!IsWindow());
}

void HeadlessWnd::RegisterObserver(MainWndCallback* callback) {
  RTC_LOG(LS_INFO) << "Registering observer";
  callback_ = callback;
  callback_->StartLogin(server_, 0);
}

bool HeadlessWnd::IsWindow() {
  return true;
}

void HeadlessWnd::MessageBox(const char* caption, const char* text, bool is_error) {
  RTC_LOG(LS_INFO) << "MessageBox: " << caption << " - " << text 
                   << (is_error ? " (Error)" : "");
}

MainWindow::UI HeadlessWnd::current_ui() {
  return current_ui_;
}

void HeadlessWnd::StartLocalRenderer(webrtc::VideoTrackInterface* local_video) {
  local_renderer_.reset(new VideoRenderer(this, local_video));
  RTC_LOG(LS_INFO) << "Local renderer started";
}

void HeadlessWnd::StopLocalRenderer() {
  local_renderer_.reset();
  RTC_LOG(LS_INFO) << "Local renderer stopped";
}

void HeadlessWnd::StartRemoteRenderer(webrtc::VideoTrackInterface* remote_video) {
  remote_renderer_.reset(new VideoRenderer(this, remote_video));
  RTC_LOG(LS_INFO) << "Remote renderer started";
}

void HeadlessWnd::StopRemoteRenderer() {
  remote_renderer_.reset();
  RTC_LOG(LS_INFO) << "Remote renderer stopped";
}

void HeadlessWnd::QueueUIThreadCallback(int msg_id, void* data) {
  if (callback_) {
    rtc::Thread* thread = rtc::Thread::Current();
    if (thread) {
      thread->PostTask([this, msg_id, data]() {
        callback_->UIThreadCallback(msg_id, data);
      });
    } else {
      RTC_LOG(LS_ERROR) << "No current thread available for UI callback";
    }
  }
}

bool HeadlessWnd::Create() {
  RTC_DCHECK(!window_created_);
  window_created_ = true;

  return true;
}

bool HeadlessWnd::Destroy() {
  if (!IsWindow())
    return false;

  window_created_ = false;
  return true;
}

void HeadlessWnd::SwitchToConnectUI() {
  current_ui_ = CONNECT_TO_SERVER;
  RTC_LOG(LS_INFO) << "Switched to Connect UI";
}

void HeadlessWnd::SwitchToPeerList(const Peers& peers) {
  current_ui_ = LIST_PEERS;
  RTC_LOG(LS_INFO) << "Switched to Peer List UI";

  RTC_LOG(LS_INFO) << "Connected peers:";
  for (const auto& peer : peers) {
    RTC_LOG(LS_INFO) << " - " << peer.second << " (id: " << peer.first << ")";
  }

  if (autocall_ && !peers.empty()) {
    callback_->ConnectToPeer(peers.begin()->first);
  }
}

void HeadlessWnd::SwitchToStreamingUI() {
  current_ui_ = STREAMING;
  RTC_LOG(LS_INFO) << "Switched to Streaming UI";
}

// VideoRenderer implementation
HeadlessWnd::VideoRenderer::VideoRenderer(
    HeadlessWnd* main_wnd,
    webrtc::VideoTrackInterface* track_to_render)
    : width_(0),
      height_(0),
      main_wnd_(main_wnd),
      rendered_track_(track_to_render) {
  rendered_track_->AddOrUpdateSink(this, rtc::VideoSinkWants());
}

HeadlessWnd::VideoRenderer::~VideoRenderer() {
  rendered_track_->RemoveSink(this);
}

void HeadlessWnd::VideoRenderer::SetSize(int width, int height) {
  if (width_ == width && height_ == height) {
    return;
  }
  width_ = width;
  height_ = height;
  image_.SetSize(width * height * 4);  // ARGB
}

void HeadlessWnd::VideoRenderer::OnFrame(const webrtc::VideoFrame& video_frame) {
  rtc::scoped_refptr<webrtc::I420BufferInterface> buffer(
      video_frame.video_frame_buffer()->ToI420());
  if (video_frame.rotation() != webrtc::kVideoRotation_0) {
    buffer = webrtc::I420Buffer::Rotate(*buffer, video_frame.rotation());
  }
  SetSize(buffer->width(), buffer->height());

  RTC_LOG(LS_VERBOSE) << "Received video frame: " << width_ << "x" << height_;

  // Store frame data but don't display it since we're headless
  libyuv::I420ToARGB(buffer->DataY(), buffer->StrideY(), 
                     buffer->DataU(), buffer->StrideU(),
                     buffer->DataV(), buffer->StrideV(),
                     image_.data(), width_ * 4, 
                     buffer->width(), buffer->height());
}