// @simsys/stream: the simulator's output contract (docs/scenario-schema.md, "simsys run --stream"
// and "The output contract") in TypeScript, shared by the precompute and the page.

// The version of simsys's JSON output this code reads: every JSON document simsys writes carries it
// as `format_version`, and a reader refuses another (the contract's rule 3: a removed, renamed or
// re-meant key raises it).
export const FORMAT_VERSION = 1;
