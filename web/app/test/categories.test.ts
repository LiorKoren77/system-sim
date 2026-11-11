import { describe, expect, it } from "vitest";
import { CATEGORIES, categoryOf } from "../src/diagram/categories";

// Every node type simsys knows (src/model/model.cppm's node_type_table).
const nodeTypes = [
  "client",
  "cdn",
  "load_balancer",
  "rate_limiter",
  "service",
  "cache",
  "sql_db",
  "kv_cluster",
  "message_queue",
  "worker_pool",
  "blob_store",
  "search_index",
  "ws_gateway",
  "service_discovery",
  "id_generator",
  "external_sink",
  "notification_fanout",
  "frontier",
];

describe("the categories a box is coloured by", () => {
  it("puts every node type in one of eight categories", () => {
    expect(CATEGORIES).toHaveLength(8);
    for (const type of nodeTypes) expect(CATEGORIES).toContain(categoryOf(type));
  });

  it("groups the types as the book's chapters talk about them", () => {
    expect(
      ["load_balancer", "rate_limiter", "ws_gateway", "service_discovery"].map(categoryOf),
    ).toEqual(Array(4).fill("traffic & routing"));
    expect(
      ["service", "worker_pool", "id_generator", "notification_fanout"].map(categoryOf),
    ).toEqual(Array(4).fill("compute"));
    expect(["cache", "cdn"].map(categoryOf)).toEqual(["cache", "cache"]);
    expect(["sql_db", "kv_cluster", "search_index"].map(categoryOf)).toEqual(
      Array(3).fill("database"),
    );
    expect(categoryOf("blob_store")).toBe("storage");
    expect(["message_queue", "frontier"].map(categoryOf)).toEqual(["queue", "queue"]);
    expect(categoryOf("external_sink")).toBe("external");
    expect(categoryOf("client")).toBe("client");
  });

  it("calls a type it does not know compute rather than failing (a newer simsys)", () => {
    expect(categoryOf("quantum_router")).toBe("compute");
  });
});
