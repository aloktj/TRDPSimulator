#include "trdp_simulator/trdp_stack_adapter.hpp"

#include <memory>

namespace trdp_sim {

std::unique_ptr<TrdpStackAdapter> create_stub_trdp_stack_adapter();
#ifdef TRDPSIM_WITH_TRDP
std::unique_ptr<TrdpStackAdapter> create_real_trdp_stack_adapter();
#endif

std::unique_ptr<TrdpStackAdapter> create_trdp_stack_adapter()
{
#ifdef TRDPSIM_WITH_TRDP
    return create_real_trdp_stack_adapter();
#else
    return create_stub_trdp_stack_adapter();
#endif
}

}  // namespace trdp_sim
