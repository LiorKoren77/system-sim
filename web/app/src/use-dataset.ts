import { DatasetStore, type DatasetSource } from "@simsys/stream";
import { useCallback, useEffect, useMemo, useSyncExternalStore } from "react";

// The store of the dataset `source` delivers, re-rendering the caller as frames arrive. A new
// source (another scenario chosen) gets a new store, and the old one stops reading (AbortController).
export function useDataset(source: DatasetSource | null): DatasetStore | null {
  const store = useMemo(() => (source === null ? null : new DatasetStore()), [source]);
  useEffect(() => {
    if (source === null || store === null) return;
    const controller = new AbortController();
    void store.consume(source, controller.signal);
    // The effect's cleanup runs when `source` changes or the component goes away.
    return () => {
      controller.abort();
    };
  }, [source, store]);
  // useSyncExternalStore (React 18): subscribes to an outside store and re-renders when the
  // snapshot (the store's version, bumped at each notification) changes.
  // A new subscribe function makes React unsubscribe and subscribe again, so it is kept across
  // renders (useCallback) and changes only with the store: one subscription per dataset, not one
  // per animation frame.
  const subscribe = useCallback(
    (onChange: () => void) => store?.subscribe(onChange) ?? (() => undefined),
    [store],
  );
  useSyncExternalStore(subscribe, () => store?.version ?? -1);
  return store;
}
