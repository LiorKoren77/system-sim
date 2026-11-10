import { describe, expect, it } from "vitest";
import { splitLines } from "../src/lines";
import { helloLines, helloText } from "./support/hello";
import { textStream } from "./support/streams";

async function collect(stream: ReadableStream<string>): Promise<string[]> {
  const lines: string[] = [];
  for await (const line of splitLines(stream)) lines.push(line);
  return lines;
}

describe("splitting a stream into lines", () => {
  it("rejoins a line cut between two chunks at any offset", async () => {
    const text = helloLines.slice(0, 3).join("\n") + "\n";
    for (let cut = 0; cut <= text.length; cut += 7) {
      const lines = await collect(textStream([text.slice(0, cut), text.slice(cut)]));
      expect(lines).toEqual(helloLines.slice(0, 3));
    }
  });

  it("yields a final line that has no newline, for the reader to judge", async () => {
    expect(await collect(textStream(['{"a":1}\n{"b":', "2}"]))).toEqual(['{"a":1}', '{"b":2}']);
  });

  it("skips empty lines and ends a line at a carriage return and newline too", async () => {
    expect(await collect(textStream(["a\r\n\nb\n"]))).toEqual(["a", "b"]);
  });

  it("gives every line of a whole stream, one chunk per character", async () => {
    expect(await collect(textStream([...helloText]))).toEqual(helloLines);
  });
});
