// @simsys/stream: the simulator's output contract (docs/scenario-schema.md, "simsys run --stream"
// and "The output contract") in TypeScript, shared by the precompute and the page.
export * from "./messages";
export { DATASET_VERSION, FORMAT_VERSION, parseLine, type Parsed } from "./parse";
export { splitLines } from "./lines";
export type { DatasetSource } from "./source";
export { MemorySource } from "./memory-source";
export { FileSource } from "./file-source";
export { bucketBalance, DatasetStore, type Problem, type StoreStatus } from "./store";
