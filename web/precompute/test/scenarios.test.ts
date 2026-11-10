import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { findScenarios, readmeHeading, simsysArgs } from "../src/scenarios";

const repoRoot = fileURLToPath(new URL("../../..", import.meta.url));

describe("the shipped scenarios the precompute runs", () => {
  it("finds all fourteen, sorted by id", async () => {
    const scenarios = await findScenarios(repoRoot);
    expect(scenarios.map((s) => s.id)).toEqual([
      "book/chat-system",
      "book/google-drive",
      "book/news-feed",
      "book/notification-system",
      "book/search-autocomplete",
      "book/url-shortener",
      "book/web-crawler",
      "book/youtube",
      "labs/consistent-hashing",
      "labs/kv-store",
      "labs/rate-limiter",
      "labs/unique-id",
      "tutorial/chain",
      "tutorial/hello",
    ]);
  });

  it("calls the seven 24 h book scenarios days and the rest runs", async () => {
    const scenarios = await findScenarios(repoRoot);
    const days = scenarios.filter((s) => s.kind === "day").map((s) => s.id);
    expect(days).toEqual([
      "book/chat-system",
      "book/google-drive",
      "book/news-feed",
      "book/notification-system",
      "book/search-autocomplete",
      "book/url-shortener",
      "book/youtube",
    ]);
    expect(scenarios.filter((s) => s.kind === "run")).toHaveLength(7);
  });

  it("runs a day for two days in 2 min buckets, and a run as it is", async () => {
    const scenarios = await findScenarios(repoRoot);
    const byId = new Map(scenarios.map((s) => [s.id, s]));
    const day = byId.get("book/chat-system");
    const run = byId.get("tutorial/hello");
    if (!day || !run) throw new Error("scenarios missing");
    expect(simsysArgs(day)).toEqual([
      "run",
      "scenarios/book/chat-system/scenario.yaml",
      "--stream",
      "--days",
      "2",
      "--every",
      "2m",
    ]);
    expect(simsysArgs(run)).toEqual(["run", "scenarios/tutorial/hello/scenario.yaml", "--stream"]);
  });

  it("reads web-crawler's diagram.yaml beside its scenario, and none where there is none", async () => {
    const scenarios = await findScenarios(repoRoot);
    const crawler = scenarios.find((s) => s.id === "book/web-crawler");
    expect(crawler?.diagram?.cells?.frontier).toEqual([1, 1]);
    expect(scenarios.find((s) => s.id === "tutorial/hello")?.diagram).toBeNull();
  });

  it("takes the title and the book chapter from the README's heading", () => {
    expect(readmeHeading("# Chat system (ch. 12)\n\nText")).toEqual({
      title: "Chat system",
      chapter: 12,
    });
    expect(readmeHeading("# Tutorial 1: hello, one client and one service\n")).toEqual({
      title: "Tutorial 1: hello, one client and one service",
      chapter: null,
    });
    expect(readmeHeading("no heading")).toEqual({ title: null, chapter: null });
  });
});
