#pragma once

namespace resource_slot
{
/** Perform all registered access and set NodeHandles.
 *
 * MUST be executed in every frame before dabfg::run_nodes()
 *
 * \param storage_id RESERVED for future use
 */
void resolve_access(unsigned storage_id = 0);
} // namespace resource_slot