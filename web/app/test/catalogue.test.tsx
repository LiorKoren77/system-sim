import { render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";
import { Catalogue } from "../src/catalogue/Catalogue";
import { entry, index } from "./support";

const datasets = index([
  entry("book/chat-system", {
    title: "Chat system",
    chapter: 12,
    kind: "day",
    loops: true,
    seam: "repeats",
  }),
  entry("book/youtube", {
    title: "YouTube",
    chapter: 14,
    kind: "day",
    loops: false,
    seam: "drifts",
  }),
  entry("labs/kv-store", { title: "Lab: key-value store", chapter: 6, duration_s: 1800 }),
]);

describe("the catalogue of datasets", () => {
  it("groups the datasets as the scenarios folder does", () => {
    render(
      <Catalogue
        index={datasets}
        selected={null}
        onSelect={() => undefined}
        open
        onToggle={() => undefined}
      />,
    );
    expect(screen.getAllByRole("heading", { level: 2 }).map((h) => h.textContent)).toEqual([
      "book",
      "labs",
    ]);
  });

  it("badges a looping day, a day that does not repeat, and a run with its length", () => {
    render(
      <Catalogue
        index={datasets}
        selected={null}
        onSelect={() => undefined}
        open
        onToggle={() => undefined}
      />,
    );
    expect(
      within(screen.getByRole("button", { name: /Chat system/ })).getByText("repeating day"),
    ).toBeVisible();
    expect(
      within(screen.getByRole("button", { name: /YouTube/ })).getByText("does not repeat"),
    ).toBeVisible();
    expect(
      within(screen.getByRole("button", { name: /key-value/ })).getByText("run · 30 min"),
    ).toBeVisible();
  });

  it("names the book chapter beside the title", () => {
    render(
      <Catalogue
        index={datasets}
        selected={null}
        onSelect={() => undefined}
        open
        onToggle={() => undefined}
      />,
    );
    expect(screen.getByRole("button", { name: /Chat system/ })).toHaveTextContent("ch. 12");
  });

  it("tells which dataset was chosen", async () => {
    const onSelect = vi.fn();
    render(
      <Catalogue
        index={datasets}
        selected="book/youtube"
        onSelect={onSelect}
        open
        onToggle={() => undefined}
      />,
    );
    expect(screen.getByRole("button", { name: /YouTube/ })).toHaveAttribute("aria-current", "true");
    await userEvent.click(screen.getByRole("button", { name: /Chat system/ }));
    expect(onSelect).toHaveBeenCalledWith("book/chat-system");
  });
});
