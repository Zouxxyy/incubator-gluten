<!--
Licensed to the Apache Software Foundation (ASF) under one or more
contributor license agreements.  See the NOTICE file distributed with
this work for additional information regarding copyright ownership.
The ASF licenses this file to You under the Apache License, Version 2.0
(the "License"); you may not use this file except in compliance with
the License.  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Native JNI lifecycle tests

The attachment tests are registered in the normal core native test build, and
the Folly thread-factory tests in the Velox native test build. The standalone
CMake entry point can also run these tests without building a backend or Spark.
It builds the production `JniThreadAttachment.cc`, not a test implementation.
No Paimon native library is needed.

Automatic exit cleanup in this patch is enabled on Linux with glibc, validated
with HotSpot Java 8 and 17. Other platforms retain the original attach behavior.
In particular, Darwin may reclaim compiler/JVM thread-local storage during its
pthread destructor passes, so deferring a JNI call to the final pass must not be
assumed portable. Lifecycle-specific tests are skipped outside Linux/glibc.

## Standalone build

Requires a C++20 compiler, CMake, GTest, pthreads, and a JDK with `libjvm`.
Run from the repository root with `JAVA_HOME` set to the JDK being tested:

```sh
cmake -S gluten-ut/native -B cpp/build/jni-tests
cmake --build cpp/build/jni-tests -j 4
ctest --test-dir cpp/build/jni-tests --output-on-failure
```

Optional integrations:

- `-DGLUTEN_JNI_TEST_FOLLY=ON` exercises real Folly CPU/IO executors and
  `folly::ThreadLocal` exit callbacks. Requires installed Folly and gflags CMake
  packages. If that installation exports static gflags through Folly but shared
  gflags through glog, `-DGLUTEN_JNI_TEST_SHARED_GFLAGS=ON` selects the existing
  shared gflags target for this standalone test only.
- `-DGLUTEN_JNI_TEST_HDFS=ON` exercises a real libhdfs file close in pthread
  cleanup, with both Gluten-first and libhdfs-first attachment. Set `HADOOP_HOME`
  to a Hadoop distribution with native libhdfs and export the Hadoop client jar
  classpath before running CTest, for example:

  ```sh
  export CLASSPATH="$($HADOOP_HOME/bin/hadoop classpath --glob)"
  ```

  The integration only creates an empty temporary local file and accesses it
  through `file:///`; it needs no NameNode, credentials, or remote data.

Use separate build directories when switching JDKs. CTest discovers individual
cases; `--repeat until-fail:25` repeats each case in a fresh test process.

## Ownership and exit ordering

`JniThreadFactory` and `withJniThreadLifecycle` register the cleanup key at
Gluten-owned worker **entry**, before work can establish any JNI attachment.
Registration alone does not attach an unused worker. The helper borrows existing
Java/library attachments and records only successful attachments it makes on
registered workers. Unregistered third-party threads retain their existing
lifecycle responsibility; new Gluten thread creators must opt in explicitly.

Attachments remain valid across tasks, iterator destruction, C++ thread-local
destructors, and the preceding pthread destructor passes. Detachment is deferred
to Gluten's callback in the final `PTHREAD_DESTRUCTOR_ITERATIONS` pass so normal
Folly thread-local and libhdfs cleanup can still use JNI. Registering only at
the first JNI call is insufficient: that call can itself occur during pthread
cleanup, after some of those passes have already elapsed.

This is not a universal ordering mechanism for arbitrary third-party destructors
that continually re-arm their keys. JNI-dependent callbacks must complete before
Gluten's final callback. After that callback, attempts to create a new attachment
are rejected with `JNI_ERR` because no cleanup pass remains; already-existing
attachments can still be borrowed. The tests cover this boundary as well as
normal Folly/libhdfs cleanup and preservation of pre-existing attachments.

The embedding JVM must outlive its native workers. The test fixtures join workers
before destroying their JVM.
