/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <pty.h>

#include <gtest/gtest.h>

#include <pthread.h>
#include <sys/ioctl.h>

#include <atomic>

#include <android-base/file.h>

#include "utils.h"

TEST(pty, openpty) {
  int master, slave;
  char name[32];
  struct winsize w = { 123, 456, 9999, 999 };
  ASSERT_EQ(0, openpty(&master, &slave, name, NULL, &w));
  ASSERT_NE(-1, master);
  ASSERT_NE(-1, slave);
  ASSERT_NE(master, slave);

  char tty_name[32];
  ASSERT_EQ(0, ttyname_r(slave, tty_name, sizeof(tty_name)));
  ASSERT_STREQ(tty_name, name);

  struct winsize w_actual;
  ASSERT_EQ(0, ioctl(slave, TIOCGWINSZ, &w_actual));
  ASSERT_EQ(w_actual.ws_row, w.ws_row);
  ASSERT_EQ(w_actual.ws_col, w.ws_col);
  ASSERT_EQ(w_actual.ws_xpixel, w.ws_xpixel);
  ASSERT_EQ(w_actual.ws_ypixel, w.ws_ypixel);

  close(master);
  close(slave);
}

TEST(pty, forkpty) {
  pid_t sid = getsid(0);

  int master;
  pid_t pid = forkpty(&master, NULL, NULL, NULL);
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // We're the child.
    ASSERT_NE(sid, getsid(0));
    _exit(0);
  }

  ASSERT_EQ(sid, getsid(0));

  AssertChildExited(pid, 0);

  close(master);
}

struct PtyReader_28979140_Arg {
 int slave_fd;
 uint32_t data_count;
 bool finished;
 std::atomic<bool> matched;
};

static void PtyReader_28979140(PtyReader_28979140_Arg* arg) {
  arg->finished = false;
  cpu_set_t cpus;
  ASSERT_EQ(0, sched_getaffinity(0, sizeof(cpu_set_t), &cpus));
  CPU_CLR(0, &cpus);
  ASSERT_EQ(0, sched_setaffinity(0, sizeof(cpu_set_t), &cpus));

  uint32_t counter = 0;
  while (counter <= arg->data_count) {
    char buf[4096];  // Use big buffer to read to hit the bug more easily.
    size_t to_read = std::min(sizeof(buf), (arg->data_count + 1 - counter) * sizeof(uint32_t));
    ASSERT_TRUE(android::base::ReadFully(arg->slave_fd, buf, to_read));
    size_t num_of_value = to_read / sizeof(uint32_t);
    uint32_t* p = reinterpret_cast<uint32_t*>(buf);
    while (num_of_value-- > 0) {
      if (*p++ != counter++) {
        arg->matched = false;
      }
    }
  }
  close(arg->slave_fd);
  arg->finished = true;
}

TEST(pty, bug_28979140) {
  // This test is to test a kernel bug, which uses a lock free ring-buffer to
  // pass data through a raw pty, but missing necessary memory barriers.
  if (sysconf(_SC_NPROCESSORS_ONLN) == 1) {
    GTEST_LOG_(INFO) << "This test tests bug happens only on multiprocessors.";
    return;
  }
  constexpr uint32_t TEST_DATA_COUNT = 200000;

  // 1. Open raw pty.
  int master;
  int slave;
  ASSERT_EQ(0, openpty(&master, &slave, nullptr, nullptr, nullptr));
  termios tattr;
  ASSERT_EQ(0, tcgetattr(slave, &tattr));
  cfmakeraw(&tattr);
  ASSERT_EQ(0, tcsetattr(slave, TCSADRAIN, &tattr));

  // 2. Create thread for slave reader.
  pthread_t thread;
  PtyReader_28979140_Arg arg;
  arg.slave_fd = slave;
  arg.data_count = TEST_DATA_COUNT;
  arg.matched = true;
  ASSERT_EQ(0, pthread_create(&thread, nullptr,
                              reinterpret_cast<void*(*)(void*)>(PtyReader_28979140),
                              &arg));

  // 3. Make master thread and slave thread running on different cpus:
  // master thread uses cpu 0, and slave thread uses other cpus.
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  CPU_SET(0, &cpus);
  ASSERT_EQ(0, sched_setaffinity(0, sizeof(cpu_set_t), &cpus));

  // 4. Send data to slave.
  uint32_t counter = 0;
  while (counter <= TEST_DATA_COUNT) {
    ASSERT_TRUE(android::base::WriteFully(master, &counter, sizeof(counter)));
    ASSERT_TRUE(arg.matched) << "failed at count = " << counter;
    counter++;
  }
  ASSERT_EQ(0, pthread_join(thread, nullptr));
  ASSERT_TRUE(arg.finished);
  ASSERT_TRUE(arg.matched);
  close(master);
}
