/*
 * Copyright 2014 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <folly/SocketAddress.h>
#include <folly/io/async/DelayedDestruction.h>
#include <folly/io/async/EventBase.h>

namespace folly {

/*
 * flags given by the application for write* calls
 */
enum class WriteFlags : uint32_t {
  NONE = 0x00,
  /*
   * Whether to delay the output until a subsequent non-corked write.
   * (Note: may not be supported in all subclasses or on all platforms.)
   */
  CORK = 0x01,
  /*
   * for a socket that has ACK latency enabled, it will cause the kernel
   * to fire a TCP ESTATS event when the last byte of the given write call
   * will be acknowledged.
   */
  EOR = 0x02,
};

/*
 * union operator
 */
inline WriteFlags operator|(WriteFlags a, WriteFlags b) {
  return static_cast<WriteFlags>(
    static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

/*
 * intersection operator
 */
inline WriteFlags operator&(WriteFlags a, WriteFlags b) {
  return static_cast<WriteFlags>(
    static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

/*
 * exclusion parameter
 */
inline WriteFlags operator~(WriteFlags a) {
  return static_cast<WriteFlags>(~static_cast<uint32_t>(a));
}

/*
 * unset operator
 */
inline WriteFlags unSet(WriteFlags a, WriteFlags b) {
  return a & ~b;
}

/*
 * inclusion operator
 */
inline bool isSet(WriteFlags a, WriteFlags b) {
  return (a & b) == b;
}


/**
 * AsyncTransport defines an asynchronous API for streaming I/O.
 *
 * This class provides an API to for asynchronously waiting for data
 * on a streaming transport, and for asynchronously sending data.
 *
 * The APIs for reading and writing are intentionally asymmetric.  Waiting for
 * data to read is a persistent API: a callback is installed, and is notified
 * whenever new data is available.  It continues to be notified of new events
 * until it is uninstalled.
 *
 * AsyncTransport does not provide read timeout functionality, because it
 * typically cannot determine when the timeout should be active.  Generally, a
 * timeout should only be enabled when processing is blocked waiting on data
 * from the remote endpoint.  For server-side applications, the timeout should
 * not be active if the server is currently processing one or more outstanding
 * requests on this transport.  For client-side applications, the timeout
 * should not be active if there are no requests pending on the transport.
 * Additionally, if a client has multiple pending requests, it will ususally
 * want a separate timeout for each request, rather than a single read timeout.
 *
 * The write API is fairly intuitive: a user can request to send a block of
 * data, and a callback will be informed once the entire block has been
 * transferred to the kernel, or on error.  AsyncTransport does provide a send
 * timeout, since most callers want to give up if the remote end stops
 * responding and no further progress can be made sending the data.
 */
class AsyncTransport : public DelayedDestruction {
 public:
  typedef std::unique_ptr<AsyncTransport, Destructor> UniquePtr;

  /**
   * Close the transport.
   *
   * This gracefully closes the transport, waiting for all pending write
   * requests to complete before actually closing the underlying transport.
   *
   * If a read callback is set, readEOF() will be called immediately.  If there
   * are outstanding write requests, the close will be delayed until all
   * remaining writes have completed.  No new writes may be started after
   * close() has been called.
   */
  virtual void close() = 0;

  /**
   * Close the transport immediately.
   *
   * This closes the transport immediately, dropping any outstanding data
   * waiting to be written.
   *
   * If a read callback is set, readEOF() will be called immediately.
   * If there are outstanding write requests, these requests will be aborted
   * and writeError() will be invoked immediately on all outstanding write
   * callbacks.
   */
  virtual void closeNow() = 0;

  /**
   * Reset the transport immediately.
   *
   * This closes the transport immediately, sending a reset to the remote peer
   * if possible to indicate abnormal shutdown.
   *
   * Note that not all subclasses implement this reset functionality: some
   * subclasses may treat reset() the same as closeNow().  Subclasses that use
   * TCP transports should terminate the connection with a TCP reset.
   */
  virtual void closeWithReset() {
    closeNow();
  }

  /**
   * Perform a half-shutdown of the write side of the transport.
   *
   * The caller should not make any more calls to write() or writev() after
   * shutdownWrite() is called.  Any future write attempts will fail
   * immediately.
   *
   * Not all transport types support half-shutdown.  If the underlying
   * transport does not support half-shutdown, it will fully shutdown both the
   * read and write sides of the transport.  (Fully shutting down the socket is
   * better than doing nothing at all, since the caller may rely on the
   * shutdownWrite() call to notify the other end of the connection that no
   * more data can be read.)
   *
   * If there is pending data still waiting to be written on the transport,
   * the actual shutdown will be delayed until the pending data has been
   * written.
   *
   * Note: There is no corresponding shutdownRead() equivalent.  Simply
   * uninstall the read callback if you wish to stop reading.  (On TCP sockets
   * at least, shutting down the read side of the socket is a no-op anyway.)
   */
  virtual void shutdownWrite() = 0;

  /**
   * Perform a half-shutdown of the write side of the transport.
   *
   * shutdownWriteNow() is identical to shutdownWrite(), except that it
   * immediately performs the shutdown, rather than waiting for pending writes
   * to complete.  Any pending write requests will be immediately failed when
   * shutdownWriteNow() is called.
   */
  virtual void shutdownWriteNow() = 0;

  /**
   * Determine if transport is open and ready to read or write.
   *
   * Note that this function returns false on EOF; you must also call error()
   * to distinguish between an EOF and an error.
   *
   * @return  true iff the transport is open and ready, false otherwise.
   */
  virtual bool good() const = 0;

  /**
   * Determine if the transport is readable or not.
   *
   * @return  true iff the transport is readable, false otherwise.
   */
  virtual bool readable() const = 0;

  /**
   * Determine if the there is pending data on the transport.
   *
   * @return  true iff the if the there is pending data, false otherwise.
   */
  virtual bool isPending() const {
    return readable();
  }
  /**
   * Determine if transport is connected to the endpoint
   *
   * @return  false iff the transport is connected, otherwise true
   */
  virtual bool connecting() const = 0;

  /**
   * Determine if an error has occurred with this transport.
   *
   * @return  true iff an error has occurred (not EOF).
   */
  virtual bool error() const = 0;

  /**
   * Attach the transport to a EventBase.
   *
   * This may only be called if the transport is not currently attached to a
   * EventBase (by an earlier call to detachEventBase()).
   *
   * This method must be invoked in the EventBase's thread.
   */
  virtual void attachEventBase(EventBase* eventBase) = 0;

  /**
   * Detach the transport from its EventBase.
   *
   * This may only be called when the transport is idle and has no reads or
   * writes pending.  Once detached, the transport may not be used again until
   * it is re-attached to a EventBase by calling attachEventBase().
   *
   * This method must be called from the current EventBase's thread.
   */
  virtual void detachEventBase() = 0;

  /**
   * Determine if the transport can be detached.
   *
   * This method must be called from the current EventBase's thread.
   */
  virtual bool isDetachable() const = 0;

  /**
   * Get the EventBase used by this transport.
   *
   * Returns nullptr if this transport is not currently attached to a
   * EventBase.
   */
  virtual EventBase* getEventBase() const = 0;

  /**
   * Set the send timeout.
   *
   * If write requests do not make any progress for more than the specified
   * number of milliseconds, fail all pending writes and close the transport.
   *
   * If write requests are currently pending when setSendTimeout() is called,
   * the timeout interval is immediately restarted using the new value.
   *
   * @param milliseconds  The timeout duration, in milliseconds.  If 0, no
   *                      timeout will be used.
   */
  virtual void setSendTimeout(uint32_t milliseconds) = 0;

  /**
   * Get the send timeout.
   *
   * @return Returns the current send timeout, in milliseconds.  A return value
   *         of 0 indicates that no timeout is set.
   */
  virtual uint32_t getSendTimeout() const = 0;

  /**
   * Get the address of the local endpoint of this transport.
   *
   * This function may throw AsyncSocketException on error.
   *
   * @param address  The local address will be stored in the specified
   *                 SocketAddress.
   */
  virtual void getLocalAddress(folly::SocketAddress* address) const = 0;

  /**
   * Get the address of the remote endpoint to which this transport is
   * connected.
   *
   * This function may throw AsyncSocketException on error.
   *
   * @param address  The remote endpoint's address will be stored in the
   *                 specified SocketAddress.
   */
  virtual void getPeerAddress(folly::SocketAddress* address) const = 0;

  /**
   * @return True iff end of record tracking is enabled
   */
  virtual bool isEorTrackingEnabled() const = 0;

  virtual void setEorTracking(bool track) = 0;

  virtual size_t getAppBytesWritten() const = 0;
  virtual size_t getRawBytesWritten() const = 0;
  virtual size_t getAppBytesReceived() const = 0;
  virtual size_t getRawBytesReceived() const = 0;

 protected:
  virtual ~AsyncTransport() {}
};


} // folly