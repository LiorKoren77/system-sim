// The address keeps what is on the page, `#book/chat-system&node=chat_servers`, so a link opens
// the same scenario and node. Each part is escaped on its own (a node named `a&b` stays one part);
// a `/` is left readable.

// The part as it was escaped, or as written when it is no valid escape (`#100%` typed by hand):
// decodeURIComponent throws a URIError then, and a page that threw here would stay blank.
function unescape(part: string): string {
  try {
    return decodeURIComponent(part);
  } catch {
    return part;
  }
}

function escape(part: string): string {
  return encodeURIComponent(part).replaceAll("%2F", "/");
}

export function readHash(): { id: string | null; node: string | null } {
  const [id, ...rest] = window.location.hash.replace(/^#/, "").split("&");
  const node = rest.find((part) => part.startsWith("node="))?.slice("node=".length);
  return {
    id: id === undefined || id === "" ? null : unescape(id),
    node: node === undefined ? null : unescape(node),
  };
}

export function writeHash(id: string, node: string | null): void {
  const hash = node === null ? `#${escape(id)}` : `#${escape(id)}&node=${escape(node)}`;
  window.history.replaceState(null, "", hash);
}
