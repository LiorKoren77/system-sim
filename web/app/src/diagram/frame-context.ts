import { createContext } from "react";
import type { NodeState } from "./to-flow-elements";

// The state of every node at the slider's frame, by id. The node cards read it from here instead of
// from their React Flow data, so a new frame re-renders the cards' contents and nothing of React
// Flow's (to-flow-elements.ts says why that matters).
export const FrameContext = createContext<ReadonlyMap<string, NodeState>>(new Map());
