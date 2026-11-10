// How it works: a short explanation of each recommender (from the spec's primer), each with its
// latest headline result from the run reports (GET /api/reports) and a link to the Evaluation page.
// The explanations are static, so they show even when the reports can't be loaded.
import { useCallback, useId, type ReactNode } from 'react';
import { Link } from 'react-router';
import { api, type Schemas } from '../../api/client';
import { ErrorMessage } from '../../components/Status';
import { modelLabel, type ModelName } from '../../lib/models';
import { useAsync, type AsyncState } from '../../lib/useAsync';
import '../../components/components.css';

type ReportSummary = Schemas['ReportSummary'];
type ReportList = Schemas['ReportList'];

interface ModelStory {
  model: ModelName;
  evidence: string;
  idea: ReactNode;
  teaches: string;
}

const STORIES: ModelStory[] = [
  {
    model: 'pop',
    evidence: "Everyone's ratings.",
    idea: (
      <>
        Rank movies by a damped mean rating: each movie's average is pulled towards the overall average until it has
        enough ratings, so a movie rated 5 stars once doesn't top the list. A pure count-based variant (most-rated
        first) is reported too.
      </>
    ),
    teaches: 'Baselines, and why they are hard to beat. Every other model has to beat this one.',
  },
  {
    model: 'content',
    evidence: 'Movie features: genres and tags.',
    idea: (
      <>
        Each movie is a vector of features. Your profile is the average of the movies you rated, weighted by how much
        more or less you liked them than you usually do. Movies whose vectors point the same way as your profile score
        highest.
      </>
    ),
    teaches: 'Cold start for new movies, and over-specialization: it keeps suggesting more of the same.',
  },
  {
    model: 'knn',
    evidence: 'Who rated what.',
    idea: (
      <>
        Two movies are similar if the same users liked them. A movie's score is a similarity-weighted sum over the
        movies you rated, which is why it can say “because you rated X”.
      </>
    ),
    teaches: 'Collaborative filtering, sparsity and explainability.',
  },
  {
    model: 'mf',
    evidence: 'Star ratings.',
    idea: (
      <>
        Learn a short vector of hidden “taste” factors for every user and every movie. Their dot product, plus a
        user bias and a movie bias, predicts the rating. The factors are fitted by stochastic gradient descent.
      </>
    ),
    teaches: 'Latent factors, regularization and gradient descent.',
  },
  {
    model: 'als',
    evidence: 'Whether you watched and liked a movie, not the exact stars.',
    idea: (
      <>
        The same factor model, but every movie you haven't seen counts as a weak “no”. It is solved by alternating
        least squares: fix the movie factors and solve for each user exactly, then the other way round.
      </>
    ),
    teaches: 'Implicit feedback and alternating least squares.',
  },
];

export default function HowItWorksPage() {
  const load = useCallback((signal: AbortSignal) => api.get('/api/reports', { signal }), []);
  const { state, reload } = useAsync(load);

  return (
    <div className="page page--prose">
      <h1>How it works</h1>
      <p className="lede">
        Every model here answers one question: given what you have rated, how much will you like each movie you
        haven't seen? They differ in the evidence they use. Each one is measured offline by NDCG@10, how well it ranks
        the movies people went on to rate 4 stars or more.
      </p>
      <p>
        <strong>Fold-in</strong> is what lets the site recommend for you even though you were not in the training data:
        the movie side of each model stays fixed, and your side is computed from your ratings when you ask. Models are
        retrained offline on a schedule.
      </p>
      {state.status === 'error' && (
        <ErrorMessage
          message="Results couldn't be loaded. The explanations below don't depend on them."
          onRetry={() => reload()}
        />
      )}
      {STORIES.map((story) => (
        <ModelSection key={story.model} story={story} reports={state} />
      ))}
      <h2>From models to a product</h2>
      <p>
        Real systems layer these: a cheap model generates a few hundred candidates, a heavier model ranks them, and a
        re-ranker adds diversity and rules. This site builds each model as a full ranker first. Try them side by side on
        the <Link to="/compare">Compare</Link> page.
      </p>
    </div>
  );
}

/** The newest schema-1 report for this model that has headline numbers. */
function latestRun(items: ReportSummary[], model: ModelName): ReportSummary | undefined {
  return items
    .filter((r) => r.schema_version === 1 && r.model === model && r.headline !== null && r.created_at !== null)
    .sort((a, b) => (b.created_at ?? '').localeCompare(a.created_at ?? ''))[0];
}

function ModelSection({ story, reports }: { story: ModelStory; reports: AsyncState<ReportList> }) {
  const headingId = useId();
  const label = modelLabel(story.model);
  return (
    <section className="model-story" aria-labelledby={headingId}>
      <h2 id={headingId}>{label}</h2>
      <p>
        <strong>Evidence:</strong> {story.evidence}
      </p>
      <p>{story.idea}</p>
      <p>
        <strong>Teaches:</strong> {story.teaches}
      </p>
      <p className="model-story__result">
        <Result model={story.model} reports={reports} />{' '}
        <Link to="/evaluation">See {label} results on the Evaluation page</Link>
      </p>
    </section>
  );
}

function Result({ model, reports }: { model: ModelName; reports: AsyncState<ReportList> }) {
  if (reports.status === 'loading') return <span className="muted">Loading results…</span>;
  if (reports.status === 'error') return null;
  const run = latestRun(reports.data.items, model);
  if (!run?.headline) return <span className="muted">Not evaluated yet.</span>;
  const ndcg = run.headline['ndcg@10'];
  const [low, high] = ndcg.ci95;
  return (
    <span>
      Latest result: NDCG@10 {ndcg.mean.toFixed(3)} (95% CI {low?.toFixed(3)}–{high?.toFixed(3)}) on {run.dataset}
      {run.evaluated_on ? ` (${run.evaluated_on} set)` : ''}.
    </span>
  );
}
