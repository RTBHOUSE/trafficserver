/** @file

  Http2Stream.cc

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#include "Http2Stream.h"

#include "HTTP2.h"
#include "Http2ClientSession.h"
#include "../http/HttpSM.h"

#include <numeric>

#define REMEMBER(e, r)                                    \
  {                                                       \
    this->_history.push_back(MakeSourceLocation(), e, r); \
  }

#define Http2StreamDebug(fmt, ...) \
  SsnDebug(_proxy_ssn, "http2_stream", "[%" PRId64 "] [%u] " fmt, _proxy_ssn->connection_id(), this->get_id(), ##__VA_ARGS__);

ClassAllocator<Http2Stream, true> http2StreamAllocator("http2StreamAllocator");

Http2Stream::Http2Stream(ProxySession *session, Http2StreamId sid, ssize_t initial_rwnd)
  : super(session), _id(sid), _client_rwnd(initial_rwnd)
{
  SET_HANDLER(&Http2Stream::main_event_handler);

  this->mark_milestone(Http2StreamMilestone::OPEN);

  this->_sm          = nullptr;
  this->_id          = sid;
  this->_thread      = this_ethread();
  this->_client_rwnd = initial_rwnd;
  this->_server_rwnd = Http2::initial_window_size;

  this->_reader = this->_request_buffer.alloc_reader();

  _req_header.create(HTTP_TYPE_REQUEST);
  response_header.create(HTTP_TYPE_RESPONSE);
  // TODO: init _req_header instead of response_header if this Http2Stream is outgoing
  http2_init_pseudo_headers(response_header);

  http_parser_init(&http_parser);
}

Http2Stream::~Http2Stream()
{
  REMEMBER(NO_EVENT, this->reentrancy_count);
  Http2StreamDebug("Destroy stream, sent %" PRIu64 " bytes", this->bytes_sent);
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
  // Clean up after yourself if this was an EOS
  ink_release_assert(this->closed);
  ink_release_assert(reentrancy_count == 0);

  uint64_t cid = 0;

  // Safe to initiate SSN_CLOSE if this is the last stream
  if (_proxy_ssn) {
    cid = _proxy_ssn->connection_id();

    Http2ClientSession *h2_proxy_ssn = static_cast<Http2ClientSession *>(_proxy_ssn);
    SCOPED_MUTEX_LOCK(lock, h2_proxy_ssn->mutex, this_ethread());
    // Make sure the stream is removed from the stream list and priority tree
    // In many cases, this has been called earlier, so this call is a no-op
    h2_proxy_ssn->connection_state.delete_stream(this);

    h2_proxy_ssn->connection_state.decrement_stream_count();

    // Update session's stream counts, so it accurately goes into keep-alive state
    h2_proxy_ssn->connection_state.release_stream();

    // Do not access `_proxy_ssn` in below. It might be freed by `release_stream`.
  }

  // Clean up the write VIO in case of inactivity timeout
  this->do_io_write(nullptr, 0, nullptr);

  this->_milestones.mark(Http2StreamMilestone::CLOSE);

  ink_hrtime total_time = this->_milestones.elapsed(Http2StreamMilestone::OPEN, Http2StreamMilestone::CLOSE);
  HTTP2_SUM_THREAD_DYN_STAT(HTTP2_STAT_TOTAL_TRANSACTIONS_TIME, this->_thread, total_time);

  // Slow Log
  if (Http2::stream_slow_log_threshold != 0 && ink_hrtime_from_msec(Http2::stream_slow_log_threshold) < total_time) {
    Error("[%" PRIu64 "] [%" PRIu32 "] [%" PRId64 "] Slow H2 Stream: "
          "open: %" PRIu64 " "
          "dec_hdrs: %.3f "
          "txn: %.3f "
          "enc_hdrs: %.3f "
          "tx_hdrs: %.3f "
          "tx_data: %.3f "
          "close: %.3f",
          cid, static_cast<uint32_t>(this->_id), this->_http_sm_id,
          ink_hrtime_to_msec(this->_milestones[Http2StreamMilestone::OPEN]),
          this->_milestones.difference_sec(Http2StreamMilestone::OPEN, Http2StreamMilestone::START_DECODE_HEADERS),
          this->_milestones.difference_sec(Http2StreamMilestone::OPEN, Http2StreamMilestone::START_TXN),
          this->_milestones.difference_sec(Http2StreamMilestone::OPEN, Http2StreamMilestone::START_ENCODE_HEADERS),
          this->_milestones.difference_sec(Http2StreamMilestone::OPEN, Http2StreamMilestone::START_TX_HEADERS_FRAMES),
          this->_milestones.difference_sec(Http2StreamMilestone::OPEN, Http2StreamMilestone::START_TX_DATA_FRAMES),
          this->_milestones.difference_sec(Http2StreamMilestone::OPEN, Http2StreamMilestone::CLOSE));
  }

  _req_header.destroy();
  response_header.destroy();

  // Drop references to all buffer data
  this->_request_buffer.clear();

  // Free the mutexes in the VIO
  read_vio.mutex.clear();
  write_vio.mutex.clear();

  if (header_blocks) {
    ats_free(header_blocks);
  }
  _clear_timers();
  clear_io_events();
  http_parser_clear(&http_parser);
}

int
Http2Stream::main_event_handler(int event, void *edata)
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
  REMEMBER(event, this->reentrancy_count);

  if (!this->_switch_thread_if_not_on_right_thread(event, edata)) {
    // Not on the right thread
    return 0;
  }
  ink_release_assert(this->_thread == this_ethread());

  Event *e = static_cast<Event *>(edata);
  reentrancy_count++;
  if (e == _read_vio_event) {
    _read_vio_event = nullptr;
    this->signal_read_event(e->callback_event);
    reentrancy_count--;
    return 0;
  } else if (e == _write_vio_event) {
    _write_vio_event = nullptr;
    this->signal_write_event(e->callback_event);
    reentrancy_count--;
    return 0;
  } else if (e == cross_thread_event) {
    cross_thread_event = nullptr;
  } else if (e == read_event) {
    read_event = nullptr;
  } else if (e == write_event) {
    write_event = nullptr;
  }

  switch (event) {
  case VC_EVENT_ACTIVE_TIMEOUT:
  case VC_EVENT_INACTIVITY_TIMEOUT:
    if (_sm && read_vio.ntodo() > 0) {
      this->signal_read_event(event);
    } else if (_sm && write_vio.ntodo() > 0) {
      this->signal_write_event(event);
    }
    break;
  case VC_EVENT_WRITE_READY:
  case VC_EVENT_WRITE_COMPLETE:
    _timeout.update_inactivity();
    if (e->cookie == &write_vio) {
      if (write_vio.mutex && write_vio.cont && this->_sm) {
        this->signal_write_event(event);
      }
    } else {
      update_write_request(true);
    }
    break;
  case VC_EVENT_READ_COMPLETE:
  case VC_EVENT_READ_READY:
    _timeout.update_inactivity();
    if (e->cookie == &read_vio) {
      if (read_vio.mutex && read_vio.cont && this->_sm) {
        signal_read_event(event);
      }
    } else {
      this->update_read_request(true);
    }
    break;
  case VC_EVENT_EOS:
    if (e->cookie == &read_vio) {
      SCOPED_MUTEX_LOCK(lock, read_vio.mutex, this_ethread());
      read_vio.cont->handleEvent(VC_EVENT_EOS, &read_vio);
    } else if (e->cookie == &write_vio) {
      SCOPED_MUTEX_LOCK(lock, write_vio.mutex, this_ethread());
      write_vio.cont->handleEvent(VC_EVENT_EOS, &write_vio);
    }
    break;
  }
  reentrancy_count--;
  // Clean stream up if the terminate flag is set and we are at the bottom of the handler stack
  terminate_if_possible();

  return 0;
}

Http2ErrorCode
Http2Stream::decode_header_blocks(HpackHandle &hpack_handle, uint32_t maximum_table_size)
{
  Http2ErrorCode error = http2_decode_header_blocks(&_req_header, header_blocks, header_blocks_length, nullptr, hpack_handle,
                                                    trailing_header, maximum_table_size);
  if (error != Http2ErrorCode::HTTP2_ERROR_NO_ERROR) {
    Http2StreamDebug("Error decoding header blocks: %u", static_cast<uint32_t>(error));
  }
  return error;
}

void
Http2Stream::send_request(Http2ConnectionState &cstate)
{
  ink_release_assert(this->_sm != nullptr);
  this->_http_sm_id = this->_sm->sm_id;

  // Convert header to HTTP/1.1 format
  http2_convert_header_from_2_to_1_1(&_req_header);

  // Write header to a buffer.  Borrowing logic from HttpSM::write_header_into_buffer.
  // Seems like a function like this ought to be in HTTPHdr directly
  int bufindex;
  int dumpoffset = 0;
  int done, tmp;
  do {
    bufindex             = 0;
    tmp                  = dumpoffset;
    IOBufferBlock *block = this->_request_buffer.get_current_block();
    if (!block) {
      this->_request_buffer.add_block();
      block = this->_request_buffer.get_current_block();
    }
    done = _req_header.print(block->start(), block->write_avail(), &bufindex, &tmp);
    dumpoffset += bufindex;
    this->_request_buffer.fill(bufindex);
    if (!done) {
      this->_request_buffer.add_block();
    }
  } while (!done);

  if (bufindex == 0) {
    // No data to signal read event
    return;
  }

  // Is the _sm ready to process the header?
  if (this->read_vio.nbytes > 0) {
    if (this->recv_end_stream) {
      this->read_vio.nbytes = bufindex;
      this->signal_read_event(VC_EVENT_READ_COMPLETE);
    } else {
      // End of header but not end of stream, must have some body frames coming
      this->has_body = true;
      this->signal_read_event(VC_EVENT_READ_READY);
    }
  }
}

bool
Http2Stream::change_state(uint8_t type, uint8_t flags)
{
  switch (_state) {
  case Http2StreamState::HTTP2_STREAM_STATE_IDLE:
    if (type == HTTP2_FRAME_TYPE_HEADERS) {
      if (recv_end_stream) {
        _state = Http2StreamState::HTTP2_STREAM_STATE_HALF_CLOSED_REMOTE;
      } else if (send_end_stream) {
        _state = Http2StreamState::HTTP2_STREAM_STATE_HALF_CLOSED_LOCAL;
      } else {
        _state = Http2StreamState::HTTP2_STREAM_STATE_OPEN;
      }
    } else if (type == HTTP2_FRAME_TYPE_CONTINUATION) {
      if (recv_end_stream) {
        _state = Http2StreamState::HTTP2_STREAM_STATE_HALF_CLOSED_REMOTE;
      } else if (send_end_stream) {
        _state = Http2StreamState::HTTP2_STREAM_STATE_HALF_CLOSED_LOCAL;
      } else {
        _state = Http2StreamState::HTTP2_STREAM_STATE_OPEN;
      }
    } else if (type == HTTP2_FRAME_TYPE_PUSH_PROMISE) {
      _state = Http2StreamState::HTTP2_STREAM_STATE_RESERVED_LOCAL;
    } else {
      return false;
    }
    break;

  case Http2StreamState::HTTP2_STREAM_STATE_OPEN:
    if (type == HTTP2_FRAME_TYPE_RST_STREAM) {
      _state = Http2StreamState::HTTP2_STREAM_STATE_CLOSED;
    } else if (type == HTTP2_FRAME_TYPE_HEADERS || type == HTTP2_FRAME_TYPE_DATA) {
      if (recv_end_stream) {
        _state = Http2StreamState::HTTP2_STREAM_STATE_HALF_CLOSED_REMOTE;
      } else if (send_end_stream) {
        _state = Http2StreamState::HTTP2_STREAM_STATE_HALF_CLOSED_LOCAL;
      } else {
        // Do not change state
      }
    } else {
      // A stream in the "open" state may be used by both peers to send frames of any type.
      return true;
    }
    break;

  case Http2StreamState::HTTP2_STREAM_STATE_RESERVED_LOCAL:
    if (type == HTTP2_FRAME_TYPE_HEADERS) {
      if (flags & HTTP2_FLAGS_HEADERS_END_HEADERS) {
        _state = Http2StreamState::HTTP2_STREAM_STATE_HALF_CLOSED_REMOTE;
      }
    } else if (type == HTTP2_FRAME_TYPE_CONTINUATION) {
      if (flags & HTTP2_FLAGS_CONTINUATION_END_HEADERS) {
        _state = Http2StreamState::HTTP2_STREAM_STATE_HALF_CLOSED_REMOTE;
      }
    } else {
      return false;
    }
    break;

  case Http2StreamState::HTTP2_STREAM_STATE_RESERVED_REMOTE:
    // Currently ATS supports only HTTP/2 server features
    return false;

  case Http2StreamState::HTTP2_STREAM_STATE_HALF_CLOSED_LOCAL:
    if (type == HTTP2_FRAME_TYPE_RST_STREAM || recv_end_stream) {
      _state = Http2StreamState::HTTP2_STREAM_STATE_CLOSED;
    } else {
      // Error, set state closed
      _state = Http2StreamState::HTTP2_STREAM_STATE_CLOSED;
      return false;
    }
    break;

  case Http2StreamState::HTTP2_STREAM_STATE_HALF_CLOSED_REMOTE:
    if (type == HTTP2_FRAME_TYPE_RST_STREAM || send_end_stream) {
      _state = Http2StreamState::HTTP2_STREAM_STATE_CLOSED;
    } else if (type == HTTP2_FRAME_TYPE_HEADERS) { // w/o END_STREAM flag
      // No state change here. Expect a following DATA frame with END_STREAM flag.
      return true;
    } else if (type == HTTP2_FRAME_TYPE_CONTINUATION) { // w/o END_STREAM flag
      // No state change here. Expect a following DATA frame with END_STREAM flag.
      return true;
    } else {
      // Error, set state closed
      _state = Http2StreamState::HTTP2_STREAM_STATE_CLOSED;
      return false;
    }
    break;

  case Http2StreamState::HTTP2_STREAM_STATE_CLOSED:
    // No state changing
    return true;

  default:
    return false;
  }

  Http2StreamDebug("%s", Http2DebugNames::get_state_name(_state));

  return true;
}

VIO *
Http2Stream::do_io_read(Continuation *c, int64_t nbytes, MIOBuffer *buf)
{
  if (buf) {
    read_vio.buffer.writer_for(buf);
  } else {
    read_vio.buffer.clear();
  }

  read_vio.mutex     = c ? c->mutex : this->mutex;
  read_vio.cont      = c;
  read_vio.nbytes    = nbytes;
  read_vio.ndone     = 0;
  read_vio.vc_server = this;
  read_vio.op        = VIO::READ;

  // TODO: re-enable read_vio

  return &read_vio;
}

VIO *
Http2Stream::do_io_write(Continuation *c, int64_t nbytes, IOBufferReader *abuffer, bool owner)
{
  if (abuffer) {
    write_vio.buffer.reader_for(abuffer);
  } else {
    write_vio.buffer.clear();
  }
  write_vio.mutex     = c ? c->mutex : this->mutex;
  write_vio.cont      = c;
  write_vio.nbytes    = nbytes;
  write_vio.ndone     = 0;
  write_vio.vc_server = this;
  write_vio.op        = VIO::WRITE;

  if (c != nullptr && nbytes > 0 && this->is_client_state_writeable()) {
    update_write_request(false);
  } else if (!this->is_client_state_writeable()) {
    // Cannot start a write on a closed stream
    return nullptr;
  }
  return &write_vio;
}

// Initiated from SM
void
Http2Stream::do_io_close(int /* flags */)
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());

  if (!closed) {
    REMEMBER(NO_EVENT, this->reentrancy_count);
    Http2StreamDebug("do_io_close");

    // When we get here, the SM has initiated the shutdown.  Either it received a WRITE_COMPLETE, or it is shutting down.  Any
    // remaining IO operations back to client should be abandoned.  The SM-side buffers backing these operations will be deleted
    // by the time this is called from transaction_done.
    closed = true;

    if (_proxy_ssn && this->is_client_state_writeable()) {
      // Make sure any trailing end of stream frames are sent
      // We will be removed at send_data_frames or closing connection phase
      Http2ClientSession *h2_proxy_ssn = static_cast<Http2ClientSession *>(this->_proxy_ssn);
      SCOPED_MUTEX_LOCK(lock, h2_proxy_ssn->mutex, this_ethread());
      h2_proxy_ssn->connection_state.send_data_frames(this);
    }

    _clear_timers();
    clear_io_events();

    // Wait until transaction_done is called from HttpSM to signal that the TXN_CLOSE hook has been executed
  }
}

/*
 *  HttpSM has called TXN_close hooks.
 */
void
Http2Stream::transaction_done()
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
  super::transaction_done();

  if (!closed) {
    do_io_close(); // Make sure we've been closed.  If we didn't close the _proxy_ssn session better still be open
  }
  ink_release_assert(closed || !static_cast<Http2ClientSession *>(_proxy_ssn)->connection_state.is_state_closed());
  _sm = nullptr;

  if (closed) {
    // Safe to initiate SSN_CLOSE if this is the last stream
    ink_assert(cross_thread_event == nullptr);
    // Schedule the destroy to occur after we unwind here.  IF we call directly, may delete with reference on the stack.
    terminate_stream = true;
    terminate_if_possible();
  }
}

void
Http2Stream::terminate_if_possible()
{
  if (terminate_stream && reentrancy_count == 0) {
    REMEMBER(NO_EVENT, this->reentrancy_count);

    Http2ClientSession *h2_proxy_ssn = static_cast<Http2ClientSession *>(this->_proxy_ssn);
    SCOPED_MUTEX_LOCK(lock, h2_proxy_ssn->mutex, this_ethread());
    THREAD_FREE(this, http2StreamAllocator, this_ethread());
  }
}

// Initiated from the Http2 side
void
Http2Stream::initiating_close()
{
  if (!closed) {
    SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
    REMEMBER(NO_EVENT, this->reentrancy_count);
    Http2StreamDebug("initiating_close");

    // Set the state of the connection to closed
    // TODO - these states should be combined
    closed = true;
    _state = Http2StreamState::HTTP2_STREAM_STATE_CLOSED;

    // leaving the reference to the SM, so we can detach from the SM when we actually destroy
    // _sm = NULL;
    // Leaving reference to client session as well, so we can signal once the
    // TXN_CLOSE has been sent
    // _proxy_ssn = NULL;

    _clear_timers();
    clear_io_events();

    // This should result in do_io_close or release being called.  That will schedule the final
    // kill yourself signal
    // We are sending signals rather than calling the handlers directly to avoid the case where
    // the HttpTunnel handler causes the HttpSM to be deleted on the stack.
    bool sent_write_complete = false;
    if (_sm) {
      // Push out any last IO events
      if (write_vio.cont) {
        SCOPED_MUTEX_LOCK(lock, write_vio.mutex, this_ethread());
        // Are we done?
        if (write_vio.nbytes > 0 && write_vio.nbytes == write_vio.ndone) {
          Http2StreamDebug("handle write from destroy (event=%d)", VC_EVENT_WRITE_COMPLETE);
          write_event = send_tracked_event(write_event, VC_EVENT_WRITE_COMPLETE, &write_vio);
        } else {
          write_event = send_tracked_event(write_event, VC_EVENT_EOS, &write_vio);
          Http2StreamDebug("handle write from destroy (event=%d)", VC_EVENT_EOS);
        }
        sent_write_complete = true;
      }
    }
    // Send EOS to let SM know that we aren't sticking around
    if (_sm && read_vio.cont) {
      // Only bother with the EOS if we haven't sent the write complete
      if (!sent_write_complete) {
        SCOPED_MUTEX_LOCK(lock, read_vio.mutex, this_ethread());
        Http2StreamDebug("send EOS to read cont");
        read_event = send_tracked_event(read_event, VC_EVENT_EOS, &read_vio);
      }
    } else if (!sent_write_complete) {
      // Transaction is already gone or not started. Kill yourself
      terminate_stream = true;
      terminate_if_possible();
    }
  }
}

/* Replace existing event only if the new event is different than the inprogress event */
Event *
Http2Stream::send_tracked_event(Event *event, int send_event, VIO *vio)
{
  if (event != nullptr) {
    if (event->callback_event != send_event) {
      event->cancel();
      event = nullptr;
    }
  }

  if (event == nullptr) {
    REMEMBER(send_event, this->reentrancy_count);
    event = this_ethread()->schedule_imm(this, send_event, vio);
  }

  return event;
}

void
Http2Stream::update_read_request(bool call_update)
{
  if (closed || _proxy_ssn == nullptr || _sm == nullptr || read_vio.mutex == nullptr) {
    return;
  }

  if (!this->_switch_thread_if_not_on_right_thread(VC_EVENT_READ_READY, nullptr)) {
    // Not on the right thread
    return;
  }
  ink_release_assert(this->_thread == this_ethread());

  SCOPED_MUTEX_LOCK(lock, read_vio.mutex, this_ethread());
  if (read_vio.nbytes == 0) {
    return;
  }

  // Try to be smart and only signal if there was additional data
  int send_event = VC_EVENT_READ_READY;
  if (read_vio.ntodo() == 0 || (this->recv_end_stream && this->read_vio.nbytes != INT64_MAX)) {
    send_event = VC_EVENT_READ_COMPLETE;
  }

  int64_t read_avail = this->read_vio.buffer.writer()->max_read_avail();
  if (read_avail > 0 || send_event == VC_EVENT_READ_COMPLETE) {
    if (call_update) { // Safe to call vio handler directly
      _timeout.update_inactivity();
      if (read_vio.cont && this->_sm) {
        read_vio.cont->handleEvent(send_event, &read_vio);
      }
    } else { // Called from do_io_read.  Still setting things up.  Send event
      // to handle this after the dust settles
      read_event = send_tracked_event(read_event, send_event, &read_vio);
    }
  }
}

void
Http2Stream::restart_sending()
{
  if (!this->response_header_done) {
    return;
  }

  IOBufferReader *reader = this->response_get_data_reader();
  if (reader && !reader->is_read_avail_more_than(0)) {
    return;
  }

  if (this->write_vio.mutex && this->write_vio.ntodo() == 0) {
    return;
  }

  this->send_response_body(true);
}

void
Http2Stream::update_write_request(bool call_update)
{
  if (!this->is_client_state_writeable() || closed || _proxy_ssn == nullptr || write_vio.mutex == nullptr ||
      write_vio.get_reader() == nullptr) {
    return;
  }

  if (!this->_switch_thread_if_not_on_right_thread(VC_EVENT_WRITE_READY, nullptr)) {
    // Not on the right thread
    return;
  }
  ink_release_assert(this->_thread == this_ethread());

  Http2ClientSession *h2_proxy_ssn = static_cast<Http2ClientSession *>(this->_proxy_ssn);

  SCOPED_MUTEX_LOCK(lock, write_vio.mutex, this_ethread());

  IOBufferReader *vio_reader = write_vio.get_reader();
  if (write_vio.ntodo() == 0 || !vio_reader->is_read_avail_more_than(0)) {
    return;
  }

  // Process the new data
  if (!this->response_header_done) {
    // Still parsing the response_header
    int bytes_used = 0;
    int state      = this->response_header.parse_resp(&http_parser, vio_reader, &bytes_used, false);
    // HTTPHdr::parse_resp() consumed the vio_reader in above (consumed size is `bytes_used`)
    write_vio.ndone += bytes_used;

    switch (state) {
    case PARSE_RESULT_DONE: {
      this->response_header_done = true;

      // Schedule session shutdown if response header has "Connection: close"
      MIMEField *field = this->response_header.field_find(MIME_FIELD_CONNECTION, MIME_LEN_CONNECTION);
      if (field) {
        int len;
        const char *value = field->value_get(&len);
        if (memcmp(HTTP_VALUE_CLOSE, value, HTTP_LEN_CLOSE) == 0) {
          SCOPED_MUTEX_LOCK(lock, h2_proxy_ssn->mutex, this_ethread());
          if (h2_proxy_ssn->connection_state.get_shutdown_state() == HTTP2_SHUTDOWN_NONE) {
            h2_proxy_ssn->connection_state.set_shutdown_state(HTTP2_SHUTDOWN_NOT_INITIATED, Http2ErrorCode::HTTP2_ERROR_NO_ERROR);
          }
        }
      }

      {
        SCOPED_MUTEX_LOCK(lock, h2_proxy_ssn->mutex, this_ethread());
        // Send the response header back
        h2_proxy_ssn->connection_state.send_headers_frame(this);
      }

      // Roll back states of response header to read final response
      if (this->response_header.expect_final_response()) {
        this->response_header_done = false;
        response_header.destroy();
        response_header.create(HTTP_TYPE_RESPONSE);
        http2_init_pseudo_headers(response_header);
        http_parser_clear(&http_parser);
        http_parser_init(&http_parser);
      }

      this->signal_write_event(call_update);

      if (vio_reader->is_read_avail_more_than(0)) {
        this->_milestones.mark(Http2StreamMilestone::START_TX_DATA_FRAMES);
        this->send_response_body(call_update);
      }
      break;
    }
    case PARSE_RESULT_CONT:
      // Let it ride for next time
      break;
    default:
      break;
    }
  } else {
    this->_milestones.mark(Http2StreamMilestone::START_TX_DATA_FRAMES);
    this->send_response_body(call_update);
  }

  return;
}

void
Http2Stream::signal_read_event(int event)
{
  if (this->read_vio.cont == nullptr || this->read_vio.cont->mutex == nullptr || this->read_vio.op == VIO::NONE) {
    return;
  }

  MUTEX_TRY_LOCK(lock, read_vio.cont->mutex, this_ethread());
  if (lock.is_locked()) {
    _timeout.update_inactivity();
    this->read_vio.cont->handleEvent(event, &this->read_vio);
  } else {
    if (this->_read_vio_event) {
      this->_read_vio_event->cancel();
    }
    this->_read_vio_event = this_ethread()->schedule_in(this, retry_delay, event, &read_vio);
  }
}

void
Http2Stream::signal_write_event(int event)
{
  // Don't signal a write event if in fact nothing was written
  if (this->write_vio.cont == nullptr || this->write_vio.cont->mutex == nullptr || this->write_vio.op == VIO::NONE ||
      this->write_vio.nbytes == 0) {
    return;
  }

  MUTEX_TRY_LOCK(lock, write_vio.cont->mutex, this_ethread());
  if (lock.is_locked()) {
    _timeout.update_inactivity();
    this->write_vio.cont->handleEvent(event, &this->write_vio);
  } else {
    if (this->_write_vio_event) {
      this->_write_vio_event->cancel();
    }
    this->_write_vio_event = this_ethread()->schedule_in(this, retry_delay, event, &write_vio);
  }
}

void
Http2Stream::signal_write_event(bool call_update)
{
  if (this->write_vio.cont == nullptr || this->write_vio.op == VIO::NONE) {
    return;
  }

  if (this->write_vio.get_writer()->write_avail() == 0) {
    return;
  }

  int send_event = this->write_vio.ntodo() == 0 ? VC_EVENT_WRITE_COMPLETE : VC_EVENT_WRITE_READY;

  if (call_update) {
    // Coming from reenable.  Safe to call the handler directly
    if (write_vio.cont && this->_sm) {
      write_vio.cont->handleEvent(send_event, &write_vio);
    }
  } else {
    // Called from do_io_write. Might still be setting up state. Send an event to let the dust settle
    write_event = send_tracked_event(write_event, send_event, &write_vio);
  }
}

bool
Http2Stream::push_promise(URL &url, const MIMEField *accept_encoding)
{
  Http2ClientSession *h2_proxy_ssn = static_cast<Http2ClientSession *>(this->_proxy_ssn);
  SCOPED_MUTEX_LOCK(lock, h2_proxy_ssn->mutex, this_ethread());
  return h2_proxy_ssn->connection_state.send_push_promise_frame(this, url, accept_encoding);
}

void
Http2Stream::send_response_body(bool call_update)
{
  Http2ClientSession *h2_proxy_ssn = static_cast<Http2ClientSession *>(this->_proxy_ssn);
  _timeout.update_inactivity();

  if (Http2::stream_priority_enabled) {
    SCOPED_MUTEX_LOCK(lock, h2_proxy_ssn->mutex, this_ethread());
    h2_proxy_ssn->connection_state.schedule_stream(this);
    // signal_write_event() will be called from `Http2ConnectionState::send_data_frames_depends_on_priority()`
    // when write_vio is consumed
  } else {
    SCOPED_MUTEX_LOCK(lock, h2_proxy_ssn->mutex, this_ethread());
    h2_proxy_ssn->connection_state.send_data_frames(this);
    this->signal_write_event(call_update);
    // XXX The call to signal_write_event can destroy/free the Http2Stream.
    // Don't modify the Http2Stream after calling this method.
  }
}

void
Http2Stream::reenable(VIO *vio)
{
  if (this->_proxy_ssn) {
    if (vio->op == VIO::WRITE) {
      SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
      update_write_request(true);
    } else if (vio->op == VIO::READ) {
      Http2ClientSession *h2_proxy_ssn = static_cast<Http2ClientSession *>(this->_proxy_ssn);
      {
        SCOPED_MUTEX_LOCK(ssn_lock, h2_proxy_ssn->mutex, this_ethread());
        h2_proxy_ssn->connection_state.restart_receiving(this);
      }

      SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());
      update_read_request(true);
    }
  }
}

IOBufferReader *
Http2Stream::response_get_data_reader() const
{
  return write_vio.get_reader();
}

void
Http2Stream::set_active_timeout(ink_hrtime timeout_in)
{
  _timeout.set_active_timeout(timeout_in);
}

void
Http2Stream::set_inactivity_timeout(ink_hrtime timeout_in)
{
  _timeout.set_inactive_timeout(timeout_in);
}

void
Http2Stream::cancel_active_timeout()
{
  _timeout.cancel_active_timeout();
}

void
Http2Stream::cancel_inactivity_timeout()
{
  _timeout.cancel_inactive_timeout();
}

bool
Http2Stream::is_active_timeout_expired(ink_hrtime now)
{
  return _timeout.is_active_timeout_expired(now);
}

bool
Http2Stream::is_inactive_timeout_expired(ink_hrtime now)
{
  return _timeout.is_inactive_timeout_expired(now);
}

void
Http2Stream::clear_io_events()
{
  if (cross_thread_event) {
    cross_thread_event->cancel();
    cross_thread_event = nullptr;
  }

  if (read_event) {
    read_event->cancel();
    read_event = nullptr;
  }

  if (write_event) {
    write_event->cancel();
    write_event = nullptr;
  }

  if (this->_read_vio_event) {
    this->_read_vio_event->cancel();
    this->_read_vio_event = nullptr;
  }

  if (this->_write_vio_event) {
    this->_write_vio_event->cancel();
    this->_write_vio_event = nullptr;
  }
}

//  release and do_io_close are the same for the HTTP/2 protocol
void
Http2Stream::release()
{
  this->do_io_close();
}

void
Http2Stream::increment_transactions_stat()
{
  HTTP2_INCREMENT_THREAD_DYN_STAT(HTTP2_STAT_CURRENT_CLIENT_STREAM_COUNT, _thread);
  HTTP2_INCREMENT_THREAD_DYN_STAT(HTTP2_STAT_TOTAL_CLIENT_STREAM_COUNT, _thread);
}

void
Http2Stream::decrement_transactions_stat()
{
  HTTP2_DECREMENT_THREAD_DYN_STAT(HTTP2_STAT_CURRENT_CLIENT_STREAM_COUNT, _thread);
}

ssize_t
Http2Stream::client_rwnd() const
{
  return this->_client_rwnd;
}

Http2ErrorCode
Http2Stream::increment_client_rwnd(size_t amount)
{
  this->_client_rwnd += amount;

  this->_recent_rwnd_increment[this->_recent_rwnd_increment_index] = amount;
  ++this->_recent_rwnd_increment_index;
  this->_recent_rwnd_increment_index %= this->_recent_rwnd_increment.size();
  double sum = std::accumulate(this->_recent_rwnd_increment.begin(), this->_recent_rwnd_increment.end(), 0.0);
  double avg = sum / this->_recent_rwnd_increment.size();
  if (avg < Http2::min_avg_window_update) {
    return Http2ErrorCode::HTTP2_ERROR_ENHANCE_YOUR_CALM;
  }
  return Http2ErrorCode::HTTP2_ERROR_NO_ERROR;
}

Http2ErrorCode
Http2Stream::decrement_client_rwnd(size_t amount)
{
  this->_client_rwnd -= amount;
  if (this->_client_rwnd < 0) {
    return Http2ErrorCode::HTTP2_ERROR_PROTOCOL_ERROR;
  } else {
    return Http2ErrorCode::HTTP2_ERROR_NO_ERROR;
  }
}

ssize_t
Http2Stream::server_rwnd() const
{
  return this->_server_rwnd;
}

Http2ErrorCode
Http2Stream::increment_server_rwnd(size_t amount)
{
  this->_server_rwnd += amount;
  return Http2ErrorCode::HTTP2_ERROR_NO_ERROR;
}

Http2ErrorCode
Http2Stream::decrement_server_rwnd(size_t amount)
{
  this->_server_rwnd -= amount;
  if (this->_server_rwnd < 0) {
    return Http2ErrorCode::HTTP2_ERROR_PROTOCOL_ERROR;
  } else {
    return Http2ErrorCode::HTTP2_ERROR_NO_ERROR;
  }
}

bool
Http2Stream::_switch_thread_if_not_on_right_thread(int event, void *edata)
{
  if (this->_thread != this_ethread()) {
    SCOPED_MUTEX_LOCK(stream_lock, this->mutex, this_ethread());
    if (cross_thread_event == nullptr) {
      // Send to the right thread
      cross_thread_event = this->_thread->schedule_imm(this, event, edata);
    }
    return false;
  }
  return true;
}

int
Http2Stream::get_transaction_priority_weight() const
{
  return priority_node ? priority_node->weight : 0;
}

int
Http2Stream::get_transaction_priority_dependence() const
{
  if (!priority_node) {
    return -1;
  } else {
    return priority_node->parent ? priority_node->parent->id : 0;
  }
}

int64_t
Http2Stream::read_vio_read_avail()
{
  MIOBuffer *writer = this->read_vio.get_writer();
  if (writer) {
    return writer->max_read_avail();
  }

  return 0;
}

bool
Http2Stream::has_request_body(int64_t content_length, bool is_chunked_set) const
{
  return has_body;
}
