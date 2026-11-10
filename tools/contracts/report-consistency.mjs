// Cross-checks inside one v1 run report that the schema can't express: per_k curves must agree
// with the headline metrics at k = 10 (and recall at k = 50). Returns a list of problems.
export function reportConsistency(report) {
  const problems = [];
  const at = (curve, k) => (curve.find(([kk]) => kk === k) ?? [])[1];
  const checks = [
    ['ndcg', 10, 'ndcg@10'], ['recall', 10, 'recall@10'], ['recall', 50, 'recall@50'], ['precision', 10, 'precision@10'],
  ];
  for (const [curve, k, metric] of checks) {
    const v = at(report.per_k?.[curve] ?? [], k);
    const m = report.metrics?.[metric]?.mean;
    if (v !== undefined && m !== undefined && Math.abs(v - m) > 1e-9) problems.push(`per_k.${curve}@${k} = ${v} but metrics.${metric}.mean = ${m}`);
  }
  for (const [name, curve] of Object.entries(report.per_k ?? {})) {
    for (let i = 1; i < curve.length; i++) if (curve[i][0] <= curve[i - 1][0]) problems.push(`per_k.${name} k values not strictly increasing`);
  }
  return problems;
}
