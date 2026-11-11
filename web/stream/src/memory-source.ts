import type { DatasetSource } from "./source";

// The lines of a stream held in memory: the tests' source, and any page that already has them.
export class MemorySource implements DatasetSource {
  readonly label = "memory";

  constructor(private readonly all: readonly string[]) {}

  async *lines(signal?: AbortSignal): AsyncGenerator<string> {
    for (const line of this.all) {
      if (signal?.aborted) return;
      // Hand control back between lines, as a network source would: the page can draw while a
      // long in-memory stream is read.
      await Promise.resolve();
      yield line;
    }
  }
}
