// One chart: a title, a one-sentence summary, the Observable Plot SVG, an optional legend and notes,
// and a data table with the same numbers.
//
// The SVG is exposed as a single image (role="img") named by the title and described by the
// summary. The table is always in the accessibility tree; "Show data" only makes it visible.
// Plot's tooltips follow the pointer only; keyboard and screen-reader users get every value from
// the data table instead.
import * as Plot from '@observablehq/plot';
import { useEffect, useId, useRef, useState, useSyncExternalStore, type ReactNode, type RefObject } from 'react';
import { modelStyle } from '../../lib/reports/models';
import { ErrorBoundary } from './ErrorBoundary';

// ---- Theme and size ----

const DARK_QUERY = '(prefers-color-scheme: dark)';

function subscribeToScheme(onChange: () => void): () => void {
  if (typeof window.matchMedia !== 'function') return () => {};
  const query = window.matchMedia(DARK_QUERY);
  query.addEventListener('change', onChange);
  return () => query.removeEventListener('change', onChange);
}

/** True when the OS is in dark mode (the site follows prefers-color-scheme). */
export function useDarkMode(): boolean {
  return useSyncExternalStore(
    subscribeToScheme,
    () => typeof window.matchMedia === 'function' && window.matchMedia(DARK_QUERY).matches,
    () => false,
  );
}

const DEFAULT_WIDTH = 640;

/** The element's content width, following resizes (DEFAULT_WIDTH where it can't be measured). */
function useWidth(ref: RefObject<HTMLElement | null>): number {
  const [width, setWidth] = useState(DEFAULT_WIDTH);
  useEffect(() => {
    const element = ref.current;
    if (!element || typeof ResizeObserver !== 'function') return;
    const observer = new ResizeObserver(([entry]) => {
      const measured = Math.floor(entry?.contentRect.width ?? 0);
      if (measured > 0) setWidth(measured);
    });
    observer.observe(element);
    return () => observer.disconnect();
  }, [ref]);
  return width;
}

/** The color of `model` in the current theme. */
export function modelColor(model: string, dark: boolean): string {
  const style = modelStyle(model);
  return dark ? style.dark : style.light;
}

// ---- Plot rendering ----

export interface PlotContext {
  width: number;
  dark: boolean;
}

function PlotSvg<T>({
  build,
  input,
  titleId,
  descriptionId,
}: {
  build: (input: T, context: PlotContext) => Plot.PlotOptions;
  input: T;
  titleId: string;
  descriptionId: string;
}) {
  const container = useRef<HTMLDivElement>(null);
  const width = useWidth(container);
  const dark = useDarkMode();

  useEffect(() => {
    const host = container.current;
    if (!host) return;
    const chart = Plot.plot({ width, ...build(input, { width, dark }) });
    const svg = chart instanceof SVGSVGElement ? chart : chart.querySelector('svg');
    if (svg) {
      svg.setAttribute('role', 'img');
      svg.setAttribute('aria-labelledby', titleId);
      svg.setAttribute('aria-describedby', descriptionId);
      // Plot names each mark group with aria-label ("line", "dot"), which is not allowed on a
      // role-less <g> and is noise inside an image anyway; keep the name as a data attribute.
      for (const node of svg.querySelectorAll('[aria-label]')) {
        node.setAttribute('data-mark', node.getAttribute('aria-label') ?? '');
        node.removeAttribute('aria-label');
      }
    }
    host.replaceChildren(chart);
    return () => chart.remove();
  }, [build, input, width, dark, titleId, descriptionId]);

  return <div ref={container} className="chart__plot" />;
}

// ---- Legend ----

export interface LegendItem {
  label: string;
  /** Model whose color and symbol to show. */
  model: string;
  /** Overrides the model's dash ('' = solid); omit for marker-only swatches. */
  dash?: string;
  line?: boolean;
  hollow?: boolean;
}

function Swatch({ item }: { item: LegendItem }) {
  const host = useRef<HTMLSpanElement>(null);
  const dark = useDarkMode();
  useEffect(() => {
    const element = host.current;
    if (!element) return;
    const style = modelStyle(item.model);
    const color = dark ? style.dark : style.light;
    const marks: Plot.Markish[] = [];
    if (item.line) {
      marks.push(Plot.ruleY([0], { stroke: color, strokeWidth: 2, strokeDasharray: item.dash ?? style.dash }));
    }
    marks.push(
      Plot.dot([0], {
        x: () => 0,
        y: () => 0,
        r: 4,
        symbol: style.symbol,
        fill: item.hollow ? 'none' : color,
        stroke: color,
        strokeWidth: item.hollow ? 1.5 : 0,
      }),
    );
    const chart = Plot.plot({
      width: 32,
      height: 14,
      margin: 0,
      axis: null,
      x: { domain: [-1, 1] },
      y: { domain: [-1, 1] },
      marks,
    });
    chart.setAttribute('aria-hidden', 'true');
    element.replaceChildren(chart);
    return () => chart.remove();
  }, [item, dark]);
  return <span ref={host} className="legend__swatch" aria-hidden="true" />;
}

export function Legend({ items }: { items: LegendItem[] }) {
  return (
    <ul className="legend" aria-label="Legend">
      {items.map((item) => (
        <li key={item.label}>
          <Swatch item={item} />
          {item.label}
        </li>
      ))}
    </ul>
  );
}

// ---- Data table ----

export interface TableData {
  /** Column headers; the first column holds row headers. */
  columns: string[];
  rows: { header: string; cells: ReactNode[] }[];
}

export function DataTable({ caption, data }: { caption: string; data: TableData }) {
  return (
    <table className="data-table">
      <caption>{caption}</caption>
      <thead>
        <tr>
          {data.columns.map((column, i) => (
            <th key={i} scope="col">
              {column}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {data.rows.map((row, i) => (
          <tr key={i}>
            <th scope="row">{row.header}</th>
            {row.cells.map((cell, i) => (
              <td key={i}>{cell}</td>
            ))}
          </tr>
        ))}
      </tbody>
    </table>
  );
}

// ---- Figure ----

export interface ChartFigureProps<T> {
  title: string;
  /** One or two sentences on what the chart shows; also the SVG's accessible description. */
  description: string;
  /**
   * Plot options for `input` at the current width and theme. Pass a module-level function and
   * a stable `input`: the chart is redrawn whenever either changes.
   */
  build: (input: T, context: PlotContext) => Plot.PlotOptions;
  input: T;
  table: TableData;
  legend?: LegendItem[];
  notes?: ReactNode;
  headingLevel?: 3 | 4;
}

export function ChartFigure<T>({
  title,
  description,
  build,
  input,
  table,
  legend,
  notes,
  headingLevel = 3,
}: ChartFigureProps<T>) {
  const titleId = useId();
  const descriptionId = useId();
  const tableId = useId();
  const [showData, setShowData] = useState(false);
  const Heading = headingLevel === 3 ? 'h3' : 'h4';

  return (
    <figure className="chart">
      <figcaption>
        <Heading id={titleId} className="chart__title">
          {title}
        </Heading>
        <p id={descriptionId} className="chart__description">
          {description}
        </p>
      </figcaption>
      {legend && legend.length > 1 && <Legend items={legend} />}
      <ErrorBoundary message={`Couldn't draw the chart "${title}". The numbers are still in the data table below.`}>
        <PlotSvg build={build} input={input} titleId={titleId} descriptionId={descriptionId} />
      </ErrorBoundary>
      {notes}
      <button
        type="button"
        className="button button--quiet chart__toggle"
        aria-expanded={showData}
        aria-controls={tableId}
        onClick={() => setShowData((shown) => !shown)}
      >
        {showData ? 'Hide data' : 'Show data'}
      </button>
      <div id={tableId} className={showData ? 'chart__data' : 'chart__data visually-hidden'}>
        <DataTable caption={`${title} (data)`} data={table} />
      </div>
    </figure>
  );
}
