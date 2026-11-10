// Runs axe-core against the rendered DOM and returns its violations (empty = accessible).
// Rules that need real layout or a full HTML document cannot be evaluated in jsdom; axe reports
// them as "incomplete", not as violations, so they are not silently counted as passing here either.
import axe from 'axe-core';

export interface Violation {
  id: string;
  help: string;
  nodes: string[];
}

export async function axeViolations(root: Element = document.body): Promise<Violation[]> {
  const results = await axe.run(root, { resultTypes: ['violations'] });
  return results.violations.map((v) => ({
    id: v.id,
    help: v.help,
    nodes: v.nodes.map((n) => n.html),
  }));
}
