#!/bin/bash

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

# Packages LLVM for inclusing in the Kudu thirdparty build.
#
# Our llvm tarball includes clang, extra clang tools, lld, and compiler-rt.
#
# See http://clang.llvm.org/get_started.html and http://lld.llvm.org/ for
# details on how they're laid out in the llvm tarball.
#
# Summary:
# 1.  Unpack the llvm tarball
# 9.  Unpack the IWYU tarball in tools/clang/tools/include-what-you-use
# 10. Create new tarball from the resulting source tree
#
# Usage:
#  $ env VERSION=11.0.0 IWYU_VERSION=0.15 thirdparty/package-llvm.sh

set -eux

if [[ -z "${VERSION:-}" ]] || [[ -z "${IWYU_VERSION:-}" ]]; then
  echo "Error: VERSION and IWYU_VERSION environment variables must be set and non-empty"
  exit 1
fi

# cleanup previous junk from a failed attempt
if [[ "${1:-}" == "-f" ]]; then
    rm -rf llvm-project-$VERSION.src llvm-$VERSION.src include-what-you-use tar xf \
      llvm-$VERSION.src \
      llvm-$VERSION-iwyu-$IWYU_VERSION.src
fi

wget https://github.com/llvm/llvm-project/releases/download/llvmorg-$VERSION/llvm-project-$VERSION.src.tar.xz
tar xf llvm-project-$VERSION.src.tar.xz
mv llvm-project-$VERSION.src llvm-$VERSION.src
rm llvm-project-$VERSION.src.tar.xz

IWYU_TAR=include-what-you-use-${IWYU_VERSION}.src.tar.gz
wget https://include-what-you-use.org/downloads/$IWYU_TAR
tar xf $IWYU_TAR
rm $IWYU_TAR

mv include-what-you-use llvm-$VERSION.src/

# We dont use these, and it makes the zip much smaller
rm -rf llvm-$VERSION.src/llvm/test
rm -rf llvm-$VERSION.src/clang/test
rm -rf llvm-$VERSION.src/mlir


tar -cJf llvm-$VERSION-iwyu-$IWYU_VERSION.src.tar.xz llvm-$VERSION.src
