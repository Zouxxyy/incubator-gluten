# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to You under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License.  You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.16)
project(gluten_jni_tests LANGUAGES CXX)

# The same test is registered by cpp/core/tests in a full native build. This
# standalone entry point needs only a JDK, GTest and pthreads.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(JNI REQUIRED)
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
include(GoogleTest)
enable_testing()

add_executable(
  jni_thread_attachment_test JniThreadAttachmentTest.cc
                             ../../cpp/core/jni/JniThreadAttachment.cc)
target_include_directories(
  jni_thread_attachment_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp/core
                                     ${JNI_INCLUDE_DIRS})
target_link_libraries(
  jni_thread_attachment_test PRIVATE GTest::gtest GTest::gtest_main
                                     Threads::Threads ${JAVA_JVM_LIBRARY})
gtest_discover_tests(jni_thread_attachment_test)

option(GLUTEN_JNI_TEST_FOLLY "Also test the native Folly thread factory" OFF)
option(GLUTEN_JNI_TEST_SHARED_GFLAGS
       "Use shared gflags when the installed Folly and glog exports disagree"
       OFF)
if(GLUTEN_JNI_TEST_FOLLY)
  find_package(gflags CONFIG REQUIRED)
  find_package(folly CONFIG REQUIRED)
  if(GLUTEN_JNI_TEST_SHARED_GFLAGS AND TARGET gflags_shared)
    # Match glog's shared gflags dependency instead of loading two flag
    # registries.
    foreach(FOLLY_TARGET Folly::folly Folly::folly_deps)
      if(TARGET ${FOLLY_TARGET})
        get_target_property(FOLLY_LINK_LIBS ${FOLLY_TARGET}
                            INTERFACE_LINK_LIBRARIES)
        if(FOLLY_LINK_LIBS MATCHES "gflags_static")
          list(TRANSFORM FOLLY_LINK_LIBS REPLACE "gflags_static"
                                                 "gflags_shared")
          set_target_properties(
            ${FOLLY_TARGET} PROPERTIES INTERFACE_LINK_LIBRARIES
                                       "${FOLLY_LINK_LIBS}")
        endif()
      endif()
    endforeach()
  endif()
  add_executable(
    jni_thread_factory_test JniThreadFactoryTest.cc
                            ../../cpp/core/jni/JniThreadAttachment.cc)
  target_include_directories(
    jni_thread_factory_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp/core
                                    ${JNI_INCLUDE_DIRS})
  target_link_libraries(
    jni_thread_factory_test PRIVATE Folly::folly GTest::gtest GTest::gtest_main
                                    Threads::Threads ${JAVA_JVM_LIBRARY})
  gtest_discover_tests(jni_thread_factory_test)
endif()

option(GLUTEN_JNI_TEST_HDFS
       "Also test real libhdfs TLS cleanup with local files" OFF)
if(GLUTEN_JNI_TEST_HDFS)
  find_path(HDFS_INCLUDE_DIR hdfs.h HINTS "$ENV{HADOOP_HOME}/include" REQUIRED)
  find_library(HDFS_LIBRARY hdfs HINTS "$ENV{HADOOP_HOME}/lib/native" REQUIRED)
  add_executable(
    jni_hdfs_thread_cleanup_test JniHdfsThreadCleanupTest.cc
                                 ../../cpp/core/jni/JniThreadAttachment.cc)
  target_include_directories(
    jni_hdfs_thread_cleanup_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp/core ${JNI_INCLUDE_DIRS}
            ${HDFS_INCLUDE_DIR})
  target_link_libraries(
    jni_hdfs_thread_cleanup_test
    PRIVATE GTest::gtest GTest::gtest_main Threads::Threads ${JAVA_JVM_LIBRARY}
            ${HDFS_LIBRARY})
  gtest_discover_tests(jni_hdfs_thread_cleanup_test)
endif()
