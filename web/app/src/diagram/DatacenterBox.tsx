import type { Node, NodeProps } from "@xyflow/react";

// A datacenter's box around its nodes, labelled with its name.
export function DatacenterBox({ data }: NodeProps<Node<{ label: string }>>) {
  return <div className="datacenter-label">{data.label}</div>;
}
