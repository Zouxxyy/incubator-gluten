/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "jni/JniThreadAttachment.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <hdfs.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <string>
#include <thread>

#if !defined(__linux__) || !defined(__GLIBC__)
TEST(JniHdfsThreadCleanupTest, requiresGlibcThreadExitOrdering) {
  GTEST_SKIP() << "Automatic JNI thread-exit cleanup is only enabled on Linux/glibc";
}
#else

namespace gluten {
namespace {

TEST(JniHdfsThreadCleanup, localFilesCloseBeforeOwnedAttachmentIsReleased) {
  const char* classPath = std::getenv("CLASSPATH");
  ASSERT_NE(classPath, nullptr) << "Set CLASSPATH to the Hadoop client jars";
  std::string classPathOption = std::string("-Djava.class.path=") + classPath;
  JavaVMOption option{};
  option.optionString = classPathOption.data();
  JavaVMInitArgs args{};
  args.version = JNI_VERSION_1_8;
  args.nOptions = 1;
  args.options = &option;
  JavaVM* vm = nullptr;
  JNIEnv* env = nullptr;
  ASSERT_EQ(JNI_CreateJavaVM(&vm, reinterpret_cast<void**>(&env), &args), JNI_OK);
  auto localClass = env->FindClass("java/lang/Thread");
  auto threadClass = static_cast<jclass>(env->NewGlobalRef(localClass));
  env->DeleteLocalRef(localClass);
  auto currentThread = env->GetStaticMethodID(threadClass, "currentThread", "()Ljava/lang/Thread;");
  auto isAlive = env->GetMethodID(threadClass, "isAlive", "()Z");

  char path[] = "/tmp/gluten-jni-hdfs-XXXXXX";
  const int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  EXPECT_EQ(close(fd), 0);
  struct FileCleanup {
    hdfsFS fs = nullptr;
    hdfsFile file = nullptr;
    bool ran = false;
  };
  // Install the file cleanup before libhdfs first creates its own TLS key, as
  // with a Folly ThreadLocal that closes a cached HdfsFile on worker exit.
  pthread_key_t fileKey;
  ASSERT_EQ(
      pthread_key_create(
          &fileKey,
          [](void* value) {
            auto* cleanup = static_cast<FileCleanup*>(value);
            cleanup->ran = true;
            EXPECT_EQ(hdfsCloseFile(cleanup->fs, cleanup->file), 0);
            EXPECT_EQ(hdfsDisconnect(cleanup->fs), 0);
          }),
      0);

  for (bool glutenAttachesFirst : {true, false}) {
    FileCleanup cleanup;
    jobject thread = nullptr;
    auto worker = std::thread(withJniThreadLifecycle([&] {
      JNIEnv* workerEnv = nullptr;
      if (glutenAttachesFirst) {
        ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(vm, &workerEnv), JNI_OK);
      }
      // Uses the real libhdfs JNI/TLS implementation, but only a local file://
      // filesystem: no NameNode, credentials or network service is required.
      cleanup.fs = hdfsConnect("file:///", 0);
      ASSERT_NE(cleanup.fs, nullptr);
      cleanup.file = hdfsOpenFile(cleanup.fs, path, O_RDONLY, 0, 0, 0);
      ASSERT_NE(cleanup.file, nullptr);
      if (!glutenAttachesFirst) {
        ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(vm, &workerEnv), JNI_OK);
      }
      auto local = workerEnv->CallStaticObjectMethod(threadClass, currentThread);
      thread = workerEnv->NewGlobalRef(local);
      workerEnv->DeleteLocalRef(local);
      ASSERT_EQ(pthread_setspecific(fileKey, &cleanup), 0);
    }));
    worker.join();
    EXPECT_TRUE(cleanup.ran);
    if (thread != nullptr) {
      EXPECT_FALSE(env->CallBooleanMethod(thread, isAlive));
      env->DeleteGlobalRef(thread);
    } else {
      ADD_FAILURE() << "Worker did not reach the JNI callback";
    }
  }
  EXPECT_EQ(pthread_key_delete(fileKey), 0);
  EXPECT_EQ(unlink(path), 0);
  env->DeleteGlobalRef(threadClass);
  EXPECT_EQ(vm->DestroyJavaVM(), JNI_OK);
}

} // namespace
} // namespace gluten

#endif
