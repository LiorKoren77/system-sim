import { describe, expect, it } from "vitest";
import { readStream, runSimsys } from "../src/run-simsys";

// A stand-in for simsys: Node itself, printing the given lines and exiting with `code`.
function fake(lines: readonly string[], code = 0) {
  const script = `process.stdout.write(${JSON.stringify(lines.join("\n") + "\n")}); process.exitCode = ${code};`;
  return runSimsys(process.execPath, ["-e", script], process.cwd());
}

const run =
  '{"type":"run","format_version":1,"scenario":"x","seed":1,"duration_s":60,"step_s":0.1,"warmup_s":0,"bucket_s":30,"frames":1}';
const graph =
  '{"type":"graph","rev":0,"nodes":[],"links":[],"ops":[],"node_columns":[],"link_columns":[],"op_columns":[]}';
const frame = '{"type":"frame","rev":0,"t_s":0,"dt_s":60,"nodes":[],"links":[],"ops":[]}';
const summary = '{"type":"summary","exit_code":2,"report":{"slo":[],"faults":[]}}';

describe("reading simsys's stream in the precompute", () => {
  it("keeps each line as simsys wrote it, beside its parsed message", async () => {
    const stream = readStream(await fake([run, graph, frame, summary], 2), "x.yaml");
    expect(stream.run.text).toBe(run);
    expect(stream.frames).toHaveLength(1);
    expect(stream.frames[0]?.text).toBe(frame);
    expect(stream.summary.message.exit_code).toBe(2); // a failed SLO is data, not a failure
  });

  it("fails with the reason when simsys cannot be started, as the run of that scenario", async () => {
    await expect(runSimsys("/nowhere/simsys", [], process.cwd())).rejects.toThrow("ENOENT");
  });

  it("refuses a stream that ends without its summary: the run was killed", async () => {
    const result = await fake([run, graph, frame], 0);
    expect(() => readStream(result, "x.yaml")).toThrow(
      "x.yaml: the stream ended without a summary (exit 0)",
    );
  });

  it("refuses a stream that ends on an error line, with its file, line and column", async () => {
    const error =
      '{"type":"error","file":"scenarios/x/scenario.yaml","line":3,"column":7,"message":"unknown key"}';
    const result = await fake([error], 1);
    expect(() => readStream(result, "scenarios/x/scenario.yaml")).toThrow(
      "scenarios/x/scenario.yaml:3:7: unknown key",
    );
  });
});
