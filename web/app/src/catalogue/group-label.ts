// The catalogue groups datasets by the first segment of their id, the folder under scenarios/. The
// folder `book` holds the full systems, so it is shown as "systems"; the id (and the URL hash)
// keep the folder's name. Any other folder is shown as it is named.
export function groupLabel(group: string): string {
  return group === "book" ? "systems" : group;
}
