import { act, render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { MemorySource } from "@simsys/stream";
import { afterEach, describe, expect, it, vi } from "vitest";
import { App } from "../src/App";
import { entry, fixtureLines, index } from "./support";

// React Flow measures the DOM, which jsdom does not lay out: the diagram's tests test its inputs
// (to-flow-elements, layout) instead, and here it is a stand-in.
vi.mock("../src/diagram/Diagram", () => ({
  Diagram: () => <div data-testid="diagram" />,
}));
// uPlot draws on a canvas jsdom does not have: its inputs are tested in series.test.ts.
vi.mock("../src/charts/Chart", () => ({
  Chart: (props: { spec: { title: string } }) => <figure aria-label={props.spec.title} />,
}));

const datasets = index([
  entry("book/url-shortener", {
    title: "URL shortener",
    chapter: 8,
    kind: "day",
    loops: true,
    seam: "repeats",
  }),
  entry("tutorial/hello", { title: "Tutorial 1: hello", duration_s: 60 }),
]);

function renderApp(lines: Record<string, string[]>) {
  return render(
    <App
      loadIndex={() => Promise.resolve(datasets)}
      sourceFor={(e) => new MemorySource(lines[e.id] ?? [])}
    />,
  );
}

afterEach(() => {
  window.location.hash = "";
  window.localStorage.clear();
});

describe("the page", () => {
  it("opens the first looping day and stands at midnight, ready to play", async () => {
    renderApp({ "book/url-shortener": fixtureLines("book-url-shortener") });
    expect(await screen.findByRole("heading", { level: 1, name: /URL shortener/ })).toBeVisible();
    expect(await screen.findByText("00:00")).toBeVisible();
    expect(screen.getByRole("button", { name: "Play" })).toBeEnabled();
  });

  it("keeps the chosen dataset in the address, and opens what the address names", async () => {
    window.location.hash = "#tutorial/hello";
    renderApp({ "tutorial/hello": fixtureLines("tutorial-hello") });
    expect(await screen.findByRole("heading", { level: 1, name: /hello/ })).toBeVisible();
    await userEvent.click(screen.getByRole("button", { name: /URL shortener/ }));
    expect(window.location.hash).toBe("#book/url-shortener");
  });

  it("says when a dataset ended early", async () => {
    renderApp({ "book/url-shortener": fixtureLines("book-url-shortener").slice(0, 50) });
    expect(
      await screen.findByText(/The dataset ended early: rebuild it with npm run precompute/),
    ).toBeVisible();
  });

  it("says when a dataset was written for another version of the page", async () => {
    const lines = fixtureLines("book-url-shortener");
    lines[1] = (lines[1] ?? "").replace('"format_version":1', '"format_version":2');
    renderApp({ "book/url-shortener": lines });
    expect(
      await screen.findByText(/this page reads format_version 1; the data says 2/),
    ).toBeVisible();
  });

  it("says what to do when there are no datasets yet", async () => {
    render(
      <App
        loadIndex={() => Promise.reject(new Error("404"))}
        sourceFor={() => new MemorySource([])}
      />,
    );
    expect(
      await screen.findByText("No datasets yet: run npm run precompute in web/"),
    ).toBeVisible();
  });

  it("plays with the space bar and moves an hour with shift and an arrow", async () => {
    renderApp({ "book/url-shortener": fixtureLines("book-url-shortener") });
    await screen.findByText("00:00");
    await userEvent.keyboard("{Shift>}{ArrowRight}{/Shift}");
    expect(screen.getByText("01:00")).toBeVisible();
    await userEvent.keyboard("{ArrowLeft}");
    expect(screen.getByText("00:59")).toBeVisible();
    await userEvent.keyboard(" ");
    expect(screen.getByRole("button", { name: "Pause" })).toBeVisible();
    act(() => undefined);
  });

  it("shows the day's SLO table, and the charts of the box the address names", async () => {
    window.location.hash = "#book/url-shortener&node=url_db";
    renderApp({ "book/url-shortener": fixtureLines("book-url-shortener") });
    expect(await screen.findByRole("table", { name: "SLOs" })).toBeVisible();
    expect(screen.getByRole("heading", { name: "url_db" })).toBeVisible();
    expect(screen.getByRole("figure", { name: "busy" })).toBeVisible();
  });

  it("asks for a box to chart while none is chosen", async () => {
    renderApp({ "book/url-shortener": fixtureLines("book-url-shortener") });
    expect(await screen.findByText("Click a box to chart its day.")).toBeVisible();
  });

  it("chooses a box from its row in the zone strip", async () => {
    renderApp({ "book/url-shortener": fixtureLines("book-url-shortener") });
    await userEvent.click(await screen.findByRole("button", { name: "web" }));
    expect(window.location.hash).toBe("#book/url-shortener&node=web");
    expect(screen.getByRole("heading", { name: "web" })).toBeVisible();
  });

  it("hides the list of scenarios to give the diagram its width, and brings it back", async () => {
    renderApp({ "book/url-shortener": fixtureLines("book-url-shortener") });
    await screen.findByText("00:00");
    await userEvent.click(screen.getByRole("button", { name: "Hide the scenarios" }));
    expect(screen.queryByRole("button", { name: /Tutorial 1: hello/ })).toBeNull();
    expect(screen.getByRole("heading", { level: 1, name: /URL shortener/ })).toBeVisible();
    await userEvent.click(screen.getByRole("button", { name: "Show the scenarios" }));
    expect(screen.getByRole("button", { name: /Tutorial 1: hello/ })).toBeVisible();
  });

  it("opens with the panes as the viewer left them", async () => {
    const first = renderApp({ "book/url-shortener": fixtureLines("book-url-shortener") });
    await screen.findByText("00:00");
    await userEvent.click(screen.getByRole("button", { name: "Hide the scenarios" }));
    screen.getByRole("separator", { name: "Resize the charts panel" }).focus();
    await userEvent.keyboard("{ArrowLeft}");
    const width = screen.getByRole("separator").getAttribute("aria-valuenow");
    first.unmount();
    renderApp({ "book/url-shortener": fixtureLines("book-url-shortener") });
    await screen.findByText("00:00");
    expect(screen.getByRole("button", { name: "Show the scenarios" })).toBeVisible();
    expect(screen.getByRole("separator").getAttribute("aria-valuenow")).toBe(width);
  });

  it("resizes the charts panel from the keyboard without moving the day", async () => {
    renderApp({ "book/url-shortener": fixtureLines("book-url-shortener") });
    await screen.findByText("00:00");
    const splitter = screen.getByRole("separator", { name: "Resize the charts panel" });
    expect(splitter.getAttribute("aria-valuenow")).toBe("420");
    splitter.focus();
    await userEvent.keyboard("{ArrowLeft}");
    expect(Number(splitter.getAttribute("aria-valuenow"))).toBeGreaterThan(420);
    expect(screen.getByText("00:00")).toBeVisible();
  });

  it("gives the charts panel its first width back on a double click of the splitter", async () => {
    renderApp({ "book/url-shortener": fixtureLines("book-url-shortener") });
    await screen.findByText("00:00");
    const splitter = screen.getByRole("separator", { name: "Resize the charts panel" });
    splitter.focus();
    await userEvent.keyboard("{ArrowLeft}{ArrowLeft}");
    await userEvent.dblClick(splitter);
    expect(splitter.getAttribute("aria-valuenow")).toBe("420");
  });
});
