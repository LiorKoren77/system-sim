import { splitLines } from "./lines";
import type { DatasetSource } from "./source";

// The first two bytes of every gzip file (RFC 1952 §2.3.1).
const gzipMagic = [0x1f, 0x8b] as const;

// A dataset served as a static file, `<id>.ndjson.gz` (phase 5b spec §8). The body is read as it
// downloads: inflated with the browser's DecompressionStream, decoded as UTF-8, cut into lines, so
// the page fills while the file arrives, as it would from a live stream. A server that sets
// `Content-Encoding: gzip` on .gz files has the browser inflate it before we see it; the body then
// does not start with gzip's magic bytes and is read as text.
export class FileSource implements DatasetSource {
  constructor(readonly label: string) {}

  async *lines(signal?: AbortSignal): AsyncGenerator<string> {
    const response = await fetch(this.label, signal ? { signal } : {});
    if (!response.ok || response.body === null) {
      throw new Error(`${this.label}: ${response.status} ${response.statusText}`);
    }
    const bytes = await withFirstChunk(response.body);
    const text = bytes.gzipped
      ? bytes.stream.pipeThrough(new DecompressionStream("gzip"))
      : bytes.stream;
    yield* splitLines(text.pipeThrough(new TextDecoderStream()));
  }
}

// The body again, whole, and whether it starts with gzip's magic bytes: the first chunk is read to
// look, then put back in front of the rest.
async function withFirstChunk(
  body: ReadableStream<Uint8Array<ArrayBuffer>>,
): Promise<{ gzipped: boolean; stream: ReadableStream<Uint8Array<ArrayBuffer>> }> {
  const reader = body.getReader();
  const first = await reader.read();
  const head: Uint8Array<ArrayBuffer> = first.done ? new Uint8Array() : first.value;
  const gzipped = head[0] === gzipMagic[0] && head[1] === gzipMagic[1];
  const stream = new ReadableStream<Uint8Array<ArrayBuffer>>({
    start(controller) {
      if (head.length > 0) controller.enqueue(head);
    },
    async pull(controller) {
      const next = await reader.read();
      if (next.done) controller.close();
      else controller.enqueue(next.value);
    },
    cancel(reason) {
      return reader.cancel(reason);
    },
  });
  return { gzipped, stream };
}
