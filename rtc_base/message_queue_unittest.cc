/*
 *  Copyright 2004 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "rtc_base/thread.h"

#include <functional>

#include "rtc_base/atomic_ops.h"
#include "rtc_base/bind.h"
#include "rtc_base/event.h"
#include "rtc_base/gunit.h"
#include "rtc_base/logging.h"
#include "rtc_base/null_socket_server.h"
#include "rtc_base/ref_count.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/task_utils/to_queued_task.h"
#include "rtc_base/thread.h"
#include "rtc_base/time_utils.h"

namespace rtc {
namespace {

using ::webrtc::ToQueuedTask;

class MessageQueueTest : public ::testing::Test, public Thread {
 public:
  MessageQueueTest() : Thread(SocketServer::CreateDefault(), true) {}
  bool IsLocked_Worker() {
    if (!CritForTest()->TryEnter()) {
      return true;
    }
    CritForTest()->Leave();
    return false;
  }
  bool IsLocked() {
    // We have to do this on a worker thread, or else the TryEnter will
    // succeed, since our critical sections are reentrant.
    std::unique_ptr<Thread> worker(Thread::CreateWithSocketServer());
    worker->Start();
    return worker->Invoke<bool>(
        RTC_FROM_HERE, rtc::Bind(&MessageQueueTest::IsLocked_Worker, this));
  }
};

struct DeletedLockChecker {
  DeletedLockChecker(MessageQueueTest* test, bool* was_locked, bool* deleted)
      : test(test), was_locked(was_locked), deleted(deleted) {}
  ~DeletedLockChecker() {
    *deleted = true;
    *was_locked = test->IsLocked();
  }
  MessageQueueTest* test;
  bool* was_locked;
  bool* deleted;
};

static void DelayedPostsWithIdenticalTimesAreProcessedInFifoOrder(Thread* q) {
  EXPECT_TRUE(q != nullptr);
  int64_t now = TimeMillis();
  q->PostAt(RTC_FROM_HERE, now, nullptr, 3);
  q->PostAt(RTC_FROM_HERE, now - 2, nullptr, 0);
  q->PostAt(RTC_FROM_HERE, now - 1, nullptr, 1);
  q->PostAt(RTC_FROM_HERE, now, nullptr, 4);
  q->PostAt(RTC_FROM_HERE, now - 1, nullptr, 2);

  Message msg;
  for (size_t i = 0; i < 5; ++i) {
    memset(&msg, 0, sizeof(msg));
    EXPECT_TRUE(q->Get(&msg, 0));
    EXPECT_EQ(i, msg.message_id);
  }

  EXPECT_FALSE(q->Get(&msg, 0));  // No more messages
}

TEST_F(MessageQueueTest,
       DelayedPostsWithIdenticalTimesAreProcessedInFifoOrder) {
  Thread q(SocketServer::CreateDefault(), true);
  DelayedPostsWithIdenticalTimesAreProcessedInFifoOrder(&q);

  NullSocketServer nullss;
  Thread q_nullss(&nullss, true);
  DelayedPostsWithIdenticalTimesAreProcessedInFifoOrder(&q_nullss);
}

TEST_F(MessageQueueTest, DisposeNotLocked) {
  bool was_locked = true;
  bool deleted = false;
  DeletedLockChecker* d = new DeletedLockChecker(this, &was_locked, &deleted);
  Dispose(d);
  Message msg;
  EXPECT_FALSE(Get(&msg, 0));
  EXPECT_TRUE(deleted);
  EXPECT_FALSE(was_locked);
}

class DeletedMessageHandler : public MessageHandler {
 public:
  explicit DeletedMessageHandler(bool* deleted) : deleted_(deleted) {}
  ~DeletedMessageHandler() override { *deleted_ = true; }
  void OnMessage(Message* msg) override {}

 private:
  bool* deleted_;
};

TEST_F(MessageQueueTest, DiposeHandlerWithPostedMessagePending) {
  bool deleted = false;
  DeletedMessageHandler* handler = new DeletedMessageHandler(&deleted);
  // First, post a dispose.
  Dispose(handler);
  // Now, post a message, which should *not* be returned by Get().
  Post(RTC_FROM_HERE, handler, 1);
  Message msg;
  EXPECT_FALSE(Get(&msg, 0));
  EXPECT_TRUE(deleted);
}

// Ensure that ProcessAllMessageQueues does its essential function; process
// all messages (both delayed and non delayed) up until the current time, on
// all registered message queues.
TEST(ThreadManager, ProcessAllMessageQueues) {
  Event entered_process_all_message_queues(true, false);
  auto a = Thread::CreateWithSocketServer();
  auto b = Thread::CreateWithSocketServer();
  a->Start();
  b->Start();

  volatile int messages_processed = 0;
  auto incrementer = [&messages_processed,
                      &entered_process_all_message_queues] {
    // Wait for event as a means to ensure Increment doesn't occur outside
    // of ProcessAllMessageQueues. The event is set by a message posted to
    // the main thread, which is guaranteed to be handled inside
    // ProcessAllMessageQueues.
    entered_process_all_message_queues.Wait(Event::kForever);
    AtomicOps::Increment(&messages_processed);
  };
  auto event_signaler = [&entered_process_all_message_queues] {
    entered_process_all_message_queues.Set();
  };

  // Post messages (both delayed and non delayed) to both threads.
  a->PostTask(ToQueuedTask(incrementer));
  b->PostTask(ToQueuedTask(incrementer));
  a->PostDelayedTask(ToQueuedTask(incrementer), 0);
  b->PostDelayedTask(ToQueuedTask(incrementer), 0);
  rtc::Thread::Current()->PostTask(ToQueuedTask(event_signaler));

  ThreadManager::ProcessAllMessageQueuesForTesting();
  EXPECT_EQ(4, AtomicOps::AcquireLoad(&messages_processed));
}

// Test that ProcessAllMessageQueues doesn't hang if a thread is quitting.
TEST(ThreadManager, ProcessAllMessageQueuesWithQuittingThread) {
  auto t = Thread::CreateWithSocketServer();
  t->Start();
  t->Quit();
  ThreadManager::ProcessAllMessageQueuesForTesting();
}

// Test that ProcessAllMessageQueues doesn't hang if a queue clears its
// messages.
TEST(ThreadManager, ProcessAllMessageQueuesWithClearedQueue) {
  Event entered_process_all_message_queues(true, false);
  auto t = Thread::CreateWithSocketServer();
  t->Start();

  auto clearer = [&entered_process_all_message_queues] {
    // Wait for event as a means to ensure Clear doesn't occur outside of
    // ProcessAllMessageQueues. The event is set by a message posted to the
    // main thread, which is guaranteed to be handled inside
    // ProcessAllMessageQueues.
    entered_process_all_message_queues.Wait(Event::kForever);
    rtc::Thread::Current()->Clear(nullptr);
  };
  auto event_signaler = [&entered_process_all_message_queues] {
    entered_process_all_message_queues.Set();
  };

  // Post messages (both delayed and non delayed) to both threads.
  t->PostTask(RTC_FROM_HERE, clearer);
  rtc::Thread::Current()->PostTask(RTC_FROM_HERE, event_signaler);
  ThreadManager::ProcessAllMessageQueuesForTesting();
}

class RefCountedHandler : public MessageHandler, public rtc::RefCountInterface {
 public:
  void OnMessage(Message* msg) override {}
};

class EmptyHandler : public MessageHandler {
 public:
  void OnMessage(Message* msg) override {}
};

TEST(ThreadManager, ClearReentrant) {
  std::unique_ptr<Thread> t(Thread::Create());
  EmptyHandler handler;
  RefCountedHandler* inner_handler(
      new rtc::RefCountedObject<RefCountedHandler>());
  // When the empty handler is destroyed, it will clear messages queued for
  // itself. The message to be cleared itself wraps a MessageHandler object
  // (RefCountedHandler) so this will cause the message queue to be cleared
  // again in a re-entrant fashion, which previously triggered a DCHECK.
  // The inner handler will be removed in a re-entrant fashion from the
  // message queue of the thread while the outer handler is removed, verifying
  // that the iterator is not invalidated in "MessageQueue::Clear".
  t->Post(RTC_FROM_HERE, inner_handler, 0);
  t->Post(RTC_FROM_HERE, &handler, 0,
          new ScopedRefMessageData<RefCountedHandler>(inner_handler));
}

}  // namespace
}  // namespace rtc
