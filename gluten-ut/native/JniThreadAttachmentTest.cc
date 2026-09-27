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

#include <gtest/gtest.h>
#include <limits.h>
#include <pthread.h>
#include <atomic>
#include <thread>
#include <vector>

namespace gluten {
namespace {

template <typename Function>
std::thread startWorker(Function&& function) {
  return std::thread(withJniThreadLifecycle(std::forward<Function>(function)));
}

class FakeJvm {
 public:
  FakeJvm() {
    functions.reserved0 = this;
    functions.GetEnv = [](JavaVM* vm, void** out, jint) -> jint {
      auto& self = from(vm);
      *out = nullptr;
      if (self.getEnvError != JNI_OK) {
        return self.getEnvError;
      }
      if (!attached) {
        return JNI_EDETACHED;
      }
      *out = &self.env;
      return JNI_OK;
    };
    functions.AttachCurrentThreadAsDaemon = [](JavaVM* vm, void** out, void*) -> jint {
      auto& self = from(vm);
      ++self.attachCalls;
      if (self.attachError != JNI_OK) {
        return self.attachError;
      }
      attached = true;
      *out = &self.env;
      return JNI_OK;
    };
    functions.DetachCurrentThread = [](JavaVM* vm) -> jint {
      ++from(vm).detachCalls;
      attached = false;
      return JNI_OK;
    };
  }

  static FakeJvm& from(JavaVM* vm) {
    return *static_cast<FakeJvm*>(vm->functions->reserved0);
  }

  static thread_local bool attached;
  JNIInvokeInterface_ functions{};
  JavaVM vm{&functions};
  JNIEnv env{};
  std::atomic<int> attachCalls{0};
  std::atomic<int> detachCalls{0};
  jint getEnvError = JNI_OK;
  jint attachError = JNI_OK;
};

thread_local bool FakeJvm::attached = false;

TEST(JniThreadAttachment, detachOnceWhenWorkerExits) {
  FakeJvm jvm;
  auto worker = startWorker([&] {
    for (int i = 0; i < 10; ++i) {
      JNIEnv* env = nullptr;
      ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(&jvm.vm, &env), JNI_OK);
      EXPECT_EQ(env, &jvm.env);
      EXPECT_EQ(jvm.detachCalls, 0);
    }
  });
  worker.join();
  EXPECT_EQ(jvm.attachCalls, 1);
  EXPECT_EQ(jvm.detachCalls, 1);
}

TEST(JniThreadAttachment, preserveExistingAttachment) {
  FakeJvm jvm;
  auto worker = startWorker([&] {
    FakeJvm::attached = true;
    JNIEnv* env = nullptr;
    ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(&jvm.vm, &env), JNI_OK);
    EXPECT_EQ(env, &jvm.env);
  });
  worker.join();
  EXPECT_EQ(jvm.attachCalls, 0);
  EXPECT_EQ(jvm.detachCalls, 0);
}

TEST(JniThreadAttachment, failedAttachmentHasNoCleanup) {
  FakeJvm jvm;
  jvm.attachError = JNI_ERR;
  auto worker = startWorker([&] {
    JNIEnv* env = nullptr;
    EXPECT_EQ(getOrAttachCurrentThreadAsDaemon(&jvm.vm, &env), JNI_ERR);
    EXPECT_EQ(env, nullptr);
  });
  worker.join();
  EXPECT_EQ(jvm.attachCalls, 1);
  EXPECT_EQ(jvm.detachCalls, 0);
}

TEST(JniThreadAttachment, propagateGetEnvError) {
  FakeJvm jvm;
  jvm.getEnvError = JNI_EVERSION;
  auto worker = startWorker([&] {
    JNIEnv* env = nullptr;
    EXPECT_EQ(getOrAttachCurrentThreadAsDaemon(&jvm.vm, &env), JNI_EVERSION);
    EXPECT_EQ(env, nullptr);
  });
  worker.join();
  EXPECT_EQ(jvm.attachCalls, 0);
  EXPECT_EQ(jvm.detachCalls, 0);
}

TEST(JniThreadAttachment, explicitDetachIsNotRepeated) {
  FakeJvm jvm;
  auto worker = startWorker([&] {
    JNIEnv* env = nullptr;
    ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(&jvm.vm, &env), JNI_OK);
    ASSERT_EQ(jvm.vm.DetachCurrentThread(), JNI_OK);
  });
  worker.join();
  EXPECT_EQ(jvm.attachCalls, 1);
  EXPECT_EQ(jvm.detachCalls, 1);
}

TEST(JniThreadAttachment, reattachedWorkerIsCleanedUp) {
  FakeJvm jvm;
  auto worker = startWorker([&] {
    JNIEnv* env = nullptr;
    ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(&jvm.vm, &env), JNI_OK);
    ASSERT_EQ(jvm.vm.DetachCurrentThread(), JNI_OK);
    ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(&jvm.vm, &env), JNI_OK);
  });
  worker.join();
  EXPECT_EQ(jvm.attachCalls, 2);
  EXPECT_EQ(jvm.detachCalls, 2);
}

TEST(JniThreadAttachment, independentWorkerLifetimes) {
  FakeJvm jvm;
  std::vector<std::thread> workers;
  for (int i = 0; i < 32; ++i) {
    workers.push_back(startWorker([&] {
      JNIEnv* env = nullptr;
      EXPECT_EQ(getOrAttachCurrentThreadAsDaemon(&jvm.vm, &env), JNI_OK);
    }));
  }
  for (auto& worker : workers) {
    worker.join();
  }
  EXPECT_EQ(jvm.attachCalls, 32);
  EXPECT_EQ(jvm.detachCalls, 32);
}

TEST(JniThreadAttachment, registrationDoesNotAttachUnusedWorkers) {
  FakeJvm jvm;
  auto worker = startWorker([] { initializeNativeThreadJni(); });
  worker.join();
  EXPECT_EQ(jvm.attachCalls, 0);
  EXPECT_EQ(jvm.detachCalls, 0);
}

TEST(JniThreadAttachment, unmanagedThreadsKeepTheirExistingLifecycle) {
  FakeJvm jvm;
  std::thread worker([&] {
    JNIEnv* env = nullptr;
    ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(&jvm.vm, &env), JNI_OK);
  });
  worker.join();
  EXPECT_EQ(jvm.attachCalls, 1);
  EXPECT_EQ(jvm.detachCalls, 0);
}

class JniThreadAttachmentJvmTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    JavaVMInitArgs args{};
    args.version = JNI_VERSION_1_8;
    ASSERT_EQ(JNI_CreateJavaVM(&vm_, reinterpret_cast<void**>(&env_), &args), JNI_OK);
    auto localThreadClass = env_->FindClass("java/lang/Thread");
    threadClass_ = static_cast<jclass>(env_->NewGlobalRef(localThreadClass));
    env_->DeleteLocalRef(localThreadClass);
    ASSERT_NE(threadClass_, nullptr);
    currentThread_ = env_->GetStaticMethodID(threadClass_, "currentThread", "()Ljava/lang/Thread;");
    isAlive_ = env_->GetMethodID(threadClass_, "isAlive", "()Z");
    ASSERT_NE(currentThread_, nullptr);
    ASSERT_NE(isAlive_, nullptr);
  }

  static void TearDownTestSuite() {
    if (vm_ != nullptr) {
      env_->DeleteGlobalRef(threadClass_);
      EXPECT_EQ(vm_->DestroyJavaVM(), JNI_OK);
    }
  }

  static jobject captureThread(JNIEnv* env) {
    auto localThread = env->CallStaticObjectMethod(threadClass_, currentThread_);
    auto thread = env->NewGlobalRef(localThread);
    env->DeleteLocalRef(localThread);
    return thread;
  }

  static void checkExited(jobject thread) {
    ASSERT_NE(thread, nullptr);
    EXPECT_FALSE(env_->CallBooleanMethod(thread, isAlive_));
    env_->DeleteGlobalRef(thread);
  }

  struct Cleanup {
    JavaVM* vm;
    JNIEnv* cachedEnv = nullptr;
    bool ran = false;
    bool detach = false;

    void run() {
      ran = true;
      JNIEnv* env = nullptr;
      // Check before using the cached env so a broken cleanup order fails an assertion,
      // rather than dereferencing a freed JNIEnv and crashing the test process.
      ASSERT_EQ(vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8), JNI_OK);
      ASSERT_EQ(env, cachedEnv);
      auto value = env->NewStringUTF("JNI is still usable during thread cleanup");
      ASSERT_NE(value, nullptr);
      env->DeleteLocalRef(value);
      if (detach) {
        ASSERT_EQ(vm->DetachCurrentThread(), JNI_OK);
      }
    }
  };

  static JavaVM* vm_;
  static JNIEnv* env_;
  static jclass threadClass_;
  static jmethodID currentThread_;
  static jmethodID isAlive_;
};

JavaVM* JniThreadAttachmentJvmTest::vm_ = nullptr;
JNIEnv* JniThreadAttachmentJvmTest::env_ = nullptr;
jclass JniThreadAttachmentJvmTest::threadClass_ = nullptr;
jmethodID JniThreadAttachmentJvmTest::currentThread_ = nullptr;
jmethodID JniThreadAttachmentJvmTest::isAlive_ = nullptr;

TEST_F(JniThreadAttachmentJvmTest, exitedWorkersAreNotAliveInJvm) {
  std::vector<jobject> threads(32, nullptr);
  for (auto& thread : threads) {
    auto worker = startWorker([&] {
      JNIEnv* env = nullptr;
      ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(vm_, &env), JNI_OK);
      thread = captureThread(env);
    });
    worker.join();
  }
  for (auto thread : threads) {
    checkExited(thread);
  }
}

TEST_F(JniThreadAttachmentJvmTest, preserveJavaThreadAndRepeatedCallbacks) {
  JNIEnv* existingEnv = nullptr;
  ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(vm_, &existingEnv), JNI_OK);
  EXPECT_EQ(existingEnv, env_);
  jobject thread = nullptr;
  auto worker = startWorker([&] {
    JNIEnv* cachedEnv = nullptr;
    ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(vm_, &cachedEnv), JNI_OK);
    thread = captureThread(cachedEnv);
    for (int i = 0; i < 32; ++i) {
      JNIEnv* env = nullptr;
      ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(vm_, &env), JNI_OK);
      EXPECT_EQ(env, cachedEnv);
      EXPECT_TRUE(env->CallBooleanMethod(thread, isAlive_));
      auto value = cachedEnv->NewStringUTF("another callback on the same worker");
      ASSERT_NE(value, nullptr);
      cachedEnv->DeleteLocalRef(value);
    }
  });
  worker.join();
  checkExited(thread);
  EXPECT_EQ(vm_->GetEnv(reinterpret_cast<void**>(&existingEnv), JNI_VERSION_1_8), JNI_OK);
  EXPECT_EQ(existingEnv, env_);
}

TEST_F(JniThreadAttachmentJvmTest, cppThreadLocalCleanupCanStillUseJni) {
  Cleanup cleanup{vm_};
  jobject thread = nullptr;
  auto worker = startWorker([&] {
    struct BeforeAttachment {
      Cleanup* cleanup;
      ~BeforeAttachment() {
        cleanup->run();
      }
    };
    // Construct before attaching: a C++ thread_local detacher would be destroyed
    // first, invalidating the env before this older thread_local object's cleanup.
    thread_local BeforeAttachment beforeAttachment{&cleanup};
    ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(vm_, &cleanup.cachedEnv), JNI_OK);
    thread = captureThread(cleanup.cachedEnv);
  });
  worker.join();
  EXPECT_TRUE(cleanup.ran);
  checkExited(thread);
}

TEST_F(JniThreadAttachmentJvmTest, pthreadCleanupCanStillUseJni) {
  Cleanup cleanup{vm_};
  pthread_key_t key{};
  bool keyCreated = false;
  jobject thread = nullptr;
  auto worker = startWorker([&] {
    ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(vm_, &cleanup.cachedEnv), JNI_OK);
    thread = captureThread(cleanup.cachedEnv);
    ASSERT_EQ(pthread_key_create(&key, [](void* value) { static_cast<Cleanup*>(value)->run(); }), 0);
    keyCreated = true;
    ASSERT_EQ(pthread_setspecific(key, &cleanup), 0);
  });
  worker.join();
  EXPECT_TRUE(cleanup.ran);
  if (keyCreated) {
    EXPECT_EQ(pthread_key_delete(key), 0);
  }
  checkExited(thread);
}

TEST_F(JniThreadAttachmentJvmTest, anotherLibraryMayDetachDuringPthreadCleanup) {
  Cleanup cleanup{vm_};
  cleanup.detach = true;
  pthread_key_t key{};
  bool keyCreated = false;
  jobject thread = nullptr;
  auto worker = startWorker([&] {
    ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(vm_, &cleanup.cachedEnv), JNI_OK);
    thread = captureThread(cleanup.cachedEnv);
    ASSERT_EQ(pthread_key_create(&key, [](void* value) { static_cast<Cleanup*>(value)->run(); }), 0);
    keyCreated = true;
    ASSERT_EQ(pthread_setspecific(key, &cleanup), 0);
  });
  worker.join();
  EXPECT_TRUE(cleanup.ran);
  if (keyCreated) {
    EXPECT_EQ(pthread_key_delete(key), 0);
  }
  checkExited(thread);
}

TEST_F(JniThreadAttachmentJvmTest, firstAttachmentDuringPthreadCleanupIsReleased) {
  // Allocate Gluten's key before the callback key, including when this test is
  // run alone. The first destructor pass can already have visited Gluten's key
  // by the time a later callback attaches for the first time.
  auto primeKey = startWorker([&] {
    JNIEnv* env = nullptr;
    ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(vm_, &env), JNI_OK);
  });
  primeKey.join();
  pthread_key_t key{};
  jobject thread = nullptr;
  ASSERT_EQ(
      pthread_key_create(
          &key,
          [](void* value) {
            JNIEnv* env = nullptr;
            ASSERT_EQ(getOrAttachCurrentThreadAsDaemon(vm_, &env), JNI_OK);
            *static_cast<jobject*>(value) = captureThread(env);
          }),
      0);
  auto worker = startWorker([&] { ASSERT_EQ(pthread_setspecific(key, &thread), 0); });
  worker.join();
  EXPECT_EQ(pthread_key_delete(key), 0);
  checkExited(thread);
}

TEST_F(JniThreadAttachmentJvmTest, lateDestructorPassCannotLeaveANewAttachmentBehind) {
  auto primeKey = startWorker([] {});
  primeKey.join();
  for (int attachPass = 2; attachPass <= PTHREAD_DESTRUCTOR_ITERATIONS; ++attachPass) {
    struct LateCallback {
      pthread_key_t key{};
      int remaining;
      jint status = JNI_ERR;
      jobject thread = nullptr;
    } callback{{}, attachPass};
    ASSERT_EQ(
        pthread_key_create(
            &callback.key,
            [](void* value) {
              auto* callback = static_cast<LateCallback*>(value);
              if (--callback->remaining > 0) {
                ASSERT_EQ(pthread_setspecific(callback->key, callback), 0);
                return;
              }
              JNIEnv* env = nullptr;
              callback->status = getOrAttachCurrentThreadAsDaemon(vm_, &env);
              if (callback->status == JNI_OK) {
                callback->thread = captureThread(env);
              } else {
                EXPECT_EQ(env, nullptr);
              }
            }),
        0);
    auto worker = startWorker([&] { ASSERT_EQ(pthread_setspecific(callback.key, &callback), 0); });
    worker.join();
    EXPECT_EQ(pthread_key_delete(callback.key), 0);
    if (attachPass < PTHREAD_DESTRUCTOR_ITERATIONS) {
      EXPECT_EQ(callback.status, JNI_OK);
    }
    // POSIX does not specify key ordering within the final pass. An attachment
    // must either be released by the pending finalizer or rejected if it ran.
    if (callback.status == JNI_OK) {
      checkExited(callback.thread);
    } else {
      EXPECT_EQ(callback.status, JNI_ERR);
      EXPECT_EQ(attachPass, PTHREAD_DESTRUCTOR_ITERATIONS);
    }
  }
}

} // namespace
} // namespace gluten
