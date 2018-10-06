#include "aur.hh"

#include <fcntl.h>
#include <string.h>

#include <unistd.h>
#include <chrono>
#include <string_view>

namespace aur {

namespace {

template <typename TimeRes, typename ClockType>
auto TimepointTo(std::chrono::time_point<ClockType> tp) {
  return std::chrono::time_point_cast<TimeRes>(tp).time_since_epoch().count();
}

struct ci_char_traits : public std::char_traits<char> {
  static bool eq(char c1, char c2) { return toupper(c1) == toupper(c2); }
  static bool ne(char c1, char c2) { return toupper(c1) != toupper(c2); }
  static bool lt(char c1, char c2) { return toupper(c1) < toupper(c2); }
  static int compare(const char* s1, const char* s2, size_t n) {
    while (n-- != 0) {
      if (toupper(*s1) < toupper(*s2)) {
        return -1;
      }
      if (toupper(*s1) > toupper(*s2)) {
        return 1;
      }
      ++s1;
      ++s2;
    }
    return 0;
  }
  static const char* find(const char* s, int n, char a) {
    while (n-- > 0 && toupper(*s) != toupper(a)) {
      ++s;
    }
    return s;
  }
};

using ci_string_view = std::basic_string_view<char, ci_char_traits>;

bool ConsumePrefix(ci_string_view* view, ci_string_view prefix) {
  if (view->find(prefix) != 0) {
    return false;
  }

  view->remove_prefix(prefix.size());
  return true;
}

class ResponseHandler {
 public:
  virtual ~ResponseHandler() = default;

  static size_t BodyCallback(char* ptr, size_t size, size_t nmemb,
                             void* userdata) {
    auto* handler = static_cast<ResponseHandler*>(userdata);

    handler->body.append(ptr, size * nmemb);

    return size * nmemb;
  }

  static size_t HeaderCallback(char* buffer, size_t size, size_t nitems,
                               void* userdata) {
    auto* handler = static_cast<ResponseHandler*>(userdata);

    // Remove 2 bytes to ignore trailing \r\n
    ci_string_view header(buffer, size * nitems - 2);

    if (ConsumePrefix(&header, "Content-Disposition:")) {
      handler->FilenameFromHeader(header);
    }

    return size * nitems;
  }

  int RunCallback(const std::string& error) const {
    int r = Run(error);
    delete this;
    return r;
  }

  void set_filename_hint(const std::string& filename_hint) {
    this->filename_hint = filename_hint;
  }

  std::string body;
  std::string filename_hint;
  char error_buffer[CURL_ERROR_SIZE]{};

 private:
  virtual int Run(const std::string& error) const = 0;

  void FilenameFromHeader(ci_string_view value) {
    constexpr char kFilenameKey[] = "filename=";

    const auto pos = value.find(kFilenameKey);
    if (pos == std::string_view::npos) {
      return;
    }

    value.remove_prefix(pos + strlen(kFilenameKey));
    if (value.size() < 3) {
      // Malformed header or empty filename
      return;
    }

    // Blind assumptions:
    // 1) filename is either quoted or the last
    // 2) filename is never a path (contains no slashes)
    ConsumePrefix(&value, "\"");
    filename_hint.assign(value.data(), value.find('"'));
  }
};

class RpcResponseHandler : public ResponseHandler {
 public:
  using CallbackType = Aur::RpcResponseCallback;

  RpcResponseHandler(Aur::RpcResponseCallback callback)
      : callback_(std::move(callback)) {}

 private:
  int Run(const std::string& error) const override {
    if (!error.empty()) {
      return callback_(error);
    }

    auto json = aur::RpcResponse::Parse(body);
    if (!json.error.empty()) {
      return callback_(json.error);
    }

    return callback_(std::move(json));
  }

  const CallbackType callback_;
};

class RawResponseHandler : public ResponseHandler {
 public:
  using CallbackType = Aur::RawResponseCallback;

  RawResponseHandler(Aur::RawResponseCallback callback)
      : callback_(std::move(callback)) {}

 private:
  int Run(const std::string& error) const override {
    if (!error.empty()) {
      return callback_(error);
    }

    return callback_(RawResponse{std::move(filename_hint), std::move(body)});
  }

  const CallbackType callback_;
};

}  // namespace

Aur::Aur(const std::string& baseurl) : baseurl_(baseurl) {
  curl_global_init(CURL_GLOBAL_SSL);
  curl_ = curl_multi_init();

  curl_multi_setopt(curl_, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);

  curl_multi_setopt(curl_, CURLMOPT_SOCKETFUNCTION, &Aur::SocketCallback);
  curl_multi_setopt(curl_, CURLMOPT_SOCKETDATA, this);

  curl_multi_setopt(curl_, CURLMOPT_TIMERFUNCTION, &Aur::TimerCallback);
  curl_multi_setopt(curl_, CURLMOPT_TIMERDATA, this);

  sd_event_default(&event_);
}

Aur::~Aur() {
  curl_multi_cleanup(curl_);
  curl_global_cleanup();

  sd_event_unref(event_);
}

// static
int Aur::SocketCallback(CURLM* curl, curl_socket_t s, int action,
                        void* userdata, void*) {
  Aur* aur = static_cast<Aur*>(userdata);

  auto iter = aur->active_io_.find(s);
  sd_event_source* io = iter != aur->active_io_.end() ? iter->second : nullptr;

  if (action == CURL_POLL_REMOVE) {
    if (io) {
      int fd;

      fd = sd_event_source_get_io_fd(io);

      sd_event_source_set_enabled(io, SD_EVENT_OFF);
      sd_event_source_unref(io);

      aur->active_io_.erase(iter);
      aur->translate_fds_.erase(fd);

      close(fd);
    }
  }

  std::uint32_t events;
  if (action == CURL_POLL_IN) {
    events = EPOLLIN;
  } else if (action == CURL_POLL_OUT) {
    events = EPOLLOUT;
  } else if (action == CURL_POLL_INOUT) {
    events = EPOLLIN | EPOLLOUT;
  }

  if (iter != aur->active_io_.end()) {
    if (sd_event_source_set_io_events(io, events) < 0) {
      return -1;
    }

    if (sd_event_source_set_enabled(io, SD_EVENT_ON) < 0) {
      return -1;
    }
  } else {
    int fd;

    /* When curl needs to remove an fd from us it closes
     * the fd first, and only then calls into us. This is
     * nasty, since we cannot pass the fd on to epoll()
     * anymore. Hence, duplicate the fds here, and keep a
     * copy for epoll which we control after use. */

    fd = fcntl(s, F_DUPFD_CLOEXEC, 3);
    if (fd < 0) {
      return -1;
    }

    if (sd_event_add_io(aur->event_, &io, fd, events, &Aur::OnIO, aur) < 0) {
      return -1;
    }

    sd_event_source_set_description(io, "curl-io");

    aur->active_io_.emplace(s, io);
    aur->translate_fds_.emplace(fd, s);
  }

  return 0;
}

// static
int Aur::OnIO(sd_event_source* s, int fd, uint32_t revents, void* userdata) {
  Aur* aur = static_cast<Aur*>(userdata);
  int action, k = 0;

  // Throwing an exception here would indicate a bug in Aur::SocketCallback.
  auto translated_fd = aur->translate_fds_[fd];

  if ((revents & (EPOLLIN | EPOLLOUT)) == (EPOLLIN | EPOLLOUT)) {
    action = CURL_POLL_INOUT;
  } else if (revents & EPOLLIN) {
    action = CURL_POLL_IN;
  } else if (revents & EPOLLOUT) {
    action = CURL_POLL_OUT;
  } else {
    action = 0;
  }

  if (curl_multi_socket_action(aur->curl_, translated_fd, action, &k) < 0) {
    return -EINVAL;
  }

  aur->ProcessDoneEvents();
  return 0;
}

// static
int Aur::OnTimer(sd_event_source* s, uint64_t usec, void* userdata) {
  Aur* aur = static_cast<Aur*>(userdata);
  int k = 0;

  if (curl_multi_socket_action(aur->curl_, CURL_SOCKET_TIMEOUT, 0, &k) !=
      CURLM_OK) {
    return -EINVAL;
  }

  aur->ProcessDoneEvents();
  return 0;
}

// static
int Aur::TimerCallback(CURLM* curl, long timeout_ms, void* userdata) {
  Aur* aur = static_cast<Aur*>(userdata);

  if (timeout_ms < 0) {
    if (aur->timer_) {
      if (sd_event_source_set_enabled(aur->timer_, SD_EVENT_OFF) < 0) {
        return -1;
      }
    }

    return 0;
  }

  auto usec = TimepointTo<std::chrono::microseconds>(
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));

  if (aur->timer_) {
    if (sd_event_source_set_time(aur->timer_, usec) < 0) {
      return -1;
    }

    if (sd_event_source_set_enabled(aur->timer_, SD_EVENT_ONESHOT) < 0) {
      return -1;
    }
  } else {
    if (sd_event_add_time(aur->event_, &aur->timer_, CLOCK_MONOTONIC, usec, 0,
                          &Aur::OnTimer, aur) < 0) {
      return -1;
    }

    sd_event_source_set_description(aur->timer_, "curl-timer");
  }

  return 0;
}

void Aur::StartRequest(CURL* curl) {
  curl_multi_add_handle(curl_, curl);
  active_requests_.insert(curl);
}

int Aur::FinishRequest(CURL* curl, CURLcode result, bool dispatch_callback) {
  ResponseHandler* handler;
  curl_easy_getinfo(curl, CURLINFO_PRIVATE, &handler);

  long response_code;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

  std::string error;
  if (result == CURLE_OK) {
    if (response_code != 200) {
      error = "HTTP " + std::to_string(response_code);
    }
  } else {
    error = strlen(handler->error_buffer) > 0 ? handler->error_buffer
                                              : curl_easy_strerror(result);
  }

  auto r = dispatch_callback ? handler->RunCallback(error) : 0;

  active_requests_.erase(curl);
  curl_multi_remove_handle(curl_, curl);
  curl_easy_cleanup(curl);

  return r;
}

int Aur::ProcessDoneEvents() {
  for (;;) {
    int msgs_left;
    auto msg = curl_multi_info_read(curl_, &msgs_left);
    if (msg == NULL) {
      break;
    }

    if (msg->msg != CURLMSG_DONE) {
      continue;
    }

    auto r = FinishRequest(msg->easy_handle, msg->data.result,
                           /* dispatch_callback = */ true);
    if (r != 0) {
      return r;
    }
  }

  return 0;
}

void Aur::Cancel() {
  while (!active_requests_.empty()) {
    FinishRequest(*active_requests_.begin(), CURLE_ABORTED_BY_CALLBACK,
                  /* dispatch_callback = */ false);
  }
}

int Aur::Wait() {
  while (!active_requests_.empty()) {
    sd_event_run(event_, 0);
  }

  return 0;
}

struct RpcRequestTraits {
  enum : bool { kNeedHeaders = false };

  using ResponseHandlerType = RpcResponseHandler;

  static constexpr char const* kEncoding = "";
  static constexpr char const* kFilenameHint = nullptr;
};

struct RawRpcRequestTraits {
  enum : bool { kNeedHeaders = false };

  using ResponseHandlerType = RawResponseHandler;

  static constexpr char const* kEncoding = "";
  static constexpr char const* kFilenameHint = nullptr;
};

struct TarballRequestTraits {
  enum : bool { kNeedHeaders = true };

  using ResponseHandlerType = RawResponseHandler;

  static constexpr char const* kEncoding = "identity";
  static constexpr char const* kFilenameHint = nullptr;
};

struct PkgbuildRequestTraits {
  enum : bool { kNeedHeaders = false };

  using ResponseHandlerType = RawResponseHandler;

  static constexpr char const* kEncoding = "";
  static constexpr char const* kFilenameHint = "PKGBUILD";
};

template <typename RequestTraits>
void Aur::QueueRequest(
    const Request& request,
    const typename RequestTraits::ResponseHandlerType::CallbackType& callback) {
  for (const auto& r : request.Build(baseurl_)) {
    auto curl = curl_easy_init();

    auto response_handler =
        new typename RequestTraits::ResponseHandlerType(callback);

    if (getenv("AURACLE_DEBUG")) {
      curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    }

    using RH = ResponseHandler;
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2);
    curl_easy_setopt(curl, CURLOPT_URL, r.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &RH::BodyCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_handler);
    curl_easy_setopt(curl, CURLOPT_PRIVATE, response_handler);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, response_handler->error_buffer);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, RequestTraits::kEncoding);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_timeout_);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Auracle/0");

    if (RequestTraits::kNeedHeaders) {
      curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &RH::HeaderCallback);
      curl_easy_setopt(curl, CURLOPT_HEADERDATA, response_handler);
    }

    if (RequestTraits::kFilenameHint) {
      response_handler->set_filename_hint(RequestTraits::kFilenameHint);
    }

    StartRequest(curl);
  }
}

void Aur::QueueRawRpcRequest(const RpcRequest& request,
                             const RawResponseCallback& callback) {
  QueueRequest<RawRpcRequestTraits>(request, callback);
}

void Aur::QueueRpcRequest(const RpcRequest& request,
                          const RpcResponseCallback& callback) {
  QueueRequest<RpcRequestTraits>(request, callback);
}

void Aur::QueueTarballRequest(const RawRequest& request,
                              const RawResponseCallback& callback) {
  QueueRequest<TarballRequestTraits>(request, callback);
}

void Aur::QueuePkgbuildRequest(const RawRequest& request,
                               const RawResponseCallback& callback) {
  QueueRequest<PkgbuildRequestTraits>(request, callback);
}

void Aur::SetConnectTimeout(long timeout) { connect_timeout_ = timeout; }

void Aur::SetMaxConnections(long connections) {
  curl_multi_setopt(curl_, CURLMOPT_MAX_TOTAL_CONNECTIONS, connections);
}

}  // namespace aur

/* vim: set et ts=2 sw=2: */
