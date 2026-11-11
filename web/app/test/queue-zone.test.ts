import { describe, expect, it } from "vitest";
import { queueZone } from "../src/diagram/queue-zone";

describe("the zone of a queue", () => {
  it("is yellow from 70% of its lag SLO and red past it, the zones' usual thresholds", () => {
    // notification-system at 09:12: ios_q's lag 3.7 min against its SLO of 5 min.
    expect(queueZone({ lag: 222, lagMax: 300, backlog: 0, room: null })).toBe("yellow");
    expect(queueZone({ lag: 150, lagMax: 300, backlog: 0, room: null })).toBe("green");
    expect(queueZone({ lag: 301, lagMax: 300, backlog: 0, room: null })).toBe("red");
  });

  it("is yellow from 70% of its room and red from 90%, where messages start to be deleted unread", () => {
    expect(queueZone({ lag: null, lagMax: null, backlog: 700, room: 1000 })).toBe("yellow");
    expect(queueZone({ lag: null, lagMax: null, backlog: 900, room: 1000 })).toBe("red");
    expect(queueZone({ lag: null, lagMax: null, backlog: 100, room: 1000 })).toBe("green");
  });

  it("is the worse of the two, as a node's zone is the worst of its bucket", () => {
    expect(queueZone({ lag: 301, lagMax: 300, backlog: 100, room: 1000 })).toBe("red");
    expect(queueZone({ lag: 10, lagMax: 300, backlog: 950, room: 1000 })).toBe("red");
  });

  it("has no zone when nothing judges it: no lag SLO and no room, a queue doing its job", () => {
    // notification-system's email_q: 43 min of lag against no rule the scenario states.
    expect(queueZone({ lag: 2574, lagMax: null, backlog: 5e6, room: null })).toBeNull();
  });

  it("judges the room alone when the lag is unbounded or unknown", () => {
    expect(queueZone({ lag: null, lagMax: 300, backlog: 950, room: 1000 })).toBe("red");
  });
});
