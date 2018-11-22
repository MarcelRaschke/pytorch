#!/bin/bash

# Required environment variable: $BUILD_ENVIRONMENT
# (This is set by default in the Docker images we build, so you don't
# need to set it yourself.

COMPACT_JOB_NAME="${BUILD_ENVIRONMENT}-test"
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

echo "Testing pytorch"

if [ -n "${IN_CIRCLECI}" ]; then
  if [[ "$BUILD_ENVIRONMENT" == *-xenial-cuda9-* ]]; then
    # TODO: move this to Docker
    sudo apt-get -qq update
    sudo apt-get -qq install --allow-downgrades --allow-change-held-packages libnccl-dev=2.2.13-1+cuda9.0 libnccl2=2.2.13-1+cuda9.0
  fi

  if [[ "$BUILD_ENVIRONMENT" == *-xenial-cuda8-* ]] || [[ "$BUILD_ENVIRONMENT" == *-xenial-cuda9-cudnn7-py2* ]]; then
    # TODO: move this to Docker
    sudo apt-get -qq update
    sudo apt-get -qq install --allow-downgrades --allow-change-held-packages openmpi-bin libopenmpi-dev
    sudo apt-get -qq install --no-install-recommends openssh-client openssh-server
    sudo mkdir -p /var/run/sshd
  fi
fi

# --user breaks ppc64le builds and these packages are already in ppc64le docker
if [[ "$BUILD_ENVIRONMENT" != *ppc64le* ]]; then
  # JIT C++ extensions require ninja.
  pip install -q ninja --user
  # ninja is installed in /var/lib/jenkins/.local/bin
  export PATH="/var/lib/jenkins/.local/bin:$PATH"

  # TODO: move this to Docker
  pip install -q hypothesis --user
fi

# DANGER WILL ROBINSON.  The LD_PRELOAD here could cause you problems
# if you're not careful.  Check this if you made some changes and the
# ASAN test is not working
if [[ "$BUILD_ENVIRONMENT" == *asan* ]]; then
    export ASAN_OPTIONS=detect_leaks=0:symbolize=1:strict_init_order=true
    # We suppress the vptr volation, since we have separate copies of
    # libprotobuf in both libtorch.so and libcaffe2.so, and it causes
    # the following problem:
    #    test_cse (__main__.TestJit) ... torch/csrc/jit/export.cpp:622:38:
    #        runtime error: member call on address ... which does not point
    #        to an object of type 'google::protobuf::MessageLite'
    #        ...: note: object is of type 'onnx_torch::ModelProto'
    #
    # This problem should be solved when libtorch.so and libcaffe2.so are
    # merged.
    export UBSAN_OPTIONS=print_stacktrace=1:suppressions=$PWD/ubsan.supp
    export PYTORCH_TEST_WITH_ASAN=1
    export PYTORCH_TEST_WITH_UBSAN=1
    # TODO: Figure out how to avoid hard-coding these paths
    export ASAN_SYMBOLIZER_PATH=/usr/lib/llvm-5.0/bin/llvm-symbolizer
    export LD_PRELOAD=/usr/lib/llvm-5.0/lib/clang/5.0.0/lib/linux/libclang_rt.asan-x86_64.so
    # Increase stack size, because ASAN red zones use more stack
    ulimit -s 81920

    function get_exit_code() {
      set +e
      "$@"
      retcode=$?
      set -e
      return $retcode
    }
    (cd test && python -c "import torch")
    echo "The next three invocations are expected to crash; if they don't that means ASAN/UBSAN is misconfigured"
    (cd test && ! get_exit_code python -c "import torch; torch._C._crash_if_csrc_asan(3)")
    (cd test && ! get_exit_code python -c "import torch; torch._C._crash_if_csrc_ubsan(0)")
    (cd test && ! get_exit_code python -c "import torch; torch._C._crash_if_aten_asan(3)")
fi

if [[ "$BUILD_ENVIRONMENT" == *rocm* ]]; then
  export PYTORCH_TEST_WITH_ROCM=1
  export LANG=C.UTF-8
  export LC_ALL=C.UTF-8
fi

if [[ "${JOB_BASE_NAME}" == *-NO_AVX-* ]]; then
  export ATEN_CPU_CAPABILITY=default
elif [[ "${JOB_BASE_NAME}" == *-NO_AVX2-* ]]; then
  export ATEN_CPU_CAPABILITY=avx
fi

test_python_nn() {
  time python test/run_test.py --include nn --verbose
}

test_python_all_except_nn() {
  time python test/run_test.py --exclude nn --verbose
}

test_aten() {
  # Test ATen
  # The following test(s) of ATen have already been skipped by caffe2 in rocm environment:
  # scalar_tensor_test, basic, native_test
  if ([[ "$BUILD_ENVIRONMENT" != *asan* ]] && [[ "$BUILD_ENVIRONMENT" != *rocm* ]]); then
    echo "Running ATen tests with pytorch lib"
    TORCH_LIB_PATH=$(python -c "import site; print(site.getsitepackages()[0])")/torch/lib
    # NB: the ATen test binaries don't have RPATH set, so it's necessary to
    # put the dynamic libraries somewhere were the dynamic linker can find them.
    # This is a bit of a hack.
    if [[ "$BUILD_ENVIRONMENT" == *ppc64le* ]]; then
      SUDO=sudo
    fi

    ${SUDO} ln -s "$TORCH_LIB_PATH"/libc10* build/bin
    ${SUDO} ln -s "$TORCH_LIB_PATH"/libcaffe2* build/bin
    ${SUDO} ln -s "$TORCH_LIB_PATH"/libmkldnn* build/bin
    ${SUDO} ln -s "$TORCH_LIB_PATH"/libnccl* build/bin

    ls build/bin
    aten/tools/run_tests.sh build/bin
  fi
}

test_torchvision() {
  rm -rf ninja

  echo "Installing torchvision at branch master"
  rm -rf vision
  # TODO: This git clone is bad, it means pushes to torchvision can break
  # PyTorch CI
  git clone https://github.com/pytorch/vision --quiet
  pushd vision
  # python setup.py install with a tqdm dependency is broken in the
  # Travis Python nightly (but not in latest Python nightlies, so
  # this should be a transient requirement...)
  # See https://github.com/pytorch/pytorch/issues/7525
  #time python setup.py install
  pip install -q --user .
  popd
}

test_libtorch() {
  if [[ "$BUILD_TEST_LIBTORCH" == "1" ]]; then
     echo "Testing libtorch"
     CPP_BUILD="$PWD/../cpp-build"
     if [[ "$BUILD_ENVIRONMENT" == *cuda* ]]; then
       "$CPP_BUILD"/caffe2/bin/test_jit
     else
       "$CPP_BUILD"/caffe2/bin/test_jit "[cpu]"
     fi
     python tools/download_mnist.py --quiet -d mnist
     OMP_NUM_THREADS=2 "$CPP_BUILD"/caffe2/bin/test_api
  fi
}

test_custom_script_ops() {
  if [[ "$BUILD_TEST_LIBTORCH" == "1" ]]; then
    echo "Testing custom script operators"
    CUSTOM_OP_BUILD="$PWD/../custom-op-build"
    pushd test/custom_operator
    cp -r "$CUSTOM_OP_BUILD" build
    # Run tests Python-side and export a script module.
    python test_custom_ops.py -v
    python model.py --export-script-module=model.pt
    # Run tests C++-side and load the exported script module.
    build/test_custom_ops ./model.pt
    popd
  fi
}

if [ -z "${JOB_BASE_NAME}" ] || [[ "${JOB_BASE_NAME}" == *-test ]]; then
  test_torchvision
  test_python_nn
  test_python_all_except_nn
  test_aten
  test_libtorch
  test_custom_script_ops
else
  if [[ "${JOB_BASE_NAME}" == *-test1 ]]; then
    test_torchvision
    test_python_nn
  elif [[ "${JOB_BASE_NAME}" == *-test2 ]]; then
    test_torchvision
    test_python_all_except_nn
    test_aten
    test_libtorch
    test_custom_script_ops
  fi
fi
