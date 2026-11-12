// When For you's list follows the controls: the filter waits for chip toggles and year typing to
// settle, and a model change applies at once.
import { useEffect, useState } from 'react';
import type { ModelName } from '../../lib/models';

/** Chip toggles and year typing settle this long before the list is fetched (recommend calls are
 * rate limited). */
export const FILTER_DEBOUNCE_MS = 350;

/** The model and filter the list should show. The filter (as its query string, see filterParams)
 * waits FILTER_DEBOUNCE_MS for the chips and typing to settle. A model change applies at once and
 * takes the pending filter with it, so a model change during the wait sends one request, not two. */
export function useSettledView(model: ModelName, filterKey: string): { model: ModelName; filterKey: string } {
  const [settled, setSettled] = useState({ model, filterKey });
  if (settled.model !== model) setSettled({ model, filterKey });
  useEffect(() => {
    if (settled.filterKey === filterKey) return;
    const timer = setTimeout(() => setSettled((s) => ({ ...s, filterKey })), FILTER_DEBOUNCE_MS);
    return () => clearTimeout(timer);
  }, [settled.filterKey, filterKey]);
  return settled;
}
