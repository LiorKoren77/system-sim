import { afterEach, describe, expect, it } from "vitest";
import { readHash, writeHash } from "../src/hash";

afterEach(() => {
  window.history.replaceState(null, "", "#");
});

describe("the address that keeps what is on the page", () => {
  it("reads the scenario and the node", () => {
    window.history.replaceState(null, "", "#book/chat-system&node=chat_servers");
    expect(readHash()).toEqual({ id: "book/chat-system", node: "chat_servers" });
  });

  it("reads a malformed escape as written, rather than leaving the page blank", () => {
    window.history.replaceState(null, "", "#100%&node=a%zz");
    expect(readHash()).toEqual({ id: "100%", node: "a%zz" });
  });

  it("gives back what it wrote, even a name holding & or %", () => {
    writeHash("book/chat-system", "a&b%c");
    expect(window.location.hash).toBe("#book/chat-system&node=a%26b%25c");
    expect(readHash()).toEqual({ id: "book/chat-system", node: "a&b%c" });
  });
});
