// A worker pool pulls from the queue it consumes (book ch. 10: the notification workers read their
// channel's queue; Kafka's consumers fetch from the broker). simsys declares that link queue → pool,
// the way the messages move, so on the diagram the pool is the caller of a link it is the target of:
// its arrowhead points at the queue (caller to callee), its dashes still move toward the pool (the
// way the data goes), and the messages it has not read yet, which simsys counts as the pool's
// backlog ("what a group has not drained waits in its pool's queue", docs/scenario-schema.md), are
// drawn in the queue's box, where a reader expects them to wait.
import type { GraphLine } from "@simsys/stream";

const queueTypes = new Set(["message_queue", "frontier"]);

// Whether `link` is a pool pulling from its queue.
export function isPull(graph: GraphLine, link: GraphLine["links"][number]): boolean {
  const type = (id: string) => graph.nodes.find((n) => n.id === id)?.type;
  return queueTypes.has(type(link.from) ?? "") && type(link.to) === "worker_pool";
}

// For each pool that pulls from a queue, that queue's id.
export function queueOfPool(graph: GraphLine): Map<string, string> {
  const queues = new Map<string, string>();
  for (const link of graph.links) if (isPull(graph, link)) queues.set(link.to, link.from);
  return queues;
}
