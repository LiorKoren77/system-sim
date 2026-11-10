// The address keeps what is on the page, `#book/chat-system&node=chat_servers`, so a link opens
// the same scenario and node.

export function readHash(): { id: string | null; node: string | null } {
  const [id, ...rest] = decodeURIComponent(window.location.hash.replace(/^#/, "")).split("&");
  const node = rest.find((part) => part.startsWith("node="))?.slice("node=".length) ?? null;
  return { id: id === undefined || id === "" ? null : id, node };
}

export function writeHash(id: string, node: string | null): void {
  const hash = node === null ? `#${id}` : `#${id}&node=${node}`;
  window.history.replaceState(null, "", hash);
}
