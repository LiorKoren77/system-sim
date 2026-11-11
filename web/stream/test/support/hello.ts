// The hello tutorial streamed in two buckets (`simsys run scenarios/tutorial/hello/scenario.yaml
// --stream --points 2`, simsys at dbcf013, the summary trimmed to the keys the tests read): the
// smallest real stream, a client and one service, pasted so these tests need no simsys binary.
export const helloLines: readonly string[] = [
  String.raw`{"type":"run","format_version":1,"scenario":"hello","seed":1,"duration_s":60,"step_s":0.1,"warmup_s":0,"bucket_s":30,"frames":2}`,
  String.raw`{"type":"graph","rev":0,"scenario":"hello","datacenters":[{"name":"main"}],"zones":["green","yellow","red"],"nodes":[{"id":"users","type":"client","datacenter":"main","replicas":1,"shards":null,"capacity_per_s":null,"unit":null,"yellow":null,"red":null,"queue_limit":null},{"id":"api","type":"service","datacenter":"main","replicas":1,"shards":null,"capacity_per_s":1000,"unit":"requests","yellow":0.7,"red":0.9,"queue_limit":1000}],"links":[{"from":"users","to":"api","direction":"inout","ops":[],"one_way_s":0.00025,"bandwidth_mb_s":null,"pipe_row":null}],"ops":[{"client":"users","op":"ping","share":1}],"slos":[],"faults":[],"node_columns":["demand_per_s","drain_per_s","dropped_per_s","rejected_per_s","backlog","peak_backlog","mean_rho","peak_rho","zone","red_s","latency_ms","queue_lag_s","connections","dead_lettered_per_s","deduplicated_per_s"],"link_columns":["demand_per_s","drain_per_s","dropped_per_s","backlog","peak_backlog","mean_rho","peak_rho","zone","red_s","latency_ms"],"op_columns":["offered_per_s","attempts_per_s","answered_per_s","timed_out_per_s","mean_ms","p99_ms"],"estimated_columns":["p99_ms"]}`,
  String.raw`{"type":"frame","rev":0,"t_s":0,"dt_s":30,"nodes":[[null,null,null,null,null,null,null,null,null,null,null,null,null,null,null],[100,100,0,0,0,0,0.1,0.1,0,0,1.11111,null,null,null,null]],"links":[],"ops":[[100,100,100,0,1.61111,5.66029]]}`,
  String.raw`{"type":"frame","rev":0,"t_s":30,"dt_s":30,"nodes":[[null,null,null,null,null,null,null,null,null,null,null,null,null,null,null],[100,100,0,0,0,0,0.1,0.1,0,0,1.11111,null,null,null,null]],"links":[],"ops":[[100,100,100,0,1.61111,5.66029]]}`,
  String.raw`{"type":"summary","exit_code":0,"report":{"format_version":1,"scenario":"hello","steps":600,"nodes":[],"links":[],"ops":[],"slo":[],"faults":[]}}`,
];

export const helloText = helloLines.join("\n") + "\n";
