import { gzipSync } from "node:zlib";
import { afterEach, describe, expect, it, vi } from "vitest";
import { FileSource } from "../src/file-source";
import { DatasetStore } from "../src/store";
import { helloText } from "./support/hello";
import { byteStream } from "./support/streams";

function serve(body: Uint8Array, status = 200, statusText = "OK", chunkSize = 100): void {
  vi.stubGlobal(
    "fetch",
    vi.fn(() => Promise.resolve(new Response(byteStream(body, chunkSize), { status, statusText }))),
  );
}

async function load(url: string): Promise<DatasetStore> {
  const store = new DatasetStore();
  await store.consume(new FileSource(url));
  return store;
}

afterEach(() => {
  vi.unstubAllGlobals();
});

describe("a dataset read from a static file", () => {
  it("inflates a gzipped file as it arrives", async () => {
    serve(gzipSync(helloText));
    const store = await load("datasets/hello.ndjson.gz");
    expect(store.status).toBe("ready");
    expect(store.frameCount).toBe(2);
  });

  it("knows a gzipped file even when the network delivers its first byte alone", async () => {
    serve(gzipSync(helloText), 200, "OK", 1);
    const store = await load("datasets/hello.ndjson.gz");
    expect(store.status).toBe("ready");
    expect(store.frameCount).toBe(2);
  });

  it("reads a body the server already inflated (Content-Encoding: gzip) as text", async () => {
    serve(new TextEncoder().encode(helloText));
    const store = await load("datasets/hello.ndjson.gz");
    expect(store.status).toBe("ready");
  });

  it("calls a download that stops mid-line truncated", async () => {
    const whole = new TextEncoder().encode(helloText);
    serve(whole.slice(0, whole.length - 40));
    const store = await load("datasets/hello.ndjson.gz");
    expect(store.status).toBe("truncated");
  });

  it("says which file is missing", async () => {
    serve(new Uint8Array(), 404, "Not Found");
    const store = await load("datasets/x.ndjson.gz");
    expect(store.status).toBe("error");
    expect(store.problem?.message).toBe("datasets/x.ndjson.gz: 404 Not Found");
  });
});
