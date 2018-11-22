#include <ATen/cuda/detail/CUDAGuardImpl.h>

namespace at {
namespace cuda {
namespace impl {

constexpr DeviceType CUDAGuardImpl::static_type;

C10_REGISTER_GUARD_IMPL(CUDA, CUDAGuardImpl);

}}} // namespace at::cuda::detail
