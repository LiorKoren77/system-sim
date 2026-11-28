// The categories a diagram's boxes are coloured by: the kinds of component architecture diagrams
// usually show, eight of them for simsys's eighteen node types. The colour says what a box is; the
// zone (its border and pill, NodeCard.tsx) says how it is doing, so the category hues keep clear of
// the zones' green, amber and red (theme.css).

export const CATEGORIES = [
  "client",
  "traffic & routing", // what spreads, limits or directs requests
  "compute", // stateless servers and workers
  "cache", // copies close to the reader
  "database", // the system of record
  "storage", // blobs and files
  "queue", // work waiting for a consumer
  "external", // outside the system: a third-party service, the web
] as const;

export type Category = (typeof CATEGORIES)[number];

const byType: Readonly<Record<string, Category>> = {
  client: "client",
  load_balancer: "traffic & routing",
  rate_limiter: "traffic & routing",
  ws_gateway: "traffic & routing",
  service_discovery: "traffic & routing",
  service: "compute",
  worker_pool: "compute",
  id_generator: "compute",
  notification_fanout: "compute",
  cache: "cache",
  cdn: "cache",
  sql_db: "database",
  kv_cluster: "database",
  search_index: "database",
  blob_store: "storage",
  message_queue: "queue",
  frontier: "queue",
  external_sink: "external",
};

// A node type's category. A type this page does not know (a newer simsys) is drawn as compute, the
// most general one, rather than breaking the page.
export function categoryOf(type: string): Category {
  return byType[type] ?? "compute";
}

// The category as a CSS class name: `traffic & routing` → `cat-traffic-routing`.
export function categoryClass(category: Category): string {
  return `cat-${category.replace(/[^a-z]+/g, "-")}`;
}
