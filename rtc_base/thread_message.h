/*
 *  Copyright (c) 2020 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#ifndef RTC_BASE_THREAD_MESSAGE_H_
#define RTC_BASE_THREAD_MESSAGE_H_

#include <list>
#include <memory>
#include <utility>

#include "api/scoped_refptr.h"
#include "rtc_base/location.h"
#include "rtc_base/message_handler.h"

namespace rtc {

// Derive from this for specialized data
// App manages lifetime, except when messages are purged

class MessageData {
 public:
  MessageData() {}
  virtual ~MessageData() {}
};

template <class T>
class TypedMessageData : public MessageData {
 public:
  explicit TypedMessageData(const T& data) : data_(data) {}
  const T& data() const { return data_; }
  T& data() { return data_; }

 private:
  T data_;
};

// Like TypedMessageData, but for pointers that require a delete.
template <class T>
class ScopedMessageData : public MessageData {
 public:
  explicit ScopedMessageData(std::unique_ptr<T> data)
      : data_(std::move(data)) {}
  // Deprecated.
  // TODO(deadbeef): Remove this once downstream applications stop using it.
  explicit ScopedMessageData(T* data) : data_(data) {}
  // Deprecated.
  // TODO(deadbeef): Returning a reference to a unique ptr? Why. Get rid of
  // this once downstream applications stop using it, then rename inner_data to
  // just data.
  const std::unique_ptr<T>& data() const { return data_; }
  std::unique_ptr<T>& data() { return data_; }

  const T& inner_data() const { return *data_; }
  T& inner_data() { return *data_; }

 private:
  std::unique_ptr<T> data_;
};

// Like ScopedMessageData, but for reference counted pointers.
template <class T>
class ScopedRefMessageData : public MessageData {
 public:
  explicit ScopedRefMessageData(T* data) : data_(data) {}
  const scoped_refptr<T>& data() const { return data_; }
  scoped_refptr<T>& data() { return data_; }

 private:
  scoped_refptr<T> data_;
};

template <class T>
inline MessageData* WrapMessageData(const T& data) {
  return new TypedMessageData<T>(data);
}

template <class T>
inline const T& UseMessageData(MessageData* data) {
  return static_cast<TypedMessageData<T>*>(data)->data();
}

template <class T>
class DisposeData : public MessageData {
 public:
  explicit DisposeData(T* data) : data_(data) {}
  virtual ~DisposeData() { delete data_; }

 private:
  T* data_;
};

const uint32_t MQID_ANY = static_cast<uint32_t>(-1);
const uint32_t MQID_DISPOSE = static_cast<uint32_t>(-2);

// No destructor

struct Message {
  Message()
      : phandler(nullptr), message_id(0), pdata(nullptr), ts_sensitive(0) {}
  inline bool Match(MessageHandler* handler, uint32_t id) const {
    return (handler == nullptr || handler == phandler) &&
           (id == MQID_ANY || id == message_id);
  }
  Location posted_from;
  MessageHandler* phandler;
  uint32_t message_id;
  MessageData* pdata;
  int64_t ts_sensitive;
};

typedef std::list<Message> MessageList;

// DelayedMessage goes into a priority queue, sorted by trigger time.  Messages
// with the same trigger time are processed in num_ (FIFO) order.

class DelayedMessage {
 public:
  DelayedMessage(int64_t delay,
                 int64_t trigger,
                 uint32_t num,
                 const Message& msg)
      : cmsDelay_(delay), msTrigger_(trigger), num_(num), msg_(msg) {}

  bool operator<(const DelayedMessage& dmsg) const {
    return (dmsg.msTrigger_ < msTrigger_) ||
           ((dmsg.msTrigger_ == msTrigger_) && (dmsg.num_ < num_));
  }

  int64_t cmsDelay_;  // for debugging
  int64_t msTrigger_;
  uint32_t num_;
  Message msg_;
};
}  // namespace rtc
#endif  // RTC_BASE_THREAD_MESSAGE_H_
