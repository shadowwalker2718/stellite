// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "stellite/fetcher/http_fetcher_core.h"

#include <stdint.h>
#include <utility>

#include "base/bind.h"
#include "base/logging.h"
#include "base/metrics/histogram_macros.h"
#include "base/profiler/scoped_tracker.h"
#include "base/sequenced_task_runner.h"
#include "base/single_thread_task_runner.h"
#include "base/stl_util.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/tracked_objects.h"
#include "net/base/elements_upload_data_stream.h"
#include "net/base/io_buffer.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/base/request_priority.h"
#include "net/base/upload_bytes_element_reader.h"
#include "net/base/upload_data_stream.h"
#include "net/base/upload_file_element_reader.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_response_info.h"
#include "net/url_request/redirect_info.h"
#include "net/url_request/url_fetcher_delegate.h"
#include "net/url_request/url_fetcher_response_writer.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_getter.h"
#include "net/url_request/url_request_throttler_manager.h"
#include "stellite/fetcher/http_fetcher_delegate.h"

namespace {

const int kBufferSize = 4096;
bool g_ignore_certificate_requests = false;

void EmptyCompletionCallback(int result) {}

}  // namespace

namespace net {

// HttpFetcherCore::Registry ---------------------------------------------------

HttpFetcherCore::Registry::Registry() {}
HttpFetcherCore::Registry::~Registry() {}

void HttpFetcherCore::Registry::AddHttpFetcherCore(HttpFetcherCore* core) {
  DCHECK(!base::ContainsKey(fetchers_, core));
  fetchers_.insert(core);
}

void HttpFetcherCore::Registry::RemoveHttpFetcherCore(HttpFetcherCore* core) {
  DCHECK(base::ContainsKey(fetchers_, core));
  fetchers_.erase(core);
}

void HttpFetcherCore::Registry::CancelAll() {
  while (!fetchers_.empty())
    (*fetchers_.begin())->CancelURLRequest(ERR_ABORTED);
}

// HttpFetcherCore -------------------------------------------------------------

HttpFetcherCore::HttpFetcherCore(URLFetcher* fetcher,
                                 const GURL& original_url,
                                 URLFetcher::RequestType request_type,
                                 stellite::HttpFetcherDelegate* d,
                                 bool stream_response)
    : fetcher_(fetcher),
      original_url_(original_url),
      request_type_(request_type),
      delegate_(d),
      delegate_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      load_flags_(LOAD_NORMAL),
      response_code_(URLFetcher::RESPONSE_CODE_INVALID),
      buffer_(new IOBuffer(kBufferSize)),
      url_request_data_key_(NULL),
      was_fetched_via_proxy_(false),
      was_cached_(false),
      received_response_content_length_(0),
      total_received_bytes_(0),
      upload_content_set_(false),
      upload_range_offset_(0),
      upload_range_length_(0),
      referrer_policy_(
          URLRequest::CLEAR_REFERRER_ON_TRANSITION_FROM_SECURE_TO_INSECURE),
      is_chunked_upload_(false),
      was_cancelled_(false),
      stop_on_redirect_(false),
      stopped_on_redirect_(false),
      automatically_retry_on_5xx_(true),
      num_retries_on_5xx_(0),
      max_retries_on_5xx_(0),
      num_retries_on_network_changes_(0),
      max_retries_on_network_changes_(0),
      current_upload_bytes_(-1),
      current_response_bytes_(0),
      total_response_bytes_(-1),
      stream_response_(stream_response),
      response_info_(nullptr) {
  CHECK(original_url_.is_valid());
}

void HttpFetcherCore::Start() {
  DCHECK(delegate_task_runner_.get());
  DCHECK(request_context_getter_.get()) << "We need an URLRequestContext!";
  if (network_task_runner_.get()) {
    DCHECK_EQ(network_task_runner_,
              request_context_getter_->GetNetworkTaskRunner());
  } else {
    network_task_runner_ = request_context_getter_->GetNetworkTaskRunner();
  }
  DCHECK(network_task_runner_.get()) << "We need an IO task runner";

  network_task_runner_->PostTask(
      FROM_HERE, base::Bind(&HttpFetcherCore::StartOnIOThread, this));
}

void HttpFetcherCore::Stop() {
  if (delegate_task_runner_.get())  // May be NULL in tests.
    DCHECK(delegate_task_runner_->BelongsToCurrentThread());

  delegate_ = NULL;
  fetcher_ = NULL;
  if (!network_task_runner_.get())
    return;
  if (network_task_runner_->RunsTasksOnCurrentThread()) {
    CancelURLRequest(ERR_ABORTED);
  } else {
    network_task_runner_->PostTask(
        FROM_HERE,
        base::Bind(&HttpFetcherCore::CancelURLRequest, this, ERR_ABORTED));
  }
}

void HttpFetcherCore::SetUploadData(const std::string& upload_content_type,
                                    const std::string& upload_content) {
  AssertHasNoUploadData();
  DCHECK(!is_chunked_upload_);
  DCHECK(upload_content_type_.empty());

  // Empty |upload_content_type| is allowed iff the |upload_content| is empty.
  DCHECK(upload_content.empty() || !upload_content_type.empty());

  upload_content_type_ = upload_content_type;
  upload_content_ = upload_content;
  upload_content_set_ = true;
}

void HttpFetcherCore::SetUploadFilePath(
    const std::string& upload_content_type,
    const base::FilePath& file_path,
    uint64_t range_offset,
    uint64_t range_length,
    scoped_refptr<base::TaskRunner> file_task_runner) {
  AssertHasNoUploadData();
  DCHECK(!is_chunked_upload_);
  DCHECK_EQ(upload_range_offset_, 0ULL);
  DCHECK_EQ(upload_range_length_, 0ULL);
  DCHECK(upload_content_type_.empty());
  DCHECK(!upload_content_type.empty());

  upload_content_type_ = upload_content_type;
  upload_file_path_ = file_path;
  upload_range_offset_ = range_offset;
  upload_range_length_ = range_length;
  upload_file_task_runner_ = file_task_runner;
  upload_content_set_ = true;
}

void HttpFetcherCore::SetUploadStreamFactory(
    const std::string& upload_content_type,
    const URLFetcher::CreateUploadStreamCallback& factory) {
  AssertHasNoUploadData();
  DCHECK(!is_chunked_upload_);
  DCHECK(upload_content_type_.empty());

  upload_content_type_ = upload_content_type;
  upload_stream_factory_ = factory;
  upload_content_set_ = true;
}

void HttpFetcherCore::SetChunkedUpload(const std::string& content_type) {
  if (!is_chunked_upload_) {
    AssertHasNoUploadData();
    DCHECK(upload_content_type_.empty());
  }

  // Empty |content_type| is not allowed here, because it is impossible
  // to ensure non-empty upload content as it is not yet supplied.
  DCHECK(!content_type.empty());

  upload_content_type_ = content_type;
  upload_content_.clear();
  is_chunked_upload_ = true;
}

void HttpFetcherCore::AppendChunkToUpload(const std::string& content,
                                          bool is_last_chunk) {
  DCHECK(delegate_task_runner_.get());
  DCHECK(network_task_runner_.get());
  DCHECK(is_chunked_upload_);
  network_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&HttpFetcherCore::CompleteAddingUploadDataChunk, this, content,
                 is_last_chunk));
}

void HttpFetcherCore::SetLoadFlags(int load_flags) {
  load_flags_ = load_flags;
}

int HttpFetcherCore::GetLoadFlags() const {
  return load_flags_;
}

void HttpFetcherCore::SetReferrer(const std::string& referrer) {
  referrer_ = referrer;
}

void HttpFetcherCore::SetReferrerPolicy(
    URLRequest::ReferrerPolicy referrer_policy) {
  referrer_policy_ = referrer_policy;
}

void HttpFetcherCore::SetExtraRequestHeaders(
    const std::string& extra_request_headers) {
  extra_request_headers_.Clear();
  extra_request_headers_.AddHeadersFromString(extra_request_headers);
}

void HttpFetcherCore::AddExtraRequestHeader(const std::string& header_line) {
  extra_request_headers_.AddHeaderFromString(header_line);
}

void HttpFetcherCore::SetRequestContext(
    URLRequestContextGetter* request_context_getter) {
  DCHECK(!request_context_getter_.get());
  DCHECK(request_context_getter);
  request_context_getter_ = request_context_getter;
}

void HttpFetcherCore::SetInitiatorURL(const GURL& initiator) {
  DCHECK(initiator_.is_empty());
  initiator_ = initiator;
}

void HttpFetcherCore::SetURLRequestUserData(
    const void* key,
    const URLFetcher::CreateDataCallback& create_data_callback) {
  DCHECK(key);
  DCHECK(!create_data_callback.is_null());
  url_request_data_key_ = key;
  url_request_create_data_callback_ = create_data_callback;
}

void HttpFetcherCore::SetStopOnRedirect(bool stop_on_redirect) {
  stop_on_redirect_ = stop_on_redirect;
}

void HttpFetcherCore::SetAutomaticallyRetryOn5xx(bool retry) {
  automatically_retry_on_5xx_ = retry;
}

void HttpFetcherCore::SetMaxRetriesOn5xx(int max_retries) {
  max_retries_on_5xx_ = max_retries;
}

int HttpFetcherCore::GetMaxRetriesOn5xx() const {
  return max_retries_on_5xx_;
}

base::TimeDelta HttpFetcherCore::GetBackoffDelay() const {
  return backoff_delay_;
}

void HttpFetcherCore::SetAutomaticallyRetryOnNetworkChanges(int max_retries) {
  max_retries_on_network_changes_ = max_retries;
}

void HttpFetcherCore::SaveResponseToFileAtPath(
    const base::FilePath& file_path,
    scoped_refptr<base::SequencedTaskRunner> file_task_runner) {
  DCHECK(delegate_task_runner_->BelongsToCurrentThread());
  SaveResponseWithWriter(std::unique_ptr<URLFetcherResponseWriter>(
      new URLFetcherFileWriter(file_task_runner, file_path)));
}

void HttpFetcherCore::SaveResponseToTemporaryFile(
    scoped_refptr<base::SequencedTaskRunner> file_task_runner) {
  DCHECK(delegate_task_runner_->BelongsToCurrentThread());
  SaveResponseWithWriter(std::unique_ptr<URLFetcherResponseWriter>(
      new URLFetcherFileWriter(file_task_runner, base::FilePath())));
}

void HttpFetcherCore::SaveResponseWithWriter(
    std::unique_ptr<URLFetcherResponseWriter> response_writer) {
  DCHECK(delegate_task_runner_->BelongsToCurrentThread());
  response_writer_ = std::move(response_writer);
}

HttpResponseHeaders* HttpFetcherCore::GetResponseHeaders() const {
  return response_headers_.get();
}

// TODO(panayiotis): socket_address_ is written in the IO thread,
// if this is accessed in the UI thread, this could result in a race.
// Same for response_headers_ above and was_fetched_via_proxy_ below.
HostPortPair HttpFetcherCore::GetSocketAddress() const {
  return socket_address_;
}

bool HttpFetcherCore::WasFetchedViaProxy() const {
  return was_fetched_via_proxy_;
}

bool HttpFetcherCore::WasCached() const {
  return was_cached_;
}

int64_t HttpFetcherCore::GetReceivedResponseContentLength() const {
  return received_response_content_length_;
}

int64_t HttpFetcherCore::GetTotalReceivedBytes() const {
  return total_received_bytes_;
}

const GURL& HttpFetcherCore::GetOriginalURL() const {
  return original_url_;
}

const GURL& HttpFetcherCore::GetURL() const {
  return url_;
}

const URLRequestStatus& HttpFetcherCore::GetStatus() const {
  return status_;
}

int HttpFetcherCore::GetResponseCode() const {
  return response_code_;
}

void HttpFetcherCore::ReceivedContentWasMalformed() {
  DCHECK(delegate_task_runner_->BelongsToCurrentThread());
  if (network_task_runner_.get()) {
    network_task_runner_->PostTask(
        FROM_HERE, base::Bind(&HttpFetcherCore::NotifyMalformedContent, this));
  }
}

bool HttpFetcherCore::GetResponseAsString(
    std::string* out_response_string) const {
  URLFetcherStringWriter* string_writer =
      response_writer_ ? response_writer_->AsStringWriter() : NULL;
  if (!string_writer)
    return false;

  *out_response_string = string_writer->data();
  UMA_HISTOGRAM_MEMORY_KB("UrlFetcher.StringResponseSize",
                          (string_writer->data().length() / 1024));
  return true;
}

bool HttpFetcherCore::GetResponseAsFilePath(bool take_ownership,
                                           base::FilePath* out_response_path) {
  DCHECK(delegate_task_runner_->BelongsToCurrentThread());

  URLFetcherFileWriter* file_writer =
      response_writer_ ? response_writer_->AsFileWriter() : NULL;
  if (!file_writer)
    return false;

  *out_response_path = file_writer->file_path();

  if (take_ownership) {
    // Intentionally calling a file_writer_ method directly without posting
    // the task to network_task_runner_.
    //
    // This is for correctly handling the case when file_writer_->DisownFile()
    // is soon followed by HttpFetcherCore::Stop(). We have to make sure that
    // DisownFile takes effect before Stop deletes file_writer_.
    //
    // This direct call should be thread-safe, since DisownFile itself does no
    // file operation. It just flips the state to be referred in destruction.
    file_writer->DisownFile();
  }
  return true;
}

void HttpFetcherCore::OnReceivedRedirect(URLRequest* request,
                                         const RedirectInfo& redirect_info,
                                         bool* defer_redirect) {
  DCHECK_EQ(request, request_.get());
  DCHECK(network_task_runner_->BelongsToCurrentThread());
  if (stop_on_redirect_) {
    stopped_on_redirect_ = true;
    url_ = redirect_info.new_url;
    response_code_ = request_->GetResponseCode();
    response_headers_ = request->response_headers();
    was_fetched_via_proxy_ = request_->was_fetched_via_proxy();
    was_cached_ = request_->was_cached();
    total_received_bytes_ += request_->GetTotalReceivedBytes();
    response_info_.reset(new HttpResponseInfo(request->response_info()));

    if (stream_response_) {
      InformDelegateFetchStream(nullptr);
    } else {
      InformDelegateFetchIsComplete();
    }

    int result = request->Cancel();
    OnReadCompleted(request, result);
  }
}

void HttpFetcherCore::OnResponseStarted(URLRequest* request, int net_error) {
  DCHECK_EQ(request, request_.get());
  DCHECK(network_task_runner_->BelongsToCurrentThread());
  DCHECK_NE(ERR_IO_PENDING, net_error);

  if (net_error == OK) {
    response_code_ = request_->GetResponseCode();
    response_headers_ = request_->response_headers();
    socket_address_ = request_->GetSocketAddress();
    was_fetched_via_proxy_ = request_->was_fetched_via_proxy();
    was_cached_ = request_->was_cached();
    total_response_bytes_ = request_->GetExpectedContentSize();
    response_info_.reset(new HttpResponseInfo(request->response_info()));

    // notify a header received
    if (stream_response_) {
      InformDelegateFetchStream(nullptr);
    }

    InformDelegateUpdateFetchTimeout();
  }

  ReadResponse();
}

void HttpFetcherCore::OnCertificateRequested(
    URLRequest* request,
    SSLCertRequestInfo* cert_request_info) {
  DCHECK_EQ(request, request_.get());
  DCHECK(network_task_runner_->BelongsToCurrentThread());

  if (g_ignore_certificate_requests) {
    request->ContinueWithCertificate(nullptr, nullptr);
  } else {
    request->Cancel();
  }
}

void HttpFetcherCore::OnReadCompleted(URLRequest* request, int bytes_read) {
  DCHECK_EQ(request, request_.get());
  DCHECK(network_task_runner_->BelongsToCurrentThread());

  InformDelegateUpdateFetchTimeout();

  if (!stopped_on_redirect_)
    url_ = request->url();

  URLRequestThrottlerManager* throttler_manager =
      request->context()->throttler_manager();
  if (throttler_manager)
    url_throttler_entry_ = throttler_manager->RegisterRequestUrl(url_);

  while (bytes_read > 0) {
    current_response_bytes_ += bytes_read;

    const int result =
        WriteBuffer(new DrainableIOBuffer(buffer_.get(), bytes_read));
    if (result < 0) {
      // Write failed or waiting for write completion.
      return;
    }
    bytes_read = request_->Read(buffer_.get(), kBufferSize);
  }

  // See comments re: HEAD requests in ReadResponse().
  if (bytes_read != ERR_IO_PENDING || request_type_ == URLFetcher::HEAD) {
    status_ = URLRequestStatus::FromError(bytes_read);
    received_response_content_length_ =
        request_->received_response_content_length();
    total_received_bytes_ += request_->GetTotalReceivedBytes();
    ReleaseRequest();

    // No more data to write.
    const int result = response_writer_->Finish(
        base::Bind(&HttpFetcherCore::DidFinishWriting, this));
    if (result != ERR_IO_PENDING)
      DidFinishWriting(result);
  }
}

void HttpFetcherCore::OnContextShuttingDown() {
  DCHECK(request_);
  CancelRequestAndInformDelegate(ERR_CONTEXT_SHUT_DOWN);
}

void HttpFetcherCore::CancelAll() {
}

int HttpFetcherCore::GetNumFetcherCores() {
  return 0;
}

void HttpFetcherCore::SetIgnoreCertificateRequests(bool ignored) {
  g_ignore_certificate_requests = ignored;
}

HttpFetcherCore::~HttpFetcherCore() {
  // |request_| should be NULL. If not, it's unsafe to delete it here since we
  // may not be on the IO thread.
  DCHECK(!request_.get());
}

void HttpFetcherCore::StartOnIOThread() {
  DCHECK(network_task_runner_->BelongsToCurrentThread());
  // Create ChunkedUploadDataStream, if needed, so the consumer can start
  // appending data.  Have to do it here because StartURLRequest() may be called
  // asynchonously.
  if (is_chunked_upload_) {
    chunked_stream_.reset(new ChunkedUploadDataStream(0));
    chunked_stream_writer_ = chunked_stream_->CreateWriter();
  }

  if (!response_writer_)
    response_writer_.reset(new URLFetcherStringWriter);

  const int result = response_writer_->Initialize(
      base::Bind(&HttpFetcherCore::DidInitializeWriter, this));
  if (result != ERR_IO_PENDING)
    DidInitializeWriter(result);
}

void HttpFetcherCore::StartURLRequest() {
  DCHECK(network_task_runner_->BelongsToCurrentThread());

  if (was_cancelled_) {
    // Since StartURLRequest() is posted as a *delayed* task, it may
    // run after the URLFetcher was already stopped.
    return;
  }

  if (!request_context_getter_->GetURLRequestContext()) {
    CancelRequestAndInformDelegate(ERR_CONTEXT_SHUT_DOWN);
    return;
  }

  DCHECK(request_context_getter_.get());
  DCHECK(!request_.get());

  current_response_bytes_ = 0;
  request_context_getter_->AddObserver(this);
  request_ = request_context_getter_->GetURLRequestContext()->CreateRequest(
      original_url_, DEFAULT_PRIORITY, this);
  int flags = request_->load_flags() | load_flags_;

  // TODO(mmenke): This should really be with the other code to set the upload
  // body, below.
  if (chunked_stream_)
    request_->set_upload(std::move(chunked_stream_));

  request_->SetLoadFlags(flags);
  request_->SetReferrer(referrer_);
  request_->set_referrer_policy(referrer_policy_);
  request_->set_first_party_for_cookies(initiator_.is_empty() ? original_url_
                                                              : initiator_);
  request_->set_initiator(initiator_.is_empty() ? url::Origin(original_url_)
                                                : url::Origin(initiator_));
  if (url_request_data_key_ && !url_request_create_data_callback_.is_null()) {
    request_->SetUserData(url_request_data_key_,
                          url_request_create_data_callback_.Run());
  }

  switch (request_type_) {
    case URLFetcher::GET:
      break;

    case URLFetcher::POST:
    case URLFetcher::PUT:
    case URLFetcher::PATCH: {
      // Upload content must be set.
      DCHECK(is_chunked_upload_ || upload_content_set_);

      request_->set_method(
          request_type_ == URLFetcher::POST ? "POST" :
          request_type_ == URLFetcher::PUT ? "PUT" : "PATCH");
      if (!upload_content_type_.empty()) {
        extra_request_headers_.SetHeader(HttpRequestHeaders::kContentType,
                                         upload_content_type_);
      }
      if (!upload_content_.empty()) {
        std::unique_ptr<UploadElementReader> reader(
            new UploadBytesElementReader(upload_content_.data(),
                                         upload_content_.size()));
        request_->set_upload(
            ElementsUploadDataStream::CreateWithReader(std::move(reader), 0));
      } else if (!upload_file_path_.empty()) {
        std::unique_ptr<UploadElementReader> reader(new UploadFileElementReader(
            upload_file_task_runner_.get(), upload_file_path_,
            upload_range_offset_, upload_range_length_, base::Time()));
        request_->set_upload(
            ElementsUploadDataStream::CreateWithReader(std::move(reader), 0));
      } else if (!upload_stream_factory_.is_null()) {
        std::unique_ptr<UploadDataStream> stream = upload_stream_factory_.Run();
        DCHECK(stream);
        request_->set_upload(std::move(stream));
      }

      current_upload_bytes_ = -1;
      // TODO(kinaba): http://crbug.com/118103. Implement upload callback in the
      //  layer and avoid using timer here.
      break;
    }

    case URLFetcher::HEAD:
      request_->set_method("HEAD");
      break;

    case URLFetcher::DELETE_REQUEST:
      request_->set_method("DELETE");
      break;

    default:
      NOTREACHED();
  }

  if (!extra_request_headers_.IsEmpty())
    request_->SetExtraRequestHeaders(extra_request_headers_);

  request_->Start();
}

void HttpFetcherCore::DidInitializeWriter(int result) {
  if (result != OK) {
    CancelRequestAndInformDelegate(result);
    return;
  }
  StartURLRequestWhenAppropriate();
}

void HttpFetcherCore::StartURLRequestWhenAppropriate() {
  DCHECK(network_task_runner_->BelongsToCurrentThread());

  if (was_cancelled_)
    return;

  DCHECK(request_context_getter_.get());

  // Check if the request should be delayed, and if so, post a task to start it
  // after the delay has expired.  Otherwise, start it now.

  URLRequestContext* context = request_context_getter_->GetURLRequestContext();
  // If the context has been shut down, or there's no ThrottlerManager, just
  // start the request.  In the former case, StartURLRequest() will just inform
  // the URLFetcher::Delegate the request has been canceled.
  if (context && context->throttler_manager()) {
    if (!original_url_throttler_entry_.get()) {
      original_url_throttler_entry_ =
          context->throttler_manager()->RegisterRequestUrl(original_url_);
    }

    if (original_url_throttler_entry_.get()) {
      int64_t delay =
          original_url_throttler_entry_->ReserveSendingTimeForNextRequest(
              GetBackoffReleaseTime());
      if (delay != 0) {
        base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
            FROM_HERE, base::Bind(&HttpFetcherCore::StartURLRequest, this),
            base::TimeDelta::FromMilliseconds(delay));
        return;
      }
    }
  }

  StartURLRequest();
}

void HttpFetcherCore::CancelURLRequest(int error) {
  DCHECK(network_task_runner_->BelongsToCurrentThread());

  if (request_.get()) {
    request_->CancelWithError(error);
    ReleaseRequest();
  }

  // Set the error manually.
  // Normally, calling URLRequest::CancelWithError() results in calling
  // OnReadCompleted() with bytes_read = -1 via an asynchronous task posted by
  // URLRequestJob::NotifyDone(). But, because the request was released
  // immediately after being canceled, the request could not call
  // OnReadCompleted() which overwrites |status_| with the error status.
  status_ = URLRequestStatus(URLRequestStatus::CANCELED, error);

  // Release the reference to the request context. There could be multiple
  // references to URLFetcher::Core at this point so it may take a while to
  // delete the object, but we cannot delay the destruction of the request
  // context.
  request_context_getter_ = NULL;
  initiator_ = GURL();
  url_request_data_key_ = NULL;
  url_request_create_data_callback_.Reset();
  was_cancelled_ = true;
}

void HttpFetcherCore::OnCompletedURLRequest(
    base::TimeDelta backoff_delay) {
  DCHECK(delegate_task_runner_->BelongsToCurrentThread());
  // Save the status and backoff_delay so that delegates can read it.
  if (delegate_) {
    backoff_delay_ = backoff_delay;
    InformDelegateFetchIsCompleteInDelegateThread();
  }
}

void HttpFetcherCore::InformDelegateFetchIsComplete() {
  delegate_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(
          &HttpFetcherCore::InformDelegateFetchIsCompleteInDelegateThread,
          this));

}

void HttpFetcherCore::InformDelegateFetchIsCompleteInDelegateThread() {
  DCHECK(delegate_task_runner_->BelongsToCurrentThread());
  if (delegate_) {
    delegate_->OnFetchComplete(fetcher_, response_info_.get());
  }
}

void HttpFetcherCore::InformDelegateFetchStream(
    scoped_refptr<DrainableIOBuffer> data) {
  delegate_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&HttpFetcherCore::InformDelegateFetchStreamInDelegateThread,
                 this, data));
}

void HttpFetcherCore::InformDelegateFetchStreamInDelegateThread(
    scoped_refptr<DrainableIOBuffer> data) {
  DCHECK(delegate_task_runner_->BelongsToCurrentThread());
  DCHECK(stream_response_);

  if (!data.get()) {
    if (delegate_) {
      delegate_->OnFetchStream(fetcher_, response_info_.get(),
                               nullptr, 0, false /* fin */);
    }
    return;
  }

  int stream_size = data->BytesRemaining();
  if (delegate_) {
    delegate_->OnFetchStream(fetcher_, response_info_.get(),
                             data->data(), stream_size, false /* fin */);
  }

  network_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&HttpFetcherCore::DidWriteBuffer, this, data, stream_size));
}

void HttpFetcherCore::InformDelegateUpdateFetchTimeout() {
  delegate_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(
          &HttpFetcherCore::InformDelegateUpdateFetchTimeoutInDelegateThread,
          this));
}

void HttpFetcherCore::InformDelegateUpdateFetchTimeoutInDelegateThread() {
  DCHECK(delegate_task_runner_->BelongsToCurrentThread());
  if (delegate_) {
    delegate_->ResetTimeout();
  }
}

void HttpFetcherCore::NotifyMalformedContent() {
  DCHECK(network_task_runner_->BelongsToCurrentThread());
  if (url_throttler_entry_.get()) {
    int status_code = response_code_;
    if (status_code == URLFetcher::RESPONSE_CODE_INVALID) {
      // The status code will generally be known by the time clients
      // call the |ReceivedContentWasMalformed()| function (which ends up
      // calling the current function) but if it's not, we need to assume
      // the response was successful so that the total failure count
      // used to calculate exponential back-off goes up.
      status_code = 200;
    }
    url_throttler_entry_->ReceivedContentWasMalformed(status_code);
  }
}

void HttpFetcherCore::DidFinishWriting(int result) {
  if (result != OK) {
    CancelRequestAndInformDelegate(result);
    return;
  }
  // If the file was successfully closed, then the URL request is complete.
  RetryOrCompleteUrlFetch();
}

void HttpFetcherCore::RetryOrCompleteUrlFetch() {
  DCHECK(network_task_runner_->BelongsToCurrentThread());
  base::TimeDelta backoff_delay;

  // Checks the response from server.
  if (response_code_ >= 500 ||
      status_.error() == ERR_TEMPORARILY_THROTTLED) {
    // When encountering a server error, we will send the request again
    // after backoff time.
    ++num_retries_on_5xx_;

    // Note that backoff_delay may be 0 because (a) the
    // URLRequestThrottlerManager and related code does not
    // necessarily back off on the first error, (b) it only backs off
    // on some of the 5xx status codes, (c) not all URLRequestContexts
    // have a throttler manager.
    base::TimeTicks backoff_release_time = GetBackoffReleaseTime();
    backoff_delay = backoff_release_time - base::TimeTicks::Now();
    if (backoff_delay < base::TimeDelta())
      backoff_delay = base::TimeDelta();

    if (automatically_retry_on_5xx_ &&
        num_retries_on_5xx_ <= max_retries_on_5xx_) {
      StartOnIOThread();
      return;
    }
  } else {
    backoff_delay = base::TimeDelta();
  }

  // Retry if the request failed due to network changes.
  if (status_.error() == ERR_NETWORK_CHANGED &&
      num_retries_on_network_changes_ < max_retries_on_network_changes_) {
    ++num_retries_on_network_changes_;

    // Retry soon, after flushing all the current tasks which may include
    // further network change observers.
    network_task_runner_->PostTask(
        FROM_HERE, base::Bind(&HttpFetcherCore::StartOnIOThread, this));
    return;
  }

  request_context_getter_ = NULL;
  initiator_ = GURL();
  url_request_data_key_ = NULL;
  url_request_create_data_callback_.Reset();
  bool posted = delegate_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&HttpFetcherCore::OnCompletedURLRequest, this, backoff_delay));

  // If the delegate message loop does not exist any more, then the delegate
  // should be gone too.
  DCHECK(posted || !delegate_);
}

void HttpFetcherCore::CancelRequestAndInformDelegate(int result) {
  CancelURLRequest(result);
  delegate_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&HttpFetcherCore::InformDelegateFetchIsComplete, this));
}

void HttpFetcherCore::ReleaseRequest() {
  request_context_getter_->RemoveObserver(this);
  request_.reset();
}

base::TimeTicks HttpFetcherCore::GetBackoffReleaseTime() {
  DCHECK(network_task_runner_->BelongsToCurrentThread());

  if (!original_url_throttler_entry_.get())
    return base::TimeTicks();

  base::TimeTicks original_url_backoff =
      original_url_throttler_entry_->GetExponentialBackoffReleaseTime();
  base::TimeTicks destination_url_backoff;
  if (url_throttler_entry_.get() &&
      original_url_throttler_entry_.get() != url_throttler_entry_.get()) {
    destination_url_backoff =
        url_throttler_entry_->GetExponentialBackoffReleaseTime();
  }

  return original_url_backoff > destination_url_backoff ?
      original_url_backoff : destination_url_backoff;
}

void HttpFetcherCore::CompleteAddingUploadDataChunk(
    const std::string& content, bool is_last_chunk) {
  DCHECK(is_chunked_upload_);
  DCHECK(!content.empty());
  chunked_stream_writer_->AppendData(
      content.data(), static_cast<int>(content.length()), is_last_chunk);
}

int HttpFetcherCore::WriteBuffer(scoped_refptr<DrainableIOBuffer> data) {
  if (stream_response_ && data->BytesRemaining() > 0) {
    InformDelegateFetchStream(data);
    return ERR_IO_PENDING;
  }

  while (data->BytesRemaining() > 0) {
    const int result = response_writer_->Write(
        data.get(),
        data->BytesRemaining(),
        base::Bind(&HttpFetcherCore::DidWriteBuffer, this, data));
    if (result < 0) {
      if (result != ERR_IO_PENDING)
        DidWriteBuffer(data, result);
      return result;
    }
    data->DidConsume(result);
  }

  return OK;
}

void HttpFetcherCore::DidWriteBuffer(scoped_refptr<DrainableIOBuffer> data,
                                     int result) {
  if (result < 0) {  // Handle errors.
    response_writer_->Finish(base::Bind(&EmptyCompletionCallback));
    CancelRequestAndInformDelegate(result);
    return;
  }

  // Continue writing.
  data->DidConsume(result);

  if (data->BytesRemaining() == 0) {
    // Finished writing buffer_. Read some more, unless the request has been
    // cancelled and deleted.
    DCHECK_EQ(0, data->BytesRemaining());
    if (request_.get())
      ReadResponse();
  } else {
    if (WriteBuffer(data) < 0)
      return;
  }
}

void HttpFetcherCore::ReadResponse() {
  // Some servers may treat HEAD requests as GET requests. To free up the
  // network connection as soon as possible, signal that the request has
  // completed immediately, without trying to read any data back (all we care
  // about is the response code and headers, which we already have).
  int bytes_read = 0;
  if (request_type_ != URLFetcher::HEAD)
    bytes_read = request_->Read(buffer_.get(), kBufferSize);

  OnReadCompleted(request_.get(), bytes_read);
}

void HttpFetcherCore::AssertHasNoUploadData() const {
  DCHECK(!upload_content_set_);
  DCHECK(upload_content_.empty());
  DCHECK(upload_file_path_.empty());
  DCHECK(upload_stream_factory_.is_null());
}

}  // namespace net
