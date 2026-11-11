import { render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";
import { FaultBlocks } from "../src/report/FaultBlocks";
import { SloTable } from "../src/report/SloTable";
import { fixtureStore } from "./support";

describe("the SLO table", () => {
  it("lists kv-store's three objectives with their verdicts", async () => {
    const store = await fixtureStore("labs-kv-store");
    render(<SloTable store={store} />);
    const rows = screen.getAllByRole("row").slice(1); // the header row first
    expect(rows).toHaveLength(3);
    expect(within(rows[0] as HTMLElement).getByText("clients · put")).toBeVisible();
    expect(within(rows[0] as HTMLElement).getByText("pass")).toBeVisible();
    expect(within(rows[1] as HTMLElement).getByText("61%")).toBeVisible();
  });
});

describe("the fault blocks", () => {
  it("say what kv-store's node_down did, during the fault and after it", async () => {
    const store = await fixtureStore("labs-kv-store");
    render(<FaultBlocks store={store} onSeek={() => undefined} />);
    const block = screen.getByRole("button", { name: /node_down store/ });
    // At 600 s for 180 s; the report has it recovered at 780 s.
    expect(within(block).getByText("at 10m 00s · for 3 min")).toBeVisible();
    // Offered and served alike during the fault: one store copy of 16 down, the rest absorbed it.
    expect(within(block).getAllByText("84.4k/s")).toHaveLength(2);
    expect(within(block).getByText("0 requests")).toBeVisible(); // lost
    expect(within(block).getByText("10m 00s → 13m 00s")).toBeVisible(); // down, then recovered
  });

  it("move the slider to the fault's frame when clicked: 600 s is frame 240 of 2.5 s buckets", async () => {
    const store = await fixtureStore("labs-kv-store");
    const onSeek = vi.fn();
    render(<FaultBlocks store={store} onSeek={onSeek} />);
    await userEvent.click(screen.getByRole("button", { name: /node_down store/ }));
    expect(onSeek).toHaveBeenCalledWith(240);
  });

  it("say so when the window had no fault", async () => {
    const store = await fixtureStore("tutorial-chain");
    render(<FaultBlocks store={store} onSeek={() => undefined} />);
    expect(screen.getByText("No scripted fault in this window.")).toBeVisible();
  });
});
