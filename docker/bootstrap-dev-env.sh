#!/bin/bash
##########################################################
#
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
#
# This script handles bootstrapping a base OS for
# the Apache Kudu base docker images.
#
##########################################################

set -xe
set -o pipefail

# Install the prerequisite libraries, if they are not installed.
# CentOS/RHEL
if [[ -f "/usr/bin/yum" ]]; then
  # Update the repo.
  yum update -y

  if [ -e /etc/os-release ]; then
    source /etc/os-release
    OS_MAJOR_VERSION=$(echo $VERSION_ID | cut -f1 -d.)
  else
    echo "Unable to get RHEL version"
    exit 1
  fi

  # Install core build libraries.
  yum install -y \
    autoconf \
    automake \
    chrony \
    chrpath \
    cyrus-sasl-devel \
    cyrus-sasl-gssapi \
    cyrus-sasl-plain \
    flex \
    gcc \
    gcc-c++ \
    gdb \
    git \
    java-1.8.0-openjdk-devel \
    krb5-server \
    krb5-workstation \
    libtool \
    lsof \
    make \
    openssl-devel \
    patch \
    pkgconfig \
    rsync \
    sudo \
    unzip \
    vim-common \
    which \
    wget


  if [[ "$OS_MAJOR_VERSION" -ge "9" ]]; then
    yum install -y  krb5-libs krb5-devel
  fi

  if [[ "$OS_MAJOR_VERSION" -lt "9" ]]; then
    yum install -y  redhat-lsb-core
  fi

  # Install exta impala packages for the impala images. They are nominal in size.
  # --no-install-recommends keeps the install smaller
  yum install -y \
    libffi-devel \
    lzo-devel \
    tzdata

  # We need to enable the PowerTools repository on versions 8.0 and newer
  # to install the ninja-build package.
  yum install -y 'dnf-command(config-manager)'
  if yum repolist all | grep -q  codeready-builder-for-rhel-$OS_MAJOR_VERSION-rhui-rpms; then
    # public cloud
    yum config-manager --set-enabled codeready-builder-for-rhel-$OS_MAJOR_VERSION-rhui-rpms
  else
    yum config-manager --set-enabled codeready-builder-for-rhel-$OS_MAJOR_VERSION-$(arch)-rpms
  fi

  # Install libraries often used for Kudu development and build performance.

  if [[ "$OS_MAJOR_VERSION" -lt "9" ]]; then
    yum install -y epel-release
  fi
  yum install -y \
    ccache \
    cmake \
    ninja-build \
    vim

  # Install docs build libraries.
  # Note: Uncomment to include in your dev images. These are excluded to reduce image size and build time.
  # yum install -y \
  #  doxygen \
  #  gem \
  #  graphviz \
  #  ruby-devel \
  #  zlib-devel

  yum groupinstall -y "Development Tools"

  # Reduce the image size by cleaning up after the install.
  yum clean all
  rm -rf /var/cache/yum /tmp/* /var/tmp/*
# Ubuntu/Debian
elif [[ -f "/usr/bin/apt-get" ]]; then
  # Ensure the Debian frontend is noninteractive.
  export DEBIAN_FRONTEND=noninteractive

  # Update the repo.
  apt-get update -y

  source /etc/os-release

  if dpkg --compare-versions "$VERSION_ID" ge "22.04"
  then
    apt-get install -y --no-install-recommends python3 python-is-python3
  else
    apt-get install -y --no-install-recommends python
  fi
  # Install core build libraries.
  # --no-install-recommends keeps the install smaller
  apt-get install -y --no-install-recommends \
    autoconf \
    automake \
    chrony \
    chrpath \
    curl \
    flex \
    g++ \
    gcc \
    gdb \
    git \
    krb5-admin-server \
    krb5-kdc \
    krb5-user \
    libkrb5-dev \
    libsasl2-dev \
    libsasl2-modules \
    libsasl2-modules-gssapi-mit \
    libssl-dev \
    libtool \
    lsb-release \
    lsof \
    make \
    openssl \
    patch \
    pkg-config \
    rsync \
    sudo \
    unzip \
    vim-common \
    wget

  # Install exta impala packages for the impala images. They are nominal in size.
  # --no-install-recommends keeps the install smaller
  apt-get install -y --no-install-recommends \
    libffi-dev \
    liblzo2-2 \
    tzdata

  # Install libraries often used for Kudu development and build performance.
  apt-get install -y --no-install-recommends \
    ccache \
    cmake \
    ninja-build \
    vim

  # Install docs build libraries.
  # Note: Uncomment to include in your dev images. These are excluded to reduce image size and build time.
  # apt-get install -y --no-install-recommends \
  #  doxygen \
  #  gem \
  #  graphviz \
  #  ruby-dev \
  #  xsltproc \
  #  zlib1g-dev

  # Reduce the image size by cleaning up after the install.
  apt-get clean
  rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

  unset DEBIAN_FRONTEND
  # OpenSUSE/SLES
elif [[ -f "/usr/bin/zypper" ]]; then
  # Update the repo.
  zypper update -y

  zypper install -y \
    autoconf \
    automake \
    chrony \
    chrpath \
    cyrus-sasl-devel \
    cyrus-sasl-gssapi \
    cyrus-sasl-plain \
    curl \
    flex \
    gcc \
    gcc-c++ \
    gdb \
    git \
    gzip \
    hostname \
    krb5-devel \
    krb5-server \
    libtool \
    lsb-release \
    lsof \
    make \
    openssl-devel \
    patch \
    pkg-config \
    python \
    rsync \
    sudo \
    unzip \
    vim \
    which \
    wget

  # Install extra impala packages for the impala images. They are nominal in size.
  # TODO(ghenke): tzdata equivalent package. This is not an issue given we currently
  # only build the Impala images with in CentOS 7.
  zypper install -y \
    libffi-devel \
    liblzo2-2

  # Install libraries often used for Kudu development and build performance.
  zypper install -y \
    ccache \
    cmake \
    ninja

  # Reduce the image size by cleaning up after the install.
  zypper clean --all
  rm -rf /var/lib/zypp/* /tmp/* /var/tmp/*
else
  echo "Unsupported OS"
  exit 1
fi
