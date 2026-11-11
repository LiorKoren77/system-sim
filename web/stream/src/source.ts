// Pattern: Strategy — where a dataset's lines come from. 5b has a static file (FileSource) and an
// in-memory list (MemorySource, for tests); later tiers add a live stream from a server and simsys
// compiled to WebAssembly in a worker (phase 5b spec §12). The page reads only the DatasetStore the
// source fills, so a new source changes no view.
//
// A source yields the stream's text lines, not parsed messages: the contract's rules (parse.ts) are
// then applied in one place, the store, whatever the transport.
export interface DatasetSource {
  readonly label: string; // for messages: the file's URL, "memory", ...
  lines(signal?: AbortSignal): AsyncIterable<string>;
}
