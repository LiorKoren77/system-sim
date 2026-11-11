// NDJSON is line-framed: one JSON object per line, so a reader cuts the text at each newline and
// parses each piece alone, however the bytes were chunked on the way (a network packet, a pipe's
// buffer). This is the one place the project does that cutting, for the page and the precompute.

// The lines of `text`, in order: a line split across chunks is rejoined, `\r\n` ends a line like
// `\n`, empty lines are skipped, and a last line without a newline is still yielded (the caller
// decides whether a stream that ended there was cut short).
// An `async function*` is an async generator: it yields values as they become available and the
// caller pulls them with `for await`, the TypeScript counterpart of a C++ coroutine generator.
export async function* splitLines(text: ReadableStream<string>): AsyncGenerator<string> {
  // A reader rather than `for await` over the stream itself, which not every browser supports yet.
  const reader = text.getReader();
  let pending = "";
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      pending += value;
      let start = 0;
      let newline = pending.indexOf("\n", start);
      while (newline !== -1) {
        const line = trimCarriageReturn(pending.slice(start, newline));
        if (line !== "") yield line;
        start = newline + 1;
        newline = pending.indexOf("\n", start);
      }
      pending = pending.slice(start);
    }
  } finally {
    reader.releaseLock();
  }
  const last = trimCarriageReturn(pending);
  if (last !== "") yield last;
}

function trimCarriageReturn(line: string): string {
  return line.endsWith("\r") ? line.slice(0, -1) : line;
}
